#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
if [ -z "${GTTS_DATA:-}" ]; then
    : "${HOME:?HOME or GTTS_DATA must be set}"
    GTTS_DATA="$HOME/Library/Application Support/google-tts-local"
    export GTTS_DATA
fi
LLD=${GTTS_LLD:-$(command -v ld.lld || true)}
if [ -z "$LLD" ]; then
    echo 'Guest C++ exception check skipped: ld.lld is not available (set GTTS_LLD to run it).'
    exit 0
fi
env -u SDKROOT TMPDIR="$ROOT/native/tmp" CLANG_MODULE_CACHE_PATH="$ROOT/native/tmp/clang-cache" \
    xcrun clang++ -target aarch64-linux-gnu -fPIC -fexceptions -O1 -c "$ROOT/native/exception-fixture.cc" -o "$ROOT/native/tmp/exception-fixture.o"
"$LLD" -flavor gnu -shared --eh-frame-hdr -o "$ROOT/native/tmp/exception-fixture.so" "$ROOT/native/tmp/exception-fixture.o"
if [ "${GTTS_NATIVE_BACKEND:-}" = macho ]; then
    FIXTURE_DIR=$(mktemp -d "$ROOT/native/tmp/exception-macho-XXXXXX")
    trap 'rm -rf "$FIXTURE_DIR"' EXIT HUP INT TERM
    PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 "$ROOT/native/build/gtts-macho-convert.py" \
        --data "$GTTS_DATA" --engine "$ROOT/native/tmp/exception-fixture.so" --output "$FIXTURE_DIR" \
        --shim "$ROOT/native/build/libgtts-shim.dylib"
    GTTS_MACHO_DIR="$FIXTURE_DIR" "$ROOT/native/build/exception-check" "$ROOT/native/tmp/exception-fixture.so"
else
    "$ROOT/native/build/exception-check" "$ROOT/native/tmp/exception-fixture.so"
fi
