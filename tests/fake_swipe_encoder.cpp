// Private ownership fixture. This module contains no model or inference runtime.
#include "framekeyboard/swipe.hpp"

#include <cstring>

extern "C" void framekeyboard_test_swipe_arm_allocation_failure();
extern "C" void framekeyboard_test_swipe_encoder_destroyed();
extern "C" void framekeyboard_test_swipe_module_unloaded();
extern "C" void framekeyboard_test_swipe_module_initialized();

namespace {
unsigned creation_calls{};
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
extern "C" unsigned framekeyboard_test_swipe_creation_calls() {
    return creation_calls;
}
extern "C" framekeyboard::SwipeEncoder* framekeyboard_create_swipe_encoder_v1(const char* model) {
    ++creation_calls;
    if (std::strcmp(model, "private-failed-creation") == 0) {
        return nullptr;
    }
    auto* encoder = new Encoder;
    // The next allocation is the production adapter wrapper, after the
    // returned encoder exists but before the wrapper can own it.
    framekeyboard_test_swipe_arm_allocation_failure();
    return encoder;
}
__attribute__((constructor)) static void initialized() {
    framekeyboard_test_swipe_module_initialized();
}
__attribute__((destructor)) static void unloaded() {
    framekeyboard_test_swipe_module_unloaded();
}
