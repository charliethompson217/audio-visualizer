#!/usr/bin/env bash
# Re-sign the build/audiovisualizer.app bundle with App Sandbox + audio
# entitlements + hardened runtime (ad-hoc), so we can validate that the
# Core Audio process tap works inside the sandbox before committing to
# App Store packaging.
#
# Run after every `cmake --build build` until Phase E plumbs this into
# the build itself.
set -euo pipefail

cd "$(dirname "$0")/.."

BUNDLE="build/audiovisualizer.app"
ENT="macos/sandbox-test.entitlements"

if [[ ! -d "$BUNDLE" ]]; then
  echo "error: $BUNDLE not found — build first with: cmake --build build" >&2
  exit 1
fi

codesign --force --options runtime \
    --entitlements "$ENT" \
    --sign - \
    --identifier org.audiovisualizer.app \
    "$BUNDLE"

echo "re-signed $BUNDLE with sandbox-test entitlements"
codesign -d --entitlements :- "$BUNDLE" 2>/dev/null \
    | plutil -convert xml1 -o - - \
    | grep -E "<key>|<true|<false" | sed 's/^[[:space:]]*/  /'
