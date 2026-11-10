#!/bin/sh
# Builds the .deb of sosuc from the laid out installation of a release (bin/, lib/, packages/).
#   installers/linux/build-deb.sh DIST VERSION ARCH
# ARCH is the Debian name (amd64, arm64). Writes sosu_VERSION_ARCH.deb in the current folder.
#
# The installation goes into /usr/lib/sosu as it is; /usr/bin/sosuc is a short script that runs
# /usr/lib/sosu/bin/sosuc by its full path, so that sosuc finds its library in ../lib.
set -eu

if [ $# -ne 3 ]; then
    echo "usage: build-deb.sh DIST VERSION ARCH" >&2
    exit 2
fi
dist="$1"
arch="$3"
# Debian sorts 0.6.0~rc.1 before 0.6.0, as SemVer sorts 0.6.0-rc.1 before 0.6.0.
version="$(echo "$2" | sed 's/-/~/')"

root="$(mktemp -d)"
trap 'rm -rf "$root"' EXIT

mkdir -p "$root/DEBIAN" "$root/usr/lib/sosu" "$root/usr/bin" "$root/usr/share/doc/sosu"
cp -R "$dist/bin" "$dist/lib" "$dist/packages" "$root/usr/lib/sosu/"
cp "$dist/README.md" "$dist/THIRD_PARTY_NOTICES.md" "$root/usr/share/doc/sosu/"
cat "$dist/LICENSE-MIT" "$dist/LICENSE-APACHE" > "$root/usr/share/doc/sosu/copyright"

cat > "$root/usr/bin/sosuc" <<'EOF'
#!/bin/sh
exec /usr/lib/sosu/bin/sosuc "$@"
EOF

find "$root" -type d -exec chmod 755 {} +
find "$root" -type f -exec chmod 644 {} +
chmod 755 "$root/usr/bin/sosuc" "$root/usr/lib/sosu/bin/sosuc"

size="$(du -sk "$root/usr" | cut -f1)"
cat > "$root/DEBIAN/control" <<EOF
Package: sosu
Version: $version
Architecture: $arch
Maintainer: Sosu <https://github.com/Clement-Bonet/Sosu/issues>
Installed-Size: $size
Depends: libc6
Recommends: gcc | clang | c-compiler
Section: devel
Priority: optional
Homepage: https://github.com/Clement-Bonet/Sosu
Description: compiler of the Sosu programming language
 sosuc checks Sosu packages, writes C from them and builds them with the
 C compiler of the system. The standard library is in /usr/lib/sosu/lib and
 the other packages of the release in /usr/lib/sosu/packages.
EOF

dpkg-deb --root-owner-group --build "$root" "sosu_${version}_${arch}.deb"
