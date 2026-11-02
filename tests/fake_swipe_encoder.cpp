// Private ownership fixture. This module contains no model or inference runtime.
#include "framekeyboard/swipe.hpp"

extern "C" void framekeyboard_test_swipe_arm_allocation_failure();
extern "C" void framekeyboard_test_swipe_encoder_destroyed();
extern "C" void framekeyboard_test_swipe_module_unloaded();

namespace {
struct Encoder final : framekeyboard::SwipeEncoder {
    ~Encoder() override { framekeyboard_test_swipe_encoder_destroyed(); }
    std::vector<float> encode(const float*, const float*, const unsigned char*, int&) override {
        return {};
    }
};
} // namespace

extern "C" int framekeyboard_test_swipe_fixture_v1() {
    return 1;
}
extern "C" framekeyboard::SwipeEncoder* framekeyboard_create_swipe_encoder_v1(const char*) {
    auto* encoder = new Encoder;
    // The next allocation is the production adapter wrapper, after the
    // returned encoder exists but before the wrapper can own it.
    framekeyboard_test_swipe_arm_allocation_failure();
    return encoder;
}
__attribute__((destructor)) static void unloaded() {
    framekeyboard_test_swipe_module_unloaded();
}
