# Project Standards

**The four shared standards are owned globally at `~/.claude/standards/`.** The
files below with the same names are **deltas**: a pointer to the global owner
plus the Qt/C++/Ants-specific rules that cannot live in a language-agnostic
standard. Read the delta for what this project adds.

Each of them then **mirrors its owner verbatim below a divider**, between
`<!-- MIRROR BEGIN … -->` and `<!-- MIRROR END -->`. That half exists because
this repo is PUBLIC: an outside contributor reading `coding.md` on GitHub
cannot open a path inside a private home directory, so a pointer alone left
the rule simply absent for them (ANTS-4133). **Do not edit a mirrored half** —
a correction goes upstream, then `tools/check-standard-mirrors.sh --write`
re-copies it down.

**This table names every file in this folder**, and
`tools/check-standards-index.sh` fails when it stops doing so (ANTS-4762). It
had quietly omitted several, four of which were named in no other document
either — so a session could breach a standard it had no way to learn existed.

**A link inside a mirrored half points at the owner's siblings, not at this
folder's.** The rule that settles which of them this repo carries (ANTS-4761):
**mirror every top-level global standard that a mirrored half links to.** That
rule converges — the mirrored set is closed under its own links, so adding a
file cannot cascade indefinitely. `roadmap-format.md` and `dependencies.md`
satisfy it already, as files this repo owns outright.

**`tools/check-standard-mirrors.sh` enforces it** and fails on an unresolvable
link out of a mirrored half, so the set cannot rot the next time the owner
gains a sibling. It exempts the classes the rule deliberately leaves behind: a
skeleton (`skeletons/`), which is a template to copy rather than a rule to
conform to; a target outside `standards/` altogether (`../workflow.md`), whose
owner is a foundation document that nothing mirrors; and `README.md`, which
resolves to *this* index rather than the global one it means (ANTS-4138).
`languages/` is **not** exempt — ANTS-4764 brought those files into the mirror
set, so a link to one must resolve.

**An exempt link does not resolve for a reader on GitHub, and `doc_integrity`
reports it.** Both tools are right; they ask different questions. The gate
names each exempt target and its reason rather than passing over it silently
(ANTS-4825). None of it can be fixed by editing a mirror, since a mirror that
differs from its owner is what the gate exists to refuse.

That arrangement dates from 2026-08-12. Until then these were verbatim
`/start-app` copies kept in sync by discipline, with nothing checking them.
When they were finally reconciled all four had drifted, and three instructed
behaviour the current global standard forbids — a `git push --tags` that
publishes every local tag, a prove-your-test recipe whose green run skipped
the rebuild and so could report a false pass, and a naming rule that mandated
camelCase for this repo's Python. **An unmarked copy that nothing checks does
not belong here.** A marked one with a drift gate is the opposite failure
mode, not the same one: `tools/hooks/pre-commit` refuses a commit whose mirror
no longer matches its owner.

| Delta file | Global owner | What the project file adds |
|----------|--------|--------|
| [coding.md](coding.md) | `~/.claude/standards/coding.md` (+ [`languages/cpp.md`](languages/cpp.md), [`qt.md`](languages/qt.md), [`python.md`](languages/python.md)) | House style (`s_` statics, K&R, `#pragma once`, signals/slots never sibling calls) and `setOwnerOnlyPerms()` in `src/secureio.h`. |
| [documentation.md](documentation.md) | `~/.claude/standards/documentation.md` | § 1.8 the `doc-examples` marker contract, § 3 ROADMAP/CHANGELOG routing, § 7 Qt accessibility, § 9 the fold-in heading. Section numbers are preserved for inbound links. |
| [testing.md](testing.md) | `~/.claude/standards/testing.md` (+ [`languages/cpp.md`](languages/cpp.md)) | The corrected prove-a-test-is-real recipe, audit-rule fixtures, the real label set, the bundle-target trap. |
| [commits.md](commits.md) | `~/.claude/standards/commits.md` (+ `releases.md`) | This repo is public; the `ci-parity.sh` / pre-push gate; `cut-rc.sh` releases; `cut-release --bump-only`. |

**Some files here are MIRRORS only, with no delta** — the owner and nothing
else, under the same markers and the same drift gate as the deltas above:
[`security.md`](security.md), [`releases.md`](releases.md),
[`local-gate.md`](local-gate.md),
[`changelog-format.md`](changelog-format.md),
[`versioning.md`](versioning.md) and [`spec-format.md`](spec-format.md).
Project-specific security rules go in `coding.md`, not in the first of them.

