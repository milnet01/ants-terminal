# ANTS-3492 — Digit-led (letter-containing) roadmap ID prefixes

**Status:** Shipped 2026-07-10 (full suite 2607/2607 green)
**Kind:** fix
**Source:** 3D_Engine/Vestige-feedback-2026-07-10

## Problem

`roadmap_query` `id`/`ids` and `roadmap_log op:flip` cannot locate any
bullet whose ID prefix starts with a digit — e.g. the Vestige/3D_Engine
project's canonical `[3D_E-NNNN]` scheme. Every **canonical** ID recogniser
in the codebase requires a **letter-leading** prefix (`^[A-Za-z]…`), so a
digit-led prefix like `3D_E` is not recognised as an ID token at all: the
whole project's ID scheme is invisible to id-lookup (items can't be
fetched or flipped by ID — HIGH for that project).

(One recogniser is already digit-permissive and is NOT the canonical
gate: `kIdIsh` inside `rcIsNonconformingIdToken` — `^[A-Za-z0-9]…` — is a
deliberately-broad "id-ish?" pre-filter paired with the letter-led
`kCanonical` to distinguish a malformed-but-id-shaped token from a
non-id string. Only `kCanonical` gates canonicity; see § New grammar and
Surface row 5. This asymmetry is load-bearing for the fix — do not widen
`kIdIsh`.)

`roadmap-format.md § 3.5.1` today mandates a prefix of "4–6 ASCII letters,
all caps," and ANTS-3387 deliberately enforces letter-leading in the flip
guard (`rcIsNonconformingIdToken`). So this is a **contract change**, not a
mechanical bug — hence a spec + cold-eyes pass before implementation
(global `CLAUDE.md` § 14).

## Decision (user, 2026-07-10)

Relax the prefix rule from **"starts with a letter"** to **"contains at
least one ASCII letter."** This:

- **Accepts** `3D_E-0042` (prefix `3D_E` contains `D`, `E`).
- **Still rejects** pure-numeric / date-like tokens: `[2026-07]`,
  `[3-2]`, `[1-1]` (prefix has no letter) — the safety property that made
  the original letter-lead rule worth having.
- **Preserves every existing project's IDs** (no rename), honouring
  § 3.5.1's own append-only "an ID never changes" invariant. The
  alternative (rename the offending project) would violate that promise
  for 3D_Engine.

Precedent: ANTS-3436 already relaxed the *spec-file* id resolver to accept
numeric-led ids (`17-emission-model`, Album Builder). This extends the same
posture to the roadmap ID grammar.

## New grammar

A canonical roadmap ID token is `PREFIX-NNNN` where:

- **PREFIX** — first char `[A-Za-z0-9]`, remaining chars `[A-Za-z0-9_-]`,
  and the prefix **contains at least one `[A-Za-z]`**.
- **NNNN** — `[0-9]+` (min-4 zero-pad is a rendering convention, not a
  match constraint — a 1-digit legacy suffix still parses).

Regex form (anchored, whole-token):

```
^(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*-[0-9]+$
```

The leading `(?=[A-Za-z0-9_-]*[A-Za-z])` lookahead asserts a letter within
the ID-char run; because the `-[0-9]+` suffix carries no letter, the letter
must fall in the prefix.

**Prefix-only variant** (for the `id_prefix` arg + its JSON-schema
`pattern` — rows 7, 12 — which validate a bare `3D_E`, not a full token):

```
^(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]{0,15}$
```

The `{0,15}` length cap is **preserved** exactly as the current
`kIdPrefixShape` enforces it; only the first-char class + letter lookahead
change.

**Per-site digit-suffix bound is preserved.** The whole-token regex above
shows `-[0-9]+` for illustration only; each widened site **keeps its
existing numeric bound** — `-[0-9]{1,8}` at `sniffPrefixFromText` /
`rlDetectCounterPrefix` / `rlMaxExistingIdForPrefix` / `rxAntsV1IdBracket`,
`[0-9]+` at `isCanonicalId`. Do not drop a site's `{1,8}` cap when applying
the lookahead.

Embedded (bracket-captured, e.g. `\[(…)\]`) sites splice the lookahead +
first-char + class into their existing capture group, keeping their
surrounding anchors and digit bound.

