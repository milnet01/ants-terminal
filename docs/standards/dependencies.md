# Dependency Version Standard (ANTS-3427)

> **Status:** Adopted 2026-07-03. Applies to every dependency the project
> pulls in: runtime libraries, build tools, the test framework, CI actions,
> CI runner images, and container base images.
>
> **Source:** user standing rule 2026-07-03.

This standard is the project-specific mechanism for the global
`~/.claude/CLAUDE.md` §5 rules (use the latest external-library version +
current idioms; §5a runtimes/CI tooling; §5b bump-and-update-callers
together; §5c sweep posture). §5 states the philosophy; this file adds the
**enforcement mechanism**: a documented ledger for any dependency we hold
*below* latest, so a stale pin is visible in one place rather than buried in a
manifest. Enforcement is **manual** (§5 sweep + code review) — no CI/audit
rule yet asserts every below-latest pin carries a ledger row, so the
discipline rests on the author; automating that check is tracked as
**ANTS-3428**.

---

## 1. Policy — latest stable by default

Every dependency tracks the **latest stable upstream release** — pursued for
**both** new features **and** security fixes (a security patch is reason
enough to bump, independent of any feature need).

- "Latest stable" = the newest non-prerelease upstream release (or the newest
  patch within a supported major line where the project deliberately holds a
  major — see §4 floors).
