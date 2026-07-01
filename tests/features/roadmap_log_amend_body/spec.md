# roadmap_log op:"amend_body" — patch a bullet's body prose in place

ANTS-3406. Cross-session request (finbreak, 2026-07-01): there is no
verb to correct a stale phrase inside an existing ROADMAP.md bullet's
continuation prose. `annotate` only *appends* a note; `flip` only
changes status. finbreak fell back to a raw `Edit` on ROADMAP.md.

## Problem

Add `op:"amend_body"` that locates a bullet (id / anchor / headline —
same locator rules as flip/annotate) and replaces an **exact,
single-line substring** of its continuation body with new text, guarded
so it cannot silently clobber unrelated text.

Scope decision: this pass ships the **exact-match patch** mode only
(`old_text` → `new_text`), which is finbreak's actual use case (fixing
a stale phrase) and the named deliverable in the ANTS-3406 headline.
Full-body-replace is deliberately out of scope — nobody requested it, it
carries a clobber risk, and it has a genuine ambiguity (what "the body"
means when a bullet has an internal blank line). It can be added later
under its own arg (`new_body`) if a real need appears.

The handler is a standalone `cmdRoadmapLogAmendBody`, NOT a new mode
threaded into the delicate `cmdRoadmapLogFlip` (ANTS-2059 history). It
reuses the free helpers (`walkGfmBullets` / `walkAntsV1Bullets`,
`rcScrubLeakedToolXml`, `rcFnv1a64` / `rcNormaliseHeadline`,
`rcGfmHeadlineMatchHashes`, the `QSaveFile` write) so `flip`/`annotate`
are untouched.

## Surface

Args: `caller_cwd` (req), `op:"amend_body"`, one locator
(`id`>`anchor`>`headline`), `old_text` (req, non-empty), `new_text`
(req — key must be present; may be empty to delete the phrase),
`dry_run` (opt), `return:"headline_only"` (opt).

Body span = the contiguous run of indented continuation lines after the
headline, terminated by a blank line, a column-0 line (next
bullet/heading), or EOF — the same span `appendBodyNote` targets. The
match is searched per-line within that span, so it can never leak into a
sibling bullet or touch the headline.

Success envelope: `{ok, op:"amend_body", format, file:"ROADMAP.md",
line (1-based bullet line), id?, body_line (1-based edited line),
bytes_written | (bytes + dry_run:true), amended:true,
note_scrubbed_params?, post_bullets?}`.

## Invariants

- **INV-1** — A unique `old_text` in the located bullet's body is
  replaced with `new_text`; the file is written atomically and the
  envelope reports `amended:true` + the 1-based `body_line`.
- **INV-2** — `old_text` absent/empty → `missing_field`; `new_text` key
  absent → `missing_field` (empty string is allowed = delete phrase).
- **INV-3** — `old_text` not present in the body span → `body_match_not_found`.
- **INV-4** — `old_text` present more than once in the body span →
  `body_match_ambiguous` (caller must narrow); the file is NOT modified.
- **INV-5** — `to_status` present → `bad_op_combo` (amend_body never
  changes status); headline alongside id/anchor → `bad_op_combo`.
- **INV-6** — `new_text` is scrubbed of leaked tool-call XML via the
  shared `rcScrubLeakedToolXml`; dropped `<parameter>` names surface as
  `note_scrubbed_params`.
- **INV-7** — `dry_run:true` returns the would-be edit (`dry_run:true` +
  `bytes`) and writes nothing.
- **INV-8** — works on both ants-v1 and GFM-task-list roadmaps; a
  pass-headings roadmap refuses `unsupported_format`; a bullet inside a
  fenced code block refuses `anchor_unsafe_context`.
- **INV-9** — schema advertises `amend_body` in the op enum and declares
  `old_text` / `new_text` props (`additionalProperties:false` requires
  them to be registered); the op descriptor documents amend_body.

## Tests

Behavioural (`RemoteControl::cmdRoadmapLogAmendBodyForTest` against a
seeded `QTemporaryDir` ROADMAP, mirroring roadmap_log_annotate) +
source-grep for the schema/descriptor surface (INV-9).
