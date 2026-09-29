cmake_minimum_required(VERSION 3.24)
project(framekeyboard VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(LAYOUT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/layouts/en-us.json")
set(LAYOUT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/default_layout.hpp")
add_custom_command(
    OUTPUT "${LAYOUT_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_layout.py"
            "${LAYOUT_FILE}" --output "${LAYOUT_HEADER}"
    DEPENDS "${LAYOUT_FILE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_layout.py"
    VERBATIM
    COMMENT "Validate and compile the keyboard layout"
)
add_executable(framekeyboard src/main.cpp "${LAYOUT_HEADER}")
target_include_directories(framekeyboard PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(framekeyboard PRIVATE FRAMEKEYBOARD_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(framekeyboard PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()
# Rendering, OpenVR and input dependencies are added with their implementation milestones.
# This bootstrap intentionally has no device access, injection or autostart.
