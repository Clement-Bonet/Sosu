#!/bin/sh
# Downloads SDL3 for iOS into vendor/SDL3-apple (spec §11.5): the SDL3.xcframework of the disk
# image of SDL, a fixed version checked by its SHA-256. On a Mac only (hdiutil). Run it once
# before `sosuc package --target ios-sim`:
#     sh packages/platform/fetch-sdl3-apple.sh
# Running it again does nothing when the right version is already there.
set -e

VERSION=3.4.18
SHA256=c1adf3da91108dbc3e4205da5dacebc203085d283fa89e86eb2c889abd03a853
NAME="SDL3-$VERSION.dmg"
URL="https://github.com/libsdl-org/SDL/releases/download/release-$VERSION/$NAME"

VENDOR="$(cd "$(dirname "$0")" && pwd)/vendor"
TARGET="$VENDOR/SDL3-apple"
if [ -f "$TARGET/VERSION.txt" ] && [ "$(cat "$TARGET/VERSION.txt")" = "$VERSION" ]; then
    echo "SDL3 $VERSION for Apple is already in $TARGET"
    exit 0
fi

mkdir -p "$VENDOR"
IMAGE="$VENDOR/$NAME"
echo "Downloading $URL"
curl -fsSL -o "$IMAGE" "$URL"
ACTUAL=$(shasum -a 256 "$IMAGE" | cut -d' ' -f1)
if [ "$ACTUAL" != "$SHA256" ]; then
    rm -f "$IMAGE"
    echo "SHA-256 mismatch for $NAME: expected $SHA256, got $ACTUAL. Nothing was installed." >&2
    exit 1
fi

MOUNT="$VENDOR/mount"
mkdir -p "$MOUNT"
hdiutil attach -nobrowse -readonly -mountpoint "$MOUNT" "$IMAGE" >/dev/null
rm -rf "$TARGET"
mkdir -p "$TARGET"
cp -R "$MOUNT/SDL3.xcframework" "$TARGET/"
hdiutil detach "$MOUNT" >/dev/null
rmdir "$MOUNT"
rm -f "$IMAGE"
echo "$VERSION" > "$TARGET/VERSION.txt"
echo "SDL3 $VERSION for Apple is in $TARGET"
