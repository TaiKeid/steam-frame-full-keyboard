#pragma once
#include "input.hpp"
#include "text_input.hpp"
#include <libei.h>

namespace framekeyboard {
// Connect directly to the running Gamescope compositor. This avoids the
// SteamVR startup-time discovery limitation of a newly created uinput device.
class EiSink : public KeySink {
  public:
    explicit EiSink(const fs::path& socket);
    ~EiSink() override;
    void send(int code, int value) override;
    bool pump() override;
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
    bool resumed_{}, disconnected_{};
    std::uint32_t sequence_{};
    std::set<int> held_;
};
} // namespace framekeyboard
