#pragma once
#include "input.hpp"
#include "text_input.hpp"
#include <libei.h>

namespace framekeyboard {
// Infer only the documented gamescope-N-ei naming convention. Unknown names
// require an explicit text socket; an empty result disables text delivery.
fs::path gamescope_text_socket(const fs::path& input_socket, const fs::path& explicit_text = {});

// Connect directly to the running Gamescope compositor. This avoids the
// SteamVR startup-time discovery limitation of a newly created uinput device.
class EiSink : public KeySink {
  public:
    explicit EiSink(const fs::path& socket, const fs::path& text_socket = {});
    ~EiSink() override;
    void send(int code, int value) override;
    bool pump() override;
    bool take_input_reset() override;
    bool can_resume() const override { return !disconnected_; }
    bool text_available() override;
    bool commit_text(const std::string& text) override;
    EiSink(const EiSink&) = delete;
    EiSink& operator=(const EiSink&) = delete;

  private:
    fs::path text_socket_;
    std::unique_ptr<GamescopeText> text_;
    bool text_attempted_{};
    ei* context_{};
    ei_device* keyboard_{};
    bool resumed_{}, disconnected_{}, input_reset_{};
    std::uint32_t sequence_{};
    std::set<int> held_;
};
} // namespace framekeyboard
