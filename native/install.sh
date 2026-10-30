#!/bin/sh
set -eu

usage() {
    cat <<'HELP'
Usage: PREFIX=~/.local ./native/install.sh [--uninstall]
Build, ad-hoc sign, and install gtts and gtts-service for the current user.
PREFIX defaults to $HOME/.local. --uninstall removes only the installed files,
not downloaded voices, data, or a running LaunchAgent.
HELP
}
case ${1-} in
    -h|--help) usage; exit 0 ;;
    ''|--uninstall) ;;
    *) usage >&2; exit 2 ;;
esac
[ "$#" -le 1 ] || { usage >&2; exit 2; }
: "${HOME:?HOME must be set, or set PREFIX and HOME to a user directory}"
PREFIX=${PREFIX:-"$HOME/.local"}
case $PREFIX in /*) ;; *) PREFIX="$PWD/$PREFIX" ;; esac
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
BIN="$PREFIX/bin"
LIBEXEC="$PREFIX/libexec/google-tts-local"
LICENSES="$PREFIX/share/google-tts-local/licenses"

if [ "${1-}" = --uninstall ]; then
    rm -f "$BIN/gtts" "$BIN/gtts-service" "$LIBEXEC/gtts-engine" \
        "$LIBEXEC/libgtts-shim.dylib" "$LIBEXEC/gtts-macho-convert.py" \
        "$LICENSES/NOTICE.md" "$LICENSES/GCC-14-COPYRIGHT.txt" "$LICENSES/GPL-3.0.txt" "$LICENSES/ZSTD-BSD.txt"
    rmdir "$LICENSES" "$PREFIX/share/google-tts-local" "$LIBEXEC" 2>/dev/null || true
    printf 'Removed gtts from %s. Voice data and any LaunchAgent were retained.\n' "$PREFIX"
    exit 0
fi

[ "$(uname -s)" = Darwin ] || { echo 'gtts requires macOS.' >&2; exit 1; }
[ "$(uname -m)" = arm64 ] || { echo 'gtts requires an Apple Silicon Mac (arm64).' >&2; exit 1; }
VERSION=$(sw_vers -productVersion)
MAJOR=${VERSION%%.*}
case $MAJOR in ''|*[!0-9]*) echo 'Could not determine the macOS version.' >&2; exit 1 ;; esac
[ "$MAJOR" -ge 13 ] || { echo 'gtts requires macOS 13 Ventura or later.' >&2; exit 1; }
[ -d /System/Library/Frameworks/Hypervisor.framework ] || { echo 'Hypervisor.framework is missing.' >&2; exit 1; }
xcode-select -p >/dev/null 2>&1 || { echo 'Install Xcode Command Line Tools: xcode-select --install' >&2; exit 1; }
mkdir -p "$SCRIPT_DIR/tmp"
TMPDIR="$SCRIPT_DIR/tmp" xcrun --find clang >/dev/null 2>&1 || { echo 'A working Xcode or Command Line Tools clang is required.' >&2; exit 1; }
TMPDIR="$SCRIPT_DIR/tmp" xcrun --sdk macosx --show-sdk-path >/dev/null 2>&1 || { echo 'A macOS SDK is required; install Xcode Command Line Tools.' >&2; exit 1; }

TMPDIR="$SCRIPT_DIR/tmp" make -C "$SCRIPT_DIR" all
mkdir -p "$BIN" "$LIBEXEC" "$LICENSES"
install -m 755 "$SCRIPT_DIR/build/gtts-native" "$BIN/gtts"
install -m 755 "$SCRIPT_DIR/build/gtts-engine" "$LIBEXEC/gtts-engine"
install -m 755 "$SCRIPT_DIR/build/libgtts-shim.dylib" "$LIBEXEC/libgtts-shim.dylib"
install -m 644 "$SCRIPT_DIR/build/gtts-macho-convert.py" "$LIBEXEC/gtts-macho-convert.py"
install -m 755 "$SCRIPT_DIR/launchagent.sh" "$BIN/gtts-service"
install -m 644 "$SCRIPT_DIR/licenses/NOTICE.md" "$SCRIPT_DIR/licenses/GCC-14-COPYRIGHT.txt" "$SCRIPT_DIR/licenses/GPL-3.0.txt" "$SCRIPT_DIR/licenses/ZSTD-BSD.txt" "$LICENSES/"
TMPDIR="$SCRIPT_DIR/tmp" codesign --force --sign - --entitlements "$SCRIPT_DIR/hypervisor.entitlements" "$LIBEXEC/gtts-engine"
TMPDIR="$SCRIPT_DIR/tmp" codesign --force --sign - "$BIN/gtts"
codesign --verify --strict "$LIBEXEC/gtts-engine"
codesign --verify --strict "$LIBEXEC/libgtts-shim.dylib"
codesign --verify --strict "$BIN/gtts"
printf 'Installed %s and %s.\n' "$BIN/gtts" "$BIN/gtts-service"
printf 'Add %s to PATH if needed, then run: gtts fetch\n' "$BIN"
