#pragma once
#include "input.hpp"
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
    EiSink(const EiSink&) = delete;
    EiSink& operator=(const EiSink&) = delete;

  private:
    ei* context_{};
    ei_device* keyboard_{};
    bool resumed_{}, disconnected_{};
    std::uint32_t sequence_{};
    std::set<int> held_;
};
} // namespace framekeyboard
