cmake_minimum_required(VERSION 3.20)
project(audio_visualizer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)

add_executable(audio-visualizer
    src/main.c
    src/audio/pcm_ring.c
    src/audio/audio_source.c
    src/audio/source_test_tone.c
    src/audio/source_file_miniaudio.c
    src/audio/source_mic_miniaudio.c
    src/audio/miniaudio_impl.c
    src/analyzer/window_fn.c
    src/analyzer/analyzer.c
    src/render/pixel_buffer.c
    src/render/color.c
    src/render/renderer.c
    src/ui/text.c
    src/ui/controls.c
    src/ui/overlay.c
)

target_include_directories(audio-visualizer PRIVATE
    src
    third_party
)

target_link_libraries(audio-visualizer PRIVATE
    PkgConfig::SDL3
    PkgConfig::FFTW3F
    m
)

if(NOT APPLE)
    # System-audio source for Linux (PulseAudio monitor) and Windows
    # (WASAPI loopback). macOS uses its own Core Audio process-tap path.
    target_sources(audio-visualizer PRIVATE
        src/audio/source_system_miniaudio.c
    )
endif()

if(UNIX AND NOT APPLE)
    # miniaudio's Linux backends (ALSA/PulseAudio) load their client libraries
    # at runtime via dlopen and use pthreads internally.
    find_package(Threads REQUIRED)
    target_link_libraries(audio-visualizer PRIVATE
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
endif()

if(APPLE)
    enable_language(OBJC)
    target_sources(audio-visualizer PRIVATE
        src/audio/platform/source_system_macos_tap.m
    )
    # ARC keeps the Obj-C bridge readable (NSDictionary literals etc.) without
    # manual retain/release.
    set_source_files_properties(src/audio/platform/source_system_macos_tap.m PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
    target_link_libraries(audio-visualizer PRIVATE
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework AudioToolbox"
        "-framework Foundation"
    )

    # Core Audio process taps require the binary to have a TCC identity:
    # an embedded Info.plist (with CFBundleIdentifier + NSAudioCaptureUsageDescription)
    # and a code signature. Without these, the OS denies the tap with
    # kAudioHardwarePermissionsError no matter what the parent terminal has
    # been granted, and never surfaces a permission prompt.
    set(AUDIO_VISUALIZER_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist")
    target_link_options(audio-visualizer PRIVATE
        "LINKER:-sectcreate,__TEXT,__info_plist,${AUDIO_VISUALIZER_INFO_PLIST}"
    )
    set_source_files_properties("${AUDIO_VISUALIZER_INFO_PLIST}" PROPERTIES
        HEADER_FILE_ONLY TRUE
    )
    # Re-link whenever the plist changes.
    set_property(TARGET audio-visualizer APPEND PROPERTY LINK_DEPENDS
        "${AUDIO_VISUALIZER_INFO_PLIST}"
    )
    # Ad-hoc sign so the binary has a stable code signature TCC can match.
    add_custom_command(TARGET audio-visualizer POST_BUILD
        COMMAND codesign --force --sign - --identifier com.audio-visualizer.cli
                "$<TARGET_FILE:audio-visualizer>"
        VERBATIM
    )
endif()

target_compile_options(audio-visualizer PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
)

# miniaudio is a vendored single-header with its own style; silence warnings
# only for its implementation translation unit.
set_source_files_properties(src/audio/miniaudio_impl.c PROPERTIES
    COMPILE_FLAGS "-w"
)

# stb_truetype is similarly a vendored single-header library that triggers
# many warnings under our flags; silence them only for its translation unit.
set_source_files_properties(src/ui/text.c PROPERTIES
    COMPILE_FLAGS "-w"
)
