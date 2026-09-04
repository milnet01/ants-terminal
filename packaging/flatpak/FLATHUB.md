# Flathub submission checklist

Living playbook for submitting (and later refreshing) Ants Terminal on
[Flathub](https://flathub.org). Pairs with H6.2 in `ROADMAP.md`.

## Prerequisites

Before opening the submission PR, all of the following must be true:

1. **Latest release builds locally as a Flatpak**, with Lua plugins
   loading inside the sandbox. From repo root:
   ```bash
   # Uses the org.flatpak.Builder flatpak — Flathub's official build tool,
   # the same one the lint step below uses. Install once with:
   #   flatpak install flathub org.flatpak.Builder
   # (A system `flatpak-builder` binary works identically if you have one.)
   flatpak run org.flatpak.Builder --user --install --force-clean \
       build-flatpak packaging/flatpak/za.co.antsprojectshub.AntsTerminal.yml
   flatpak run za.co.antsprojectshub.AntsTerminal
   ```
   Inside the running Flatpak, confirm:
   - Host shell launches (bash/zsh/fish via `flatpak-spawn --host`).
   - `Settings → Plugins` shows the plugin manager UI (plugins enabled).
   - A small test plugin at `~/.config/ants-terminal/plugins/smoketest/init.lua`
     loads without error.

2. **Manifest lint is clean.**
   ```bash
   flatpak run --command=flatpak-builder-lint org.flatpak.Builder \
       manifest packaging/flatpak/za.co.antsprojectshub.AntsTerminal.yml
   ```

3. **Metainfo validates online.** (The `raw.githubusercontent.com`
   screenshot URLs must already be live on the `main` branch.)
   ```bash
   appstreamcli validate packaging/linux/za.co.antsprojectshub.AntsTerminal.metainfo.xml
   ```

4. **The full feature suite passes.**
   ```bash
   ctest --test-dir build -L features --output-on-failure
   ```

5. **Tag is pushed.** The Flathub manifest pins `tag: v<VERSION>` (the
   latest **stable** release — never an `-rcN` prerelease); the tag must
   exist on GitHub before the PR goes in or CI cannot check out the source.
   ```bash
   git tag v<VERSION> && git push origin v<VERSION>   # e.g. the current stable v0.7.97
   ```

## Opening the Flathub PR

> **A person opens this PR, writes its body, and answers its review. Not an
> agent.** Flathub's Generative AI policy, in its own words: *"AI tools or
> agents must not open or automate Flathub submission pull requests, or
> generate their commit messages, descriptions, review comments, or replies.
> Submitters must not request AI-agent reviews."* The penalty escalates to
> *"a permanent ban from future submissions and activities"*. The checkbox
> enforcing it landed in Flathub's PR template on 2026-09-03 — after
> finbreak's submission, so that one was not in breach when it was made, but
> any further comment on it is covered.
>
> An agent may prepare the packaging — manifest, metainfo, desktop entry,
> icon — and verify it builds and lints. That material is permitted, and the
> template's *other* AI checkbox requires it be **disclosed**, naming the
> affected parts and approximate extent. The two checkboxes are different
> questions; do not answer them the same way.

1. **Fork** [`flathub/flathub`](https://github.com/flathub/flathub) and
   branch off `master`. The PR's **base branch must be `new-pr`**, not
   `master` — the template says so in its first line, and a submission
   against `master` is rejected.

2. **Generate the submission carriers.** They go at the **repo root** of the
   fork branch, not in a per-app subdirectory:
   ```bash
   D=/path/to/flathub-fork
   # Pin the newest STABLE tag — never an -rcN. Pass it explicitly: the
   # auto-detect reads CMakeLists.txt, which under the RC cadence names the
   # NEXT unreleased version.
   packaging/flatpak/make-flathub-manifest.sh <VERSION> \
       > "$D/za.co.antsprojectshub.AntsTerminal.yml"
   # metainfo via the generator so the unreleased "Patron RC preview"
   # <release> placeholder is stripped (shipped stable leads the list):
   packaging/flatpak/make-flathub-manifest.sh <VERSION> --metainfo \
       > "$D/za.co.antsprojectshub.AntsTerminal.metainfo.xml"
   cp packaging/linux/za.co.antsprojectshub.AntsTerminal.desktop "$D/"
   cp assets/ants-terminal-256.png "$D/za.co.antsprojectshub.AntsTerminal.png"
   ```
   Verified on finbreak's submission: `flatpak-builder-lint manifest` exits 0
   standalone, i.e. with the manifest at a repo root and no `packaging/`
   tree beside it.

3. **FILL IN Flathub's PR template. Do not replace it.** This is the single
   most common way a submission dies: the submission-checker bot auto-closed
   finbreak's PR **27 seconds** after it opened — 13:14:46Z to 13:15:13Z —
   with *"Checklist(s) not completed or missing"*, because the body had been
   replaced with a hand-written description instead of the template with its
   boxes ticked. Open the PR compose view, keep every line the template
   gives you, and replace each `[ ]` with `[X]` as it is satisfied. An item
   that does not apply is ticked with `N/A` **and a reason**.

   Items that need a human and cannot be prepared ahead:
   - **A video showcasing the app running as a Flatpak on Linux.**
   - **The AI-disclosure line**, naming the affected parts and extent.
   - **The author/developer/upstream-contributor declaration.**

   `packaging/flatpak/flathub-submission/PR_BODY.md` is **not** a PR body and
   must never be pasted as one — it is exactly the shape that got finbreak
   auto-closed. Treat it as source material: the app description and the
   three permission justifications are worth pasting into the template's
   description slot and into a follow-up comment respectively.

4. **If the bot closes it anyway, comment — do not reopen or re-open a new
   PR.** The bot's own message says so: *"please post a comment below instead
   of opening or reopening (new) PRs."* A second PR while the first stands
   reads as PR-spam to the reviewers. Only a maintainer can reopen.

5. **After merge:** Flathub provisions a new repo at
   `flathub/za.co.antsprojectshub.AntsTerminal` with the submitted files.
   Future version bumps + Lua tarball refreshes go there, not here —
   `flatpak-external-data-checker` opens those PRs automatically. Accept the
   repo-write invitation **within one week**, with 2FA enabled on the GitHub
   account; both are Flathub requirements.

## On every subsequent release

For each `v<VERSION>` tag after the initial Flathub landing:

1. Push the tag on this repo.
2. Clone the downstream `flathub/za.co.antsprojectshub.AntsTerminal` repo.
3. Regenerate the manifest body:
   ```bash
   packaging/flatpak/make-flathub-manifest.sh \
       > /path/to/flathub-repo/za.co.antsprojectshub.AntsTerminal.yml
   ```
4. Re-copy the metainfo XML (release notes for the new version).
5. Open a PR against `master` in the Flathub repo. CI rebuilds; on
   green, merge — Flathub delivers the update to end users.

## Flip the ROADMAP on landing

Once the first `flathub/flathub` PR merges and the build shows up at
`flathub.org/apps/za.co.antsprojectshub.AntsTerminal`:

- Flip `H6.2` in `ROADMAP.md` from 📋 to ✅.
- Flip the "gating item 1: no distro packages anywhere" entry in the
  Distribution-adoption overview from "H5 + H6 unblock this" to
  "unblocked".
- Add a `## [<NEXT>]` section to `CHANGELOG.md` announcing the
  Flathub landing (user-facing: one-command install via
  `flatpak install flathub za.co.antsprojectshub.AntsTerminal`).

## Regression safety

- `tests/features/flathub_manifest_transform/` pins the transformer
  output shape — a regression in `make-flathub-manifest.sh` fails the
  test at `ctest` time, not at Flathub-PR time.
- `tests/features/flatpak_lua_module/` pins the Lua module shape in
  the dev manifest, which the transformer preserves byte-identical.
- `tests/features/flatpak_host_shell/` pins the `ptyhandler.cpp` host-
  shell detection — a regression that breaks the Flatpak shell would
  fail CI before anything reaches Flathub.
