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

add_executable(audiovisualizer
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

target_include_directories(audiovisualizer PRIVATE
    src
    third_party
)

target_link_libraries(audiovisualizer PRIVATE
    PkgConfig::SDL3
    PkgConfig::FFTW3F
    m
)

if(NOT APPLE)
    # System-audio source for Linux (PulseAudio monitor) and Windows
    # (WASAPI loopback). macOS uses its own Core Audio process-tap path.
    target_sources(audiovisualizer PRIVATE
        src/audio/source_system_miniaudio.c
    )
endif()

if(UNIX AND NOT APPLE)
    # miniaudio's Linux backends (ALSA/PulseAudio) load their client libraries
    # at runtime via dlopen and use pthreads internally.
    find_package(Threads REQUIRED)
    target_link_libraries(audiovisualizer PRIVATE
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
endif()

if(APPLE)
    enable_language(OBJC)
    target_sources(audiovisualizer PRIVATE
        src/audio/platform/source_system_macos_tap.m
    )
    # ARC keeps the Obj-C bridge readable (NSDictionary literals etc.) without
    # manual retain/release.
    set_source_files_properties(src/audio/platform/source_system_macos_tap.m PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
    target_link_libraries(audiovisualizer PRIVATE
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework AudioToolbox"
        "-framework Foundation"
    )

    # Build a proper .app bundle: Finder/Dock launch (no Terminal window),
    # icon in the Dock, and Info.plist at Contents/Info.plist where TCC reads
    # it. Core Audio process taps still need a stable code-signed identity
    # matching CFBundleIdentifier + NSAudioCaptureUsageDescription, but the
    # bundle layout makes the old `-sectcreate __TEXT,__info_plist` hack
    # unnecessary.
    set(AUDIO_VISUALIZER_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist")
    set(AUDIO_VISUALIZER_ICON_SRC "${CMAKE_CURRENT_SOURCE_DIR}/icon.png")
    set(AUDIO_VISUALIZER_ICNS "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.icns")

    # Generate AppIcon.icns from icon.png using macOS built-ins (sips +
    # iconutil). Re-runs only when icon.png changes.
    set(_iconset "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.iconset")
    add_custom_command(
        OUTPUT "${AUDIO_VISUALIZER_ICNS}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_iconset}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_iconset}"
        COMMAND sips -z 16   16   "${AUDIO_VISUALIZER_ICON_SRC}" --out "${_iconset}/icon_16x16.png"      > /dev/null
        COMMAND sips -z 32   32   "${AUDIO_VISUALIZER_ICON_SRC}" --out "${_iconset}/icon_16x16@2x.png"   > /dev/null
        COMMAND sips -z 32   32   "${AUDIO_VISUALIZER_ICON_SRC}" --out "${_iconset}/icon_32x32.png"      > /dev/null
        COMMAND sips -z 64   64   "${AUDIO_VISUALIZER_ICON_SRC}" --out "${_iconset}/icon_32x32@2x.png"   > /dev/null
        COMMAND sips -z 128  128  "${AUDIO_VISUALIZER_ICON_SRC}" --out "${_iconset}/icon_128x128.png"    > /dev/null
        COMMAND sips -z 256  256  "${AUDIO_VISUALIZER_ICON_SRC}" --out "${_iconset}/icon_128x128@2x.png" > /dev/null
        COMMAND sips -z 256  256  "${AUDIO_VISUALIZER_ICON_SRC}" --out "${_iconset}/icon_256x256.png"    > /dev/null
        COMMAND sips -z 512  512  "${AUDIO_VISUALIZER_ICON_SRC}" --out "${_iconset}/icon_256x256@2x.png" > /dev/null
        COMMAND sips -z 512  512  "${AUDIO_VISUALIZER_ICON_SRC}" --out "${_iconset}/icon_512x512.png"    > /dev/null
        COMMAND sips -z 1024 1024 "${AUDIO_VISUALIZER_ICON_SRC}" --out "${_iconset}/icon_512x512@2x.png" > /dev/null
        COMMAND iconutil --convert icns --output "${AUDIO_VISUALIZER_ICNS}" "${_iconset}"
        DEPENDS "${AUDIO_VISUALIZER_ICON_SRC}"
        VERBATIM
    )

    target_sources(audiovisualizer PRIVATE "${AUDIO_VISUALIZER_ICNS}")
    set_source_files_properties("${AUDIO_VISUALIZER_ICNS}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
        GENERATED TRUE
    )

    set_target_properties(audiovisualizer PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${AUDIO_VISUALIZER_INFO_PLIST}"
    )

    # Ad-hoc sign the whole .app so TCC can match it by a stable identity.
    add_custom_command(TARGET audiovisualizer POST_BUILD
        COMMAND codesign --force --deep --sign - --identifier org.audiovisualizer.app
                "$<TARGET_BUNDLE_DIR:audiovisualizer>"
        VERBATIM
    )
endif()

if(WIN32)
    # Use the Windows GUI subsystem so launching the .exe doesn't pop a
    # console window. SDL3 provides the WinMain -> main shim via SDL_main.h
    # (already included from src/main.c).
    set_target_properties(audiovisualizer PROPERTIES WIN32_EXECUTABLE TRUE)

    # Embed the application icon (used by Explorer, taskbar, Start Menu).
    target_sources(audiovisualizer PRIVATE windows/app.rc)
endif()

target_compile_options(audiovisualizer PRIVATE
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
