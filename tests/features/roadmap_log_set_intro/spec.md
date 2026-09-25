# Feature: roadmap_log writes a section's intro and the roadmap's preamble

## Invariants

**INV-1 — `set_intro` replaces a section's intro verbatim.** `section` names
the section and `new_text` is the intro. Indentation and blank lines inside
it survive the render. Trailing whitespace and leading or trailing blank
lines are trimmed. The envelope reports `replaced_intro_chars`.

**INV-2 — an intro holds no `#` to `###` heading.** A `new_text` line
matching `^#{1,3}\s` refuses `bad_intro` and nothing is written, because
the next import would read it as a new section. `####` and deeper are
intro text, which is what migration stores (ANTS-5373).

**INV-3 — `set_intro` never reaches the preamble.** A missing `section`
refuses `missing_field`, and the message names `set_preamble`.

**INV-4 — an unknown slug refuses `section_not_found` with `candidates`.**

**INV-5 — `dry_run` writes nothing.**

**INV-6 — `set_preamble` replaces the title and preamble.** It takes
`new_text` only and writes the live file's root intro. The published file
still opens on the format marker and carries one `# ` line.

**INV-7 — the preamble holds one title and no other heading.** A second
`# ` line, or any `##` to `######` line, refuses `bad_intro`.

**INV-9 (ANTS-5373) — `amend_intro` replaces one match.** It takes
`section`, `old_text` and `new_text`. `old_text` must occur exactly once in
the stored intro: none refuses `intro_match_not_found`, several refuse
`intro_match_ambiguous`, and an empty one refuses `missing_field`. The rest
of the intro is kept. The result is checked as INV-2 checks `new_text`.

**INV-10 (ANTS-5373) — a dry run echoes `previous_intro`,** the text
`replaced_intro_chars` counts.

All three ops are store-only.

**INV-8 (ANTS-4555) — every rendered file says it is generated.** The line
under the format marker is a comment naming the roadmap store and
`roadmap_log`. It appears once, including when a stored preamble already
carries it.

## Rationale

ANTS-4949, ANTS-4968, ANTS-4539, ANTS-4766 and ANTS-4832 report one gap. An
intro is written once, by the migration or by `create_section`. On a
store-backed project a hand edit to it is discarded by the next render. The
title is the root section's intro, which has an empty slug, so no
slug-keyed op can address it.

## Test surface

`test_roadmap_log_set_intro.cpp` migrates a fixture into a sandboxed store
and drives `RemoteControl::cmdRoadmapLogSetIntroForTest`.

## Regression history

- **ANTS-4949 / ANTS-4968:** no op wrote an intro or the preamble.
