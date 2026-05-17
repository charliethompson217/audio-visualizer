#!/usr/bin/env bash
# Smoke-test for the C.2 persistence layer. Seeds a known settings.conf,
# launches the app briefly with SIGINT to trigger graceful exit (so
# config_save runs), then verifies CLI overrides and fallback behavior.
set -u
SETTINGS="$HOME/Library/Application Support/audiovisualizer/settings.conf"
BIN="build/audiovisualizer.app/Contents/MacOS/audiovisualizer"

run_app() {
  local args="$1" logfile="$2"
  # shellcheck disable=SC2086
  $BIN $args >"$logfile" 2>&1 &
  local pid=$!
  sleep 2
  kill -INT "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  echo "[run] pid=$pid args='$args' exit=$?"
}

echo "=== TEST 1: seed kind=2 (mic), width=950, brightness=1.7 ==="
cat >"$SETTINGS" <<EOF
version=1
source.kind=2
source.file_path=
window.width=950
window.height=600
visualizer.brightness_power=1.7000
visualizer.length_power=1.0000
visualizer.min_semitone=12.0000
visualizer.max_semitone=108.0000
visualizer.show_labels=1
visualizer.show_cursor=0
visualizer.show_spectrum=1
visualizer.show_waveform=0
visualizer.waveform_gain=1.0000
visualizer.waveform_samples_per_px=1.0000
visualizer.f0=8.175800
visualizer.note_hues=0.00,25.00,45.00,75.00,110.00,166.00,190.00,210.00,240.00,270.00,300.00,330.00
analyzer.fft_size=32768
analyzer.min_db=-70.00
analyzer.max_db=-12.00
analyzer.smoothing=0.8000
analyzer.window_function=1
EOF
run_app "" /tmp/av_test1.log
head -3 /tmp/av_test1.log
echo "--- settings after run ---"
grep -E "^(source.kind|window.width|window.height|visualizer.brightness_power|visualizer.show_labels)=" "$SETTINGS"

echo ""
echo "=== TEST 2: --test-tone overrides saved kind=2 ==="
run_app "--test-tone" /tmp/av_test2.log
head -1 /tmp/av_test2.log
grep -E "^source\.kind=" "$SETTINGS"

echo ""
echo "=== TEST 3: no args, should restore kind=0 from prev save ==="
run_app "" /tmp/av_test3.log
head -1 /tmp/av_test3.log

echo ""
echo "=== TEST 4: kind=1 (file) with empty path -> fall back to system ==="
sed -i '' 's/^source.kind=.*/source.kind=1/' "$SETTINGS"
run_app "" /tmp/av_test4.log
head -2 /tmp/av_test4.log
