#!/usr/bin/env bash
# Builds .deb and .rpm packages for the Jolt engine from a staged install tree.
#
# Usage: package-linux.sh <version> <stage-dir> <out-dir>
#
#   stage-dir  the tree produced by `cmake --install --prefix <dir>`, i.e.
#              <dir>/bin/jolt and <dir>/share/jolt/std/*.jolt
#   out-dir    where the .deb and .rpm are written
#
# The package layout mirrors the staged tree under the filesystem root:
#
#   /usr/bin/jolt
#   /usr/share/jolt/std/*.jolt
#
# `standardLibraryDir` looks for `../share/jolt/std` relative to the
# executable, so `/usr/bin/jolt` resolves `/usr/share/jolt/std` with no
# environment variable.
#
# `dpkg-deb` ships on every Debian/Ubuntu image. `rpmbuild` does not ship on
# the Ubuntu runner, so it is installed on demand; the RPM is skipped, not
# failed, if it cannot be installed, so a missing network never breaks the
# rest of the release.
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <version> <stage-dir> <out-dir>" >&2
  exit 2
fi

version="$1"
stage="$(cd "$2" && pwd)"
out="$3"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

if [ ! -x "$stage/bin/jolt" ]; then
  echo "package-linux: $stage/bin/jolt not found; did the install step run?" >&2
  exit 1
fi
if [ ! -f "$stage/share/jolt/std/math.jolt" ]; then
  echo "package-linux: $stage/share/jolt/std/math.jolt not found" >&2
  exit 1
fi

# Debian packages version 0.1.0 as-is; the release tag is `v0.1.0`, so strip the
# leading `v` if the caller passed one.
deb_version="${version#v}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# ---------------------------------------------------------------------- .deb
root="$work/deb"
mkdir -p "$root/DEBIAN" "$root/usr/bin" "$root/usr/share/jolt"
install -m 0755 "$stage/bin/jolt" "$root/usr/bin/jolt"
cp -a "$stage/share/jolt/std" "$root/usr/share/jolt/std"

installed_size="$(du -sk "$root" | cut -f1)"
cat > "$root/DEBIAN/control" <<EOF
Package: jolt
Version: $deb_version
Section: devel
Priority: optional
Architecture: amd64
Maintainer: Jolt contributors <noreply@example.com>
Installed-Size: $installed_size
Homepage: https://github.com/yasmramos/Jolt
Description: Jolt programming language engine
 The Jolt development engine and its standard library. Jolt is a dynamic,
 general-purpose language with a Lua-style bytecode architecture.
EOF

dpkg-deb --build --root-owner-group "$root" "$out/jolt_${deb_version}_amd64.deb"
echo "wrote $out/jolt_${deb_version}_amd64.deb"

# ---------------------------------------------------------------------- .rpm
if ! command -v rpmbuild >/dev/null 2>&1; then
  if command -v apt-get >/dev/null 2>&1; then
    echo "package-linux: installing rpmbuild"
    sudo apt-get update -qq
    sudo apt-get install -y -qq rpm
  fi
fi

if ! command -v rpmbuild >/dev/null 2>&1; then
  echo "package-linux: rpmbuild unavailable, skipping the .rpm" >&2
  exit 0
fi

rpm_top="$work/rpm"
mkdir -p "$rpm_top"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
cat > "$rpm_top/SPECS/jolt.spec" <<EOF
Name:           jolt
Version:        $deb_version
Release:        1
Summary:        Jolt programming language engine
License:        MIT
URL:            https://github.com/yasmramos/Jolt
BuildArch:      x86_64

%description
The Jolt development engine and its standard library. Jolt is a dynamic,
general-purpose language with a Lua-style bytecode architecture.

%install
mkdir -p %{buildroot}/usr/bin %{buildroot}/usr/share/jolt
install -m 0755 "$stage/bin/jolt" %{buildroot}/usr/bin/jolt
cp -a "$stage/share/jolt/std" %{buildroot}/usr/share/jolt/std

%files
/usr/bin/jolt
/usr/share/jolt/std

%changelog
* Thu Jan 01 2026 Jolt contributors <noreply@example.com> - $deb_version-1
- Automated release build.
EOF

rpmbuild --define "_topdir $rpm_top" -bb "$rpm_top/SPECS/jolt.spec"
rpm_file="$(find "$rpm_top/RPMS" -name '*.rpm' | head -n1)"
cp "$rpm_file" "$out/jolt-${deb_version}-1.x86_64.rpm"
echo "wrote $out/jolt-${deb_version}-1.x86_64.rpm"
