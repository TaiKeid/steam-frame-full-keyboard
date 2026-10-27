#pragma once
#include "clipboard.hpp"
#include "desktop_focus.hpp"
#include "input.hpp"
#include "text_input.hpp"
#include <functional>
#include <libei.h>

namespace framekeyboard {
// Infer only the documented gamescope-N-ei naming convention. Unknown names
// require an explicit text socket; an empty result disables text delivery.
fs::path gamescope_text_socket(const fs::path& input_socket, const fs::path& explicit_text = {});

// Connect directly to the running Gamescope compositor. This avoids the
// SteamVR startup-time discovery limitation of a newly created uinput device.
class EiSink : public KeySink {
  public:
    explicit EiSink(const fs::path& socket, const fs::path& text_socket = {},
                    GamescopeText::Clock clock = {});
    ~EiSink() override;
    void send(int code, int value) override;
    bool pump() override;
    bool take_input_reset() override;
    bool can_resume() const override { return true; }
    bool text_available() override;
    bool commit_text(const std::string& text) override;
    // Local text always replaces in one ordered input-method request. While that
    // connection is briefly down, replacing fails instead of falling back to
    // separate libei Backspace keys, which would delete the word and type nothing.
    bool replaces_text() override { return true; }
    bool replace_text(std::size_t delete_before, const std::string& text) override;
    bool text_pending() const override {
        return (text_ && text_->pending()) || (entry_ && !entry_->cancelled);
    }
    std::size_t delivered_characters() const override;
    bool output_blocked() const override { return (text_ && text_->busy()) || entering(); }
    void cancel_text() override;
    int shortcut_code(xkb_keysym_t symbol, int fallback) override;
    std::optional<bool> num_lock_state() override { return num_lock_; }
    // Measurement only: forces every multi-character commit to go whole (false)
    // or per character (true) instead of the on_keymap() choice.
    void force_per_character(std::optional<bool> value) { per_character_ = value; }
    // Kept across text reconnections.
    void set_character_gap(double seconds) override;
    // Tests replace Gamescope's focus with their own answer.
    void set_desktop_focus(std::function<bool()> focused) { desktop_focused_ = std::move(focused); }
    void set_desktop_paste(Clipboard* clipboard) override { desktop_paste_ = clipboard; }
    // How long a paste waits for the application to take the clipboard's text.
    static constexpr double paste_wait =
        2; // The pause between the key events of Unicode entry. IBus reorders keys
    // that arrive closer together (measured on the Frame: none at 0 ms, exact at 5 ms).
    static constexpr double entry_gap = .005;
    EiSink(const EiSink&) = delete;
    EiSink& operator=(const EiSink&) = delete;

  private:
    // Text typed into a nested desktop as IBus Unicode entry: Ctrl+Shift+U,
    // the code point in hex, Space, as plain key events that the desktop's
    // own keymap reads correctly. Gamescope's temporary keymap cannot reach it.
    // With a paste clipboard, the keys are Ctrl+V instead, sent once the
    // clipboard holds the text, and the entry ends when an application takes it.
    struct Entry {
        std::string text;
        std::size_t deletions{};
        std::vector<std::pair<int, int>> keys;
        std::vector<std::size_t> character_ends; // key index after each character
        std::size_t next{};
        double next_at{};
        bool failed{}, cancelled{};
        std::optional<std::uint64_t> write; // the clipboard write a paste waits for
        std::size_t paste_at{};             // key index of Ctrl+V's first event
        std::uint64_t served{};
        double deadline{};
        bool taken{};
    };
    bool desktop_target();
    bool needs_entry(const std::string& text);
    bool enter(const std::string& text, std::size_t deletions);
    void advance_entry();
    bool entering() const {
        return entry_ && !entry_->failed &&
               (entry_->next < entry_->keys.size() || (entry_->write && !entry_->taken));
    }
    void read_keymap();
    bool on_keymap(const std::string& text) const;
    void service_text();
    bool connect();
    void connection_lost();
    std::unique_ptr<xkb_context, decltype(&xkb_context_unref)> xkb_{nullptr, xkb_context_unref};
    std::unique_ptr<xkb_keymap, decltype(&xkb_keymap_unref)> keymap_{nullptr, xkb_keymap_unref};
    xkb_layout_index_t group_{};
    std::optional<bool> per_character_;
    double character_gap_{GamescopeText::default_character_gap};
    std::optional<bool> num_lock_;
    fs::path socket_, text_socket_;
    std::unique_ptr<GamescopeText> text_;
    std::optional<Entry> entry_;
    std::size_t entry_delivered_{};
    bool last_entry_{}, desktop_now_{};
    std::unique_ptr<DesktopFocus> desktop_;
    std::function<bool()> desktop_focused_;
    Clipboard* desktop_paste_{};
    GamescopeText::Clock clock_;
    double text_retry_at_{}, text_retry_delay_{1};
    double retry_at_{}, retry_delay_{1}, connect_deadline_{};
    ei* context_{};
    ei_device* keyboard_{};
    bool resumed_{}, disconnected_{}, input_reset_{};
    std::uint32_t sequence_{};
    std::set<int> held_;
};
} // namespace framekeyboard
