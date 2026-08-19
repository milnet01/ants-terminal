# Feature: read_region markdown `section:` selector (ANTS-2221)

## Problem

Reading one section of a spec/ADR today means `file_outline` (headings +
line numbers), then hand-computing a `start_line`/`end_line` range. There was
no markdown analogue of symbol-mode for code (DOOM_Ants feedback S3).

## Contract

`read_region` gains a third selector, `section`, for `.md` files. Exactly one
of {line range, symbol, section} must be selected.

- **MD-1 body by slug** — `section:"4-2-emission-model"` returns the matching
  heading line through the line BEFORE the next same-or-higher-level heading.
  Echoes `section` + `section_slug`.
- **MD-2 body by heading text** — the slug is idempotent, so passing the
  heading text (`"4.2 Emission model"`) resolves to the same range as its slug
  (`"4-2-emission-model"`).
- **MD-3 subheadings included** — a `##` section that contains `###`
  subsections includes them; the section ends only at the next heading of the
  same or a higher level (≤ `#` count).
- **MD-4 last section to EOF** — a section with no following same-or-higher
  heading runs to end-of-file.
- **MD-5 fence-aware** — a `#` line inside a ``` / ~~~ fenced code block is not
  treated as a heading boundary.
- **MD-6 not found** — an unknown slug refuses with `section_not_found`.
- **MD-7 selector exclusivity** — `section` combined with a line range or
  `symbol` refuses with `bad_args`.
- **MD-8 wiring** — `cmdReadRegion` reads the `section` arg; the
  `read_region` inputSchema declares the `section` property.

### Forgiving short-title match (ANTS-2234)

A heading's short title is the natural key, but headings often carry a
trailing parenthetical. When no exact text/slug match exists, a
dash-bounded prefix (`<wantSlug>-…`) resolves the heading — but only when
it is unique.

- **MD-9 short-title prefix** — `section:"7. Build order"` resolves
  `## 7. Build order (cheapest-first; …)`; the echoed `section_slug` is the
  RESOLVED heading slug, not the input.
- **MD-10 ambiguous prefix** — when a short title prefixes ≥2 headings, refuse
  with `section_ambiguous` and a `candidates[]` array of the qualifying slugs.
- **MD-11 exact wins** — an exact slug match still wins even when it also
  prefixes a longer heading; the prefix fallback fires only on no exact match.
- **MD-12 full text back-compat** — the full parenthetical heading text still
  resolves exactly (the prefix logic must not regress the exact path).

- **MD-9 inline fence example (ANTS-3674)** — a heading following an inline
  code span that *demonstrates* a fence (```` ```cpp ```` on one line) still
  resolves. CommonMark forbids a backtick inside a backtick fence's info
  string, so such a span is not an opener. Section mode hand-rolled its fence
  tracking until this fix and read it as one, going blind to every later
  heading and refusing `section_not_found` on any document that teaches fenced
  code — reproduced live against `docs/specs/ANTS-3661.md`, where four
  headings `file_outline` listed were unreachable. Fence tracking is now
  `MarkdownScan::fenceMask`, the shared primitive, and this was the last
  hand-rolled tracker in the tree.

### ANTS-4520 — a heading inside a blockquote

`> ## Foo` refused `section_not_found` and the ANTS-4350 candidate list omitted
it, so a caller could not tell a missing heading from an unsupported shape and
doubted their spelling. Not exotic: a OneUp plan document uses blockquoted
headings as its standing convention for run-state blocks — one
`> ## You are here` above a stack of `> ## Previously` — and its session handoff
says to read the *You are here* block first. The one section a new session is
told to read was the one section mode could not address.

It degraded in the worst direction: the fallback is `workspace_search` for the
literal text then `read_region` with a **line range** — three calls instead of
one, and a line range is exactly the stale-anchor problem section mode exists to
remove. On a file that grows a new block at the top every session, a range
cached from one session is wrong by the next.

| Case | Asserts |
|---|---|
| `Ants4520BlockquotedHeadingResolves` | `> ## You are here` resolves by slug; a deeper quoted heading (`> ###`) is nested, and the next `> ##` ends the body. |
| `Ants4520QuotedSectionDoesNotSwallowPlainOne` | A less-deeply-quoted heading always terminates, so `> ## Previously` stops before the plain `## Plain section`; the plain heading's own span is unaffected. |
| `Ants4520FencedQuotedHeadingIsStillNotAHeading` | The fence mask wins — `> ## …` inside a ```` ```markdown ```` block is sample text (the ANTS-3674 defect must not return by a new route). |

The strip and the depth rule live in `MarkdownScan` (INV-14 there), shared with
`file_outline`'s md mode so the two cannot disagree.

## Out of scope

- **The CommonMark closer-LENGTH rule** — a 3-backtick line closing a
  4-backtick fence. `MarkdownScan::fenceMask` matches the closer on fence
  *character* only, so the rule is unimplemented one layer down and a row here
  would test the wrong component. Tracked as **ANTS-3678**, where one fix
  serves all six `MarkdownScan` consumers.
- Setext (`===` / `---` underline) headings — ATX (`#`) only, matching
  `file_outline`'s markdown heading detection.
- Slug disambiguation for duplicate headings — first match wins (line mode
  covers the rare collision).
