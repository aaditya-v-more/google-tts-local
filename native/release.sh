#!/bin/sh
set -eu

NATIVE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
ROOT=$(CDPATH= cd -- "$NATIVE/.." && pwd -P)
VERSION=${1:-$(cat "$NATIVE/VERSION")}
case $VERSION in ''|*[!0-9.]*) echo 'Version must contain only digits and dots.' >&2; exit 2 ;; esac
[ "$VERSION" = "$(git -C "$ROOT" show HEAD:native/VERSION)" ] || {
    echo 'Version must match the committed native/VERSION.' >&2; exit 2;
}

# The formula is kept outside the archive so it can contain the archive hash.
# All files in the archive come from the current commit.
git -C "$ROOT" diff --quiet HEAD -- .gitignore Dockerfile LICENSE README.md app gtts native || {
    echo 'Commit the release source changes before building the archive.' >&2; exit 1;
}
mkdir -p "$NATIVE/tmp" "$NATIVE/dist"
STAGE=$(mktemp -d "$NATIVE/tmp/release-XXXXXX")
trap 'rm -rf "$STAGE"' EXIT HUP INT TERM
PACKAGE="google-tts-local-$VERSION"
git -C "$ROOT" archive --format=tar --prefix="$PACKAGE/" HEAD -- \
    .gitignore Dockerfile LICENSE README.md app gtts native > "$STAGE/source.tar"
tar -xf "$STAGE/source.tar" -C "$STAGE"
TZ=UTC0; LC_ALL=C; export TZ LC_ALL
find "$STAGE/$PACKAGE" -exec touch -t 202001010000 {} +
(cd "$STAGE" && find "$PACKAGE" -print | sort) > "$STAGE/files"
ARCHIVE="$NATIVE/dist/$PACKAGE.tar.gz"
TEMP="$STAGE/$PACKAGE.tar.gz"
COPYFILE_DISABLE=1 tar -cf - --format ustar --uid 0 --gid 0 --uname root --gname wheel \
    --no-recursion -C "$STAGE" -T "$STAGE/files" | gzip -n > "$TEMP"
if tar -tzf "$TEMP" | grep -E '(\.so(\.|$)|\.dylib$|\.zvoice$|\.wav$|/assets/|/ref/|/tmp/|/build/)' >/dev/null; then
    echo 'Release archive contains a forbidden binary or data path.' >&2; exit 1
fi
mv -f "$TEMP" "$ARCHIVE"
printf 'Created %s\n' "$ARCHIVE"
shasum -a 256 "$ARCHIVE"
