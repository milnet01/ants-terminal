# Feature spec: ANTS-5246 — `changelog_log op:"amend"`

`changelog_log` could only add. Correcting a phrase in a fresh `[Unreleased]`
entry needed a native read and edit of the whole file.

`op:"amend"` replaces one exact phrase under `## [Unreleased]`, and an empty
`new_text` deletes it. Released sections are frozen records, so the op cannot
reach them. There is no force switch: a released section is corrected by hand.

## Invariants under test

- **INV-1 / it replaces one phrase.** `old_text` occurring once under
  `[Unreleased]` becomes `new_text`, and the rest of the file is unchanged byte
  for byte. The reply says `changed:true` and names the 1-based `line`.
- **INV-2 / a phrase may span a hard wrap.** The match follows `WrapMatch`, the
  rule `roadmap_log op:"amend_body"` uses, and the reply says `wrapped:true`.
- **INV-3 / an empty `new_text` deletes.** A line left blank by the deletion is
  removed. A deletion leaving a bare `- ` refuses `empty_bullet`.
- **INV-4 / required inputs.** A missing or blank `old_text`, or a missing
  `new_text`, refuses `missing_field`.
- **INV-5 / a miss and an ambiguity both refuse and write nothing.** The codes
  are `text_not_found` and `text_ambiguous`.
- **INV-6 / `id` narrows the search.** With `id`, only entries citing that id
  are searched, which settles an ambiguity. An id on no entry refuses
  `id_not_in_unreleased`.
- **INV-7 / released sections are out of reach.** Text only in a released
  section refuses `text_not_found`. Passing `version` refuses `not_unreleased`.
- **INV-8 / `dry_run` writes nothing.** It reports the same `line` and `changed`
  as the real call, and the file's bytes stay the same.
- **INV-9 / the same text again changes nothing.** `new_text` equal to `old_text`
  reports `changed:false`.
