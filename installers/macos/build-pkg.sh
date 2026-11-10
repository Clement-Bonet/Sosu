#!/bin/sh
# Builds the .pkg of sosuc from the laid out installation of a release (bin/, lib/, packages/).
#   installers/macos/build-pkg.sh DIST VERSION [IDENTITY]
# IDENTITY, when given, signs the package ("Developer ID Installer: ..."); without it the package
# is unsigned. Writes sosu-VERSION-macos-universal.pkg in the current folder.
#
# The installation goes into /usr/local/lib/sosu as it is; /usr/local/bin/sosuc is a short script
# that runs /usr/local/lib/sosu/bin/sosuc by its full path, so that sosuc finds ../lib.
set -eu

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
    echo "usage: build-pkg.sh DIST VERSION [IDENTITY]" >&2
    exit 2
fi
dist="$1"
version="$2"
identity="${3:-}"

root="$(mktemp -d)"
trap 'rm -rf "$root"' EXIT

mkdir -p "$root/usr/local/lib/sosu" "$root/usr/local/bin"
cp -R "$dist/bin" "$dist/lib" "$dist/packages" "$root/usr/local/lib/sosu/"
cp "$dist/README.md" "$dist/LICENSE-APACHE" "$dist/LICENSE-MIT" "$dist/THIRD_PARTY_NOTICES.md" \
   "$root/usr/local/lib/sosu/"

cat > "$root/usr/local/bin/sosuc" <<'EOF'
#!/bin/sh
exec /usr/local/lib/sosu/bin/sosuc "$@"
EOF
chmod -R u+rwX,go+rX,go-w "$root"
chmod 755 "$root/usr/local/bin/sosuc" "$root/usr/local/lib/sosu/bin/sosuc"

# A new version replaces the old one whole: the old library is removed before the copy.
scripts="$(mktemp -d)"
trap 'rm -rf "$root" "$scripts"' EXIT
cat > "$scripts/preinstall" <<'EOF'
#!/bin/sh
rm -rf /usr/local/lib/sosu
exit 0
EOF
chmod 755 "$scripts/preinstall"

set -- --root "$root" --identifier io.github.clement-bonet.sosu --version "$version" \
       --install-location / --scripts "$scripts"
if [ -n "$identity" ]; then
    set -- "$@" --sign "$identity" --timestamp
fi
pkgbuild "$@" "sosu-$version-macos-universal.pkg"
