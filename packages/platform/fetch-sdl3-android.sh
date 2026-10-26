#!/bin/sh
# Downloads SDL3 for Android into vendor/SDL3-android (spec §11.5), as fetch-sdl3-android.ps1
# does on Windows: the archive of SDL (`SDL3-<version>.aar`, for the Gradle project) and
# `lib/<abi>/libSDL3.so` (to link `libmain.so`). Run it once before building for Android:
#     sh packages/platform/fetch-sdl3-android.sh
# Running it again does nothing when the right version is already there.
set -e

VERSION=3.4.18
SHA256=e09e4d6593335d089ba124375e8b8abd0fd75173dffcd2c17012b45af121d4e9
NAME="SDL3-devel-$VERSION-android.zip"
URL="https://github.com/libsdl-org/SDL/releases/download/release-$VERSION/$NAME"

VENDOR="$(cd "$(dirname "$0")" && pwd)/vendor"
TARGET="$VENDOR/SDL3-android"
if [ -f "$TARGET/VERSION.txt" ] && [ "$(cat "$TARGET/VERSION.txt")" = "$VERSION" ]; then
    echo "SDL3 $VERSION for Android is already in $TARGET"
    exit 0
fi

mkdir -p "$VENDOR"
ZIP="$VENDOR/$NAME"
echo "Downloading $URL"
curl -fsSL -o "$ZIP" "$URL"
if command -v sha256sum >/dev/null; then
    ACTUAL=$(sha256sum "$ZIP" | cut -d' ' -f1)
else
    ACTUAL=$(shasum -a 256 "$ZIP" | cut -d' ' -f1)
fi
if [ "$ACTUAL" != "$SHA256" ]; then
    rm -f "$ZIP"
    echo "SHA-256 mismatch for $NAME: expected $SHA256, got $ACTUAL. Nothing was installed." >&2
    exit 1
fi

EXTRACT="$VENDOR/extract-android"
rm -rf "$EXTRACT"
mkdir -p "$EXTRACT/aar"
unzip -q "$ZIP" -d "$EXTRACT"
AAR=$(find "$EXTRACT" -name "SDL3-*.aar" | head -n 1)
[ -n "$AAR" ] || { echo "no SDL3-*.aar in $NAME" >&2; exit 1; }
unzip -q "$AAR" -d "$EXTRACT/aar"

rm -rf "$TARGET"
mkdir -p "$TARGET"
cp "$AAR" "$TARGET/SDL3-$VERSION.aar"
for ABI in arm64-v8a x86_64; do
    LIBRARY=$(find "$EXTRACT/aar" -path "*android.$ABI*" -name libSDL3.so | head -n 1)
    [ -n "$LIBRARY" ] || { echo "no libSDL3.so for $ABI" >&2; exit 1; }
    mkdir -p "$TARGET/lib/$ABI"
    cp "$LIBRARY" "$TARGET/lib/$ABI/"
done
echo "$VERSION" > "$TARGET/VERSION.txt"
rm -rf "$EXTRACT" "$ZIP"
echo "SDL3 $VERSION for Android is in $TARGET"
