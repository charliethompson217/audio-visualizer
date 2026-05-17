# audiovisualizer

A small native C application that visualizes real-time audio as a log-frequency
(musical semitone) bar spectrum and waveform. Audio input is modular: the core
app doesn't care whether samples come from a file, microphone, system output,
or a generated test tone.

## Audio sources

| Flag          | Source              | macOS | Linux | Windows |
| ------------- | ------------------- | :---: | :---: | :-----: |
| `--test-tone` | Generated sine      |  ✅   |  ✅   |   ✅    |
| `--file PATH` | Audio file playback |  ✅   |  ✅   |   ✅    |
| `--mic`       | Microphone capture  |  ✅   |  ✅   |   ✅    |
| `--system`    | System output mix   |  ✅   |  ✅   |   ✅    |

System-audio capture is platform-specific:

- macOS uses a Core Audio process tap (requires macOS 14.2+).
- Linux captures from the default sink's PulseAudio monitor source. Works
  against either PulseAudio or PipeWire's pulse compatibility layer
  (`pipewire-pulse`).
- Windows uses WASAPI loopback on the default render endpoint.

## Dependencies

- CMake ≥ 3.20 and a C11 compiler (clang, gcc, or MSVC)
- [SDL3](https://wiki.libsdl.org/SDL3/) — window, renderer, input
- [FFTW3](https://www.fftw.org/) (single-precision, `fftw3f`) — FFT
- `pkg-config` — used by CMake to locate the above
- Vendored single-header libs (no install needed): `miniaudio`, `stb_truetype`

### macOS

```sh
brew install cmake pkg-config sdl3 fftw
```

On Apple Silicon, if `pkg-config` can't see the Homebrew packages:

```sh
export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig:$PKG_CONFIG_PATH"
```

### Linux

Arch:

```sh
sudo pacman -S cmake pkgconf sdl3 fftw
```

Debian/Ubuntu: SDL3 may not yet be in the default repositories; build it from
source from <https://github.com/libsdl-org/SDL> if no `libsdl3-dev` package
is available. FFTW3F is provided by `libfftw3-dev`.

### Windows

Either MSYS2 (UCRT64 or MinGW64) or MSVC + vcpkg works.

MSYS2 (UCRT64 shell):

```sh
pacman -S mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-pkgconf \
          mingw-w64-ucrt-x86_64-sdl3 \
          mingw-w64-ucrt-x86_64-fftw
```

vcpkg:

```sh
vcpkg install sdl3 fftw3
# then point CMake at the vcpkg toolchain when configuring:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
```

The WASAPI loopback path miniaudio uses links only against system libraries
that are already pulled in by the Windows SDK, so no extra link flags are
needed.

## Build

```sh
cmake -S . -B build
cmake --build build -j
```

Build outputs:

- **macOS**: `build/audiovisualizer.app` — a self-contained `.app` bundle.
  SDL3 is embedded in `Contents/Frameworks/`; no Homebrew installation is
  required on the target machine. The bundle is ad-hoc signed with App
  Sandbox, Hardened Runtime, and the audio-input entitlement — all handled
  automatically by CMake. Double-click it in Finder, drag it to
  `/Applications`, or pin it to the Dock. The executable inside is
  `build/audiovisualizer.app/Contents/MacOS/audiovisualizer` if you want to
  run with CLI flags.
- **Windows**: `build/audiovisualizer.exe` — a GUI-subsystem executable (no
  console window opens) with the icon embedded as a resource.
- **Linux**: `build/audiovisualizer` — a plain ELF binary. Packaging into
  `.desktop` entries / Flatpak / AppImage / distro packages is left to the
  user.

## OS permissions

The app needs the OS to allow it to read certain audio streams. The first time
you use `--mic` or `--system` you will be prompted by the OS; if you deny the
prompt you must re-grant access in system settings.

### macOS

- **Microphone** (`--mic`): grant in *System Settings → Privacy & Security →
  Microphone* (the terminal application running the binary appears in the list).
- **System audio** (`--system`): uses a Core Audio process tap, which requires
  macOS 14.2+ and grant in *System Settings → Privacy & Security → Audio
  Capture*. The `.app` bundle ships an `Info.plist` (with `CFBundleIdentifier`,
  `NSAudioCaptureUsageDescription`, `NSMicrophoneUsageDescription`) and is
  ad-hoc code-signed so TCC can identify it; without this the OS denies the
  tap silently. This is handled automatically by CMake — no manual signing
  required.

### Linux

- **Microphone**: handled by the desktop's audio server (PipeWire / PulseAudio
  portal). On most distros, the first capture triggers a portal prompt.
- **System audio** (`--system`): captures from the default sink's monitor
  source over PulseAudio (or `pipewire-pulse`). No special permission prompt
  on most setups — monitor sources are world-readable by default. If nothing
  appears, ensure `pactl info` reports a `Default Sink` and that a `.monitor`
  source exists in `pactl list short sources`.

### Windows

- **Microphone** (`--mic`): grant in *Settings → Privacy & security →
  Microphone* (enable "Let desktop apps access your microphone").
- **System audio** (`--system`): uses WASAPI loopback on the default output
  device — no permission prompt required. Whatever is currently playing
  through the default speakers/headphones is captured.

## Run

From the build directory (Linux, or to pass CLI flags on any platform):

```sh
./build/audiovisualizer --test-tone
./build/audiovisualizer --file path/to/song.wav
./build/audiovisualizer --mic
./build/audiovisualizer --system
```

On macOS you can also just launch the `.app` (defaults to `--system`):

```sh
open build/audiovisualizer.app
# or, to pass flags:
build/audiovisualizer.app/Contents/MacOS/audiovisualizer --mic
```

On Windows, double-click `build\audiovisualizer.exe` or invoke it from a
shell with flags.

## Runtime controls

- `Tab` — toggle the settings panel (FFT size, dB range, smoothing,
  semitone range, brightness/length power, waveform toggles, etc.)
- `Esc` — close an open dropdown or the settings panel (does not quit)

## License

This project is licensed under the [GNU GENERAL PUBLIC LICENSE](LICENSE).