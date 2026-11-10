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
// dictionaries and models. So
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
    PrepareCjkEngine prepare{};
    ReleasePreparedCjkEngine release{};
    SaveCjkLearning save{};
    ClearCjkLearning clear{};
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
        prepare = reinterpret_cast<PrepareCjkEngine>(dlsym(handle, "framekeyboard_prepare_cjk_v1"));
        release = reinterpret_cast<ReleasePreparedCjkEngine>(
            dlsym(handle, "framekeyboard_release_prepared_cjk_v1"));
        if (!release) {
            prepare = nullptr;
        }
        save = reinterpret_cast<SaveCjkLearning>(dlsym(handle, "framekeyboard_save_cjk_v1"));
        clear = reinterpret_cast<ClearCjkLearning>(dlsym(handle, "framekeyboard_clear_cjk_v1"));
    }
    ~Module() { dlclose(handle); }
};
// Each phonetic method has one retained engine; no legacy fallback is loaded.
std::string module_name(const std::string& method) {
    if (method == "korean-2set") {
        return "hangul";
    }
    const bool pinyin = method == "chinese-pinyin-simplified" || method == "chinese-pinyin-traditional";
    if (!pinyin && method != "chinese-zhuyin") {
        throw std::runtime_error("Unknown Chinese/Korean input method");
    }
    if (const char* engine = std::getenv("FRAMEKEYBOARD_CHINESE_ENGINE"); engine && *engine) {
        const std::string name = engine;
        if (name != "libime" && name != "mcbopomofo") {
            throw UiError(ui("ime.engine-unavailable", {{"engine", name}}));
        }
        return name;
    }
    return pinyin ? "libime" : "mcbopomofo";
}
// Applying a profile briefly keeps old and new contexts alive together.
// Share their module, then unload after its last context is destroyed.
std::map<std::string, std::weak_ptr<Module>>& modules() {
    static std::map<std::string, std::weak_ptr<Module>> loaded;
    return loaded;
}
std::shared_ptr<Module> load_module(const std::string& name) {
    auto& slot = modules()[name];
    auto module = slot.lock();
    if (!module) {
        module = std::make_shared<Module>(name);
        slot = module;
    }
    return module;
}
std::shared_ptr<Module> load(const std::string& method) {
    return load_module(module_name(method));
}
// A module's data for the method, loaded before the engines' lock so a
// preview's engine holds up no typing (R140); create() then finds it loaded.
// The handle goes after the lock is released.
std::shared_ptr<void> prepare_engine(const std::string& method,
                                     const std::filesystem::path& learning_dir) {
    std::shared_ptr<Module> module;
    {
        std::lock_guard lock(engine_mutex());
        try {
            module = load(method);
        } catch (const std::exception&) {
            return {}; // Creating the engine reports it.
        }
    }
    void* prepared = module->prepare ? module->prepare(method.c_str(), learning_dir.c_str()) : nullptr;
    if (!prepared) {
        return {};
    }
    return {prepared, [module](void* handle) { module->release(handle); }};
}
} // namespace
struct CjkComposer::State {
    TableEngine* table_engine{}; // Built-in table only; no module ABI change.
    std::shared_ptr<Module> module;
    std::unique_ptr<CjkEngine> engine; // Reverse destruction keeps module code alive.
    std::string name = "table";
    // Zhuyin's sentence experiment: libime proposing sentences (propose()),
    // and the runs it last converted, which a key that finishes no syllable
    // leaves as they were.
    std::unique_ptr<State> sentences;
    std::vector<std::string> proposed_for;
    // An engine of a named module, for a method only it has.
    State(const std::string& module_name, const std::string& method) {
        module = load_module(module_name);
        name = module->name;
        engine.reset(engine_call([&] { return module->create(method.c_str(), ""); }));
        if (!engine) {
            throw std::runtime_error("Could not create composition engine");
        }
    }
    // A table method's table comes loaded (TableEngine::load()).
    State(const std::string& method, const std::filesystem::path& learning_dir,
          std::shared_ptr<const TableEngine::Table> table) {
        if (table) {
            const auto input_table = table_for(method);
            engine = std::make_unique<TableEngine>(std::move(table), input_table->max_keys,
                                                   input_table->continues);
            table_engine = static_cast<TableEngine*>(engine.get());
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
void CjkComposer::save_learning() {
    std::lock_guard lock(engine_mutex());
    for (const auto& [name, weak] : modules()) {
        if (const auto module = weak.lock(); module && module->save) {
            module->save();
        }
    }
}
void CjkComposer::clear_learning(const std::filesystem::path& learning_dir) {
    std::lock_guard lock(engine_mutex());
    for (const auto& [name, weak] : modules()) {
        if (const auto module = weak.lock(); module && module->clear) {
            module->clear(learning_dir.c_str());
        }
    }
}
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
          // Reading a whole table or dictionary takes a moment: it happens
          // before the engines' lock, so a preview loading one holds up no
          // typing. Declared before the lock, prepared data outlives it.
          std::shared_ptr<const TableEngine::Table> table;
          std::shared_ptr<void> prepared;
          if (const auto input_table = table_for(method)) {
              table = TableEngine::load(input_table->path);
          } else {
              prepared = prepare_engine(method, learning_dir);
          }
          std::lock_guard lock(engine_mutex());
          return std::make_unique<State>(method, learning_dir, std::move(table));
      }()),
      phonetic_keys_(method == "chinese-zhuyin"), syllables_(method.starts_with("chinese-pinyin-")) {
    // Being compared: Zhuyin through libime's sentences, the tones choosing.
    const char* sentences = std::getenv("FRAMEKEYBOARD_ZHUYIN_SENTENCES");
    if (method == "chinese-zhuyin" && state_->name == "mcbopomofo" && sentences &&
        std::string(sentences) == "1") {
        std::lock_guard lock(engine_mutex());
        try {
            state_->sentences =
                std::make_unique<State>("libime", "chinese-pinyin-traditional-sentences");
        } catch (const std::exception&) {
            // Without libime, McBopomofo converts alone.
        }
    }
}
// libime's Traditional sentences for each run of the reading, for Zhuyin to
// keep the best that fits its tones (CjkEngine::prefer()). Under the lock.
void CjkComposer::propose() {
    if (!state_->sentences) {
        return;
    }
    auto runs = state_->engine->pinyin_runs();
    if (runs == state_->proposed_for) {
        return;
    }
    std::vector<std::vector<std::string>> sentences;
    for (const auto& run : runs) {
        state_->sentences->engine->type_reading(run);
        sentences.push_back(state_->sentences->engine->candidates());
    }
    state_->engine->prefer(sentences);
    state_->proposed_for = std::move(runs);
}
CjkComposer::~CjkComposer() {
    std::lock_guard lock(engine_mutex());
    state_.reset();
}
void CjkComposer::type(char key) {
    std::lock_guard lock(engine_mutex());
    if (state_->engine) {
        engine_call([&] { state_->engine->type(key); });
        propose();
    }
}
void CjkComposer::backspace() {
    std::lock_guard lock(engine_mutex());
    state_->engine->backspace();
    propose();
}
void CjkComposer::cancel() {
    std::lock_guard lock(engine_mutex());
    state_->engine->cancel();
    state_->proposed_for.clear(); // The engine dropped what it was given.
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
    propose();
}
void CjkComposer::committed(bool learn) {
    std::lock_guard lock(engine_mutex());
    if (state_->engine) {
        state_->engine->committed(learn);
    }
}
bool CjkComposer::table_context_enabled() const {
    std::lock_guard lock(engine_mutex());
    return state_->table_engine && state_->table_engine->context_enabled();
}
void CjkComposer::set_table_context(const std::string& previous) {
    std::lock_guard lock(engine_mutex());
    if (state_->table_engine) {
        state_->table_engine->set_context(previous);
    }
}
void CjkComposer::clear_table_context() {
    std::lock_guard lock(engine_mutex());
    if (state_->table_engine) {
        state_->table_engine->clear_context();
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
        propose();
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
    const bool inserted =
        state_->engine && engine_call([&] { return state_->engine->insert_symbol(symbol, text); });
    if (inserted) {
        propose();
    }
    return inserted;
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
