# audio-visualizer

A small native C application that visualizes real-time audio as a log-frequency
(musical semitone) bar spectrum and waveform. Audio input is modular: the core
app doesn't care whether samples come from a file, microphone, system output,
or a generated test tone.

## Audio sources

| Flag             | Source              | macOS | Linux            |
| ---------------- | ------------------- | :---: | :--------------: |
| `--test-tone`    | Generated sine      |  ✅   |        ✅        |
| `--file PATH`    | Audio file playback |  ✅   |        ✅        |
| `--mic`          | Microphone capture  |  ✅   |        ✅        |
| `--system`       | System output mix   |  ✅   | ⏳ not yet (TBD) |

Linux system-audio capture (PipeWire sink monitor) is planned but not yet
implemented.

## Dependencies

- CMake ≥ 3.20 and a C11 compiler (clang or gcc)
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

## Build

```sh
cmake -S . -B build
cmake --build build -j
```

The binary is produced at `build/audio-visualizer`.

## OS permissions

The app needs the OS to allow it to read certain audio streams. The first time
you use `--mic` or `--system` you will be prompted by the OS; if you deny the
prompt you must re-grant access in system settings.

### macOS

- **Microphone** (`--mic`): grant in *System Settings → Privacy & Security →
  Microphone* (the terminal application running the binary appears in the list).
- **System audio** (`--system`): uses a Core Audio process tap, which requires
  macOS 14.2+ and grant in *System Settings → Privacy & Security → Audio
  Capture*. The build embeds an `Info.plist` (with `CFBundleIdentifier`,
  `NSAudioCaptureUsageDescription`, `NSMicrophoneUsageDescription`) and ad-hoc
  code-signs the binary so TCC can identify it; without this the OS denies the
  tap silently. This is handled automatically by CMake — no manual signing
  required.

### Linux

- **Microphone**: handled by the desktop's audio server (PipeWire / PulseAudio
  portal). On most distros, the first capture triggers a portal prompt.

## Run

```sh
./build/audio-visualizer --test-tone
./build/audio-visualizer --file path/to/song.wav
./build/audio-visualizer --mic
./build/audio-visualizer --system        # macOS only for now
```

## Runtime controls

- `Tab` — toggle the settings panel (FFT size, dB range, smoothing,
  semitone range, brightness/length power, waveform toggles, etc.)
- `Esc` — quit (or close an open dropdown / the settings panel)

## License

This project is licensed under the [GNU GENERAL PUBLIC LICENSE](LICENSE).