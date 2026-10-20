# `duplicate_blocks` — ANTS-5785

Code written out more than once across the given files, including copies
that have drifted apart. A shortlist for `coding.md` § 1.3: it grades
nothing, and a reader confirms or dismisses each group by its id. The
shape was agreed with claude-config (CFG-0830, 2026-10-19). The seam is
`src/duplicateblocks.{h,cpp}`; the file walk is `src/codewalk.{h,cpp}`.

## What is compared

Each file becomes its compared lines: trimmed, with blank lines, comment
lines, lines of only brackets and punctuation, and include or import lines
dropped. Under `normalise_names`, identifiers, numbers and string literals
are each replaced by one placeholder, so a copy with renamed variables
matches.

## Invariants

| # | Invariant | Test |
|---|---|---|
| 1 | A run of at least `min_lines` compared lines (default 5) found in two or more places is one group: `{id, lines, copies, diverged, finding_shape, similarity, sites:[{file, start, end}], diff}`. `start` and `end` are the first and last line of the site; `lines` is the longest match in compared lines. | `DuplicateBlocks.Inv1ExactCopiesFormAGroup` |
| 2 | A match continues across up to three edited lines on either side while matched lines stay at least `similarity` (default 0.8) of the longer side. Such a group is `diverged`, and `diff` lists the edited lines of each side, `{file, line, text}`, at most 20. | `DuplicateBlocks.Inv2DivergedCopiesStayOneGroup` |
| 3 | Three copies are one group with `copies` 3, not three pairs. `finding_shape` is true when `copies` is 3 or more, or the group is `diverged` (coding.md § 1.3, the Rule of Three). | `DuplicateBlocks.Inv3ThreeCopiesAreOneGroup` |
| 4 | Without `normalise_names`, renamed copies are not matched; with it they are, and a group whose sites differ in raw text is `diverged`. | `DuplicateBlocks.Inv4NormaliseNames` |
| 5 | Two sites of one group never overlap in one file, and a line repeated over and over in one file is not a group. | `DuplicateBlocks.Inv5NoSelfOverlap` |
| 6 | `id` is `dup:` and the first 12 hex digits of the SHA-1 of the first site's compared text, so it survives a move of the code. Groups are ordered by `lines`, then `copies`, both largest first. `top` caps them (default 50). | `DuplicateBlocks.Inv6StableIdAndOrder` |
| 7 | Tests and generated files are left out unless asked for. `scanned` holds the files read, the lines compared, `groups_total`, and `rows_truncated`, true when `top` cut groups. | `DuplicateBlocks.Inv7ScopeAndCounts` |
| 8 | `min_lines` outside 2 to 200, `similarity` outside 0.5 to 1, `top` outside 1 to 500, or a non-string `paths` entry refuses `bad_args`. A path outside the root or missing refuses `bad_path`. | `DuplicateBlocks.Inv8Refusals` |
| 9 | The verb is registered in `registerProjectScopedVerbs` with `CallerCwdContract::Required`, matches `callerCwdContractFor`, and its schema is a closed object. | `DuplicateBlocks.Inv9Wiring` |
