cmake_minimum_required(VERSION 3.24)
project(framekeyboard VERSION 0.18.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Off builds only the keyboard's core and the desktop simulator
# (docs/simulator.md), for a computer without SteamVR or Vulkan.
option(FRAMEKEYBOARD_VR "Build the VR keyboard, which needs OpenVR and Vulkan" ON)
# Development builds only: dashboard focus timing in the journal (src/vr.cpp's trace_focus).
option(FRAMEKEYBOARD_FOCUS_TRACE "Log dashboard focus clicks, samples and decisions" OFF)
option(FRAMEKEYBOARD_FRAME_TRACE "Log the VR loop's slowest frames and each Japanese conversion's time" OFF)
# Local builds have debug mode: a Debug card in Settings, always on, also in
# the simulator. Releases (the frame-arm64-release preset, scripts/package.sh)
# turn it off, which leaves its code out and --debug unread.
option(FRAMEKEYBOARD_DEBUG_MODE "Debug card in Settings and --debug" ON)
# Keep build-machine directories out of __FILE__, assertions and debug paths.
# RelWithDebInfo remains useful locally; packaging strips only its installed copy.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options("-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=."
                        "-ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=./build")
    if(CMAKE_SYSROOT)
        add_compile_options("-ffile-prefix-map=${CMAKE_SYSROOT}=/")
    endif()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(NATIVE REQUIRED IMPORTED_TARGET cairo pangocairo harfbuzz xkbcommon json-c)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
