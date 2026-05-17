cmake_minimum_required(VERSION 3.20)
project(audio_visualizer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Enable link-time optimization for Release. Per-target so Debug builds
# stay fast to iterate. check_ipo_supported() will fail clean on toolchains
# that don't have a working linker plugin (e.g. some Linux distros).
include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_DIAG LANGUAGES C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)

# On macOS we want a self-contained .app with no Homebrew runtime
# dependencies, so link libfftw3f as a static archive. Linux/Windows
# builds remain dynamic via pkg-config -- they're build-from-source
# targets per the roadmap, not distribution artifacts.
if(APPLE)
    pkg_get_variable(FFTW3F_LIBDIR fftw3f libdir)
    pkg_get_variable(FFTW3F_INCDIR fftw3f includedir)
    set(FFTW3F_STATIC_ARCHIVE "${FFTW3F_LIBDIR}/libfftw3f.a")
    if(NOT EXISTS "${FFTW3F_STATIC_ARCHIVE}")
        message(FATAL_ERROR
            "FFTW3F static archive not found at ${FFTW3F_STATIC_ARCHIVE}. "
            "On Homebrew this ships with the fftw formula by default; "
            "try `brew reinstall fftw`.")
    endif()
    add_library(fftw3f_static STATIC IMPORTED)
    set_target_properties(fftw3f_static PROPERTIES
        IMPORTED_LOCATION "${FFTW3F_STATIC_ARCHIVE}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFTW3F_INCDIR}"
    )
else()
    pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
endif()

add_executable(audiovisualizer
    src/main.c
    src/config.c
    src/platform/paths.c
    src/audio/pcm_ring.c
    src/audio/audio_source.c
    src/audio/source_demo.c
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
    m
)

if(APPLE)
    target_link_libraries(audiovisualizer PRIVATE fftw3f_static)
else()
    target_link_libraries(audiovisualizer PRIVATE PkgConfig::FFTW3F)
endif()

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

    # Bundle the public-domain demo song into Contents/Resources/ so the
    # demo source can locate it via SDL_GetBasePath(). See LICENSES/ for
    # the CC Public Domain Mark attribution.
    set(AUDIO_VISUALIZER_DEMO_SONG "${CMAKE_CURRENT_SOURCE_DIR}/demo-song.mp3")
    target_sources(audiovisualizer PRIVATE "${AUDIO_VISUALIZER_DEMO_SONG}")
    set_source_files_properties("${AUDIO_VISUALIZER_DEMO_SONG}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
        HEADER_FILE_ONLY TRUE
    )

    set_target_properties(audiovisualizer PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${AUDIO_VISUALIZER_INFO_PLIST}"
        # Resolve @rpath/* dylib references against the bundled
        # Frameworks dir so the .app runs without Homebrew installed.
        BUILD_RPATH "@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks"
    )

    # Keep debug info in Release so dsymutil has something to extract.
    # -g + -O3 is the standard Apple workflow: full optimization with
    # symbols recoverable via the .dSYM bundle after strip.
    target_compile_options(audiovisualizer PRIVATE
        $<$<CONFIG:Release>:-g>
    )

    # Extract a .dSYM bundle next to the .app (Apple convention is to
    # ship it as a sibling, not embedded in the bundle) and then, in
    # Release only, strip local symbols from the binary. Both run on
    # the just-linked, not-yet-relocated executable -- before
    # bundle_macos.sh or codesign, so the final signature covers
    # the stripped binary.
    add_custom_command(TARGET audiovisualizer POST_BUILD
        COMMAND dsymutil "$<TARGET_FILE:audiovisualizer>"
                -o "$<TARGET_BUNDLE_DIR:audiovisualizer>.dSYM"
        VERBATIM
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_custom_command(TARGET audiovisualizer POST_BUILD
            COMMAND strip -x "$<TARGET_FILE:audiovisualizer>"
            VERBATIM
        )
    endif()

    # Relocate any non-system dylib (currently just libSDL3.0.dylib --
    # FFTW3F is statically linked, miniaudio is single-header) into
    # Contents/Frameworks/ and rewrite the binary's references to
    # @rpath. Must run before codesign so the bundled copies get a
    # matching ad-hoc signature.
    add_custom_command(TARGET audiovisualizer POST_BUILD
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/bundle_macos.sh"
                "$<TARGET_BUNDLE_DIR:audiovisualizer>"
        VERBATIM
    )

    # Ad-hoc sign the whole .app with App Sandbox + Hardened Runtime
    # + the entitlements the distribution build needs (audio input,
    # user-selected file read, library-validation off because nested
    # ad-hoc code has no Team ID). --deep cascades the signature into
    # Contents/Frameworks/ so the bundled SDL3 dylib is signed in the
    # same pass. TCC matches on the stable --identifier.
    add_custom_command(TARGET audiovisualizer POST_BUILD
        COMMAND codesign --force --deep --options runtime
                --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/macos/audiovisualizer.entitlements"
                --sign - --identifier org.audiovisualizer.app
                "$<TARGET_BUNDLE_DIR:audiovisualizer>"
        VERBATIM
    )
endif()

# LTO for Release. Gated on toolchain support so a Linux build on a
# distro without LLVMgold.so / lld doesn't break.
if(IPO_SUPPORTED)
    set_property(TARGET audiovisualizer PROPERTY
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
else()
    message(STATUS "IPO/LTO unavailable, building without: ${IPO_DIAG}")
endif()

if(WIN32)
    # Use the Windows GUI subsystem so launching the .exe doesn't pop a
    # console window. SDL3 provides the WinMain -> main shim via SDL_main.h
    # (already included from src/main.c).
    set_target_properties(audiovisualizer PROPERTIES WIN32_EXECUTABLE TRUE)

    # Embed the application icon (used by Explorer, taskbar, Start Menu).
    target_sources(audiovisualizer PRIVATE windows/app.rc)
endif()

if(NOT APPLE)
    # Linux/Windows: drop the demo song next to the executable so
    # SDL_GetBasePath() (which returns the exe directory on these
    # platforms) resolves the demo source's path correctly.
    add_custom_command(TARGET audiovisualizer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/demo-song.mp3"
                "$<TARGET_FILE_DIR:audiovisualizer>/demo-song.mp3"
        VERBATIM
    )
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
