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
