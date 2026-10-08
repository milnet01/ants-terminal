---
paths:
  - "CMakeLists.txt"
  - "CHANGELOG.md"
  - "packaging/**"
  - "README.md"
  - ".claude/bump.json"
---

## Versioning & release

- SemVer. **`project(... VERSION X.Y.Z)` in `CMakeLists.txt` is the single
  source of truth.** Never hardcode a version in `.cpp` / `.h`.
- Bump with `cut-release --bump-only`: it touches `CMakeLists.txt`, the
  `Version <strong>X.Y.Z</strong>` banner in `README.md`, and the packaging
  files in `.claude/bump.json`. Re-check README's prose each cycle, and change
  it only where a user-visible claim has drifted.
  `tools/check-readme-claims.sh` runs pre-push.
- CHANGELOG bullets under `[Unreleased]` are written as work lands. The
  version heading is written and dated by `packaging/release.sh release`,
  never by the bump.
- Update `PLUGINS.md` in the same commit as any `ants.*` Lua surface change.
- **Every release is a full public release, made when there is something
  meaningful to ship.** No release candidates and no cadence. The tag is
  `vX.Y.Z`; "rc" appears in no tag, title or file name.
- Flow: `cut-release --bump-only`, then `packaging/release.sh release --push`.
  Without `--push` it rehearses: it still builds and runs the feature tests
  (unless `--skip-build`), prints what the rest would do, and writes no file,
  commit or tag. With `--push` it merges `[Unreleased]`
  into the dated version section, commits, builds and tests, and pushes main.
  It then builds that commit on every distro in the OBS staging project,
  which publishes nothing, and waits for GitHub CI on it. It tags only if
  both are green. `release.yml`
  creates the GitHub release with its files attached. `release.sh status`
  shows where things stand.
- Before releasing, put a `**Theme:**` line at the top of `[Unreleased]`: a
  plain-language summary of what is new. `release` refuses without one. It
  becomes the GitHub release text, which the project website shows.
- `release` builds, and with `--push` also waits on OBS and GitHub: run it
  through `cc-job`, never under a short timeout, with or without `--push`. It
  is safe to re-run after a failure. After a killed ninja, run
  `ninja -C build -n` and `-t recompact` first.
- Before `release`, run `bash tools/check-shipped-coverage.sh`. It lists shipped
  items no CHANGELOG bullet cites and bullets that copy a headline, and exits
  non-zero on either, so keep it out of a `set -e` chain. Review each hit.
- **A CHANGELOG entry states what shipped, never the defect.** Do not copy a
  roadmap headline that states a problem. Prefer `changelog_log op:"add"` with
  an authored summary: `add_from_roadmap`, and an id-only `add_batch` entry,
  copy the headline.