**The `kCanonical` / `kIdIsh` asymmetry (Surface row 5).** Only
`kCanonical` (letter-led → letter-containing) is widened.
`rcIsNonconformingIdToken` = `kIdIsh.match && !kCanonical.match`. After the
change: a digit-led *letter-containing* token (`3D_E-0042`) now matches
`kCanonical` → the function returns **false** (canonical, not flagged) → the
flip/annotate guard lets it through. A *letter-free* id-shaped token
(`2026-07`, `3-2`) still matches `kIdIsh` but NOT `kCanonical` → the
function returns **true** (id-shaped but non-canonical) → the guard emits
`bad_id_format`, exactly as it does today (pre-existing behaviour,
unchanged by this fix). Widening `kIdIsh` too would collapse the guard to
`X && !X` ≡ always-false — never do it.

## Surface — sites WIDENED

Each `^[A-Za-z]…` / `[A-Za-z][A-Za-z0-9_-]*` ID recogniser on the roadmap
id-lookup + allocation path gains the `contains-a-letter` lookahead:

| # | File:line | Symbol | Role |
|---|---|---|---|
| 0 | docs/standards/roadmap-format.md § 3.5.1 | prefix rule + illustrative `\[[A-Z]…\]` regex | **the contract** — relax "4–6 ASCII letters, all caps" to "contains ≥1 ASCII letter" and update the sample regex |
| 1 | roadmapindex.cpp:59 | `isCanonicalId` | full-token canonical test |
| 2 | roadmapfoldin.cpp:206 | `sniffPrefixFromText` (called by `sniffIdPrefix` :224 + `corpusHighWater` :244) | prefix-capture from `\[(…)-NNNN\]` |
| 3 | src/remotecontrol.cpp | `rlDetectCounterPrefix` | allocation prefix sniff |
| 4 | src/remotecontrol.cpp | `rlMaxExistingIdForPrefix` | high-water scan per prefix |
| 5 | src/remotecontrol.cpp | `rcIsNonconformingIdToken` **`kCanonical` only** (NOT `kIdIsh`) | ANTS-3387 flip/annotate id guard |
| 6 | src/remotecontrol.cpp | `rxAntsV1IdBracket` | ants-v1 bullet parser (id extraction) |
| 7 | src/remotecontrol.cpp | `kIdPrefixShape` | `id_prefix` arg validator (op:append) — prefix-only variant, keep `{0,15}` |
| 8 | `cmdRoadmapLogAppend` (`src/remotecontrol_roadmap_log.cpp`) and `cmdRoadmapLogAppendBatch` (`src/remotecontrol_roadmap_log_batch.cpp`) — each reuses row 7's `kIdPrefixShape` for the check and carries its own error-message literal | op:append / op:append_batch id_prefix validation | widening row 7 covers the check; update the error-message text if it quotes the old pattern |
| 9 | roadmapdialog.cpp:612 | `idTokenPattern` (a **bare, unanchored** fragment) | shared by TWO consumers: `parseBullets` (:939, bracket-anchored `\[(…)\]`) AND `parseShippedDates` (:2440, `\b(…)\b` CHANGELOG shipped-date scan). Widening the one fragment covers both; prepend the lookahead to the returned fragment. The CHANGELOG-prose parse then also recognises a digit-led `[3D_E-0042]` token — intended and safe (the `-\d+` suffix still gates it). |
| 10 | roadmapdialog.cpp:2164 | `rxLeadId` (ANTS-1660) | strips a leading `<PREFIX>-NNNN —` token off the summary column |
| 11 | roadmapdialog.cpp:2540 | `rxInProgress` | dialog in-progress scan |
| 12 | claudeintegration.cpp:8500 | `idPrefixProp["pattern"]` | op:append `id_prefix` JSON-schema pattern — prefix-only variant, keep `{0,15}` |

## Surface — sites LEFT (with rationale)

