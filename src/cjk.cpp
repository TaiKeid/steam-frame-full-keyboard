#include "framekeyboard/cjk.hpp"
#include "cjk_engine.hpp"
#include "framekeyboard/cheonjiin.hpp"

#include <dlfcn.h>
#include <filesystem>
#include <map>
#include <stdexcept>

namespace framekeyboard {
namespace {
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
            throw std::runtime_error(name + " engine unavailable. Choose another language.");
        }
        create = reinterpret_cast<CreateCjkEngine>(dlsym(handle, "framekeyboard_create_cjk_v1"));
        if (!create) {
            dlclose(handle);
            throw std::runtime_error(name + " engine module is incompatible. Reinstall this release.");
        }
    }
    ~Module() { dlclose(handle); }
};
std::shared_ptr<Module> load(const std::string& method) {
    const std::string name =
        method == "korean-2set"                                                           ? "hangul"
        : method == "chinese-pinyin-simplified" || method == "chinese-pinyin-traditional" ? "pinyin"
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
    // Set instead of module and engine for 천지인.
    std::unique_ptr<CheonjiinComposer> cheonjiin;
    explicit State(const std::string& method) {
        if (method == "korean-cheonjiin") {
            cheonjiin = std::make_unique<CheonjiinComposer>();
            return;
        }
        module = load(method);
        engine.reset(module->create(method.c_str()));
        if (!engine) {
            throw std::runtime_error("Could not create composition engine");
        }
    }
};
bool CjkComposer::supported(const std::string& method) {
    if (method == "korean-cheonjiin") {
        return true;
    }
    try {
        return static_cast<bool>(load(method));
    } catch (const std::exception&) {
        return false;
    }
}
CjkComposer::CjkComposer(const std::string& method) : state_(std::make_unique<State>(method)) {
}
CjkComposer::~CjkComposer() = default;
void CjkComposer::type(char key) {
    if (state_->engine) {
        state_->engine->type(key);
    }
}
void CjkComposer::backspace() {
    state_->cheonjiin ? state_->cheonjiin->backspace() : state_->engine->backspace();
}
void CjkComposer::cancel() {
    state_->cheonjiin ? state_->cheonjiin->cancel() : state_->engine->cancel();
}
void CjkComposer::cycle(int direction) {
    if (state_->engine) {
        state_->engine->cycle(direction);
    }
}
void CjkComposer::choose(int index) {
    if (state_->engine) {
        state_->engine->choose(index);
    }
}
std::string CjkComposer::preedit() const {
    return state_->cheonjiin ? state_->cheonjiin->text() : state_->engine->preedit();
}
std::string CjkComposer::text() const {
    return state_->cheonjiin ? state_->cheonjiin->text() : state_->engine->text();
}
std::vector<std::string> CjkComposer::candidates() const {
    return state_->engine ? state_->engine->candidates() : std::vector<std::string>{};
}
int CjkComposer::selected() const {
    return state_->engine ? state_->engine->selected() : 0;
}
bool CjkComposer::empty() const {
    return state_->cheonjiin ? state_->cheonjiin->empty() : state_->engine->empty();
}
bool CjkComposer::has_reading() const {
    return state_->cheonjiin ? !state_->cheonjiin->empty() : state_->engine->has_reading();
}
bool CjkComposer::chinese() const {
    return state_->engine && state_->engine->chinese();
}
bool CjkComposer::flick_input() const {
    return static_cast<bool>(state_->cheonjiin);
}
void CjkComposer::flick(const std::string& text, bool replace) {
    if (!state_->cheonjiin || text.empty()) {
        return; // Empty: a direction this key does not use.
    }
    replace ? state_->cheonjiin->replace(text) : state_->cheonjiin->add(text);
}
std::string CjkComposer::last_flick() const {
    return state_->cheonjiin ? state_->cheonjiin->last() : std::string{};
}
} // namespace framekeyboard
