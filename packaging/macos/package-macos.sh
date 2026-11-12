#!/usr/bin/env bash
# Builds a .pkg installer and a .dmg disk image for the Jolt engine on macOS.
#
# Usage: package-macos.sh <version> <stage-dir> <out-dir>
#
#   stage-dir  the tree produced by `cmake --install --prefix <dir>`, i.e.
#              <dir>/bin/oskel and <dir>/share/oskel/std/*.oskl
#   out-dir    where the .pkg and .dmg are written
#
# Both payloads install the same layout as the Linux packages, under
# /usr/local (the conventional prefix for hand-installed Unix software):
#
#   /usr/local/bin/oskel
#   /usr/local/share/oskel/std/*.oskl
#
# `standardLibraryDir` looks for `../share/oskel/std` relative to the
# executable, so `/usr/local/bin/oskel` resolves `/usr/local/share/oskel/std`
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

if [ ! -x "$stage/bin/oskel" ]; then
  echo "package-macos: $stage/bin/oskel not found; did the install step run?" >&2
  exit 1
fi
if [ ! -f "$stage/share/oskel/std/math.oskl" ]; then
  echo "package-macos: $stage/share/oskel/std/math.oskl not found" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# A fresh payload root: `pkgbuild` refuses to build from a directory that
# already holds package metadata from a previous run.
payload="$work/payload"
mkdir -p "$payload/usr/local/bin" "$payload/usr/local/share/oskel"
install -m 0755 "$stage/bin/oskel" "$payload/usr/local/bin/oskel"
cp -a "$stage/share/oskel/std" "$payload/usr/local/share/oskel/std"

# ---------------------------------------------------------------------- .pkg
pkg_root="$work/pkg"
mkdir -p "$pkg_root"
pkgbuild \
  --root "$payload" \
  --identifier "org.oskel-lang.oskel" \
  --version "$version" \
  --install-location "/" \
  "$pkg_root/oskel-component.pkg"

cat > "$work/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
  <title>Jolt $version</title>
  <organization>org.oskel-lang</organization>
  <domains enable_localSystem="true"/>
  <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
  <choices-outline>
    <line choice="default">
      <line choice="org.oskel-lang.oskel"/>
    </line>
  </choices-outline>
  <choice id="default"/>
  <choice id="org.oskel-lang.oskel" visible="false">
    <pkg-ref id="org.oskel-lang.oskel"/>
  </choice>
  <pkg-ref id="org.oskel-lang.oskel" version="$version" onConclusion="none">oskel-component.pkg</pkg-ref>
</installer-gui-script>
EOF

productbuild \
  --distribution "$work/distribution.xml" \
  --package-path "$pkg_root" \
  "$out/oskel-${version}.pkg"
echo "wrote $out/oskel-${version}.pkg"

# ---------------------------------------------------------------------- .dmg
# A drag-and-drop image: the engine tree plus a symlink to /usr/local, so the
# user can drag `oskel` and `share` into place. The .pkg above is the scripted
# alternative for a one-click install.
dmg_src="$work/dmg"
mkdir -p "$dmg_src"
install -m 0755 "$stage/bin/oskel" "$dmg_src/oskel"
cp -a "$stage/share" "$dmg_src/share"
ln -s /usr/local "$dmg_src/usr-local"

hdiutil create \
  -volname "Jolt $version" \
  -srcfolder "$dmg_src" \
  -ov -format UDZO \
  "$out/oskel-${version}.dmg"
echo "wrote $out/oskel-${version}.dmg"
