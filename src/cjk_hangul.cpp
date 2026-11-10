// Two-set Hangul composition only. The app owns delivery and acknowledges each
// finished syllable before dropping it; this adapter never sends input itself.
#include "cjk_engine.hpp"
#include <glib.h>
#include <hangul.h>
#include <memory>

namespace framekeyboard {
namespace {
std::string utf8(const ucschar* text) {
    auto* converted = g_ucs4_to_utf8(text, -1, nullptr, nullptr, nullptr);
    std::string result = converted ? converted : "";
    g_free(converted);
    return result;
}
class HangulEngine final : public CjkEngine {
  public:
    explicit HangulEngine(const std::string& method) {
        if (method != "korean-2set") {
            throw CjkMessage("ime.engine-missing");
        }
        hangul_.reset(hangul_ic_new("2"));
        if (!hangul_) {
            throw CjkMessage("ime.hangul-unavailable");
        }
    }
    void type(char key) override {
        // Finished text builds up only while the app cannot deliver it.
        if (static_cast<std::size_t>(g_utf8_strlen(finished_.c_str(), -1)) >= 30) {
            throw CjkMessage("ime.commit-first");
        }
        // A jamo starting the next syllable finishes the open one, as on a PC.
        hangul_ic_process(hangul_.get(), key);
        finished_ += utf8(hangul_ic_get_commit_string(hangul_.get()));
    }
    void backspace() override {
        // Edit the open syllable first, then text not yet sent to the target.
        if (!hangul_ic_backspace(hangul_.get()) && !finished_.empty()) {
            finished_.erase(static_cast<std::size_t>(
                g_utf8_find_prev_char(finished_.c_str(), finished_.c_str() + finished_.size()) -
                finished_.c_str()));
        }
    }
    void cancel() override {
        finished_.clear();
        hangul_ic_reset(hangul_.get());
    }
    void cancel_open() override { hangul_ic_reset(hangul_.get()); }
    void cycle(int) override {}
    void choose(int) override {}
    std::string preedit() const override {
        return finished_ + utf8(hangul_ic_get_preedit_string(hangul_.get()));
    }
    std::string text() const override { return preedit(); }
    std::vector<std::string> candidates() const override { return {}; }
    int selected() const override { return 0; }
    bool empty() const override { return hangul_ic_is_empty(hangul_.get()) && finished_.empty(); }
    bool has_reading() const override { return !empty(); }
    bool chinese() const override { return false; }
    std::string finished() const override { return finished_; }
    void drop_finished() override { finished_.clear(); }

  private:
    std::unique_ptr<HangulInputContext, decltype(&hangul_ic_delete)> hangul_{nullptr, hangul_ic_delete};
    std::string finished_;
};
} // namespace
} // namespace framekeyboard

// The module and host are built together. Hangul keeps no learned history.
extern "C" __attribute__((visibility("default"))) framekeyboard::CjkEngine*
framekeyboard_create_cjk_v3(const char* method, const char*) {
    return new framekeyboard::HangulEngine(method);
}