`security.md` is carried because this project is bound by it directly. The
rest arrived with the rule above: each is linked from a mirrored half, so
without a copy those links dead-ended for exactly the reader the mirrors
exist to serve.

**`languages/` is mirrored too** (ANTS-4764) — the `coding.md` table above
names those three files as owning this project's C++, Qt and Python
spellings, so a reader who could not open them was missing the rules that
decide how the code is actually written. They are the one place the mirror
set reaches below the top level. **A skeleton is not mirrored**: it is a
template to copy rather than a rule to conform to, and this project owns
spec shape itself in `specs.md`. Nor is a path that leaves `standards/`.

**Two files here are NOT deltas, deliberately.** `roadmap-format.md` is
**upstream of** its global copy (CFG-0069, 2026-08-12) — the parser, the store
and the migration live here, so where the two disagree this one governs.
`specs.md` is a full standard; its § 0 records why.

Sub-spec (upstream of the global copy — see above):

| Sub-spec | Covers |
|----------|--------|
| [roadmap-format.md](roadmap-format.md) | Detailed `ROADMAP.md` and `CHANGELOG.md` format spec — file-header marker, status / theme emojis, stable IDs (`PROJ-NNNN`), insertion semantics, `Kind:` / `Source:` taxonomy, current-work signaling, fold-in subsections, anti-patterns. Read when authoring either file or any tooling that consumes them. |

Project-local standards (not part of the shareable `/start-app`
template):

| Standard | Covers |
|----------|--------|
| [dialogs.md](dialogs.md) | Dialog convention for every `QDialog` — theme conformance via `DialogChrome`, user-resizable, size persisted to `Config`, always re-centered over the terminal window on open (D1–D4). |
| [mcp-tools.md](mcp-tools.md) | Ordered authoring checklist for adding a tool to the Ants MCP surface — registration, caller-cwd contract, path validation, response wrap, refusal codes, ETag / `fields=` opt-ins, cache contract, required tests. Umbrella over mcp-error-codes.md + mcp-caches.md. |
| [status-bar.md](status-bar.md) | Status-bar widget convention specific to this codebase's `MainWindow` + `ClaudeStatusBarController` architecture. |
| [audit-false-positives.md](audit-false-positives.md) | False-positive ledger (`.ants_review_falsepos.jsonl`) shared across the `/audit`, `review-contract`, `/code-quality-review` and `/test-audit` sweep skills — schema, CC write contract, MCP read contract. |
| [mcp-caches.md](mcp-caches.md) | Keying + relocation contract for every MCP cache (ANTS-1439). Invariant: a path-keyed cache may go cold/orphan on project move but must never shadow. Inventory table + "adding a new cache" checklist. |
| [mcp-error-codes.md](mcp-error-codes.md) | Canonical taxonomy for the `code` field on MCP refusal envelopes (ANTS-1353). Five categories: input validation, resource state, caller-cwd contract, I/O, dispatcher. |
| [specs.md](specs.md) | Spec-authoring standard for `docs/specs/ANTS-NNNN.md` (ANTS-1728): required structure, INV-N bullet form, grounding/RAM/security conventions, `spec_query` machine-readability contract. |
| [test-audit-resume.md](test-audit-resume.md) | Resume recipe for picking up a partially-completed `/test-audit` in a follow-up session — `partition_token` is in-process LRU, not durable; the recipe covers the explicit `session_memory` round-trip and the partition-re-run fallback (ANTS-1580). |
| [mcp-feedback-files.md](mcp-feedback-files.md) | Format spec for the cross-session `*_Ants_MCP_Feedback.md` files that other CC sessions use to report MCP issues — contributor/maintainer roles, the maintainer-block watermark anchor, and the un-triaged-delta parser contract the `feedback_query` verb (ANTS-1961) consumes. |
| [mcp-behavioural-notes.md](mcp-behavioural-notes.md) | Per-verb behavioural reference for the MCP surface — what an individual verb actually does, as against how to author one (that is `mcp-tools.md`). Relocated out of the always-loaded `CLAUDE.md` preamble by ANTS-2088, so it is read on demand. |
| [mcp-config-keys.md](mcp-config-keys.md) | Config-file / Settings keys for the Ants-MCP integration (ANTS-3429) — the `claude.mcp_enabled` master gate, the feedback corpus root, the autonomous model switcher, result offload, tabular encoding, the advisory-hint latch and `project_query`. |
| [roadmap-data-model.md](roadmap-data-model.md) | The roadmap store's data model (ANTS-3753) — the three artifacts, what an item must carry at write and before publish, and what migration accepts from historical items. Partly implemented; its own status block says which parts have shipped. |
| [ci-build.md](ci-build.md) | What the CI workflows must guarantee about a release — B1, that CI builds on the release's Qt / runner baseline, and the corollary that the release tooling gates on CI rather than on a local build. |
| [config-hot-reload.md](config-hot-reload.md) | Contract between the `config.json` file watcher and the in-app writers sharing that file — C1, an in-app write must not trigger the external-reload reaction, and C2, what a new config-writing component inherits for free. |
| [menus.md](menus.md) | Menu-bar convention (M1–M3) — a checkbox toggle keeps its menu open where a radio pick closes it, menus are themed by the app stylesheet cascade rather than per-menu, and actions carry a mnemonic. |
| [versioning-overrides.md](versioning-overrides.md) | The two answers the global versioning standard refuses to supply for a `0.x` project — its § 3 breaking surfaces, and its § 4 one-line `1.0` exit condition. Also carries this project's one override: inside `0.x` the MINOR is the milestone rather than a fact about breakage, so read it before choosing a release's level. |

