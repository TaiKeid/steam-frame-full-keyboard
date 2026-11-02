#include "framekeyboard/swipe.hpp"

#include <array>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
enum class Event { EncoderDestroyed, ModuleUnloaded };
std::array<Event, 4> events;
std::size_t event_count{};
bool inject{}, fail_next{};
void require(bool valid, const char* message) {
    if (!valid) {
        throw std::runtime_error(message);
    }
}
void record(Event event) {
    if (event_count < events.size()) {
        events[event_count++] = event;
    }
}
void verify_fixture() {
    const auto path = std::filesystem::canonical("/proc/self/exe").parent_path() / "engines" /
                      "framekeyboard-swipe.so";
    void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    require(library, "load the private ownership fixture before creating any encoder");
    auto marker = reinterpret_cast<int (*)()>(dlsym(library, "framekeyboard_test_swipe_fixture_v1"));
    const bool fixture = marker && marker() == 1;
    dlclose(library); // No extra reference may keep the actual test module loaded.
    require(fixture, "only the private module may run; no real model is loaded");
}
void ownership(bool failure) {
    event_count = 0;
    inject = failure;
    fail_next = false;
    bool caught = false;
    try {
        auto encoder = framekeyboard::load_swipe_encoder("private-ownership-fixture");
        require(encoder != nullptr, "the fixture created an encoder");
    } catch (const std::bad_alloc&) {
        caught = true;
    }
    require(caught == failure, "only the requested wrapper allocation fails");
    require(event_count == 2 && events[0] == Event::EncoderDestroyed &&
                events[1] == Event::ModuleUnloaded,
            "normal and exceptional cleanup destroy the encoder before unloading its module");
}
} // namespace

extern "C" void framekeyboard_test_swipe_arm_allocation_failure() {
    fail_next = inject;
}
extern "C" void framekeyboard_test_swipe_encoder_destroyed() {
    record(Event::EncoderDestroyed);
}
extern "C" void framekeyboard_test_swipe_module_unloaded() {
    record(Event::ModuleUnloaded);
}

// Fail precisely once, after the fake engine has been returned. The fixture's
// destruction callbacks record into fixed storage and cannot allocate here.
void* operator new(std::size_t size) {
    if (fail_next) {
        fail_next = false;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size ? size : 1)) {
        return memory;
    }
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    try {
        verify_fixture();
        ownership(false);
        ownership(true);
    } catch (const std::exception& error) {
        std::cerr << "swipe ownership test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Swipe module ownership and allocation-failure cleanup passed.\n";
}
