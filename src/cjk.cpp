#include "framekeyboard/cjk.hpp"
#include "cjk_engine.hpp"
#include "framekeyboard/localization.hpp"
#include "table_engine.hpp"

#include <dlfcn.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>

namespace framekeyboard {
namespace {
// A shell preview builds its own App, and engine, on a worker thread. Engines
// share process-wide state: the module map below, the cached Pinyin library and
// PyZy's single database and init/finalize. So every engine creation, call and
// destruction, on any thread, holds this one lock.
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
    void* handle{};
    CreateCjkEngine create{};
    explicit Module(const std::string& name) {
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
        create = reinterpret_cast<CreateCjkEngine>(dlsym(handle, "framekeyboard_create_cjk_v2"));
        if (!create) {
            dlclose(handle);
            throw UiError(ui("ime.engine-incompatible", {{"engine", name}}));
        }
    }
    ~Module() { dlclose(handle); }
};
std::shared_ptr<Module> load(const std::string& method) {
    const std::string name = method == "korean-2set" ? "hangul"
                             : method == "chinese-pinyin-simplified" ||
                                     method == "chinese-pinyin-traditional" || method == "chinese-zhuyin"
                                 ? "pinyin"
                                 : "";
    if (name.empty()) {
        throw std::runtime_error("Unknown Chinese/Korean input method");
    }
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
} // namespace
struct CjkComposer::State {
    std::shared_ptr<Module> module;
    std::unique_ptr<CjkEngine> engine; // Reverse destruction keeps module code alive.
    explicit State(const std::string& method) {
        if (const auto table = table_for(method)) {
            engine = std::make_unique<TableEngine>(table->path, table->max_keys);
            return;
        }
        module = load(method);
        engine.reset(engine_call([&] { return module->create(method.c_str()); }));
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
CjkComposer::CjkComposer(const std::string& method)
    : state_([&] {
          std::lock_guard lock(engine_mutex());
          return std::make_unique<State>(method);
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
bool CjkComposer::phonetic_keys() const {
    return phonetic_keys_;
}
bool CjkComposer::syllables() const {
    return syllables_;
}
} // namespace framekeyboard