## Historical / superseded

| File | Notes |
|------|-------|
| [mcp-errors.md](mcp-errors.md) | Earlier (2026-05-12) draft of MCP error-code families. Superseded by `mcp-error-codes.md` (ANTS-1353) — kept as a historical breadcrumb only. Do not cite in new code or specs. |

## How they fit together

The standards plus `ROADMAP.md` form a closed loop (§-numbers below are the
**global** files' unless the link points into this folder):

1. **ROADMAP item** declares an `[ID]`, `Kind:`, and `Source:`
   (per [roadmap-format § 3](roadmap-format.md)).
2. **Implementation** follows the standard for that Kind:
   - `implement` / `fix` / `refactor` → [coding.md](coding.md)
   - `doc` / `doc-fix` → [documentation.md](documentation.md)
   - `test` → [testing.md](testing.md)
   - `chore` / `release` → global `releases.md` + [commits.md](commits.md)
3. **Tests** follow [testing.md](testing.md) — TDD by default.
4. **Commit** uses `<ID>: <description>` per global `commits.md` § 1.1.
5. **CHANGELOG** entry under `[Unreleased]` cites the ID per
   [roadmap-format § 4.2](roadmap-format.md).
6. **Release** flips the ROADMAP bullet from 🚧 to ✅, moves the
   `[Unreleased]` entry to a dated section per
   [roadmap-format § 4.3](roadmap-format.md).

Every step has a single owner and a single source of truth — no
rules buried in commit messages, no conventions inferred from
existing code, no "ask the original author".

## Adopting these standards in another project

**Read `~/.claude/standards/` in place; that is what it is for.** This folder
previously told you to copy the four files verbatim and keep them in sync by
hand, and this project is the worked example of why that fails: the copies
drifted for three months, nothing noticed, and three of them ended up
instructing behaviour the owner forbids.

If a project genuinely needs to differ, write a **delta** like the four above —
a pointer to the global owner plus only what is specific to that project — and
say in its header why the difference exists. A delta that grows back into a
restatement of its owner has become a second standard.

**Mirror the owner only if the project is public**, and only under the marker
pair with a drift gate wired in the same change. The distinction that makes it
safe is not the copying; it is that a machine, not a person's memory, notices
when the copy stops matching. A mirror without a gate is exactly the
arrangement above that failed.

## Versioning

Each standard carries a version marker in its first-line HTML
comment:

```html
<!-- ants-coding-standards: 3 -->
```

Future revisions increment the version number. Backwards-
incompatible changes (renaming a section, removing a Kind value,
adding a required field) require a major version bump. Additive
changes stay on the current version.

**The four delta files went to `: 2` on 2026-08-12** — the cut from
full copy to delta renames and removes sections, which is
backwards-incompatible by the rule above. **They went to `: 3` the
same day**, when the owner's text was mirrored back in below the
delta: the file gained a whole second half, and a reader who had
bookmarked a section anchor now finds two documents under one path.
`security.md` stays at `: mirror` — it has no delta half to version.

**Exception — sub-specs.** `roadmap-format.md` uses
`ants-roadmap-format-spec: 1.1` rather than the standard
`ants-<name>-standards: N` pattern. The `spec` suffix signals
it tracks the *data-file format* (what conforming `ROADMAP.md`
files must look like), not an authoring guideline for
practitioners. The `1.1` subversion marks one additive revision
(the `Layman:` field). This is the only sub-spec exception;
all other files in this directory follow the `-standards: N`
form.
