#!/bin/sh
# Installs sosuc for one user, without root by default, on Linux and macOS.
#   curl -fsSL https://github.com/Clement-Bonet/Sosu/releases/latest/download/install.sh | sh
#   ./install.sh                     from an unpacked archive: installs that archive
#   ... | sh -s -- --version 0.6.0   a given version rather than the latest
#   ... | sh -s -- --prefix /usr/local (with sudo) for every user
#   ... | sh -s -- --uninstall       removes what an earlier install.sh put there
#
# Run from an unpacked archive, it installs that archive. Otherwise it downloads the archive of
# the release for this system and checks it against the SHA256SUMS.txt of the release.
# The archive goes into PREFIX/lib/sosu as it is (PREFIX is ~/.local by default); PREFIX/bin/sosuc
# is a short script that runs PREFIX/lib/sosu/bin/sosuc by its full path, so that sosuc finds its
# library in ../lib. SOSU_RELEASES replaces https://github.com/Clement-Bonet/Sosu/releases.
set -eu

prefix="$HOME/.local"
version=""
uninstall=0
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) [ $# -ge 2 ] || { echo "install.sh: --prefix needs a folder" >&2; exit 2; }
                  prefix="$2"; shift 2 ;;
        --prefix=*) prefix="${1#--prefix=}"; shift ;;
        --version) [ $# -ge 2 ] || { echo "install.sh: --version needs a version" >&2; exit 2; }
                   version="${2#v}"; shift 2 ;;
        --version=*) version="${1#--version=}"; version="${version#v}"; shift ;;
        --uninstall) uninstall=1; shift ;;
        -h|--help) sed -n '2,13p' "$0" 2>/dev/null | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "install.sh: unknown option $1 (see --help)" >&2; exit 2 ;;
    esac
done

case "$prefix" in
    /*) ;;
    *) prefix="$(pwd)/$prefix" ;;
esac
case "$prefix" in
    *\'*) echo "install.sh: the folder cannot have a ' in its name: $prefix" >&2; exit 2 ;;
esac

home="$prefix/lib/sosu"
launcher="$prefix/bin/sosuc"
marker="# sosuc launcher written by install.sh"

if [ "$uninstall" = 1 ]; then
    if [ -f "$launcher" ] && grep -q "^$marker" "$launcher"; then
        rm -f "$launcher"
    elif [ -e "$launcher" ]; then
        echo "install.sh: $launcher was not written by install.sh, it is left alone" >&2
    fi
    rm -rf "$home"
    echo "Sosu is uninstalled from $prefix."
    exit 0
fi

if [ -e "$launcher" ] && ! grep -q "^$marker" "$launcher"; then
    echo "install.sh: $launcher exists and was not written by install.sh; remove it first" >&2
    exit 1
fi

# The folder of the script, when it is a file: piped into sh, $0 is the shell.
source=""
if [ -f "$0" ]; then
    here="$(cd "$(dirname "$0")" && pwd)"
    if [ -x "$here/bin/sosuc" ] && [ -d "$here/lib/core" ]; then source="$here"; fi
fi

fetch() {  # fetch URL FILE
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then
        wget -q "$1" -O "$2"
    else
        echo "install.sh: curl or wget is needed to download Sosu" >&2
        exit 1
    fi
}

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

work=""
trap '[ -z "$work" ] || rm -rf "$work"' EXIT

if [ -z "$source" ]; then
    case "$(uname -s)-$(uname -m)" in
        Linux-x86_64|Linux-amd64) target=linux-x64 ;;
        Linux-aarch64|Linux-arm64) target=linux-arm64 ;;
        Darwin-*) target=macos-universal ;;
        *) echo "install.sh: no release of Sosu for $(uname -s) $(uname -m); build it from the sources" >&2
           exit 1 ;;
    esac
    releases="${SOSU_RELEASES:-https://github.com/Clement-Bonet/Sosu/releases}"
    work="$(mktemp -d)"
    if [ -z "$version" ]; then
        # The latest release is the one its page redirects to: .../releases/tag/vX.Y.Z.
        if command -v curl >/dev/null 2>&1; then
            version="$(curl -fsSLI -o /dev/null -w '%{url_effective}' "$releases/latest" | sed -n 's|.*/tag/v||p')"
        fi
        if [ -z "$version" ]; then
            echo "install.sh: the latest version is not known; give one with --version" >&2
            exit 1
        fi
    fi
    name="sosu-$version-$target"
    echo "Downloading $name.tar.gz"
    fetch "$releases/download/v$version/$name.tar.gz" "$work/$name.tar.gz"
    fetch "$releases/download/v$version/SHA256SUMS.txt" "$work/SHA256SUMS.txt"
    expected="$(grep " $name.tar.gz\$" "$work/SHA256SUMS.txt" | cut -d' ' -f1)"
    got="$(sha256 "$work/$name.tar.gz")"
    if [ -z "$expected" ] || [ "$expected" != "$got" ]; then
        echo "install.sh: the SHA-256 of $name.tar.gz is not the one of SHA256SUMS.txt" >&2
        exit 1
    fi
    tar -xzf "$work/$name.tar.gz" -C "$work"
    source="$work/$name"
fi

# A new version replaces the old one whole: no file of the old library stays behind.
rm -rf "$home"
mkdir -p "$home" "$prefix/bin"
cp -R "$source/bin" "$source/lib" "$source/packages" "$home/"
for file in README.md LICENSE-APACHE LICENSE-MIT THIRD_PARTY_NOTICES.md; do
    if [ -f "$source/$file" ]; then cp "$source/$file" "$home/"; fi
done
# A browser marks what it downloads on macOS; Gatekeeper would then stop the unsigned sosuc.
if [ "$(uname -s)" = Darwin ]; then
    xattr -dr com.apple.quarantine "$home" 2>/dev/null || true
fi

cat > "$launcher" <<EOF
#!/bin/sh
$marker
exec '$home/bin/sosuc' "\$@"
EOF
chmod 755 "$launcher"

echo "Sosu $("$launcher" --version | sed 's/^sosuc //') is installed in $home."
echo "The packages are in $home/packages."
case ":$PATH:" in
    *":$prefix/bin:"*) ;;
    *) echo "Add $prefix/bin to your PATH, for instance in ~/.profile:"
       echo "  export PATH=\"$prefix/bin:\$PATH\"" ;;
esac
