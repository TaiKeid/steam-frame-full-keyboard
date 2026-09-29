cmake_minimum_required(VERSION 3.24)
project(framekeyboard VERSION 0.3.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NATIVE REQUIRED IMPORTED_TARGET cairo pangocairo xkbcommon json-c)
pkg_check_modules(EI REQUIRED IMPORTED_TARGET libei-1.0)
pkg_check_modules(SDL REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)

file(GLOB LAYOUT_FILES CONFIGURE_DEPENDS layouts/*.json)
file(GLOB PROFILE_FILES CONFIGURE_DEPENDS layouts/*.json languages/*.json themes/*.json)
set(PROFILE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/bundled_profiles.hpp")
add_custom_command(OUTPUT "${PROFILE_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_layout.py"
            ${LAYOUT_FILES}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_defaults.py"
            "${CMAKE_CURRENT_SOURCE_DIR}" "${PROFILE_HEADER}"
    DEPENDS ${PROFILE_FILES} tools/embed_defaults.py tools/validate_layout.py
    VERBATIM)
add_library(keyboard_core src/config.cpp src/input.cpp src/panel.cpp src/app.cpp src/instance.cpp src/placement.cpp src/placement_store.cpp src/grip.cpp src/ei_input.cpp "${PROFILE_HEADER}")
target_include_directories(keyboard_core PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(keyboard_core PUBLIC PkgConfig::NATIVE PkgConfig::EI)
target_compile_options(keyboard_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

# Link the matching architecture from the sysroot; embed only the device runtime path.
if(CMAKE_CROSSCOMPILING)
    set(OPENVR_LIBRARY "${CMAKE_SYSROOT}/opt/steamvr/bin/linuxarm64/libopenvr_api.so" CACHE FILEPATH "OpenVR library")
    set(OPENVR_RUNTIME_PATH "/opt/steamvr/bin/linuxarm64")
else()
    find_library(OPENVR_LIBRARY openvr_api HINTS
        "$ENV{HOME}/.local/share/Steam/steamapps/common/SteamVR/bin/linux64"
        "$ENV{HOME}/.local/share/Steam/ubuntu12_64" REQUIRED)
    get_filename_component(OPENVR_RUNTIME_PATH "${OPENVR_LIBRARY}" DIRECTORY)
endif()
add_library(overlay_texture third_party/overlay_texture/vk_texture.cpp)
target_include_directories(overlay_texture PUBLIC third_party/overlay_texture third_party/openvr)
target_link_libraries(overlay_texture PUBLIC PkgConfig::VULKAN "${OPENVR_LIBRARY}")
add_executable(framekeyboard src/main.cpp src/preview.cpp src/vr.cpp)
target_link_libraries(framekeyboard PRIVATE keyboard_core overlay_texture PkgConfig::SDL)
target_compile_options(framekeyboard PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
target_compile_definitions(framekeyboard PRIVATE FRAMEKEYBOARD_VERSION="${PROJECT_VERSION}")
set_target_properties(framekeyboard PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_RUNTIME_PATH}")

include(CTest)
if(BUILD_TESTING)
    add_executable(keyboard-tests tests/keyboard_tests.cpp)
    target_link_libraries(keyboard-tests PRIVATE keyboard_core)
    target_compile_options(keyboard-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME keyboard-core COMMAND keyboard-tests)
    find_package(Threads REQUIRED)
    add_executable(placement-instance-tests tests/placement_instance_tests.cpp)
    target_link_libraries(placement-instance-tests PRIVATE keyboard_core Threads::Threads)
    target_compile_options(placement-instance-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME placement-instance COMMAND placement-instance-tests)
    # Opt-in only: maps a disposable window and verifies focus before each key.
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
    add_executable(ei-target-probe tests/ei_target_probe.cpp)
    target_link_libraries(ei-target-probe PRIVATE keyboard_core PkgConfig::X11)
    # Opt-in read-only validation against the installed Frame controller models.
    add_executable(stick-component-probe tests/stick_component_probe.cpp)
    target_link_libraries(stick-component-probe PRIVATE keyboard_core overlay_texture)
    set_target_properties(stick-component-probe PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_RUNTIME_PATH}")
    add_executable(vr-panel-probe tests/vr_panel_probe.cpp)
    target_link_libraries(vr-panel-probe PRIVATE keyboard_core overlay_texture)
    set_target_properties(vr-panel-probe PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_RUNTIME_PATH}")
    # Intentionally excluded from CTest: requires an accessible kernel uinput device.
    add_executable(uinput-smoke-test tests/uinput_smoke.cpp)
    target_link_libraries(uinput-smoke-test PRIVATE keyboard_core)
endif()
install(TARGETS framekeyboard RUNTIME DESTINATION bin)
install(DIRECTORY layouts languages themes config DESTINATION share/framekeyboard)
install(FILES third_party/README.md DESTINATION share/framekeyboard/licenses)
install(FILES third_party/openvr/LICENSE DESTINATION share/framekeyboard/licenses RENAME OpenVR-LICENSE)
install(FILES third_party/overlay_texture/LICENSE DESTINATION share/framekeyboard/licenses RENAME overlay-texture-LICENSE)
