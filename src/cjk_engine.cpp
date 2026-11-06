#include "cjk_engine.hpp"
#include <memory>

#include <algorithm>
#include <glib.h>
#include <map>
#include <stdexcept>
#ifdef FRAMEKEYBOARD_HAVE_PYZY
#include "ram_directory.hpp"
#include <PyZy/InputContext.h>
#include <PyZy/Variant.h>
#endif
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
#include <hangul.h>
#endif

namespace framekeyboard {
class Engine final : public CjkEngine {
  public:
    explicit Engine(const std::string& method);
    ~Engine() override;
    void type(char key) override;
    void backspace() override;
    void cancel() override;
    void cycle(int direction) override;
    void choose(int index) override;
    std::string preedit() const override;
    std::string text() const override;
    std::vector<std::string> candidates() const override;
    int selected() const override;
    bool empty() const override;
    bool has_reading() const override;
    bool chinese() const override;
    std::string finished() const override;
    void drop_finished() override;
    static bool supported(const std::string& method);

  private:
    struct State;
    std::unique_ptr<State> state_;
};

namespace {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
// The standard Zhuyin keyboard, which is PyZy's default: what each key means.
const std::map<char, std::string> zhuyin_symbols{
    {'1', "ㄅ"}, {'q', "ㄆ"}, {'a', "ㄇ"}, {'z', "ㄈ"}, {'2', "ㄉ"}, {'w', "ㄊ"}, {'s', "ㄋ"},
    {'x', "ㄌ"}, {'e', "ㄍ"}, {'d', "ㄎ"}, {'c', "ㄏ"}, {'r', "ㄐ"}, {'f', "ㄑ"}, {'v', "ㄒ"},
    {'5', "ㄓ"}, {'t', "ㄔ"}, {'g', "ㄕ"}, {'b', "ㄖ"}, {'y', "ㄗ"}, {'h', "ㄘ"}, {'n', "ㄙ"},
    {'u', "ㄧ"}, {'j', "ㄨ"}, {'m', "ㄩ"}, {'8', "ㄚ"}, {'i', "ㄛ"}, {'k', "ㄜ"}, {',', "ㄝ"},
    {'9', "ㄞ"}, {'o', "ㄟ"}, {'l', "ㄠ"}, {'.', "ㄡ"}, {'0', "ㄢ"}, {'p', "ㄣ"}, {';', "ㄤ"},
    {'/', "ㄥ"}, {'-', "ㄦ"}, {'6', "ˊ"},  {'3', "ˇ"},  {'4', "ˋ"},  {'7', "˙"}};
std::string zhuyin_text(const std::string& keys) {
    std::string result;
    for (const char key : keys) {
        const auto symbol = zhuyin_symbols.find(key);
        result += symbol == zhuyin_symbols.end() ? std::string(1, key) : symbol->second;
    }
    return result;
}
struct PinyinObserver : PyZy::InputContext::Observer {
    void commitText(PyZy::InputContext*, const std::string&) override {}
    void inputTextChanged(PyZy::InputContext*) override {}
    void cursorChanged(PyZy::InputContext*) override {}
    void preeditTextChanged(PyZy::InputContext*) override {}
    void auxiliaryTextChanged(PyZy::InputContext*) override {}
    void candidatesChanged(PyZy::InputContext*) override {}
};
struct PinyinLibrary {
    PinyinLibrary() {
        // PyZy leaks a failed SQLite handle before its memory fallback. Give
        // initialization a valid private RAM directory instead. It clones the
        // startup schema into memory and closes the file before returning.
        RamDirectory startup;
        try {
            PyZy::InputContext::init(startup.path().string(), "/dev/null");
            // Remove startup metadata before exposing any typing context. We
            // never call native commit/selectCandidate, which invoke learning.
            startup.remove();
        } catch (...) {
            PyZy::InputContext::finalize();
            throw; // The scratch guard cleans up after native handles close.
        }
    }
    ~PinyinLibrary() { PyZy::InputContext::finalize(); }
};
std::shared_ptr<PinyinLibrary> pinyin_library() {
    static std::weak_ptr<PinyinLibrary> cached;
    auto library = cached.lock();
    if (!library) {
        library = std::make_shared<PinyinLibrary>();
        cached = library;
    }
    return library;
}
#endif
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
std::string utf8(const ucschar* text) {
    auto* converted = g_ucs4_to_utf8(text, -1, nullptr, nullptr, nullptr);
    std::string result = converted ? converted : "";
    g_free(converted);
    return result;
}
#endif
} // namespace
struct Engine::State {
    bool pinyin{}, zhuyin{};
    // Pinyin and Zhuyin keep the whole reading's keys; Hangul keeps only its
    // open syllable, in the context, and what it finished before the app sent it.
    std::string keys, prefix, finished;
    // Undo a selected Pinyin segment without guessing syllable boundaries.
    std::vector<std::pair<std::string, std::string>> history;
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    // Declaration order ensures all contexts die before the shared library state.
    std::shared_ptr<PinyinLibrary> library;
    PinyinObserver observer;
    std::unique_ptr<PyZy::InputContext> context;
    void rebuild_pinyin() {
        context->reset();
        for (char ch : keys) {
            context->insert(ch);
        }
    }
    // Zhuyin's restText stays empty after a partial choice. Each character is
    // one syllable, so the keys chosen are the longest start of the reading
    // that converts to that many characters; a key more begins the next one.
    std::string zhuyin_rest(const std::string& chosen) {
        const auto wanted = g_utf8_strlen(chosen.c_str(), -1);
        std::size_t used = keys.size();
        context->reset();
        for (std::size_t count = 1; count <= keys.size(); ++count) {
            context->insert(keys[count - 1]);
            PyZy::Candidate candidate;
            if (context->getCandidate(0, candidate) &&
                g_utf8_strlen(candidate.text.c_str(), -1) == wanted) {
                used = count;
            }
        }
        return keys.substr(used);
    }
#endif
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
    std::unique_ptr<HangulInputContext, decltype(&hangul_ic_delete)> hangul{nullptr, hangul_ic_delete};
    std::string open_syllable() const { return utf8(hangul_ic_get_preedit_string(hangul.get())); }
#endif
};
bool Engine::supported(const std::string& method) {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    if (method == "chinese-pinyin-simplified" || method == "chinese-pinyin-traditional" ||
        method == "chinese-zhuyin") {
        return true;
    }
#endif
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
    if (method == "korean-2set") {
        return true;
    }
#endif
    (void)method;
    return false;
}
Engine::Engine(const std::string& method) : state_(std::make_unique<State>()) {
    if (!supported(method)) {
        throw std::runtime_error("This build lacks the selected Chinese/Korean input engine");
    }
    auto& s = *state_;
    s.pinyin = method.starts_with("chinese-");
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    if (s.pinyin) {
        s.library = pinyin_library();
        s.zhuyin = method == "chinese-zhuyin";
        s.context.reset(PyZy::InputContext::create(
            s.zhuyin ? PyZy::InputContext::BOPOMOFO : PyZy::InputContext::FULL_PINYIN, &s.observer));
        if (!s.context) {
            throw std::runtime_error("Pinyin engine unavailable");
        }
        s.context->setProperty(PyZy::InputContext::PROPERTY_SPECIAL_PHRASE,
                               PyZy::Variant::fromBool(false));
        s.context->setProperty(PyZy::InputContext::PROPERTY_MODE_SIMP,
                               PyZy::Variant::fromBool(method == "chinese-pinyin-simplified"));
        // Check the installed dictionary before accepting a profile. No output,
        // history learning, or app focus is involved in this capability check.
        // ni in Pinyin, and in Zhuyin the keys for ㄋㄧ.
        s.context->insert(s.zhuyin ? 's' : 'n');
        s.context->insert(s.zhuyin ? 'u' : 'i');
        PyZy::Candidate candidate;
        const bool ready = s.context->getCandidate(0, candidate);
        s.context->reset();
        if (!ready) {
            throw std::runtime_error("Pinyin dictionary unavailable");
        }
    }
#endif
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
    if (!s.pinyin) {
        s.hangul.reset(hangul_ic_new("2"));
        if (!s.hangul) {
            throw std::runtime_error("Korean two-set engine unavailable");
        }
    }
#endif
}
Engine::~Engine() = default;
bool Engine::chinese() const {
    return state_->pinyin;
}
bool Engine::has_reading() const {
    return state_->pinyin ? !state_->keys.empty() : !empty();
}
bool Engine::empty() const {
    const auto& s = *state_;
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
    if (s.hangul && !hangul_ic_is_empty(s.hangul.get())) {
        return false;
    }
#endif
    return s.keys.empty() && s.prefix.empty() && s.finished.empty();
}
void Engine::type(char key) {
    auto& s = *state_;
    // Hangul's finished text only builds up while the app can't send it.
    if (s.keys.size() + static_cast<std::size_t>(g_utf8_strlen(s.prefix.c_str(), -1)) +
            static_cast<std::size_t>(g_utf8_strlen(s.finished.c_str(), -1)) >=
        30) {
        throw std::runtime_error("Commit this composition before typing more");
    }
    if (s.pinyin) {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
        key = g_ascii_tolower(key);
        if (s.context->insert(key)) {
            s.keys = s.context->inputText();
        }
#endif
    } else {
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
        // A jamo that starts the next syllable finishes the open one, as on
        // a PC: 간 + ㅏ finishes 가 and leaves 나 open.
        hangul_ic_process(s.hangul.get(), key);
        s.finished += utf8(hangul_ic_get_commit_string(s.hangul.get()));
#endif
    }
}
void Engine::backspace() {
    auto& s = *state_;
    if (s.pinyin) {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
        if (!s.keys.empty()) {
            s.context->removeCharBefore();
            s.keys = s.context->inputText();
        } else if (!s.history.empty()) {
            auto previous = std::move(s.history.back());
            s.history.pop_back();
            s.prefix = std::move(previous.first);
            s.keys = std::move(previous.second);
            s.rebuild_pinyin();
        }
#endif
    } else {
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
        // A jamo of the open syllable (값 → 갑), else a character the app
        // hasn't sent yet. Sent syllables are the target's to delete.
        if (!hangul_ic_backspace(s.hangul.get()) && !s.finished.empty()) {
            s.finished.erase(static_cast<std::size_t>(
                g_utf8_find_prev_char(s.finished.c_str(), s.finished.c_str() + s.finished.size()) -
                s.finished.c_str()));
        }
#endif
    }
}
void Engine::cancel() {
    auto& s = *state_;
    s.keys.clear();
    s.prefix.clear();
    s.history.clear();
    s.finished.clear();
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    if (s.context) {
        s.context->reset();
    }
#endif
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
    if (s.hangul) {
        hangul_ic_reset(s.hangul.get());
    }
#endif
}
std::vector<std::string> Engine::candidates() const {
    std::vector<std::string> result;
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    if (state_->context) {
        PyZy::Candidate candidate;
        // Bound candidate work and menu size for controller interaction.
        for (std::size_t i = 0; i < 50 && state_->context->getCandidate(i, candidate); ++i) {
            result.push_back(candidate.text);
        }
    }
#endif
    return result;
}
int Engine::selected() const {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    if (state_->context) {
        return static_cast<int>(state_->context->focusedCandidate());
    }
#endif
    return 0;
}
void Engine::cycle(int direction) {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    if (state_->context) {
        const int count = static_cast<int>(candidates().size());
        if (count) {
            state_->context->focusCandidate(
                static_cast<std::size_t>((selected() + direction + count) % count));
        }
    }
#else
    (void)direction;
#endif
}
void Engine::choose(int index) {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    auto& s = *state_;
    if (!s.context || index < 0 || index >= static_cast<int>(candidates().size()) ||
        !s.context->focusCandidate(static_cast<std::size_t>(index))) {
        return;
    }
    // Read the chosen segment and remaining phonetic input instead of invoking
    // PyZy's select/commit methods, which would learn the user's text.
    const auto chosen = s.context->conversionText();
    const auto rest = s.zhuyin ? s.zhuyin_rest(chosen) : s.context->restText();
    if (chosen.empty()) {
        return;
    }
    s.history.emplace_back(s.prefix, s.keys);
    s.prefix += chosen;
    s.keys = rest;
    s.rebuild_pinyin();
#else
    (void)index;
#endif
}
std::string Engine::preedit() const {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    if (state_->zhuyin) {
        return state_->prefix + zhuyin_text(state_->keys);
    }
#endif
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
    if (!state_->pinyin) {
        return state_->finished + state_->open_syllable();
    }
#endif
    return state_->prefix + state_->keys;
}
std::string Engine::text() const {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    if (state_->context) {
        return state_->prefix + state_->context->conversionText() + state_->context->restText();
    }
#endif
    return preedit();
}
std::string Engine::finished() const {
    return state_->finished;
}
void Engine::drop_finished() {
    state_->finished.clear();
}
} // namespace framekeyboard

// This factory is the only exported entry point. The module and host ship in
// the same release and share this internal C++ interface, not PyZy's ABI.
extern "C" __attribute__((visibility("default"))) framekeyboard::CjkEngine*
framekeyboard_create_cjk_v2(const char* method) {
    return new framekeyboard::Engine(method);
}
