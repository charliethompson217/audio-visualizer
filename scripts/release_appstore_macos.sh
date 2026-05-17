#!/usr/bin/env bash
# release_appstore_macos.sh — Build, sign, package, and upload Audio Visualizer
# to the Mac App Store (Apple Distribution, not Developer ID).
#
# Prerequisites
# ─────────────
# 1. Xcode Command Line Tools installed.
#
# 2. Two certificates installed in your keychain
#    (generate both at developer.apple.com → Certificates):
#      • "Apple Distribution: Your Name (TEAMID)"          — signs the .app
#      • "3rd Party Mac Developer Installer: Your Name (TEAMID)"  — signs the .pkg
#
# 3. A Mac App Store distribution provisioning profile for bundle ID
#    org.audiovisualizer.app downloaded from developer.apple.com → Profiles.
#    Pass its path via PROVISION_PROFILE (see below).
#
# 4. An App Store Connect API key (.p8 file) with the key file placed at
#      ~/.private_keys/AuthKey_<ASC_API_KEY_ID>.p8
#    Obtain one at: App Store Connect → Users & Access → Integrations → App Store Connect API
#
# 5. Your app record must already exist in App Store Connect (New App → macOS).
#
# Usage
# ─────
#   export APPLE_DIST_ID="Apple Distribution: Your Name (TEAMID)"
#   export INSTALLER_ID="3rd Party Mac Developer Installer: Your Name (TEAMID)"
#   export PROVISION_PROFILE="/path/to/AudioVisualizer_AppStore.provisionprofile"
#   export ASC_API_KEY_ID="ABCDEF1234"
#   export ASC_ISSUER_ID="xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
#   ./scripts/release_appstore_macos.sh
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
APP_NAME="audiovisualizer"
DISPLAY_NAME="Audio Visualizer"
INFO_PLIST="$REPO_ROOT/macos/Info.plist"
ENTITLEMENTS="$REPO_ROOT/macos/audiovisualizer-distribution.entitlements"

APPLE_DIST_ID="${APPLE_DIST_ID:-}"
INSTALLER_ID="${INSTALLER_ID:-}"
PROVISION_PROFILE="${PROVISION_PROFILE:-}"
ASC_API_KEY_ID="${ASC_API_KEY_ID:-}"
ASC_ISSUER_ID="${ASC_ISSUER_ID:-}"

APP_VERSION="${APP_VERSION:-$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$INFO_PLIST")}"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"

# ── Validate inputs ───────────────────────────────────────────────────────────
_die() { echo "❌  $*" >&2; exit 1; }

[[ -n "$APPLE_DIST_ID"    ]] || _die "APPLE_DIST_ID is not set.
    export APPLE_DIST_ID=\"Apple Distribution: Your Name (TEAMID)\""

[[ -n "$INSTALLER_ID"     ]] || _die "INSTALLER_ID is not set.
    export INSTALLER_ID=\"3rd Party Mac Developer Installer: Your Name (TEAMID)\""

[[ -n "$PROVISION_PROFILE" && -f "$PROVISION_PROFILE" ]] || _die "PROVISION_PROFILE is not set or file not found.
    export PROVISION_PROFILE=\"/path/to/AudioVisualizer_AppStore.provisionprofile\""

[[ -n "$ASC_API_KEY_ID"   ]] || _die "ASC_API_KEY_ID is not set (App Store Connect API key ID)."
[[ -n "$ASC_ISSUER_ID"    ]] || _die "ASC_ISSUER_ID is not set (App Store Connect issuer UUID)."

P8_FILE="$HOME/.private_keys/AuthKey_${ASC_API_KEY_ID}.p8"
[[ -f "$P8_FILE" ]] || _die "API key file not found at $P8_FILE
    Download AuthKey_${ASC_API_KEY_ID}.p8 from App Store Connect and place it there."

APP_BUNDLE="$BUILD_DIR/$APP_NAME.app"

echo "▶  Audio Visualizer $APP_VERSION — App Store build"
echo "   Distribution : $APPLE_DIST_ID"
echo "   Installer    : $INSTALLER_ID"
echo "   Profile      : $PROVISION_PROFILE"
echo "   Output       : $DIST_DIR"
echo

# ── Step 1/6 · Build ──────────────────────────────────────────────────────────
echo "── Step 1/6: Build ──────────────────────────────────────────────────────"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --config Release
echo "   ✅ Build OK"
echo

# ── Step 2/6 · Embed provisioning profile ────────────────────────────────────
echo "── Step 2/6: Embed provisioning profile ─────────────────────────────────"
cp "$PROVISION_PROFILE" "$APP_BUNDLE/Contents/embedded.provisionprofile"
echo "   Embedded: $(basename "$PROVISION_PROFILE")"
echo "   ✅ Provisioning profile embedded"
echo

