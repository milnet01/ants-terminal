# Versioning overrides — Ants Terminal

Deltas only. Everything not named here follows
`~/.claude/standards/versioning.md` unmodified. It holds the two answers
that standard requires — § 3's breaking surfaces, asked of every
project, and § 4's `1.0` exit condition, asked of a `0.x` one — which
the global standards index pins to this path, **and one override of
§ 4's level rule**, below.

## The `1.0` exit condition

> **MAJOR stays `0` until [ANTS-1084] is shipped — the `ants.*` API
> carrying its stability pledge — and a second maintainer holds commit
> rights, the bus-factor half of [ANTS-1088].**

**The governance doc [ANTS-1088] also names is deliberately not part of
the gate.** The bar is commit rights. Cite the item for the bus factor
only; a `1.0` is not held for the document.

Both halves are observable by someone else, which is what § 4 asks for in
place of a judgement about maturity. Both are already items under
`ROADMAP.md` § 1.0.0.

The bus-factor half is not ceremony: distributions treat a
single-maintainer project as a risk, and this project is packaged for
several. Decided by the user 2026-09-04.

**[ANTS-1087] is not a gate.** Its own roadmap entry records the
retraction (2026-08-25) — no paid security review is committed or funded
— and says not to cite the item as a commitment. **The item is still
listed under § 1.0.0 as `planned`, so read the entry, not the status.**

## Override — what moves the MINOR inside `0.x`

**Global rule.** `versioning.md` § 4: inside `0.x` a breaking change
bumps the MINOR; everything else, a new capability included, bumps the
PATCH.

**This project.** Inside `0.x` the MINOR is the **milestone**: a release
completing one takes it, breaking or not. The PATCH is everything else —
fixes, breaking changes, and capabilities that complete no milestone.
Decided by the user 2026-09-04.

| Version | Milestone | The release is cut when |
|---|---|---|
| `0.8.0` | Multiplexing + marketplace | Every item under `ROADMAP.md` § 0.8.0 is shipped or moved out of the section |
| `0.9.0` | Platform + accessibility | Every item under `ROADMAP.md` § 0.9.0 is shipped or moved out of the section |
| `1.0.0` | Stability | The exit condition above |

**Why.** The global rule makes the MINOR a fact about breakage, which is
the right answer for something other code imports. A terminal emulator is
mostly used, not imported. Applied unmodified it produced a long run of
patch releases through work that changed what the thing *is*, with the
MINOR never moving and the version telling a user nothing on the way —
which is the inert leading zero § 4 warns about. The milestones were
already the plan and already in `ROADMAP.md`; this makes the number match
them rather than contradict them.

**What this costs.** A breaking change inside `0.x` no longer announces
itself in the version number. One thing carries it instead, and it is
required rather than encouraged: every breaking change is a
`### Changed` or `### Removed` entry in `CHANGELOG.md` that says, in its
first clause, what stops working. `releases.md` § 2 makes that section
the single description of what shipped, and asks the same of a security
fix for the same reason — someone on the previous version cannot decide
whether to upgrade if nobody tells them.

## Breaking surfaces

`versioning.md` § 3 asks each project to name its own rather than borrow
a list. Something breaks if a user, a plugin author, an integrator or a
packager who upgrades has something that used to work stop working. For
this project that is:

- **The `ants.*` Lua API**, and the plugin sandbox's limits. Plugins are
  written by other people; a plugin that stops loading is their work
  broken. `PLUGINS.md` is updated in the same commit that changes this
  surface.
- **`~/.config/ants-terminal/config.json`** — its keys, and the defaults
  of any key that changes behaviour someone relies on.
- **`<root>/.ants/project.json`** — its keys. This file is committed to
  other people's repositories, so a key that changes meaning breaks a
  project this one does not control.
- **The roadmap store's schema.** The store is machine-global, and
  `RoadmapStore::open()` refuses outright when the store's version
  exceeds the build's — so the first binary to upgrade it locks every
  older build out of every project in it. A `kSchemaVersion` bump is a
  one-way door and is breaking whatever `ALTER TABLE ... DEFAULT`
  suggests.
- **The MCP verb contracts** — a verb's name, its arguments, its refusal
  codes and the shape of its response envelope. Other Claude sessions and
  other projects call these.
- **The remote-control protocol**, including `--remote-json`, for the
  same reason.
- **The command-line interface** — flags and output shape both.
- **Key bindings**, and the action names they bind to. A binding people
  have in their fingers is a surface whether or not it is written down.
- **The session-persistence format.** A session that cannot be restored
  after an upgrade is work lost.
- **The audit ledger formats** — `audit_rules.json`, `.audit_suppress`
  and the false-positive ledger. Each is written by a user and read back
  later; a suppression that stops matching re-opens findings someone
  already dismissed.

## What is deliberately not a surface

Named because over-caution costs as much as carelessness. Each is a
judgement that nobody relies on it, not a limit on the rule below.

- **Internal C++ APIs between the subsystems.** Nothing outside the
  binary links to them.
- **The debug log's shape.** It exists to be read by a person.
- **A cache that can be rebuilt** — the codebase index and the
  roadmap-query bullet cache. Losing one costs a re-scan.

## A surface nobody wrote down is still a surface

`versioning.md` § 3's rule, repeated rather than cited because this list
is new. If a user relies on something and an upgrade stops it working,
that release was breaking whether or not this file names it — including
where the section above excludes it. § 3 says the list makes the common
cases cheap and may not bound the promise.

## What checks this

| Claim | What checks it |
|---|---|
| A release's level matches the milestone rule above | Nothing. `cut-release` does not choose the level, and its `Added`-forbids-a-PATCH floor is skipped while MAJOR is `0`. |
| The milestone table matches `ROADMAP.md`'s sections | Nothing. Both have to be changed together. |
| A breaking change reached the CHANGELOG | Nothing automated. The override above makes this the only carrier, so it rests on review. |
| The store schema is a one-way door | `RoadmapStore::open()` enforces the refusal itself. |

## Cold-eyes loop log

Kept in [`docs/reviews/versioning-overrides-review-log.md`](../reviews/versioning-overrides-review-log.md).
A standard carries rules; its review history is read far less often.
