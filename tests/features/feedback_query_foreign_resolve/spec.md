# Feature spec: feedback_query resolves foreign-prefix mapped ids against the sibling roadmap that owns the prefix (ANTS-3519)

Follow-up to the ANTS-3518 `foreign_repo` honest-signal. Feedback files live at
the shared root (e.g. `/mnt/Games/Scripts/Linux`); the ANTS canonical roadmap
is a sibling subdir (`Ants_Terminal/ROADMAP.md`). ANTS-3518 marks a mapped id
whose prefix is absent from the caller roadmap as `"foreign_repo"` but does not
resolve it. ANTS-3519 goes one step further: for those foreign-prefix ids,
locate the sibling project whose roadmap owns the prefix and resolve the real
`status` / `shipped_date` from it — so "did my cross-project suggestion ship?"
works from any consumer project.

## Design

- **Shared root** = the directory containing the feedback file
  (`QFileInfo(feedback_path).absolutePath()`).
- **Ownership by prefix-scan** (not a hard-coded dir name): scan the shared
  root's immediate subdirs; for each, sniff its roadmap's counter prefix from
  the file HEAD (32 KiB, via `rlDetectCounterPrefix`) — cheap, no full parse.
  A subdir whose sniffed prefix matches a needed foreign prefix is the owner.
- **Cost gate**: the scan runs only when at least one mapped id is
  foreign-prefixed and unresolved by the caller roadmap.
- **RAM budget**: bounded — the owning roadmap is fully parsed at most ONCE per
  distinct foreign prefix, and only the mapped foreign ids are retained; the
  parse is discarded after the id→status map is built. No persistent cache.
- The caller roadmap (ANTS-3518) is skipped in the scan (`== roadmapPath`).

## Surface

- `mapped_id_status[]` entries for a resolved foreign id carry the real
  `status` emoji (📋/🚧/✅), a `resolved_from` field naming the owning sibling
  project's directory leaf, and `shipped_date` when ✅ with a parseable date —
  instead of `"foreign_repo"`.
- A foreign id whose prefix no sibling owns stays `"foreign_repo"`; the
  top-level `mapped_id_status_note` fires only while at least one foreign id
  remains unresolved.

## Invariants

- **INV-1 / resolve foreign id from sibling roadmap.** A consumer feedback
  file cites `ANTS-*` ids; the caller project's roadmap uses a different
  prefix; a sibling subdir owns the `ANTS` prefix. `feedback_query` resolves
  those ids to the sibling roadmap's live status (✅/📋), NOT `"foreign_repo"`,
  and stamps `resolved_from` = the sibling dir leaf. Behavioural via
  `cmdFeedbackQuery`.
- **INV-2 / shipped_date carried across repos.** A ✅ foreign id whose sibling
  bullet has a parseable `Resolved (YYYY-MM-DD)` body carries `shipped_date`.
  Behavioural.
- **INV-3 / no owner → stays foreign_repo (regression).** When no sibling subdir
  owns the foreign prefix, the id stays `"foreign_repo"` and the
  `mapped_id_status_note` still fires — the ANTS-3518 behaviour is preserved.
  Behavioural.
- **INV-4 / same-prefix caller ids unaffected (regression).** When the mapped
  ids share the caller roadmap's own prefix, no sibling scan runs and the
  ANTS-3478 caller-roadmap resolution (📋/✅/unknown) is unchanged. Behavioural.

## ANTS-4905 — a corpus in a directory of its own

Reported by claude_config: from `~/.claude`, all 64 mapped ids came back
`foreign_repo`, none with a status or a ship date. The field's whole
purpose is the "which of my prior suggestions shipped?" workflow, so for
every contributor it was uniformly uninformative — and ANTS-4741's
`possibly_stale_binary` comparison, which needs a status this path never
produced, could not fire either.

**The cause is topological, not a missing feature.** The resolver derives
the shared root from the feedback FILE's own directory and scans that
directory's subdirs for the owning project. That held while the corpus
WAS the shared root. On 2026-09-06 the corpus moved into
`Ants_MCP_Feedback_Files/` — which is what happens once a machine carries
two dozen feedback files — and that directory has no project subdirs at
all. INV-1 above kept passing throughout, because its topology is the
pre-move one.

### INV-4 — the corpus directory, then its parent

Search roots are ordered (nearer first) and deduped, so the pre-move
layout still scans one directory once. Ownership is still decided by the
head sniff: a sibling whose roadmap does not own the prefix resolves
nothing, and a prefix nobody owns stays `foreign_repo` with the
unresolved-foreign note.
