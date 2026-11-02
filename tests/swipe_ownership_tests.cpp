#include "framekeyboard/swipe.hpp"

#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
struct Counts {
    unsigned initialized{}, unloaded{}, created{}, destroyed{};
    bool unloaded_with_encoder{};
} counts;
bool inject{}, fail_next{};
void require(bool valid, const char* message) {
    if (!valid) {
        throw std::runtime_error(message);
    }
}
std::filesystem::path fixture_path() {
    return std::filesystem::canonical("/proc/self/exe").parent_path() / "engines" /
           "framekeyboard-swipe.so";
}
void verify_fixture() {
    void* library = dlopen(fixture_path().c_str(), RTLD_NOW | RTLD_LOCAL);
    require(library, "load the private ownership fixture before creating any encoder");
    auto marker = reinterpret_cast<int (*)()>(dlsym(library, "framekeyboard_test_swipe_fixture_v1"));
    const bool fixture = marker && marker() == 1;
    dlclose(library); // No extra reference may keep the actual test module loaded.
    require(fixture, "only the private module may run; no real model is loaded");
    require(counts.initialized == 1 && counts.unloaded == 1,
            "preflight really unloads and cannot pin the module for production");
    counts = {};
}
void verify_generation(unsigned calls) {
    // Observing an already mapped module must not promote its lifetime or
    // keep an extra reference that would conceal a production-loader failure.
    void* library = dlopen(fixture_path().c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    require(library, "the production loader retained its process-global module");
    auto created =
        reinterpret_cast<unsigned (*)()>(dlsym(library, "framekeyboard_test_swipe_creation_calls"));
    const bool expected = created && created() == calls;
    dlclose(library);
    require(expected && counts.initialized == 1 && counts.unloaded == 0 && !counts.unloaded_with_encoder,
            "factory calls share one mapped global generation");
}
void failed_creation() {
    bool refused = false;
    try {
        auto encoder = framekeyboard::load_swipe_encoder("private-failed-creation");
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(refused && counts.created == 0 && counts.destroyed == 0,
            "failed model creation retains no encoder instance");
    verify_generation(1);
}
void ownership(bool failure, unsigned calls) {
    const auto before = counts.created;
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
    require(counts.created == before + 1 && counts.destroyed == counts.created,
            "normal and exceptional cleanup destroy each encoder instance");
    verify_generation(calls);
}
} // namespace

extern "C" void framekeyboard_test_swipe_arm_allocation_failure() {
    ++counts.created;
    fail_next = inject;
}
extern "C" void framekeyboard_test_swipe_encoder_destroyed() {
    ++counts.destroyed;
}
extern "C" void framekeyboard_test_swipe_module_unloaded() {
    ++counts.unloaded;
    counts.unloaded_with_encoder |= counts.created != counts.destroyed;
}
extern "C" void framekeyboard_test_swipe_module_initialized() {
    ++counts.initialized;
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
        failed_creation();
        ownership(false, 2);
        ownership(true, 3);
        ownership(false, 4);
    } catch (const std::exception& error) {
        std::cerr << "swipe ownership test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Swipe module ownership and allocation-failure cleanup passed.\n";
}
