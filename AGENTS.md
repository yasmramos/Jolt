# AGENTS.md — workflow notes

Repository-specific workflow for AI agents. Project conventions live in
`AGENT.md`; this file only records how to deliver work.

## Delivery

- **Every finished task ends with a pull request against `develop`.** Do not
  wait to be asked. `develop` is the base branch (CONVENTIONS.md).
- Branch naming: `feat/phase-X.Y.Z-<slug>`, `fix/<slug>`, `docs/<slug>`.
- Commit messages: English, Conventional Commits, imperative summary.
  Author/committer `yasmramos <yasmramos95@gmail.com>`. **No AI attribution**
  of any kind (AGENT.md §5.11 and §7): no `Generated with ...`, no
  `Co-Authored-By:` bot trailers.
- One logical change per commit.
- Before opening the PR: build clean and the full CTest suite green
  (`ctest` in the build directory).

## Response language

- Reply to the user in **Spanish**. All repository artifacts (code, comments,
  docs, commit messages, PR titles/bodies) stay in **English**.

## CI

- Two workflows: `cmake-multi-platform.yml` builds and runs the full CTest
  suite, and `artifacts.yml` builds the release artifacts. Both cover Linux,
  Windows and macOS — keep the three in step. A platform missing from the
  `cmake-multi-platform.yml` matrix passes CI while never being built, which
  is how a macOS-only break slips through.
- The `cmake-multi-platform.yml` matrix is `os` × `c_compiler` with
  `include`/`exclude` pinning one `cpp_compiler` per job. Adding an `os` means
  adding its real compiler to `include` *and* excluding the impossible pairs,
  or the product grows dead jobs.

## Release and artifacts

- `.github/workflows/artifacts.yml` builds the engine on Linux, Windows and
  macOS, packages the VS Code extension plus a per-platform installable
  (`packaging/windows/jolt.iss`, `packaging/linux/package-linux.sh`,
  `packaging/macos/package-macos.sh`), uploads them as artifacts, and publishes
  a GitHub Release with a `SHA256SUMS` file when a `v*` tag is pushed. Only a
  tag releases; branch pushes and pull requests stop at the artifacts.
- The engine ships as `bin/jolt` + `share/jolt/std/*.jolt`; `standardLibraryDir`
  finds the standard library through `../share/jolt/std` relative to the
  executable, so `import std.math` resolves from any unpacked archive with no
  environment variable. Every packaging format keeps that layout:
  `/usr/bin/jolt` + `/usr/share/jolt/std` (deb, rpm) and `/usr/local/bin/jolt` +
  `/usr/local/share/jolt/std` (pkg, dmg).
- Version comes from the tag, or from `CMakeLists.txt` on a branch. Pre-release
  tags need per-format handling: RPM rejects a `-` in `Version`, and GitHub
  rewrites `~` to `.` in asset names (which breaks `SHA256SUMS`), so the `.deb`
  file name avoids `~` while its control file keeps it.
- `macos-latest` is the Apple Silicon image (arm64). The dev engine is portable
  C++; the native backend stays x86-64 Linux only (AGENT.md PHASE 7), and its
  execution tests skip on other platforms.
- To verify the release path without a real release, push a throwaway tag
  (`v0.1.0-<something>test`), confirm the assets and `sha256sum -c SHA256SUMS`,
  then delete the release *and* the tag. A failed run can leave a draft release
  behind; delete that too. Do not leave test tags or drafts.
- Inno Setup ships on the Windows runner but is not on `PATH` and its version
  appears in its directory name, so the workflow locates `ISCC.exe` instead of
  hard-coding the path. `dpkg-deb`/`pkgbuild`/`hdiutil` are on their images;
  `rpmbuild` is installed on demand and the `.rpm` is skipped, not failed, if it
  cannot be.
