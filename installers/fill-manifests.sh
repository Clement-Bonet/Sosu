#!/bin/sh
# Fills the formula of Homebrew and the manifests of Scoop and winget with a release: its version
# and the SHA-256 of its files.
#   installers/fill-manifests.sh VERSION FILES OUT
# FILES is the folder of the files of the release, OUT the folder written (homebrew/, scoop/,
# winget/). Nothing is published: the files are kept for 1.0 (see the comment of each one).
set -eu

if [ $# -ne 3 ]; then
    echo "usage: fill-manifests.sh VERSION FILES OUT" >&2
    exit 2
fi
version="$1"
files="$2"
out="$3"
here="$(cd "$(dirname "$0")" && pwd)"

sum() {
    if [ ! -f "$files/$1" ]; then
        echo "fill-manifests.sh: $files/$1 is missing" >&2
        exit 1
    fi
    sha256sum "$files/$1" | cut -d' ' -f1
}
macos="$(sum "sosu-$version-macos-universal.tar.gz")"
linux_x64="$(sum "sosu-$version-linux-x64.tar.gz")"
linux_arm64="$(sum "sosu-$version-linux-arm64.tar.gz")"
windows_zip="$(sum "sosu-$version-windows-x64.zip")"
windows_setup="$(sum "sosu-$version-windows-x64-setup.exe")"

for template in homebrew/sosu.rb scoop/sosu.json winget/ClementBonet.Sosu.yaml \
                winget/ClementBonet.Sosu.installer.yaml winget/ClementBonet.Sosu.locale.en-US.yaml; do
    mkdir -p "$out/$(dirname "$template")"
    sed -e "s/@VERSION@/$version/g" \
        -e "s/@SHA256_MACOS@/$macos/g" \
        -e "s/@SHA256_LINUX_X64@/$linux_x64/g" \
        -e "s/@SHA256_LINUX_ARM64@/$linux_arm64/g" \
        -e "s/@SHA256_WINDOWS_ZIP@/$windows_zip/g" \
        -e "s/@SHA256_WINDOWS_SETUP@/$windows_setup/g" \
        "$here/$template" > "$out/$template"
done
if grep -rn '@[A-Z0-9_]*@' "$out"; then
    echo "fill-manifests.sh: a placeholder is left" >&2
    exit 1
fi
