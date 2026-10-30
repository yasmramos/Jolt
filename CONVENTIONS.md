# Project Conventions

These rules apply to every repository owned by `yasmramos` and are enforced in all
work performed from this workspace.

## 1. Git identity

| Setting | Value |
| ------- | ----- |
| `user.name`  | `yasmramos` |
| `user.email` | `yasmramos95@gmail.com` |

GitHub authentication uses HTTPS with a personal access token (stored in
`~/.git-credentials`, helper: `store`). Never print or commit the token.

SSH URLs are rewritten to authenticated HTTPS, so either form works for clones:

```bash
git clone git@github.com:yasmramos/<repo>.git        # rewritten to HTTPS + token
git clone https://github.com/yasmramos/<repo>.git
```

To attach this workspace to a repository:

```bash
git remote add origin https://github.com/yasmramos/<repo>.git
git fetch origin && git checkout -b develop --track origin/develop   # if it exists
git push -u origin develop                                            # if it does not
```

## 2. Branching model

* `main` / `master` — production-ready code only, never committed to directly.
* `develop` — **the working branch for all new work.** Every task starts here.
* `feature/<short-name>` — optional topic branches cut from `develop`, merged back
  into `develop`.
* `release/x.y.z` and `hotfix/<name>` — created from/to `main` when shipping.

Standard start of work:

```bash
git checkout develop          # create it if missing: git checkout -b develop
git pull --rebase origin develop
```

## 3. Commit message format (always English, Conventional Commits)

```
<type>(<scope>): <imperative summary, <= 72 chars, no trailing period>

<body: what and why, wrapped at 72 chars, optional>

<footer: BREAKING CHANGE: ..., Refs: #123, optional>
```

Allowed types: `feat`, `fix`, `docs`, `style`, `refactor`, `perf`, `test`,
`build`, `ci`, `chore`, `revert`.

Examples:

```text
feat(auth): add JWT refresh token rotation
fix(parser): handle empty input without throwing
docs(readme): document local development setup
chore(deps): bump vite to 5.4.2
refactor(api): extract retry logic into a helper
```

Rules:
* Summary line in imperative mood ("add", not "added"/"adds").
* One logical change per commit; stage selectively (`git add -p`).
* No messages in Spanish; no vague messages such as "wip", "fix bug", "updates".

## 4. Source code language: English only

Everything written must be in English:

* Identifiers: classes, functions, variables, constants (`userName`, not `nombreUsuario`).
* Comments: inline, block and doc comments.
* Documentation: README, guides, API docs, JSDoc/Javadoc/docstrings.
* User-facing strings, error messages and log messages.
* Test names and test descriptions.
* Commit messages, branch names, PR titles and review comments.

## 5. Code quality baseline

* Follow the idiomatic style of the language/framework in use; run the project's
  formatter and linter before committing.
* Keep functions small and single-purpose; prefer explicit over clever.
* Add or update tests alongside any behavioural change.
* No secrets, tokens or credentials in source files — use environment variables.