- The rule fires at three moments:
  1. **On add** — a new dependency enters at its latest stable version.
  2. **On manifest-adjacent work** — touching `CMakeLists.txt`, a CI workflow,
     or the container base referenced inline in `tools/ci-parity.sh` for any
     reason means glancing at the version pins you pass (§5c "check on your
     way past").
  3. **On the periodic sweep** (§5) — at the start of a release cycle.
- A bump updates the calling code / idioms in the **same change** (global §5b).
  A patch-only bump with no caller change is fine — say so explicitly in the
  commit ("no caller changes — patch only").

## 2. The only exception — a documented downgrade

Hold a dependency **below** its latest release **only** when **both** are true:

- **(a)** the newer version *explicitly* breaks a feature, the build, or a
  test — observed, not assumed; **and**
- **(b)** there is no reasonable workaround (a code change, config, or idiom
  update that keeps us on latest).

When both hold, the pin **MUST** be recorded in the **Downgrade Ledger** (§3)
in the same change that introduces it. **An undocumented below-latest pin is a
defect** — it reads as neglect, not intent, and it strands us on a version
whose breakage no one remembers.

This is deliberately narrow. "It's more convenient", "I haven't tested the new
one", or "it works, why touch it" are **not** grounds for a pin — they are
grounds to test the new one (§5) or to open a ledger row only once (a) is
actually observed.

## 3. Downgrade Ledger

The authoritative list of every dependency held below latest, **and why**.
Each row carries a **re-test trigger** so a later upstream release that might
fix the breakage is actually re-checked — not silently pinned forever.

| Dependency | Pinned at | Latest tested | First broken version | What breaks (symptom + link) | Logged | Re-test trigger |
|---|---|---|---|---|---|---|
| _(none — no active downgrades)_ | | | | | | |

*Column note — **"Latest tested"** is the newest version you have actually
re-tested and found **still broken** (it starts equal to *First broken
version* and only advances; it is never the last *working* version). It drives
the re-test trigger.*

**Row lifecycle:**
- **Add** a row the moment a below-latest pin lands (with the pin).
- **Re-test trigger** is always of the form: *"when upstream ships a version
  `> <Latest tested>`, re-test the broken feature."* It keys off the
  **latest version you have actually tested** (the *Latest tested*
  column), **not** the first-broken one — so the trigger advances on each
  re-test instead of re-firing forever on a version you already checked. If the
  newer release fixes it → bump to latest, update callers, **delete the row**.
  If it still breaks → update *Latest tested* to the newly-tested version
  (which also advances the trigger) and note it in the symptom cell; leave
  *First broken version* unchanged (it records the *first* version observed to
  break, kept for history).
- A row with no re-test trigger is malformed.

> Example (illustrative — not an active pin). Pinned at 2.3.1 because 2.4.0
> (the current latest) broke us; the trigger keys off *Latest tested* — here
> identical to the first-broken 2.4.0, since no newer release has been
> re-tested yet — so any release after 2.4.0 prompts a re-test:
> `| foolib | 2.3.1 | 2.4.0 | 2.4.0 | 2.4.0 dropped the sync API we depend on (upstream #1234) | 2026-07-03 | re-test when foolib > 2.4.0 ships |`

## 4. Minimum-supported floors — NOT downgrades

A **floor** is the *oldest* version we still support and build against. It is
**not** a below-latest pin: we still build and run against **latest** too (dev
boxes and the primary CI jobs use current versions). A floor exists for reach
(distro/LTS coverage), so it does **not** get a Downgrade Ledger row.

| Floor | Where | Why | Guard |
|---|---|---|---|
| Qt6 ≥ **6.2** | `find_package(Qt6 6.2 …)` in `CMakeLists.txt` | First LTS shipping the full component set we need — DBus stabilised in 6.2, OpenGLWidgets reached parity (the comment block above that `find_package`); 6.0/6.1 compile but silently drop DBus / trip shader edge cases | `ci.yml` `qt62-baseline` job (compiles on Ubuntu 22.04 / Qt 6.2.x); `tools/ci-parity.sh --qt62` mirrors it in a podman container |
| Lua **5.4** _(optional)_ | `pkg_check_modules(LUA lua5.4)` / `find_package(Lua 5.4 QUIET)` | Current stable Lua line; when present the plugin ABI targets 5.4 | `find_package(Lua 5.4 QUIET)` — **not** REQUIRED; absent Lua compiles the plugin system out (no hard build floor), so this is a *conditional* floor |
| C++ **20** | `set(CMAKE_CXX_STANDARD 20)` | Language baseline for the codebase | compiler |
| CMake ≥ **3.20** | `cmake_minimum_required` | Project-mandated build baseline (`set(CMAKE_OPTIMIZE_DEPENDENCIES ON)` needs ≥ 3.19; the mandate rounds up to 3.20) | `cmake_minimum_required` |
| GoogleTest ≥ **1.13** | `find_package(GTest 1.13 QUIET)` | `gtest_discover_tests` `DISCOVERY_MODE PRE_TEST` semantics | system `find_package`; FetchContent `v1.15.2` fallback when no system pkg ≥ 1.13 (§7) |

If a floor ever needs *raising* (e.g. dropping Qt 6.2 support), that is a
deliberate decision with its own ROADMAP entry — not this ledger.

## 5. Sweep posture

Check, don't wait for breakage (global §5c). Run at the **start of a release
cycle**, and opportunistically when editing a manifest. Per dependency type:

| Type | How to check latest |
|---|---|
| Qt / system libs | `zypper info <pkg>` / distro repos + upstream release notes |
| Lua | upstream `lua.org` release line |
| GoogleTest (the `FetchContent_Declare(googletest …)` `GIT_TAG`) | compare the pinned `GIT_TAG` against `github.com/google/googletest` releases |
| CI actions (`.github/workflows/*.yml`) | `gh api repos/<owner>/<repo>/releases/latest -q .tag_name`; bump the pinned **SHA** *and* its `# vX.Y.Z` comment together (§6) |
| CI runner images (`runs-on:`) | GitHub's runner-image release notes (`ubuntu-24.04` → next LTS). **Caveat:** the `qt62-baseline` job's `ubuntu-22.04` is *not* a stale pin — it mirrors the §4 Qt 6.2 floor. Do **not** bump it in the runner sweep; it moves only if the Qt 6.2 floor itself is deliberately raised. |
| Container base (`tools/ci-parity.sh` `qt62_image`) | keep in lockstep with the `qt62-baseline` `runs-on:` Ubuntu version it mirrors |
| LayerShellQt (`find_package(LayerShellQt CONFIG QUIET)`) | optional `CONFIG`-discovered dep, no version pin — sweep is present-or-absent only (nothing to bump) |

On any bump: apply §5b (update callers/idioms in the same change) and re-run
`tools/ci-parity.sh --full` before pushing.

## 6. Where the versions live (map)

Keep this list current — it is the checklist for a sweep. This section is the
**authoritative** version-location map, and it cites by **directive name only**
per `documentation.md` § 1.7: every pin below is a `grep` for the directive, so
the map survives any edit that shifts a line. It previously carried `(L…)` hints
and every one of them had drifted by 2026-07-30 — `find_package(Qt6 …)` was
cited at L60 and sat at L86, Lua at L65/L69 and sat at L91/L95, `LayerShellQt`
at L83 and sat at L109 — which is the argument for the rule, not against the
map.

- **`CMakeLists.txt`** — `cmake_minimum_required`, `project(… VERSION)`,
  `set(CMAKE_CXX_STANDARD …)`, `find_package(Qt6 6.2 …)`; Lua is the
  `pkg_check_modules(LUA lua5.4)` / `find_package(Lua 5.4 QUIET)` pair;
  GoogleTest is the `find_package(GTest 1.13 QUIET)` system floor plus the
  `FetchContent_Declare(googletest …)` block's `GIT_TAG`.
  `find_package(LayerShellQt CONFIG QUIET)` is an optional Wayland dep with no
  version floor — nothing to pin, listed here only so a sweep knows it exists.
- **`.github/workflows/ci.yml` + `release.yml`** — CI action SHAs (each pinned
  by 40-char SHA with a `# vX.Y.Z` human comment — a mutable tag is never
  trusted; cf. the `tj-actions/changed-files` 2025 incident), `runs-on:`
  runner images, the apt Qt/Lua package sets.
- **`tools/ci-parity.sh`** — the `qt62_image` container base + its apt set
  (kept in lockstep with `ci.yml`'s `qt62-baseline` step).

## 7. Current sweep candidates (as of 2026-07-03)

Flagged during the 2026-07-03 adoption sweep; not yet actioned (each is its
own change, gated on the suite staying green):

- **GoogleTest `v1.15.2`** (the `FetchContent_Declare(googletest …)` `GIT_TAG`) — from 2024; verify against
  the latest upstream release and bump the `GIT_TAG` if the suite stays green.
- **`actions/cache@v5.0.5`** (`ci.yml:74`, `:191`) — the workflow already
  carries an in-file "bump when actions/cache > v5.0.5 ships" note (Node-20 →
  Node-24 runtime). Bump when a newer release lands.

These are **not** Downgrade Ledger rows — they are "behind latest, not yet
swept", the ordinary state the sweep exists to close. A candidate only becomes
a ledger row if a bump is *attempted* and the new version is found to break
something (§2).

---

**Cross-references:** global `~/.claude/CLAUDE.md` §5 / §5a / §5b / §5c;
[`commits.md`](commits.md) (the `chore:` dep-bump commit type, `commits.md:45`);
the pinned-SHA supply-chain note at the top of `.github/workflows/ci.yml`.
