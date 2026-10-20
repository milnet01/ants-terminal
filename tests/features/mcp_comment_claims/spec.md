# `comment_claims` — ANTS-5786

Code comments whose claims can be checked against the code. The shape was
agreed with claude-config (CFG-0830, 2026-10-19). The seam is
`src/commentclaims.{h,cpp}`; the file walk is `src/codewalk.{h,cpp}`.

Two buckets. `findings` are decided by the tool and go to check-code.
`candidates` need a reader's judgement and go to a readability lane. Each
row is `{id, file, line, kind, quote, why}`.

## Invariants

| # | Invariant | Test |
|---|---|---|
| 1 | Comments are `//` and `/* */` in C-family files, `#` in Python, shell, CMake and YAML files, and `--` in Lua. Text inside a string literal is not a comment. Consecutive line comments form one block. A directory walk reads the languages `codebase_index` indexes; a Lua, shell, CMake or YAML file is read when `paths` names it. | `CommentClaims.Inv1CommentsAreHarvested` |
| 2 | `missing_quotation` (finding): a double-quoted text of at least 12 characters which the existing file attributed to it does not contain. A file is attributed when its name sits beside the quotation with at most punctuation or one linking word between (`src/a.cpp says "…"`). Whitespace differences do not count; a quotation holding `...` is not checked. | `CommentClaims.Inv2QuotationMissingFromItsFile` |
| 3 | `dead_reference` (finding): `see X`, where X holds a `/` and no such file or directory exists under the root or the comment's directory, or X is `a::b`, or a `camelCase` or `snake_case` name written with `()` or in backticks, and no definition of its last part exists. A `see` inside a code span is not checked. Plain words, a bare name with neither, a bare file name, an all-caps name, and Qt, `std::`, `boost::` and `testing::` names are not checked. | `CommentClaims.Inv3DeadReference` |
| 4 | `unknown_ticket` (finding): an id with a prefix the roadmap uses, absent from `ROADMAP.md` and `docs/roadmap/*.md`. An id with another prefix is not checked. | `CommentClaims.Inv4UnknownTicket` |
| 5 | `census_number` (candidate): an approximate number (`~140`), an `N-line` size, or a number of 2 or more before a plural noun (`the 5 keys`). Dates, versions, ids and time units are not counted. | `CommentClaims.Inv5CensusNumbers` |
| 6 | A row's `id` is `file:line:kind:n`, with `n` counting rows of that kind on that line from 0, so it is the same on every run over the same text. | `CommentClaims.Inv6StableIds` |
| 7 | Tests and generated files are left out unless asked for. `limit` caps each bucket (default 500), and `scanned.rows_truncated` is true when it cut either; `scanned` holds the totals, the files and comments read, and how many symbols were not looked up because the walk was capped. | `CommentClaims.Inv7ScopeAndCounts` |
| 8 | A non-string `paths` entry or `limit` outside 1 to 5000 refuses `bad_args`. A path outside the root or missing refuses `bad_path`. | `CommentClaims.Inv8Refusals` |
| 9 | The verb is registered in `registerProjectScopedVerbs` with `CallerCwdContract::Required`, matches `callerCwdContractFor`, and its schema is a closed object. | `CommentClaims.Inv9Wiring` |