# SteamOS libei 1.4.1 crashes on an aborted negotiation. Keep the fixed client
# inside this app; never replace the runtime library used by Steam itself.
add_subdirectory(third_party/libei)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(IME_XML "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gamescope/gamescope-input-method.xml")
set(IME_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/gamescope-input-method-client.h")
set(IME_CODE "${CMAKE_CURRENT_BINARY_DIR}/generated/gamescope-input-method-protocol.c")
add_custom_command(OUTPUT "${IME_HEADER}" "${IME_CODE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND "${WAYLAND_SCANNER}" client-header "${IME_XML}" "${IME_HEADER}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${IME_XML}" "${IME_CODE}"
    DEPENDS "${IME_XML}" VERBATIM)
set(DATA_CONTROL_XML "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wlr-protocols/wlr-data-control-unstable-v1.xml")
set(DATA_CONTROL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/wlr-data-control-client.h")
set(DATA_CONTROL_CODE "${CMAKE_CURRENT_BINARY_DIR}/generated/wlr-data-control-protocol.c")
set(EXT_DATA_CONTROL_XML "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wayland-protocols/ext-data-control-v1.xml")
set(EXT_DATA_CONTROL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/ext-data-control-client.h")
set(EXT_DATA_CONTROL_CODE "${CMAKE_CURRENT_BINARY_DIR}/generated/ext-data-control-protocol.c")
add_custom_command(OUTPUT "${EXT_DATA_CONTROL_HEADER}" "${EXT_DATA_CONTROL_CODE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND "${WAYLAND_SCANNER}" client-header "${EXT_DATA_CONTROL_XML}" "${EXT_DATA_CONTROL_HEADER}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${EXT_DATA_CONTROL_XML}" "${EXT_DATA_CONTROL_CODE}"
    DEPENDS "${EXT_DATA_CONTROL_XML}" VERBATIM)
add_custom_command(OUTPUT "${DATA_CONTROL_HEADER}" "${DATA_CONTROL_CODE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND "${WAYLAND_SCANNER}" client-header "${DATA_CONTROL_XML}" "${DATA_CONTROL_HEADER}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${DATA_CONTROL_XML}" "${DATA_CONTROL_CODE}"
    DEPENDS "${DATA_CONTROL_XML}" VERBATIM)
pkg_check_modules(SDL REQUIRED IMPORTED_TARGET sdl2)
if(FRAMEKEYBOARD_VR)
    pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
endif()

file(GLOB LAYOUT_FILES CONFIGURE_DEPENDS layouts/*.json)
file(GLOB PROFILE_FILES CONFIGURE_DEPENDS layouts/*.json languages/*.json themes/*.json locales/*.json)
set(PROFILE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/bundled_profiles.hpp")
add_custom_command(OUTPUT "${PROFILE_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_layout.py"
            ${LAYOUT_FILES}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_defaults.py"
            "${CMAKE_CURRENT_SOURCE_DIR}" "${PROFILE_HEADER}"
    DEPENDS ${PROFILE_FILES} emoji/emoji.tsv tools/embed_defaults.py tools/validate_layout.py
    VERBATIM)
add_library(keyboard_core src/vr_lifecycle.cpp src/vr_runtime.cpp src/config.cpp src/input.cpp src/output.cpp src/panel.cpp src/settings_ui.cpp src/settings_writer.cpp src/key_sounds.cpp src/startup.cpp src/stream_support.cpp src/session_environment.cpp src/app.cpp src/app_table_context.cpp src/app_languages.cpp src/localization.cpp src/key_areas.cpp src/table_engine.cpp src/instance.cpp src/placement.cpp src/placement_store.cpp src/resizer.cpp src/emoji.cpp src/glide.cpp src/candidates.cpp src/emoji_glyphs.cpp src/grip.cpp src/ei_input.cpp src/japanese.cpp src/japanese_candidates.cpp src/chinese_candidates.cpp src/japanese_worker.cpp src/mozc.cpp src/next_words.cpp src/private_file.cpp src/word_suggestions.cpp src/emoji_words.cpp src/cjk.cpp src/swipe.cpp src/text_input.cpp src/native_replacement.cpp src/updates.cpp src/dictation.cpp src/dictation_editor.cpp src/dictation_mode.cpp src/dictation_send.cpp src/clipboard.cpp src/desktop_focus.cpp src/desktop_terminal.cpp src/terminal_keys.cpp src/shell_protocol.cpp src/shell_sink.cpp src/app_host.cpp src/shell_host.cpp "${IME_HEADER}" "${IME_CODE}" "${DATA_CONTROL_HEADER}" "${DATA_CONTROL_CODE}" "${EXT_DATA_CONTROL_HEADER}" "${EXT_DATA_CONTROL_CODE}" "${PROFILE_HEADER}")
target_include_directories(keyboard_core PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(keyboard_core PUBLIC PkgConfig::NATIVE framekeyboard_ei PkgConfig::WAYLAND_CLIENT PkgConfig::XCB ${CMAKE_DL_LIBS} Threads::Threads)
target_compile_options(keyboard_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
if(FRAMEKEYBOARD_FRAME_TRACE)
    target_compile_definitions(keyboard_core PUBLIC FRAMEKEYBOARD_FRAME_TRACE)
endif()
if(FRAMEKEYBOARD_DEBUG_MODE)
    target_compile_definitions(keyboard_core PUBLIC FRAMEKEYBOARD_DEBUG_MODE)
endif()
# Haptic sounds load PipeWire at run time, so its headers are all the build needs.
pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
if(PIPEWIRE_FOUND)
    target_include_directories(keyboard_core SYSTEM PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
    target_compile_definitions(keyboard_core PRIVATE FRAMEKEYBOARD_PIPEWIRE)
else()
    message(WARNING "PipeWire headers not found: haptic sounds will stay silent in this build.")
endif()
# Frame supplies libhangul. Minimal host builds can still run the ordinary
# keyboard; unsupported composition profiles fail explicitly on Apply.
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(HANGUL QUIET IMPORTED_TARGET libhangul)
# Remove retired adapters from this active build tree. Packaging also names
# retained modules explicitly, so stale files can never become release assets.
file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/engines/framekeyboard-pinyin.so"
            "${CMAKE_CURRENT_BINARY_DIR}/engines/framekeyboard-pyzy.so"
            "${CMAKE_CURRENT_BINARY_DIR}/engines/framekeyboard-chewing.so")
if(HANGUL_FOUND)
    add_library(cjk_hangul MODULE src/cjk_hangul.cpp)
    target_link_libraries(cjk_hangul PRIVATE PkgConfig::GLIB PkgConfig::HANGUL)
    target_compile_options(cjk_hangul PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    if(CMAKE_CROSSCOMPILING)
        target_link_options(cjk_hangul PRIVATE -fuse-ld=lld)
    endif()
    set_target_properties(cjk_hangul PROPERTIES
        PREFIX "" OUTPUT_NAME "framekeyboard-hangul"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/engines"
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    add_dependencies(keyboard_core cjk_hangul)
    install(TARGETS cjk_hangul LIBRARY DESTINATION lib/framekeyboard)
else()
    file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/engines/framekeyboard-hangul.so")
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Frame releases require libhangul headers and libraries in the sysroot")
    endif()
    message(STATUS "libhangul unavailable: this host build cannot compose Korean")
endif()

# Optional swipe typing engine: the FUTO Swipe encoder through a pinned
# ExecuTorch build (tools/build-executorch.sh). Without it the build still
# works and Settings keeps swipe typing disabled.
set(FRAMEKEYBOARD_EXECUTORCH_DIR "" CACHE PATH "ExecuTorch install prefix for the swipe engine")
set(FRAMEKEYBOARD_SWIPE_ENGINE OFF)
if(FRAMEKEYBOARD_EXECUTORCH_DIR)
    set(executorch_DIR "${FRAMEKEYBOARD_EXECUTORCH_DIR}/lib/cmake/ExecuTorch")
    find_package(executorch REQUIRED CONFIG NO_CMAKE_FIND_ROOT_PATH)
    add_library(swipe_engine MODULE src/swipe_engine.cpp)
    target_include_directories(swipe_engine PRIVATE include)
    target_link_libraries(swipe_engine PRIVATE executorch extension_module_static extension_tensor
        extension_threadpool xnnpack_backend portable_ops_lib portable_kernels)
    target_compile_options(swipe_engine PRIVATE -Wall -Wextra)
    # Keep ExecuTorch/XNNPACK symbols private to the module.
    target_link_options(swipe_engine PRIVATE "-Wl,--exclude-libs,ALL")
    if(CMAKE_CROSSCOMPILING)
        target_link_options(swipe_engine PRIVATE -fuse-ld=lld)
    endif()
    set_target_properties(swipe_engine PROPERTIES
        PREFIX "" OUTPUT_NAME "framekeyboard-swipe"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/engines"
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    add_dependencies(keyboard_core swipe_engine)
    install(TARGETS swipe_engine LIBRARY DESTINATION lib/framekeyboard)
    install(FILES third_party/futo-swipe/model_fp32.pte third_party/futo-swipe/LICENSE.md
            DESTINATION share/framekeyboard/swipe/futo-swipe)
    set(FRAMEKEYBOARD_SWIPE_ENGINE ON)
    # Development binaries find the model beside themselves, as for the VR manifests.
    configure_file(third_party/futo-swipe/model_fp32.pte swipe/futo-swipe/model_fp32.pte COPYONLY)
else()
    file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/engines/framekeyboard-swipe.so"
                "${CMAKE_CURRENT_BINARY_DIR}/swipe/futo-swipe/model_fp32.pte")
    if(CMAKE_CROSSCOMPILING)
        message(WARNING "FRAMEKEYBOARD_EXECUTORCH_DIR is unset: this release cannot offer swipe typing")
    else()
        message(STATUS "Swipe engine unavailable: set FRAMEKEYBOARD_EXECUTORCH_DIR to build it")
    endif()
endif()
# Optional on-device dictation engine: whisper.cpp at a pinned commit, built
# by tools/build-whisper.sh outside Git (docs/dictation.md, On-device
# recognition). Without it, dictation offers only the speech server.
set(FRAMEKEYBOARD_WHISPER_DIR "" CACHE PATH "whisper.cpp install prefix for the on-device dictation engine")
if(FRAMEKEYBOARD_WHISPER_DIR)
    set(whisper_DIR "${FRAMEKEYBOARD_WHISPER_DIR}/lib/cmake/whisper")
    set(ggml_DIR "${FRAMEKEYBOARD_WHISPER_DIR}/lib/cmake/ggml")
    find_package(whisper REQUIRED CONFIG NO_CMAKE_FIND_ROOT_PATH)
    add_executable(whisper_engine src/whisper_engine.cpp)
    target_link_libraries(whisper_engine PRIVATE whisper PkgConfig::NATIVE)
    target_compile_options(whisper_engine PRIVATE -Wall -Wextra)
    if(CMAKE_CROSSCOMPILING)
        target_link_options(whisper_engine PRIVATE -fuse-ld=lld)
    endif()
    set_target_properties(whisper_engine PROPERTIES OUTPUT_NAME "framekeyboard-whisper"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/engines")
    # whisper.cpp's quantize tool makes the development 4-bit model on the Frame.
    file(COPY "${FRAMEKEYBOARD_WHISPER_DIR}/bin/whisper-quantize" DESTINATION engines)
    install(TARGETS whisper_engine RUNTIME DESTINATION lib/framekeyboard)
    install(PROGRAMS "${FRAMEKEYBOARD_WHISPER_DIR}/bin/whisper-quantize" DESTINATION lib/framekeyboard)
    # A prebuilt program is copied as is: strip it as --strip does built targets.
    install(CODE "if(CMAKE_INSTALL_DO_STRIP)
        execute_process(COMMAND \"${CMAKE_STRIP}\"
            \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/lib/framekeyboard/whisper-quantize\"
            COMMAND_ERROR_IS_FATAL ANY)
    endif()")
elseif(CMAKE_CROSSCOMPILING)
    message(WARNING "FRAMEKEYBOARD_WHISPER_DIR is unset: this release cannot offer on-device dictation")
else()
    message(STATUS "On-device dictation engine unavailable: set FRAMEKEYBOARD_WHISPER_DIR to build it")
endif()
# Optional Japanese conversion engine: Mozc's server, a prebuilt program
# fetched by tools/fetch-mozc.sh outside Git (docs/japanese.md, Mozc). It is
# installed as framekeyboard-mozc; without it, Anthy converts.
set(FRAMEKEYBOARD_MOZC_SERVER "" CACHE FILEPATH "mozc_server program for Japanese conversion")
if(FRAMEKEYBOARD_MOZC_SERVER)
    configure_file("${FRAMEKEYBOARD_MOZC_SERVER}" "${CMAKE_CURRENT_BINARY_DIR}/engines/framekeyboard-mozc" COPYONLY)
    install(PROGRAMS "${FRAMEKEYBOARD_MOZC_SERVER}" DESTINATION lib/framekeyboard RENAME framekeyboard-mozc)
    install(FILES third_party/mozc/credits_en.html DESTINATION third_party/mozc)
else()
    file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/engines/framekeyboard-mozc")
    if(CMAKE_CROSSCOMPILING)
        message(WARNING "FRAMEKEYBOARD_MOZC_SERVER is unset: this release converts Japanese with Anthy alone")
    else()
        message(STATUS "Mozc unavailable: set FRAMEKEYBOARD_MOZC_SERVER to convert Japanese with it")
    endif()
endif()
# Chinese engines: libime for Pinyin, with OpenCC for
# Traditional, and McBopomofo for Zhuyin (docs/languages.md, Chinese engines). They come
# from a prefix tools/build-chinese.sh made, outside Git. Each is a module, and
# their libraries and data go in chinese/ beside the modules: engines/chinese
# here, linked to the prefix, and lib/framekeyboard/chinese in a package.
set(FRAMEKEYBOARD_CHINESE_PREFIX "" CACHE PATH "Prefix with the Chinese engines, from tools/build-chinese.sh")
set(CHINESE_OUT "${CMAKE_CURRENT_BINARY_DIR}/engines/chinese")
if(FRAMEKEYBOARD_CHINESE_PREFIX)
    set(chinese "${FRAMEKEYBOARD_CHINESE_PREFIX}")
    if(NOT EXISTS "${chinese}/share/framekeyboard/chinese-pins.sh")
        message(FATAL_ERROR "FRAMEKEYBOARD_CHINESE_PREFIX is not a prefix from tools/build-chinese.sh")
    endif()
    function(framekeyboard_chinese_module name)
        add_library(cjk_${name} MODULE src/cjk_${name}.cpp ${ARGN})
        target_include_directories(cjk_${name} PRIVATE include)
        target_compile_options(cjk_${name} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
        # The engines' shared libraries sit in chinese/ beside the module.
        # They are linked even where a symbol isn't used directly, so the
        # module's run path finds them before they look for each other.
        target_link_options(cjk_${name} PRIVATE LINKER:--no-as-needed)
        if(CMAKE_CROSSCOMPILING)
            target_link_options(cjk_${name} PRIVATE -fuse-ld=lld)
        endif()
        set_target_properties(cjk_${name} PROPERTIES
            PREFIX "" OUTPUT_NAME "framekeyboard-${name}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/engines"
            # The build tree's run path would name the prefix too; the
            # installed one is all a copy, such as the AppImage's, needs.
            BUILD_WITH_INSTALL_RPATH YES INSTALL_RPATH "\$ORIGIN/chinese"
            CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
        add_dependencies(keyboard_core cjk_${name})
        install(TARGETS cjk_${name} LIBRARY DESTINATION lib/framekeyboard)
    endfunction()
    framekeyboard_chinese_module(libime src/private_file.cpp)
    target_include_directories(cjk_libime SYSTEM PRIVATE "${chinese}/include" "${chinese}/include/LibIME"
        "${chinese}/include/Fcitx5/Utils")
    target_link_libraries(cjk_libime PRIVATE "${chinese}/lib/libIMEPinyin.so" "${chinese}/lib/libIMECore.so"
        "${chinese}/lib/libFcitx5Utils.so" "${chinese}/lib/libopencc.a" "${chinese}/lib/libmarisa.a")
    framekeyboard_chinese_module(mcbopomofo src/private_file.cpp)
    target_include_directories(cjk_mcbopomofo SYSTEM PRIVATE "${chinese}/include/mcbopomofo")
    target_link_libraries(cjk_mcbopomofo PRIVATE "${chinese}/lib/mcbopomofo/libMcBopomofoLMLib.a"
        "${chinese}/lib/mcbopomofo/libgramambular2_lib.a" "${chinese}/lib/mcbopomofo/libMandarinLib.a")
    # The files each engine reads, by where the modules look for them.
    set(chinese_files
        "libime/sc.dict=share/libime/sc.dict" "libime/extb.dict=share/libime/extb.dict"
        "libime/zh_CN.lm=lib/libime/zh_CN.lm"
        "mcbopomofo/data.txt=share/mcbopomofo/data.txt")
    foreach(library libFcitx5Utils.so.2 libIMECore.so.0 libIMEPinyin.so.0)
        list(APPEND chinese_files "${library}=lib/${library}")
    endforeach()
    file(REMOVE_RECURSE "${CHINESE_OUT}")
    foreach(entry ${chinese_files})
        string(REPLACE "=" ";" pair "${entry}")
        list(GET pair 0 destination)
        list(GET pair 1 source)
        if(NOT EXISTS "${chinese}/${source}" OR IS_DIRECTORY "${chinese}/${source}")
            message(FATAL_ERROR "Chinese engine prefix is missing required asset: ${source}")
        endif()
        file(REAL_PATH "${chinese}/${source}" real)
        get_filename_component(folder "${destination}" DIRECTORY)
        get_filename_component(filename "${destination}" NAME)
        file(MAKE_DIRECTORY "${CHINESE_OUT}/${folder}")
        file(CREATE_LINK "${real}" "${CHINESE_OUT}/${destination}" SYMBOLIC)
        install(FILES "${real}" DESTINATION "lib/framekeyboard/chinese/${folder}" RENAME "${filename}")
    endforeach()
    # OpenCC's Taiwan configuration with the dictionaries it names.
    set(opencc "${chinese}/share/opencc")
    if(NOT EXISTS "${opencc}/s2twp.json")
        message(FATAL_ERROR "Chinese engine prefix is missing OpenCC s2twp.json")
    endif()
    file(READ "${opencc}/s2twp.json" opencc_config)
    string(REGEX MATCHALL "\"file\"[ \t\r\n]*:[ \t\r\n]*\"[^\"]+\"" opencc_dictionaries "${opencc_config}")
    foreach(entry ${opencc_dictionaries})
        string(REGEX REPLACE "^.*:[ \t\r\n]*\"([^\"]+)\"$" "\\1" dictionary "${entry}")
        if(NOT EXISTS "${opencc}/${dictionary}" OR IS_DIRECTORY "${opencc}/${dictionary}")
            message(FATAL_ERROR "Chinese engine prefix is missing OpenCC dictionary: ${dictionary}")
        endif()
    endforeach()
    file(CREATE_LINK "${chinese}/share/opencc" "${CHINESE_OUT}/opencc" SYMBOLIC)
    install(DIRECTORY "${chinese}/share/opencc/" DESTINATION "lib/framekeyboard/chinese/opencc")
    install(DIRECTORY third_party/chinese-engines/ DESTINATION third_party/chinese-engines)
else()
    file(REMOVE_RECURSE "${CHINESE_OUT}")
    file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/engines/framekeyboard-libime.so"
                "${CMAKE_CURRENT_BINARY_DIR}/engines/framekeyboard-mcbopomofo.so")
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Frame releases require FRAMEKEYBOARD_CHINESE_PREFIX (tools/build-chinese.sh)")
    endif()
    message(STATUS "Chinese engines unavailable: set FRAMEKEYBOARD_CHINESE_PREFIX to build them")
endif()
# Packaging follows this configuration even if old engine files were left in the tree.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/simulator-engines.conf"
    CONTENT "pack_hangul=$<BOOL:${HANGUL_FOUND}>\npack_mozc=$<BOOL:${FRAMEKEYBOARD_MOZC_SERVER}>\npack_swipe=$<BOOL:${FRAMEKEYBOARD_SWIPE_ENGINE}>\npack_chinese=$<BOOL:${FRAMEKEYBOARD_CHINESE_PREFIX}>\n")
file(GLOB SWIPE_LEXICONS CONFIGURE_DEPENDS swipe/lexicons/*.tsv)
foreach(lexicon ${SWIPE_LEXICONS})
    get_filename_component(name "${lexicon}" NAME)
    configure_file("${lexicon}" "swipe/lexicons/${name}" COPYONLY)
endforeach()
# Japanese completion's common words, beside the executable as in share/.
configure_file(japanese/words.tsv japanese/words.tsv COPYONLY)
configure_file(japanese/kaomoji.tsv japanese/kaomoji.tsv COPYONLY)
configure_file(japanese/preferred.tsv japanese/preferred.tsv COPYONLY)
configure_file(japanese/mozc-words.tsv japanese/mozc-words.tsv COPYONLY)
configure_file(chinese/char-ranks.txt chinese/char-ranks.txt COPYONLY)

if(FRAMEKEYBOARD_VR)
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
    add_executable(framekeyboard-native-guard src/native_restore_guard.cpp)
    target_include_directories(framekeyboard-native-guard PRIVATE include third_party/openvr)
    target_link_libraries(framekeyboard-native-guard PRIVATE PkgConfig::NATIVE "${OPENVR_LIBRARY}")
    set_target_properties(framekeyboard-native-guard PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_RUNTIME_PATH}")
    add_dependencies(framekeyboard framekeyboard-native-guard)
    target_link_libraries(framekeyboard PRIVATE keyboard_core overlay_texture PkgConfig::SDL)
    target_compile_options(framekeyboard PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    target_compile_definitions(framekeyboard PRIVATE FRAMEKEYBOARD_VERSION="${PROJECT_VERSION}")
    if(FRAMEKEYBOARD_FOCUS_TRACE)
        target_compile_definitions(framekeyboard PRIVATE FRAMEKEYBOARD_FOCUS_TRACE)
    endif()
    # Development binaries find these beside themselves; packages use share/framekeyboard.
    # Do not embed an absolute source-tree path in the executable for this fallback.
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vr")
    foreach(manifest actions.json frame-controller.json)
        configure_file("vr/${manifest}" "vr/${manifest}" COPYONLY)
    endforeach()
    set_target_properties(framekeyboard PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_RUNTIME_PATH}")
endif()

# The desktop simulator (docs/simulator.md): the keyboard typing into a text
# field in its own window, for trying it without a Steam Frame. No OpenVR or
# Vulkan, and not part of the release package.
if(NOT CMAKE_CROSSCOMPILING)
    add_library(simulator_core STATIC src/simulator/field.cpp src/simulator/simulator.cpp)
    target_include_directories(simulator_core PUBLIC src)
    target_link_libraries(simulator_core PUBLIC keyboard_core)
    target_compile_options(simulator_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    add_executable(framekeyboard-simulator src/simulator/main.cpp src/simulator/platform_linux.cpp)
    target_link_libraries(framekeyboard-simulator PRIVATE simulator_core PkgConfig::SDL)
    target_compile_options(framekeyboard-simulator PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    target_compile_definitions(framekeyboard-simulator PRIVATE FRAMEKEYBOARD_VERSION="${PROJECT_VERSION}")
endif()

# The installer (docs/installer.md): a window that downloads, installs and
# sets up a release, shipped as its own AppImage and not in the package.
# Host builds always compile it, for its --simulate mode; an ARM64 tree
# builds it only when asked (scripts/installer-appimage.sh).
set(INSTALLER_PICTURE "${CMAKE_CURRENT_BINARY_DIR}/generated/installer_keyboard.cpp")
add_custom_command(OUTPUT "${INSTALLER_PICTURE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_file.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/images/keyboard-graphite.png" "${INSTALLER_PICTURE}"
            framekeyboard::installer keyboard_png
    DEPENDS docs/images/keyboard-graphite.png tools/embed_file.py
    VERBATIM)
add_executable(framekeyboard-installer src/installer/main.cpp src/installer/installer.cpp
    src/installer/simulated_backend.cpp src/installer/system_backend.cpp "${INSTALLER_PICTURE}")
if(CMAKE_CROSSCOMPILING)
    set_target_properties(framekeyboard-installer PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
target_link_libraries(framekeyboard-installer PRIVATE keyboard_core PkgConfig::SDL)
# Only the libraries it calls: the keyboard's other dependencies stay out of the AppImage.
target_link_options(framekeyboard-installer PRIVATE -Wl,--as-needed)
target_compile_options(framekeyboard-installer PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
target_compile_definitions(framekeyboard-installer PRIVATE FRAMEKEYBOARD_VERSION="${PROJECT_VERSION}")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bridge")
foreach(bridge_file native_bridge.py steam_bridge.js)
    configure_file("bridge/${bridge_file}" "bridge/${bridge_file}" COPYONLY)
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/update")
configure_file(update/update_helper.py update/update_helper.py COPYONLY)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dictation")
configure_file(dictation/dictation_helper.py dictation/dictation_helper.py COPYONLY)
include(CTest)
# Steam Link typing: the library the Frame's streaming client loads in place of
# libopenvr_api.so (stream/openvr_passthrough.c). It names that library as a
# dependency without a path, and searches only its own folder after the
# client's LD_LIBRARY_PATH: the gate keeps a link to Steam's copy there, so it
# binds that copy whatever the environment; the package check in
# scripts/package.sh holds it to that. A host build exists only for
# the test, against a stand-in for the real library.
if((CMAKE_CROSSCOMPILING AND FRAMEKEYBOARD_VR) OR (NOT CMAKE_CROSSCOMPILING AND BUILD_TESTING))
    add_library(stream_passthrough MODULE stream/openvr_passthrough.c)
    target_compile_options(stream_passthrough PRIVATE -Wall -Wextra)
    # No OpenVR symbol is bound at link time: the wrapped call is looked up at
    # run time and every other one has to resolve through this file's dependency.
    target_link_options(stream_passthrough PRIVATE "LINKER:--no-as-needed" "LINKER:--enable-new-dtags")
    # RUNPATH is exactly $ORIGIN, in the build tree too: nothing of the build
    # machine's, and searched after LD_LIBRARY_PATH.
    set_target_properties(stream_passthrough PROPERTIES
        PREFIX "" OUTPUT_NAME "framekeyboard-stream"
        C_STANDARD 11 C_VISIBILITY_PRESET hidden
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/stream"
        BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "\$ORIGIN")
    # The gate (stream/stream_gate.c) runs before Steam starts and keeps the
    # folder Steam loads the library from; it is installed beside the library.
    add_executable(stream_gate stream/stream_gate.c)
    target_compile_options(stream_gate PRIVATE -Wall -Wextra)
    target_link_libraries(stream_gate PRIVATE ${CMAKE_DL_LIBS})
    set_target_properties(stream_gate PROPERTIES
        OUTPUT_NAME "framekeyboard-stream-gate" C_STANDARD 11
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/stream"
        SKIP_BUILD_RPATH TRUE)
    if(CMAKE_CROSSCOMPILING)
        target_link_libraries(stream_passthrough PRIVATE "${OPENVR_LIBRARY}")
        install(TARGETS stream_passthrough LIBRARY DESTINATION lib/framekeyboard)
        install(TARGETS stream_gate RUNTIME DESTINATION lib/framekeyboard)
        add_dependencies(framekeyboard stream_passthrough stream_gate)
    else()
        add_library(stream_fake_openvr SHARED tests/stream_fake_openvr.c)
        set_target_properties(stream_fake_openvr PROPERTIES OUTPUT_NAME "openvr_api" C_STANDARD 11
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/stream")
        target_link_libraries(stream_passthrough PRIVATE stream_fake_openvr)
        # The library looks SDL's functions up in the program that loaded it.
        add_executable(stream-passthrough-tests tests/stream_passthrough_tests.c)
        set_target_properties(stream-passthrough-tests PROPERTIES ENABLE_EXPORTS TRUE C_STANDARD 11)
        target_compile_options(stream-passthrough-tests PRIVATE -Wall -Wextra)
        target_link_libraries(stream-passthrough-tests PRIVATE ${CMAKE_DL_LIBS})
        add_dependencies(stream-passthrough-tests stream_passthrough)
        add_test(NAME stream-passthrough COMMAND stream-passthrough-tests
            "$<TARGET_FILE:stream_passthrough>" "$<TARGET_FILE:stream_fake_openvr>")
        add_test(NAME stream-gate COMMAND "${Python3_EXECUTABLE}" -I
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/stream_gate_tests.py" "$<TARGET_FILE:stream_gate>"
            "$<TARGET_FILE:stream_passthrough>" "$<TARGET_FILE_DIR:stream_fake_openvr>")
        add_dependencies(stream-passthrough-tests stream_gate)
    endif()
endif()
if(BUILD_TESTING)
    # LeakSanitizer accepts one suppression file; keep each filter's source separate.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        tests/lsan-fontconfig.supp tests/lsan-anthy.supp)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan-fontconfig.supp" fontconfig_suppressions)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan-anthy.supp" anthy_suppressions)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lsan-third-party.supp"
        CONTENT "${fontconfig_suppressions}\n${anthy_suppressions}")
    add_executable(key-sound-activity-tests tests/key_sound_activity_tests.cpp)
    target_include_directories(key-sound-activity-tests PRIVATE include)
    target_compile_options(key-sound-activity-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME key-sound-activity COMMAND key-sound-activity-tests)
    add_executable(vr-lifecycle-tests tests/vr_lifecycle_tests.cpp)
    target_link_libraries(vr-lifecycle-tests PRIVATE keyboard_core)
    target_compile_options(vr-lifecycle-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME vr-lifecycle COMMAND vr-lifecycle-tests)
    add_executable(ei-handshake-tests tests/ei_handshake_tests.cpp)
    target_link_libraries(ei-handshake-tests PRIVATE framekeyboard_ei)
    add_test(NAME ei-handshake COMMAND ei-handshake-tests)
    add_executable(output-tests tests/output_tests.cpp)
    target_link_libraries(output-tests PRIVATE keyboard_core)
    add_test(NAME ordered-output COMMAND output-tests)
    add_executable(startup-tests tests/startup_tests.cpp)
    target_link_libraries(startup-tests PRIVATE keyboard_core)
    add_test(NAME startup COMMAND startup-tests)
    add_executable(stream-support-tests tests/stream_support_tests.cpp)
    target_link_libraries(stream-support-tests PRIVATE keyboard_core)
    add_test(NAME stream-support COMMAND stream-support-tests)
    add_executable(localization-tests tests/localization_tests.cpp)
    target_link_libraries(localization-tests PRIVATE keyboard_core)
    add_test(NAME localization COMMAND localization-tests)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME locales COMMAND "${Python3_EXECUTABLE}" -I
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_locales.py" "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME locales-tool COMMAND "${Python3_EXECUTABLE}" -I
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_locales_tests.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_locales.py")
    endif()
    add_executable(word-suggestions-tests tests/word_suggestions_tests.cpp)
    target_link_libraries(word-suggestions-tests PRIVATE keyboard_core)
    add_test(NAME word-suggestions COMMAND word-suggestions-tests)
    add_executable(session-environment-tests tests/session_environment_tests.cpp)
    target_link_libraries(session-environment-tests PRIVATE keyboard_core)
    add_test(NAME session-environment COMMAND session-environment-tests)
    find_program(SYSTEMD_ANALYZE systemd-analyze)
    if(SYSTEMD_ANALYZE AND NOT CMAKE_CROSSCOMPILING)
        add_test(NAME startup-unit COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/startup_unit_tests.py"
            "$<TARGET_FILE:startup-tests>" "${SYSTEMD_ANALYZE}")
    endif()
    add_executable(update-tests tests/update_tests.cpp)
    target_link_libraries(update-tests PRIVATE keyboard_core)
    add_test(NAME updates COMMAND update-tests)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME update-helper COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/update_helper_tests.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/update/update_helper.py")
    endif()
    add_executable(dictation-editor-tests tests/dictation_editor_tests.cpp)
    target_link_libraries(dictation-editor-tests PRIVATE keyboard_core)
    target_compile_options(dictation-editor-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME dictation-editor COMMAND dictation-editor-tests)
    add_executable(dictation-send-tests tests/dictation_send_tests.cpp)
    target_link_libraries(dictation-send-tests PRIVATE keyboard_core)
    target_compile_options(dictation-send-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME dictation-send COMMAND dictation-send-tests)
    add_executable(dictation-mode-tests tests/dictation_mode_tests.cpp)
    target_link_libraries(dictation-mode-tests PRIVATE keyboard_core)
    target_include_directories(dictation-mode-tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_options(dictation-mode-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME dictation-mode COMMAND dictation-mode-tests)
    add_executable(dictation-tests tests/dictation_tests.cpp)
    target_link_libraries(dictation-tests PRIVATE keyboard_core)
    target_compile_options(dictation-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME dictation COMMAND dictation-tests "${CMAKE_CURRENT_SOURCE_DIR}/dictation/dictation_helper.py")
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dictation-helper COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/dictation_helper_tests.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/dictation/dictation_helper.py")
    endif()
    add_executable(native-replacement-tests tests/native_replacement_tests.cpp)
    target_link_libraries(native-replacement-tests PRIVATE keyboard_core)
    add_test(NAME native-replacement COMMAND native-replacement-tests)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME timing-ordering COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/timing_order_tests.py"
            "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME native-helper-spawn COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_helper_spawn_tests.py"
            "$<TARGET_FILE:native-replacement-tests>"
            "${CMAKE_CURRENT_SOURCE_DIR}/bridge/native_bridge.py")
        add_test(NAME native-bridge-transport COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_bridge_transport_tests.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/bridge/native_bridge.py")
        add_test(NAME replacement-measurement COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/replacement_measurement_tests.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/measure-replacement.py")
        find_program(NODE_EXECUTABLE node)
        if(NODE_EXECUTABLE)
            add_test(NAME native-bridge COMMAND "${NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_bridge_tests.js"
                "${CMAKE_CURRENT_SOURCE_DIR}/bridge/steam_bridge.js")
        endif()
    endif()
    if(FRAMEKEYBOARD_VR)
        # Opt-in timing probe: never sends input; --vr uses its own hidden overlay.
        add_executable(render-benchmark tests/render_benchmark.cpp)
        target_link_libraries(render-benchmark PRIVATE keyboard_core overlay_texture Threads::Threads)
        target_compile_options(render-benchmark PRIVATE -Wall -Wextra -Wpedantic)
        set_target_properties(render-benchmark PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_RUNTIME_PATH}")
    endif()
    add_executable(language-tests tests/language_tests.cpp)
    target_link_libraries(language-tests PRIVATE keyboard_core)
    target_compile_options(language-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME languages COMMAND language-tests --core)
    add_executable(table-composition-tests tests/table_composition_tests.cpp)
    target_link_libraries(table-composition-tests PRIVATE keyboard_core)
    target_compile_options(table-composition-tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(table-composition-tests PRIVATE
        "-Wl,--wrap=_ZN13framekeyboard9table_forERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE")
    add_test(NAME table-composition COMMAND table-composition-tests)
    set_tests_properties(table-composition PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME korean-input COMMAND language-tests --korean)
    add_test(NAME chinese-input COMMAND language-tests --chinese)
    set_tests_properties(korean-input chinese-input PROPERTIES SKIP_RETURN_CODE 77)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME optional-engines COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_loading_tests.py" "$<TARGET_FILE:language-tests>")
    endif()
    add_executable(japanese-tests tests/japanese_tests.cpp)
    target_link_libraries(japanese-tests PRIVATE keyboard_core)
    # A stand-in Mozc server the tests start in place of the real one.
    add_executable(fake-mozc tests/fake_mozc.cpp)
    target_compile_options(fake-mozc PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(japanese-tests PRIVATE FRAMEKEYBOARD_FAKE_MOZC="$<TARGET_FILE_NAME:fake-mozc>")
    add_dependencies(japanese-tests fake-mozc)
    add_test(NAME japanese-input COMMAND japanese-tests)
    add_executable(japanese-worker-tests tests/japanese_worker_tests.cpp)
    target_link_libraries(japanese-worker-tests PRIVATE keyboard_core)
    target_compile_options(japanese-worker-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME japanese-worker COMMAND japanese-worker-tests)
    add_executable(swipe-tests tests/swipe_tests.cpp)
    target_link_libraries(swipe-tests PRIVATE keyboard_core)
    target_compile_options(swipe-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME swipe COMMAND swipe-tests --core)
    add_test(NAME typing-suggestions COMMAND swipe-tests --typing)
    # Real encoder and bundled English list; skipped when the engine is not built.
    add_test(NAME swipe-model COMMAND swipe-tests --model)
    add_test(NAME swipe-chinese COMMAND swipe-tests --chinese)
    set_tests_properties(swipe-model swipe-chinese PROPERTIES SKIP_RETURN_CODE 77)
    # A private module exercises encoder/wrapper ownership without loading a
    # model. Keep both targets apart from development and installed engines.
    add_library(fake_swipe_encoder MODULE tests/fake_swipe_encoder.cpp)
    target_include_directories(fake_swipe_encoder PRIVATE include)
    target_compile_options(fake_swipe_encoder PRIVATE -Wall -Wextra -Wpedantic)
    if(CMAKE_CROSSCOMPILING)
        target_link_options(fake_swipe_encoder PRIVATE -fuse-ld=lld)
    endif()
    set_target_properties(fake_swipe_encoder PROPERTIES PREFIX "" OUTPUT_NAME framekeyboard-swipe
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-swipe-memory/engines")
    add_executable(swipe-ownership-tests tests/swipe_ownership_tests.cpp)
    target_link_libraries(swipe-ownership-tests PRIVATE keyboard_core)
    target_compile_options(swipe-ownership-tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(swipe-ownership-tests PRIVATE
        "-Wl,--export-dynamic-symbol=framekeyboard_test_swipe_arm_allocation_failure"
        "-Wl,--export-dynamic-symbol=framekeyboard_test_swipe_encoder_destroyed"
        "-Wl,--export-dynamic-symbol=framekeyboard_test_swipe_module_initialized"
        "-Wl,--export-dynamic-symbol=framekeyboard_test_swipe_module_unloaded")
    set_target_properties(swipe-ownership-tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-swipe-memory")
    add_dependencies(swipe-ownership-tests fake_swipe_encoder)
    add_test(NAME swipe-ownership COMMAND swipe-ownership-tests)
    # Screenshots of the interface in one language, for review (not a test).
    add_executable(interface-tour tests/interface_tour.cpp)
    target_link_libraries(interface-tour PRIVATE keyboard_core)
    add_executable(settings-tests tests/settings_tests.cpp)
    target_link_libraries(settings-tests PRIVATE keyboard_core)
    target_compile_options(settings-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME settings COMMAND settings-tests)
    if(PIPEWIRE_FOUND)
        add_library(fake_pipewire SHARED tests/fake_pipewire.cpp)
        target_include_directories(fake_pipewire SYSTEM PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
        target_link_libraries(fake_pipewire PRIVATE Threads::Threads)
        target_compile_options(fake_pipewire PRIVATE -Wall -Wextra -Wpedantic)
        if(CMAKE_CROSSCOMPILING)
            target_link_options(fake_pipewire PRIVATE -fuse-ld=lld)
        endif()
        set_target_properties(fake_pipewire PROPERTIES OUTPUT_NAME pipewire-0.3 SUFFIX .so.0
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-libs")
        add_executable(key-sound-player-tests tests/key_sound_player_tests.cpp)
        target_link_libraries(key-sound-player-tests PRIVATE keyboard_core)
        target_compile_options(key-sound-player-tests PRIVATE -Wall -Wextra -Wpedantic)
        add_dependencies(key-sound-player-tests fake_pipewire)
        add_test(NAME key-sound-player COMMAND key-sound-player-tests)
        set_tests_properties(key-sound-player PROPERTIES
            ENVIRONMENT "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:fake_pipewire>" TIMEOUT 45)
    endif()
    add_executable(keyboard-tests tests/keyboard_tests.cpp)
    add_executable(render-tests tests/render_tests.cpp)
    target_link_libraries(render-tests PRIVATE keyboard_core)
    target_compile_options(render-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rendering COMMAND render-tests)
    target_link_libraries(keyboard-tests PRIVATE keyboard_core)
    target_compile_options(keyboard-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME keyboard-core COMMAND keyboard-tests)
    add_executable(placement-instance-tests tests/placement_instance_tests.cpp)
    target_link_libraries(placement-instance-tests PRIVATE keyboard_core Threads::Threads)
    target_compile_options(placement-instance-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME placement-instance COMMAND placement-instance-tests)
    add_executable(shell-tests tests/shell_tests.cpp)
    target_link_libraries(shell-tests PRIVATE keyboard_core)
    target_compile_options(shell-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME shell COMMAND shell-tests)
    add_executable(desktop-focus-tests tests/desktop_focus_tests.cpp)
    target_link_libraries(desktop-focus-tests PRIVATE keyboard_core)
    add_test(NAME desktop-focus COMMAND desktop-focus-tests)
    add_executable(terminal-keys-tests tests/terminal_keys_tests.cpp)
    target_link_libraries(terminal-keys-tests PRIVATE keyboard_core)
    target_compile_options(terminal-keys-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME terminal-keys COMMAND terminal-keys-tests)
    add_executable(emoji-tests tests/emoji_tests.cpp)
    target_link_libraries(emoji-tests PRIVATE keyboard_core)
    target_compile_options(emoji-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME emoji COMMAND emoji-tests)
    if(NOT CMAKE_CROSSCOMPILING)
        if(FRAMEKEYBOARD_VR)
            add_test(NAME installer COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer_tests.py"
                "${CMAKE_CURRENT_SOURCE_DIR}" "$<TARGET_FILE:framekeyboard>")
            add_test(NAME hosted-launch COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted_launch_tests.py" "$<TARGET_FILE:framekeyboard>")
        endif()
        add_test(NAME uninstaller COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/uninstall_tests.py" "${CMAKE_CURRENT_SOURCE_DIR}")
        pkg_check_modules(EIS QUIET IMPORTED_TARGET libeis-1.0>=1.6)
        if(EIS_FOUND)
            add_executable(ei-recovery-tests tests/ei_recovery_tests.cpp)
            target_link_libraries(ei-recovery-tests PRIVATE keyboard_core PkgConfig::EIS Threads::Threads)
            pkg_check_modules(WAYLAND_SERVER QUIET IMPORTED_TARGET wayland-server)
            if(WAYLAND_SERVER_FOUND)
                set(IME_SERVER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/gamescope-input-method-server.h")
                add_custom_command(OUTPUT "${IME_SERVER_HEADER}"
                    COMMAND "${WAYLAND_SCANNER}" server-header "${IME_XML}" "${IME_SERVER_HEADER}"
                    DEPENDS "${IME_XML}" VERBATIM)
                target_sources(ei-recovery-tests PRIVATE "${IME_SERVER_HEADER}")
                target_include_directories(ei-recovery-tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
                target_link_libraries(ei-recovery-tests PRIVATE PkgConfig::WAYLAND_SERVER)
                target_compile_definitions(ei-recovery-tests PRIVATE FRAMEKEYBOARD_TEST_WAYLAND_SERVER=1)
            endif()
            add_test(NAME ei-recovery COMMAND ei-recovery-tests)
        endif()
        pkg_check_modules(WAYLAND_SERVER QUIET IMPORTED_TARGET wayland-server)
        if(WAYLAND_SERVER_FOUND)
            set(DATA_CONTROL_SERVER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/wlr-data-control-server.h")
            add_custom_command(OUTPUT "${DATA_CONTROL_SERVER_HEADER}"
                COMMAND "${WAYLAND_SCANNER}" server-header "${DATA_CONTROL_XML}" "${DATA_CONTROL_SERVER_HEADER}"
                DEPENDS "${DATA_CONTROL_XML}" VERBATIM)
            set(EXT_DATA_CONTROL_SERVER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/ext-data-control-server.h")
            add_custom_command(OUTPUT "${EXT_DATA_CONTROL_SERVER_HEADER}"
                COMMAND "${WAYLAND_SCANNER}" server-header "${EXT_DATA_CONTROL_XML}" "${EXT_DATA_CONTROL_SERVER_HEADER}"
                DEPENDS "${EXT_DATA_CONTROL_XML}" VERBATIM)
            add_executable(clipboard-tests tests/clipboard_tests.cpp "${DATA_CONTROL_SERVER_HEADER}"
                "${EXT_DATA_CONTROL_SERVER_HEADER}")
            target_include_directories(clipboard-tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
            target_link_libraries(clipboard-tests PRIVATE keyboard_core PkgConfig::WAYLAND_SERVER)
            add_test(NAME clipboard COMMAND clipboard-tests)
        endif()
    endif()
    if(NOT CMAKE_CROSSCOMPILING)
        add_executable(simulator-tests tests/simulator_tests.cpp)
        target_link_libraries(simulator-tests PRIVATE simulator_core)
        add_dependencies(simulator-tests framekeyboard-simulator)
        target_compile_options(simulator-tests PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME simulator COMMAND simulator-tests)
        # Renders the simulator's window contents in typed states for review; not a test.
        add_executable(simulator-tour tests/simulator_tour.cpp src/simulator/platform_linux.cpp)
        target_link_libraries(simulator-tour PRIVATE simulator_core)
    endif()
    # Opt-in only: maps a disposable window and verifies focus before each key.
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
    add_executable(unicode-target-probe tests/unicode_target_probe.cpp)
    target_link_libraries(unicode-target-probe PRIVATE keyboard_core PkgConfig::X11)
    # Renders the Japanese keyboard's states for review; not a test.
    add_executable(japanese-tour tests/japanese_tour.cpp)
    target_link_libraries(japanese-tour PRIVATE keyboard_core)
    add_executable(japanese-target-probe tests/japanese_target_probe.cpp)
    target_link_libraries(japanese-target-probe PRIVATE keyboard_core PkgConfig::X11)
    add_executable(ei-target-probe tests/ei_target_probe.cpp)
    target_link_libraries(ei-target-probe PRIVATE keyboard_core PkgConfig::X11)
    # Opt-in only: drives commits and replacements for a disposable receiver
    # that the caller has focused and verifies before every command.
    add_executable(text-replace-probe tests/text_replace_probe.cpp)
    target_link_libraries(text-replace-probe PRIVATE keyboard_core)
    # Compares the Chinese engines' answers, time and memory where the
    # simulator isn't built, as on the Frame; sends nothing.
    add_executable(chinese-candidates-probe tests/chinese_candidates_probe.cpp)
    target_link_libraries(chinese-candidates-probe PRIVATE keyboard_core)
    if(FRAMEKEYBOARD_VR)
        # Opt-in read-only validation against the installed Frame controller models.
        add_executable(stick-component-probe tests/stick_component_probe.cpp)
        target_link_libraries(stick-component-probe PRIVATE keyboard_core overlay_texture)
        set_target_properties(stick-component-probe PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_RUNTIME_PATH}")
        add_executable(vr-panel-probe tests/vr_panel_probe.cpp)
        target_link_libraries(vr-panel-probe PRIVATE keyboard_core overlay_texture)
        set_target_properties(vr-panel-probe PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_RUNTIME_PATH}")
        # Opt-in: sends physical haptic output, so it must not run in CTest.
        add_executable(haptics-probe tests/haptics_probe.cpp)
        target_link_libraries(haptics-probe PRIVATE keyboard_core overlay_texture)
        set_target_properties(haptics-probe PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_RUNTIME_PATH}")
    endif()
    # Intentionally excluded from CTest: requires an accessible kernel uinput device.
    add_executable(uinput-smoke-test tests/uinput_smoke.cpp)
    target_link_libraries(uinput-smoke-test PRIVATE keyboard_core)
endif()
if(FRAMEKEYBOARD_VR)
    install(TARGETS framekeyboard framekeyboard-native-guard RUNTIME DESTINATION bin)
endif()
install(DIRECTORY layouts languages themes config vr DESTINATION share/framekeyboard)
install(DIRECTORY swipe/lexicons DESTINATION share/framekeyboard/swipe)
install(FILES japanese/words.tsv japanese/kaomoji.tsv japanese/preferred.tsv japanese/mozc-words.tsv
        japanese/README.md
        DESTINATION share/framekeyboard/japanese)
# Cangjie and Quick are built in, so their rank list and its notices always ship.
install(FILES chinese/char-ranks.txt chinese/README.md DESTINATION share/framekeyboard/chinese)
install(DIRECTORY third_party/chinese-ranks/ DESTINATION third_party/chinese-ranks)
install(FILES bridge/native_bridge.py bridge/steam_bridge.js DESTINATION share/framekeyboard/bridge)
install(FILES update/update_helper.py DESTINATION share/framekeyboard/update)
install(FILES dictation/dictation_helper.py DESTINATION share/framekeyboard/dictation)
install(FILES third_party/README.md DESTINATION third_party)
install(FILES third_party/openvr/LICENSE DESTINATION third_party/openvr)
install(FILES third_party/overlay_texture/LICENSE DESTINATION third_party/overlay_texture)
install(FILES third_party/libei/COPYING third_party/libei/README.md third_party/libei/provenance.json DESTINATION third_party/libei)

install(FILES third_party/gamescope/gamescope-input-method.xml DESTINATION third_party/gamescope)
install(FILES third_party/wlr-protocols/wlr-data-control-unstable-v1.xml DESTINATION third_party/wlr-protocols)
install(FILES third_party/wayland-protocols/ext-data-control-v1.xml DESTINATION third_party/wayland-protocols)
install(FILES third_party/unicode/LICENSE DESTINATION third_party/unicode)
install(FILES third_party/mozc/LICENSE DESTINATION third_party/mozc)
if(FRAMEKEYBOARD_SWIPE_ENGINE)
    install(FILES third_party/futo-swipe/LICENSE.md third_party/futo-swipe/README.md
            DESTINATION third_party/futo-swipe)
    install(DIRECTORY third_party/executorch DESTINATION third_party)
endif()
if(FRAMEKEYBOARD_WHISPER_DIR)
    install(FILES third_party/whisper.cpp/LICENSE DESTINATION third_party/whisper.cpp)
endif()

# A package leaves out docs/ and CONTRIBUTING.md, which nothing installed
# reads. Its README and changelog link to them online instead, at the
# release's tag, in the repository the updater uses (update/update_helper.py).
set(FRAMEKEYBOARD_REPOSITORY "TaiKeid/steam-frame-full-keyboard")
set(packaged_docs "https://github.com/${FRAMEKEYBOARD_REPOSITORY}/blob/${PROJECT_VERSION}")
set(packaged_images "https://raw.githubusercontent.com/${FRAMEKEYBOARD_REPOSITORY}/${PROJECT_VERSION}")
# Every README goes in, as each one's language links name the others.
file(GLOB packaged_readmes CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} README.*.md)
foreach(document README.md ${packaged_readmes} CHANGELOG.md)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${document})
    file(READ ${document} text)
    # Images first, by their raw files, so the links after them keep theirs.
    string(REPLACE "](docs/images/" "](${packaged_images}/docs/images/" text "${text}")
    string(REPLACE "src=\"docs/images/" "src=\"${packaged_images}/docs/images/" text "${text}")
    string(REPLACE "](docs/" "](${packaged_docs}/docs/" text "${text}")
    string(REPLACE "](CONTRIBUTING.md" "](${packaged_docs}/CONTRIBUTING.md" text "${text}")
    file(WRITE ${CMAKE_BINARY_DIR}/packaged/${document} "${text}")
    install(FILES ${CMAKE_BINARY_DIR}/packaged/${document} DESTINATION .)
endforeach()
install(FILES LICENSE DESTINATION .)
