#include "framekeyboard/cjk.hpp"
#include "cjk_engine.hpp"
#include "framekeyboard/localization.hpp"
#include "table_engine.hpp"

#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>

namespace framekeyboard {
namespace {
// A shell preview builds its own App, and engine, on a worker thread. Engines
// share process-wide state: the module map below, each module's loaded
// dictionaries and models, and PyZy's single database and init/finalize. So
// every engine creation, call and destruction, on any thread, holds this one lock.
std::mutex& engine_mutex() {
    static std::mutex mutex;
    return mutex;
}
// An engine module's messages for the user come as keys; they go on as
// UiErrors, with their English for logs.
template <class Call> auto engine_call(Call&& call) {
    try {
        return call();
    } catch (const CjkMessage& message) {
        throw UiError(ui(message.what()));
    }
}
struct Module {
    std::string name;
    void* handle{};
    CreateCjkEngine create{};
    explicit Module(const std::string& module) : name(module) {
        const auto executable = std::filesystem::read_symlink("/proc/self/exe").parent_path();
        const auto filename = "framekeyboard-" + name + ".so";
        // Only load sibling modules, never working-directory or system-path
        // lookalikes. Development builds use engines/; packages use lib/.
        auto path = executable / "engines" / filename;
        if (!std::filesystem::exists(path)) {
            path = executable.parent_path() / "lib/framekeyboard" / filename;
        }
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            throw UiError(ui("ime.engine-unavailable", {{"engine", name}}));
        }
        create = reinterpret_cast<CreateCjkEngine>(dlsym(handle, "framekeyboard_create_cjk_v3"));
        if (!create) {
            dlclose(handle);
            throw UiError(ui("ime.engine-incompatible", {{"engine", name}}));
        }
    }
    ~Module() { dlclose(handle); }
};
// The modules that may compose a method, best first. framekeyboard-pinyin is
// PyZy's adapter; the override names one engine alone (CjkComposer).
std::vector<std::string> module_names(const std::string& method) {
    if (method == "korean-2set") {
        return {"hangul"};
    }
    const bool pinyin = method == "chinese-pinyin-simplified" || method == "chinese-pinyin-traditional";
    if (!pinyin && method != "chinese-zhuyin") {
        throw std::runtime_error("Unknown Chinese/Korean input method");
    }
    if (const char* engine = std::getenv("FRAMEKEYBOARD_CHINESE_ENGINE"); engine && *engine) {
        const std::string name = engine;
        return {name == "pyzy" ? "pinyin" : name};
    }
    return {pinyin ? "libime" : "mcbopomofo", "pinyin"};
}
std::shared_ptr<Module> load_module(const std::string& name) {
    // Applying a profile briefly keeps old and new contexts alive together.
    // Share their module, then unload after its last context is destroyed.
    static std::map<std::string, std::weak_ptr<Module>> modules;
    auto module = modules[name].lock();
    if (!module) {
        module = std::make_shared<Module>(name);
        modules[name] = module;
    }
    return module;
}
// The first module that loads; a newer engine's absence falls back to PyZy.
std::shared_ptr<Module> load(const std::string& method) {
    const auto names = module_names(method);
    for (std::size_t i = 0;; ++i) {
        try {
            return load_module(names[i]);
        } catch (const UiError&) {
            if (i + 1 == names.size()) {
                throw;
            }
        }
    }
}
} // namespace
struct CjkComposer::State {
    std::shared_ptr<Module> module;
    std::unique_ptr<CjkEngine> engine; // Reverse destruction keeps module code alive.
    std::string name = "table";
    // A table method's table comes loaded (TableEngine::load()).
    State(const std::string& method, const std::filesystem::path& learning_dir,
          std::shared_ptr<const TableEngine::Table> table) {
        if (table) {
            const auto input_table = table_for(method);
            engine = std::make_unique<TableEngine>(std::move(table), input_table->max_keys,
                                                   input_table->continues);
            return;
        }
        module = load(method);
        name = module->name;
        engine.reset(engine_call([&] { return module->create(method.c_str(), learning_dir.c_str()); }));
        if (!engine) {
            throw std::runtime_error("Could not create composition engine");
        }
    }
};
bool CjkComposer::supported(const std::string& method) {
    std::lock_guard lock(engine_mutex());
    if (const auto table = table_for(method)) {
        return TableEngine::available(table->path);
    }
    try {
        return static_cast<bool>(load(method));
    } catch (const std::exception&) {
        return false;
    }
}
CjkComposer::CjkComposer(const std::string& method, const std::filesystem::path& learning_dir)
    : state_([&] {
          // Reading a whole table takes a moment: it happens before the
          // engines' lock, so a preview loading one holds up no typing.
          std::shared_ptr<const TableEngine::Table> table;
          if (const auto input_table = table_for(method)) {
              table = TableEngine::load(input_table->path);
          }
          std::lock_guard lock(engine_mutex());
          return std::make_unique<State>(method, learning_dir, std::move(table));
      }()),
      phonetic_keys_(method == "chinese-zhuyin"), syllables_(method.starts_with("chinese-pinyin-")) {
}
CjkComposer::~CjkComposer() {
    std::lock_guard lock(engine_mutex());
    state_.reset();
}
void CjkComposer::type(char key) {
    std::lock_guard lock(engine_mutex());
    if (state_->engine) {
        engine_call([&] { state_->engine->type(key); });
    }
}
void CjkComposer::backspace() {
    std::lock_guard lock(engine_mutex());
    state_->engine->backspace();
}
void CjkComposer::cancel() {
    std::lock_guard lock(engine_mutex());
    state_->engine->cancel();
}
void CjkComposer::cycle(int direction) {
    std::lock_guard lock(engine_mutex());
    if (state_->engine) {
        state_->engine->cycle(direction);
    }
}
void CjkComposer::choose(int index) {
    std::lock_guard lock(engine_mutex());
    if (state_->engine) {
        state_->engine->choose(index);
    }
}
std::string CjkComposer::preedit() const {
    std::lock_guard lock(engine_mutex());
    return state_->engine->preedit();
}
std::string CjkComposer::text() const {
    std::lock_guard lock(engine_mutex());
    return state_->engine->text();
}
std::vector<std::string> CjkComposer::candidates() const {
    std::lock_guard lock(engine_mutex());
    return state_->engine ? state_->engine->candidates() : std::vector<std::string>{};
}
int CjkComposer::selected() const {
    std::lock_guard lock(engine_mutex());
    return state_->engine ? state_->engine->selected() : 0;
}
bool CjkComposer::empty() const {
    std::lock_guard lock(engine_mutex());
    return state_->engine->empty();
}
bool CjkComposer::has_reading() const {
    std::lock_guard lock(engine_mutex());
    return state_->engine->has_reading();
}
bool CjkComposer::chinese() const {
    std::lock_guard lock(engine_mutex());
    return state_->engine && state_->engine->chinese();
}
std::string CjkComposer::finished() const {
    std::lock_guard lock(engine_mutex());
    return state_->engine ? state_->engine->finished() : std::string{};
}
void CjkComposer::drop_finished() {
    std::lock_guard lock(engine_mutex());
    if (state_->engine) {
        state_->engine->drop_finished();
    }
}
void CjkComposer::cancel_open() {
    std::lock_guard lock(engine_mutex());
    state_->engine->cancel_open();
}
void CjkComposer::committed(bool learn) {
    std::lock_guard lock(engine_mutex());
    if (state_->engine) {
        state_->engine->committed(learn);
    }
}
bool CjkComposer::takes_space() const {
    std::lock_guard lock(engine_mutex());
    return state_->engine && state_->engine->takes_space();
}
void CjkComposer::as_typed() {
    std::lock_guard lock(engine_mutex());
    if (state_->engine) {
        state_->engine->as_typed();
    }
}
bool CjkComposer::has_cursor() const {
    std::lock_guard lock(engine_mutex());
    return state_->engine && state_->engine->has_cursor();
}
void CjkComposer::move(int syllables) {
    std::lock_guard lock(engine_mutex());
    if (state_->engine) {
        state_->engine->move(syllables);
    }
}
void CjkComposer::delete_forward() {
    std::lock_guard lock(engine_mutex());
    if (state_->engine) {
        state_->engine->delete_forward();
    }
}
bool CjkComposer::choosing() const {
    std::lock_guard lock(engine_mutex());
    return state_->engine && state_->engine->choosing();
}
void CjkComposer::set_choosing(bool choosing) {
    std::lock_guard lock(engine_mutex());
    if (state_->engine) {
        state_->engine->set_choosing(choosing);
    }
}
bool CjkComposer::insert_symbol(char symbol, const std::string& text) {
    std::lock_guard lock(engine_mutex());
    return state_->engine && engine_call([&] { return state_->engine->insert_symbol(symbol, text); });
}
std::optional<CjkComposer::Highlight> CjkComposer::highlight() const {
    std::lock_guard lock(engine_mutex());
    Highlight shown;
    if (state_->engine && state_->engine->highlight(shown.begin, shown.end, shown.caret)) {
        return shown;
    }
    return std::nullopt;
}
const std::string& CjkComposer::engine() const {
    return state_->name;
}
bool CjkComposer::phonetic_keys() const {
    return phonetic_keys_;
}
bool CjkComposer::syllables() const {
    return syllables_;
}
} // namespace framekeyboard
