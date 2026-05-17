#!/usr/bin/env bash
# release_github_macos.sh — Build, sign, notarize, and package Audio Visualizer
# for distribution via GitHub Releases (Developer ID, not App Store).
#
# Prerequisites
# ─────────────
# 1. Xcode Command Line Tools installed.
#
# 2. A "Developer ID Application: ..." certificate installed in your keychain.
#    Obtain one at: https://developer.apple.com/account/resources/certificates/list
#
# 3. A notarytool keychain profile stored once with:
#      xcrun notarytool store-credentials "AC_PROFILE" \
#        --apple-id you@example.com \
#        --team-id YOURTEAMID \
#        --password <app-specific-password>
#    Create an app-specific password at: https://appleid.apple.com/account/manage
#
# Usage
# ─────
#   export DEVELOPER_ID="Developer ID Application: Your Name (TEAMID)"
#   export NOTARYTOOL_PROFILE="AC_PROFILE"
#   ./scripts/release_github_macos.sh
#
# Optional overrides
# ──────────────────
#   APP_VERSION   — defaults to CFBundleShortVersionString from Info.plist
#   DIST_DIR      — defaults to dist/ in the repo root (created if absent)
#   BUILD_DIR     — defaults to build/ in the repo root
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Config ────────────────────────────────────────────────────────────────────
BUNDLE_ID="org.audiovisualizer.app"
APP_NAME="audiovisualizer"          # must match CFBundleExecutable / cmake target
DISPLAY_NAME="Audio Visualizer"
INFO_PLIST="$REPO_ROOT/macos/Info.plist"
ENTITLEMENTS="$REPO_ROOT/macos/audiovisualizer-distribution.entitlements"

DEVELOPER_ID="${DEVELOPER_ID:-}"
NOTARYTOOL_PROFILE="${NOTARYTOOL_PROFILE:-}"
APP_VERSION="${APP_VERSION:-$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$INFO_PLIST")}"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"

# ── Validate inputs ───────────────────────────────────────────────────────────
if [[ -z "$DEVELOPER_ID" ]]; then
    echo "❌  DEVELOPER_ID is not set." >&2
    echo "    export DEVELOPER_ID=\"Developer ID Application: Your Name (TEAMID)\"" >&2
    exit 1
fi

if [[ -z "$NOTARYTOOL_PROFILE" ]]; then
    echo "❌  NOTARYTOOL_PROFILE is not set." >&2
    echo "    Store credentials first:" >&2
    echo "      xcrun notarytool store-credentials \"AC_PROFILE\" \\" >&2
    echo "        --apple-id you@example.com --team-id TEAMID --password <app-specific-pw>" >&2
    echo "    Then: export NOTARYTOOL_PROFILE=\"AC_PROFILE\"" >&2
    exit 1
fi

APP_BUNDLE="$BUILD_DIR/$APP_NAME.app"

echo "▶  Audio Visualizer $APP_VERSION — GitHub release build"
echo "   Identity  : $DEVELOPER_ID"
echo "   Profile   : $NOTARYTOOL_PROFILE"
echo "   Output    : $DIST_DIR"
echo

# ── Step 1/6 · Build ──────────────────────────────────────────────────────────
echo "── Step 1/6: Build ──────────────────────────────────────────────────────"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --config Release
echo "   ✅ Build OK"
echo

# ── Step 2/6 · Strip quarantine attribute ────────────────────────────────────
echo "── Step 2/6: Strip quarantine attribute ─────────────────────────────────"
# macOS tags downloaded files with com.apple.quarantine; strip it before signing
# so the attribute is never present inside the signed bundle.
xattr -cr "$APP_BUNDLE"
echo "   ✅ Quarantine attribute removed"
echo

# ── Step 3/6 · Sign with Developer ID ────────────────────────────────────────
echo "── Step 3/6: Code-sign (Developer ID) ──────────────────────────────────"

# Sign bundled dylibs first, then the app shell.  Order matters: the outer
# bundle signature covers nested code, so inner components must be signed first.
FRAMEWORKS="$APP_BUNDLE/Contents/Frameworks"
if [[ -d "$FRAMEWORKS" ]]; then
    for dylib in "$FRAMEWORKS"/*.dylib; do
        [[ -f "$dylib" ]] || continue
        codesign --force --options runtime \
            --entitlements "$ENTITLEMENTS" \
            --sign "$DEVELOPER_ID" \
            "$dylib"
        echo "   signed: $(basename "$dylib")"
    done
fi

codesign --force --options runtime \
    --entitlements "$ENTITLEMENTS" \
    --sign "$DEVELOPER_ID" \
    --identifier "$BUNDLE_ID" \
    "$APP_BUNDLE"
echo "   signed: $APP_NAME.app"

# Verify the signature only — spctl (Gatekeeper) check runs after stapling.
codesign --verify --deep --strict --verbose=2 "$APP_BUNDLE"
echo "   ✅ Signature OK"
echo

# ── Step 4/6 · Notarize ───────────────────────────────────────────────────────
echo "── Step 4/6: Notarize ───────────────────────────────────────────────────"
NOTARIZE_ZIP="$BUILD_DIR/${APP_NAME}-${APP_VERSION}-notarize.zip"
ditto -c -k --keepParent "$APP_BUNDLE" "$NOTARIZE_ZIP"
xcrun notarytool submit "$NOTARIZE_ZIP" \
    --keychain-profile "$NOTARYTOOL_PROFILE" \
    --wait
rm -f "$NOTARIZE_ZIP"
echo "   ✅ Notarization OK"
echo

# ── Step 4/6 · Staple ─────────────────────────────────────────────────────────
echo "── Step 4/6: Staple ─────────────────────────────────────────────────────"
xcrun stapler staple "$APP_BUNDLE"
xcrun stapler validate "$APP_BUNDLE"
# Now that the notarization ticket is stapled, Gatekeeper should accept it.
spctl --assess --type exec --verbose "$APP_BUNDLE"
echo "   ✅ Staple OK"
echo

# ── Step 5/6 · Package as DMG ─────────────────────────────────────────────────
echo "── Step 5/6: Create DMG ─────────────────────────────────────────────────"
mkdir -p "$DIST_DIR"
DMG_NAME="${DISPLAY_NAME// /_}-${APP_VERSION}-macOS"
STAGE_DIR="$BUILD_DIR/dmg_stage"
DMG_FINAL="$DIST_DIR/${DMG_NAME}.dmg"

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"
cp -R "$APP_BUNDLE" "$STAGE_DIR/"
ln -s /Applications "$STAGE_DIR/Applications"

hdiutil create \
    -volname "$DISPLAY_NAME" \
    -srcfolder "$STAGE_DIR" \
    -ov \
    -format UDZO \
    "$DMG_FINAL"

rm -rf "$STAGE_DIR"
echo "   Created: $DMG_FINAL"
echo

# ── Step 6/6 · Sign, notarize, and staple the DMG ────────────────────────────
echo "── Step 6/6: Sign & notarize DMG ────────────────────────────────────────"
codesign --force --sign "$DEVELOPER_ID" "$DMG_FINAL"

xcrun notarytool submit "$DMG_FINAL" \
    --keychain-profile "$NOTARYTOOL_PROFILE" \
    --wait

xcrun stapler staple "$DMG_FINAL"
echo "   ✅ DMG signed, notarized, and stapled"
echo

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  ✅  Release artifact ready for GitHub:"
echo "      $DMG_FINAL"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
