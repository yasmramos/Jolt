#!/usr/bin/env bash
# Builds a .pkg installer and a .dmg disk image for the Jolt engine on macOS.
#
# Usage: package-macos.sh <version> <stage-dir> <out-dir>
#
#   stage-dir  the tree produced by `cmake --install --prefix <dir>`, i.e.
#              <dir>/bin/jolt and <dir>/share/jolt/std/*.jolt
#   out-dir    where the .pkg and .dmg are written
#
# Both payloads install the same layout as the Linux packages, under
# /usr/local (the conventional prefix for hand-installed Unix software):
#
#   /usr/local/bin/jolt
#   /usr/local/share/jolt/std/*.jolt
#
# `standardLibraryDir` looks for `../share/jolt/std` relative to the
# executable, so `/usr/local/bin/jolt` resolves `/usr/local/share/jolt/std`
# with no environment variable.
#
# `pkgbuild`, `productbuild` and `hdiutil` ship with the Xcode command-line
# tools, which the macOS runner image already has. The .pkg is unsigned: it
# installs locally, but Gatekeeper will warn when it is downloaded, which is
# expected until the project has a Developer ID certificate.
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <version> <stage-dir> <out-dir>" >&2
  exit 2
fi

version="${1#v}"
stage="$(cd "$2" && pwd)"
out="$3"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

if [ ! -x "$stage/bin/jolt" ]; then
  echo "package-macos: $stage/bin/jolt not found; did the install step run?" >&2
  exit 1
fi
if [ ! -f "$stage/share/jolt/std/math.jolt" ]; then
  echo "package-macos: $stage/share/jolt/std/math.jolt not found" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# A fresh payload root: `pkgbuild` refuses to build from a directory that
# already holds package metadata from a previous run.
payload="$work/payload"
mkdir -p "$payload/usr/local/bin" "$payload/usr/local/share/jolt"
install -m 0755 "$stage/bin/jolt" "$payload/usr/local/bin/jolt"
cp -a "$stage/share/jolt/std" "$payload/usr/local/share/jolt/std"

# ---------------------------------------------------------------------- .pkg
pkg_root="$work/pkg"
mkdir -p "$pkg_root"
pkgbuild \
  --root "$payload" \
  --identifier "org.jolt-lang.jolt" \
  --version "$version" \
  --install-location "/" \
  "$pkg_root/jolt-component.pkg"

cat > "$work/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
  <title>Jolt $version</title>
  <organization>org.jolt-lang</organization>
  <domains enable_localSystem="true"/>
  <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
  <choices-outline>
    <line choice="default">
      <line choice="org.jolt-lang.jolt"/>
    </line>
  </choices-outline>
  <choice id="default"/>
  <choice id="org.jolt-lang.jolt" visible="false">
    <pkg-ref id="org.jolt-lang.jolt"/>
  </choice>
  <pkg-ref id="org.jolt-lang.jolt" version="$version" onConclusion="none">jolt-component.pkg</pkg-ref>
</installer-gui-script>
EOF

productbuild \
  --distribution "$work/distribution.xml" \
  --package-path "$pkg_root" \
  "$out/jolt-${version}.pkg"
echo "wrote $out/jolt-${version}.pkg"

# ---------------------------------------------------------------------- .dmg
# A drag-and-drop image: the engine tree plus a symlink to /usr/local, so the
# user can drag `jolt` and `share` into place. The .pkg above is the scripted
# alternative for a one-click install.
dmg_src="$work/dmg"
mkdir -p "$dmg_src"
install -m 0755 "$stage/bin/jolt" "$dmg_src/jolt"
cp -a "$stage/share" "$dmg_src/share"
ln -s /usr/local "$dmg_src/usr-local"

hdiutil create \
  -volname "Jolt $version" \
  -srcfolder "$dmg_src" \
  -ov -format UDZO \
  "$out/jolt-${version}.dmg"
echo "wrote $out/jolt-${version}.dmg"