- **Stable-prefix scheme** (`rlDetectStablePrefixId` in
  `src/remotecontrol.cpp`, with the stable_id shape checks and error
  literals in the append and append_batch handlers;
  claudeintegration.cpp:8410 stable_id JSON-schema `pattern`;
  roadmapdialog.cpp:1121/1142 `rxIdShaped` / `rxLeadBracketId`): a
  *different* ID mechanism (bare `Cl9` / `Sh4` /
  `Ts20-DE1`, no `-NNNN` counter tail). 3D_Engine uses the dashed counter
  scheme, which the primary dashed matchers (above) cover; the bare-token
  fallback is not on the reported path. Verified: `parseBullets`
  (roadmapdialog.cpp:1208-1218) resolves id primary `rxId` → `boldId`
  (`rxIdShaped`) → `bracketId` (`rxLeadBracketId`), so a dashed
  `[3D_E-0022]` is captured by the widened primary before either fallback
  fires. **By design, the bold/bare fallbacks stay letter-led:** a
  bold-authored `- 🚧 **3D_E-0042** —` (no brackets) would still not
  resolve — only the bracket form `[3D_E-NNNN]` (which 3D_Engine actually
  authors) is supported. Out of scope.
- **`prefix_hint`** (the flip and flip_batch handlers in
  `src/remotecontrol_roadmap_log.cpp` and `_log_batch.cpp`;
  claudeintegration.cpp `^[A-Z][A-Z0-9_-]{0,15}$`): the caret-anchor
  injection hint on flip — cosmetic, upper-case-only by its own design;
  not an id recogniser. Left.
- **plan_template `isValidAntsId`** (plantemplateengine.cpp:37/229,
  `^[A-Z][A-Z0-9]{0,15}-[0-9]{1,9}$`): a distinct upper-case-only internal
  validator for `plan_template`'s `ants_id` field, not roadmap parsing.
  Left; noted as a related follow-up if a 3D_Engine session needs it.
- **spec-file id/title parsers** (`src/remotecontrol_state.cpp`, beside
  `cmdSpecQuery`): the
  `spec_query`/`spec_log` domain, separate from roadmap id-lookup (and the
  id arm already accepts numeric-led via ANTS-3436). Left; related
  follow-up.
- **pass designators** (passheadingwrite.cpp), **subsystem name tokens**
  (subsystemmap.cpp): unrelated grammars.

No shared "roadmap id grammar" module is introduced: the recognisers serve
distinct capture shapes (whole-token, `\[(…)\]`, `^- 🚧 \[(…)\]`,
prefix-only) and live across four libs. Each is widened in place with the
identical documented sub-pattern; consolidating the pre-existing drift into
one module is noted as a separate follow-up (not this fix).

## Invariants

- **INV-1** — A digit-led, letter-containing id token (`3D_E-0042`,
  `3D_E-42`) is canonical: recognised by the parser, fetchable by
  `roadmap_query id/ids`, and flippable by `roadmap_log op:flip` (not
  refused `bad_id_format`).
- **INV-2** — A pure-numeric / letter-free prefix is not **canonical**: no
  canonical recogniser or id-lookup (`isCanonicalId`, `idTokenPattern`,
  `rxAntsV1IdBracket`, `kCanonical`) treats `[2026-07]`, `[3-2]`, `[1-1]`
  as a bullet id, so a date/version bracket in prose is never fetched or
  allocated as one. (The `kIdIsh` pre-filter still classes such a token as
  *id-shaped*, so `op:flip id="2026-07"` returns `bad_id_format` — an
  explicit "id-shaped but not canonical" refusal — rather than
  `bullet_not_found`. This is unchanged from today.)
- **INV-3** — Every pre-existing letter-led id still matches unchanged
  (`ANTS-0001`, `RETRO-1234`, `MYPRJ-0042`).
- **INV-4** — `sniffIdPrefix` / allocation resolves a digit-led prefix:
  a roadmap whose dominant ids are `3D_E-NNNN` sniffs prefix `3D_E`, and
  `op:append` allocates the next `3D_E-NNNN` (not the `ANTS` fallback).
- **INV-5** — `id_prefix` arg + its JSON-schema `pattern` accept a
  digit-led, letter-containing prefix (`3D_E`) and still reject a
  letter-free one (`2026`).

## Tests

New feature test `roadmap_id_digit_led_prefix`:

