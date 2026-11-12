#!/usr/bin/env bash
# Builds .deb and .rpm packages for the Jolt engine from a staged install tree.
#
# Usage: package-linux.sh <version> <stage-dir> <out-dir>
#
#   stage-dir  the tree produced by `cmake --install --prefix <dir>`, i.e.
#              <dir>/bin/oskel and <dir>/share/oskel/std/*.oskl
#   out-dir    where the .deb and .rpm are written
#
# The package layout mirrors the staged tree under the filesystem root:
#
#   /usr/bin/oskel
#   /usr/share/oskel/std/*.oskl
#
# `standardLibraryDir` looks for `../share/oskel/std` relative to the
# executable, so `/usr/bin/oskel` resolves `/usr/share/oskel/std` with no
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

if [ ! -x "$stage/bin/oskel" ]; then
  echo "package-linux: $stage/bin/oskel not found; did the install step run?" >&2
  exit 1
fi
if [ ! -f "$stage/share/oskel/std/math.oskl" ]; then
  echo "package-linux: $stage/share/oskel/std/math.oskl not found" >&2
  exit 1
fi

# Debian versions may contain `-`, but it separates the upstream version from
# the revision, so a pre-release tag (`0.1.0-rc1`) is rewritten with `~`, which
# sorts *before* the final `0.1.0` — the ordering a pre-release wants. This is
# the version recorded in the package control file, which is what `dpkg`
# compares.
deb_version="${version//-/\~}"

# GitHub rewrites `~` to `.` in release asset names, which would make the
# uploaded `.deb` not match its line in `SHA256SUMS`. Name the file with the
# safe form instead; only the control file keeps the `~`.
deb_file_version="${deb_version//\~/.}"

# RPM splits Version and Release at the first `-`, and Version may not contain
# one at all, so `0.1.0-rc1` becomes Version `0.1.0` + Release `0.rc1`, and a
# plain `0.1.0` stays Release `1`.
case "$version" in
  *-*) rpm_version="${version%%-*}"; rpm_release="0.${version#*-}" ;;
  *)   rpm_version="$version";       rpm_release="1" ;;
esac

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# ---------------------------------------------------------------------- .deb
root="$work/deb"
mkdir -p "$root/DEBIAN" "$root/usr/bin" "$root/usr/share/oskel"
install -m 0755 "$stage/bin/oskel" "$root/usr/bin/oskel"
cp -a "$stage/share/oskel/std" "$root/usr/share/oskel/std"

installed_size="$(du -sk "$root" | cut -f1)"
cat > "$root/DEBIAN/control" <<EOF
Package: oskel
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

dpkg-deb --build --root-owner-group "$root" "$out/oskel_${deb_file_version}_amd64.deb"
echo "wrote $out/oskel_${deb_file_version}_amd64.deb"

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
cat > "$rpm_top/SPECS/oskel.spec" <<EOF
Name:           oskel
Version:        $rpm_version
Release:        $rpm_release
Summary:        Jolt programming language engine
License:        MIT
URL:            https://github.com/yasmramos/Jolt
BuildArch:      x86_64

%description
The Jolt development engine and its standard library. Jolt is a dynamic,
general-purpose language with a Lua-style bytecode architecture.

%install
mkdir -p %{buildroot}/usr/bin %{buildroot}/usr/share/oskel
install -m 0755 "$stage/bin/oskel" %{buildroot}/usr/bin/oskel
cp -a "$stage/share/oskel/std" %{buildroot}/usr/share/oskel/std

%files
/usr/bin/oskel
/usr/share/oskel/std

%changelog
* Thu Jan 01 2026 Jolt contributors <noreply@example.com> - $rpm_version-$rpm_release
- Automated release build.
EOF

rpmbuild --define "_topdir $rpm_top" -bb "$rpm_top/SPECS/oskel.spec"
rpm_file="$(find "$rpm_top/RPMS" -name '*.rpm' | head -n1)"
cp "$rpm_file" "$out/oskel-${rpm_version}-${rpm_release}.x86_64.rpm"
echo "wrote $out/oskel-${rpm_version}-${rpm_release}.x86_64.rpm"