# ── Step 3/6 · Strip quarantine attribute ────────────────────────────────────
echo "── Step 3/6: Strip quarantine attribute ─────────────────────────────────"
# Must run AFTER embedding the provisioning profile: the .provisionprofile is
# itself a downloaded file and would re-introduce the attribute if stripped first.
xattr -cr "$APP_BUNDLE"
echo "   ✅ Quarantine attribute removed"
echo

# ── Step 4/6 · Sign with Apple Distribution ──────────────────────────────────
echo "── Step 4/6: Code-sign (Apple Distribution) ─────────────────────────────"

# Extract the Team ID from the provisioning profile so we can add the
# com.apple.application-identifier entitlement (TEAMID.BUNDLEID).
# Transporter/App Store Connect requires this to match the profile; Xcode adds
# it automatically but we must inject it ourselves in a manual build.
PROFILE_PLIST="$BUILD_DIR/profile-decoded.plist"
security cms -D -i "$PROVISION_PROFILE" -o "$PROFILE_PLIST" 2>/dev/null
TEAM_ID=$(/usr/libexec/PlistBuddy -c "Print :TeamIdentifier:0" "$PROFILE_PLIST" 2>/dev/null)
rm -f "$PROFILE_PLIST"
if [[ -z "$TEAM_ID" ]]; then
    echo "❌  Could not extract Team ID from provisioning profile." >&2
    exit 1
fi
echo "   Team ID : $TEAM_ID"

# Build a merged entitlements file that adds com.apple.application-identifier.
MERGED_ENT="$BUILD_DIR/entitlements-appstore-merged.plist"
cp "$ENTITLEMENTS" "$MERGED_ENT"
/usr/libexec/PlistBuddy -c \
    "Add :com.apple.application-identifier string ${TEAM_ID}.${BUNDLE_ID}" \
    "$MERGED_ENT"

# Sign bundled dylibs first; inner components must be signed before the outer
# bundle signature is applied, because the bundle signature covers nested code.
FRAMEWORKS="$APP_BUNDLE/Contents/Frameworks"
if [[ -d "$FRAMEWORKS" ]]; then
    for dylib in "$FRAMEWORKS"/*.dylib; do
        [[ -f "$dylib" ]] || continue
        codesign --force --options runtime \
            --entitlements "$MERGED_ENT" \
            --sign "$APPLE_DIST_ID" \
            "$dylib"
        echo "   signed: $(basename "$dylib")"
    done
fi

codesign --force --options runtime \
    --entitlements "$MERGED_ENT" \
    --sign "$APPLE_DIST_ID" \
    --identifier "$BUNDLE_ID" \
    "$APP_BUNDLE"
echo "   signed: $APP_NAME.app"

codesign --verify --deep --strict --verbose=2 "$APP_BUNDLE"
echo "   ✅ Signature OK"
echo

# ── Step 5/6 · Package as .pkg ────────────────────────────────────────────────
echo "── Step 5/6: Create installer package (.pkg) ────────────────────────────"
mkdir -p "$DIST_DIR"
PKG_NAME="${DISPLAY_NAME// /_}-${APP_VERSION}-AppStore"
PKG_FINAL="$DIST_DIR/${PKG_NAME}.pkg"

productbuild \
    --component "$APP_BUNDLE" /Applications \
    --sign "$INSTALLER_ID" \
    "$PKG_FINAL"

echo "   Created: $PKG_FINAL"
echo "   ✅ Package OK"
echo

# ── Step 6/6 · Upload via Transporter ────────────────────────────────────────
echo "── Step 6/6: Upload to App Store Connect ────────────────────────────────"

TRANSPORTER="/Applications/Transporter.app/Contents/MacOS/Transporter"
if [[ ! -x "$TRANSPORTER" ]]; then
    echo "❌  Transporter not found at $TRANSPORTER" >&2
    echo "    Install it free from the Mac App Store:" >&2
    echo "    https://apps.apple.com/us/app/transporter/id1450874784" >&2
    exit 1
fi

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  ✅  Package ready:"
echo "      $PKG_FINAL"
echo
echo "  Upload via Transporter.app (most reliable):"
echo "    1. Open Transporter from your Applications folder"
echo "    2. Sign in with your Apple ID"
echo "    3. Click + and select the .pkg above, then click Deliver"
echo
echo "  Or upload from the command line:"
echo "    \"$TRANSPORTER\" \\"
echo "      -m upload -f \"$PKG_FINAL\" \\"
echo "      -apiKey \"$ASC_API_KEY_ID\" \\"
echo "      -apiIssuer \"$ASC_ISSUER_ID\""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