- `isCanonicalId`: `"3D_E-0042"` true; `"3D_E-42"` true (short suffix
  parses); `"ANTS-0001"` true; `"2026-07"` false; `"3-2"` false. Boundary
  rejects: `"_-1"` false (first char must be `[A-Za-z0-9]`), `"A-"` false
  (suffix must be `[0-9]+`), `"-42"` false (empty prefix).
- `rcIsNonconformingIdToken`: `"3D_E-0042"` **false** (now canonical → guard
  passes it); `"2026-07"` **true** (id-shaped via `kIdIsh` but letter-free
  → non-canonical → `bad_id_format`; pre-existing value, unchanged by the
  fix); `"ANTS-0001"` false (canonical). This asserts `kIdIsh` was NOT
  widened — if an implementer widens it too, `"2026-07"` flips to false and
  this test catches the collapsed guard.
- ants-v1 parse: a `- 📋 [3D_E-0022] **x.**` bullet yields id
  `3D_E-0022` (round-trips through `roadmap_query id=3D_E-0022`).
- `sniffIdPrefix` on a `3D_E-`-dominant roadmap returns `3D_E`; `op:append`
  then allocates the next `3D_E-NNNN` (INV-4).
- `kIdPrefixShape` accepts `3D_E`, rejects `2026`, and still rejects a
  >15-char prefix (the `{0,15}` cap is retained).

Existing suites (roadmap_query_by_id, roadmap_log_flip_*, foldin_id_prefix,
roadmap_index_engine) must stay green — INV-3 guards the no-regression.

## Cold-eyes loop log

- **Loop 1 (2026-07-10)** — 2 cold reviewers (full-dimension + doc-vs-code
  accuracy). Tally: CRITICAL 2 · HIGH 1 · MEDIUM 2 · LOW 5 · INFO 4
  (verified 10 / unverified 0). Both reviewers independently found the same
  top defect. Fixed all verified findings:
  - C1/H1 — `rcIsNonconformingIdToken("2026-07")` test expectation was
    `false`; code yields `true` (`kIdIsh` is digit-permissive). Corrected
    the expectation + documented the `kCanonical`/`kIdIsh` asymmetry (only
    `kCanonical` widens) so an implementer can't collapse the ANTS-3387
    guard.
  - C2 — added `roadmap-format.md § 3.5.1` (the contract) as Surface row 0.
  - H1 — Problem "every ID recogniser" → "every *canonical* recogniser" +
    called out `kIdIsh`.
  - M1 — wrote out the prefix-only regex + `{0,15}`-cap-preserved note.
  - M2 — Surface row 2 symbol corrected to `sniffPrefixFromText`.
  - L1 — per-site digit-suffix bound (`{1,8}`) preserved note.
  - L2 — added empty-prefix / trailing-dash / underscore-lead boundary
    reject tests.
  - L3 — INV-2 scoped to canonical recognisers + reconciled with the flip
    guard's `bad_id_format`.
  - Row-10 label corrected to `rxLeadId` (summary-column strip); bold/bare
    fallback stays-letter-led note added to sites-LEFT.
- **Loop 2 (2026-07-10)** — 1 cold reviewer (full-dimension + doc-vs-code +
  completeness). Tally: CRITICAL 0 · HIGH 0 · MEDIUM 1 · LOW 3 · INFO 2
  (verified 4 / unverified 0). The loop-1 critical defect did NOT resurface
  (fix held). Reviewer independently re-verified the grammar correct, the
  `kCanonical`/`kIdIsh` asymmetry correct, and the WIDENED site list
  **complete** for the dashed-counter path. Remaining findings were all
  doc-completeness polish (no structural/mechanical/architectural defect) →
  **polish convergence**. Fixed: M1 — row 9 now names both `idTokenPattern`
  consumers (`parseBullets` + `parseShippedDates`) + the fragment splice
  recipe; L1 — stable-prefix sites-LEFT list completed
  (claudeintegration.cpp:8410, remotecontrol.cpp:5950); L2 —
  `rlMaxExistingIdForPrefix` added to the `{1,8}` digit-bound list; L3 —
  row-8 `:8620` `kIdPrefixShape` check confirmed. Converged.
