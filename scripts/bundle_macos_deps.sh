#!/usr/bin/env bash
# Copy every non-system dylib that the bundle's main executable links
# against into Contents/Frameworks/, rewrite the executable's reference
# to @rpath/<libname>, and rewrite the bundled copy's own install_name
# to match. Run as a POST_BUILD step before codesign --deep so the
# bundled dylibs get a fresh ad-hoc signature with the rest of the app.
#
# Idempotent: already-bundled @rpath references are skipped on rerun.
# Scope is intentionally one level deep -- if a bundled dylib ever
# starts pulling in its own Homebrew deps, extend this to recurse.
#
# Usage: bundle_macos_deps.sh <path/to/Foo.app>
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <path/to/.app>" >&2
  exit 2
fi

BUNDLE="$1"
if [[ ! -d "$BUNDLE" ]]; then
  echo "error: bundle not found: $BUNDLE" >&2
  exit 1
fi

BIN_NAME=$(basename "$BUNDLE" .app)
BINARY="$BUNDLE/Contents/MacOS/$BIN_NAME"
FRAMEWORKS="$BUNDLE/Contents/Frameworks"

if [[ ! -x "$BINARY" ]]; then
  echo "error: executable not found at $BINARY" >&2
  exit 1
fi

mkdir -p "$FRAMEWORKS"

# Skip system paths (always present, always signed by Apple) and any
# reference that's already been rewritten by a previous run.
is_system_dep() {
  case "$1" in
    /usr/lib/*|/System/*|@rpath/*|@executable_path/*|@loader_path/*) return 0 ;;
    *) return 1 ;;
  esac
}

bundled_count=0
# `otool -L` first line is the binary's own path; skip it with tail -n +2.
# Each subsequent line is "\tpath (compatibility ..., current ...)".
while IFS= read -r line; do
  dep=$(echo "$line" | awk '{print $1}')
  [[ -z "$dep" ]] && continue
  if is_system_dep "$dep"; then
    continue
  fi
  name=$(basename "$dep")
  target="$FRAMEWORKS/$name"
  if [[ ! -f "$target" ]]; then
    cp "$dep" "$target"
    chmod u+w "$target"
    install_name_tool -id "@rpath/$name" "$target"
    echo "[bundle] copied $dep -> Frameworks/$name"
  fi
  install_name_tool -change "$dep" "@rpath/$name" "$BINARY"
  bundled_count=$((bundled_count + 1))
done < <(otool -L "$BINARY" | tail -n +2)

if [[ $bundled_count -eq 0 ]]; then
  echo "[bundle] no third-party dylibs to relocate"
fi

# Strip any LC_RPATH that points outside the bundle (e.g. /opt/homebrew/lib
# inherited from SDL3's pkg-config flags). Harmless on machines with
# Homebrew but a residual reference we don't want shipped.
while IFS= read -r rpath; do
  case "$rpath" in
    @executable_path/*|@loader_path/*) continue ;;
    "") continue ;;
  esac
  install_name_tool -delete_rpath "$rpath" "$BINARY" 2>/dev/null || true
  echo "[bundle] removed external rpath: $rpath"
done < <(otool -l "$BINARY" \
  | awk '/cmd LC_RPATH/{flag=1; next} flag && /path /{print $2; flag=0}')
