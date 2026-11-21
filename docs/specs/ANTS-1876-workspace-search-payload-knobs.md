# ANTS-1876 — `workspace_search` payload knobs

**Status:** shipped 2026-05-26 (cold-eyes loops 1 + 2 folded);
ANTS-3548 amendment (default-ON `max_match_bytes` clip) 2026-07-17 —
cold-eyes converged in 3 loops (H3→H1→polish; see §10).
**Kind:** enhancement.
**Source:** ROADMAP.md ANTS-1876 (vestige-feedback-2026-05-26 Issue #19).

**Layman:** Add two switches to the workspace-search tool so a
research-style sweep can clip each match line to ~400 characters
(`max_match_bytes`) or drop the full text + context, keeping only a
short headline per match (`headline_only`) — useful when the caller
only needs the catalogue, not the body.

## 1. Problem

Vestige's 87 388-char threading-keyword sweep on ROADMAP.md hit the
persisted-output path because each match line was a full 2–5 KB
bullet body plus context. Today `workspace_search` has only coarse
controls: `max_results` limits the count, `context` controls context
lines, `max_bytes` truncates the entire response. There is no way to
say *"give me the catalogue (file + line + a short snippet), I don't
need 5 KB per match"*.

The cost is paid twice: once on the wire (giant response), once on
the assistant's context budget (the full snippet is held even when
only the file/line was wanted).

## 2. Surface

### 2.1 Args

Two new optional args on `workspace_search`:

| Arg                | Type    | Default | Clamp / values         |
|--------------------|---------|---------|------------------------|
| `max_match_bytes`  | integer | **512** (ANTS-3548 default-ON) | absent → `kDefaultMaxMatchBytes` (512). An explicit value in `[50, 10000]` overrides. Explicit `<= 0` **opts OUT** (0 = no clip — the off switch). An out-of-range *positive* value clamps into `[50, 10000]` (a mistyped small/large value still clips predictably rather than silently disabling the saver). A **non-numeric** value (`isDouble()==false`) → the 512 default (clip ON) — so the ONLY opt-out is a numeric `<= 0`, not a string sentinel. See §3 INV-1 / ANTS-3548 amendment. |
| `headline_only`    | boolean | `false` | when `true`, drop `text`, `context_before`, `context_after`; emit only `{file, line, headline:"<the matched line>"}` per match (plus `also_at` if dedup collapsed copies into this one). Arg name + return shape from ROADMAP ANTS-1876 option 2. |

When `headline_only:true`, `max_match_bytes` continues to clip the
`headline` field (same matched-line content, renamed for the summary
shape) — composition is legal and useful.

### 2.2 max_match_bytes — per-match `text` / `headline` clip

When `max_match_bytes > 0`, **after** the dedup step (so the dedup
key sees the unclipped text — see §2.5 below), every `text` /
`headline` field is clipped to at most `max_match_bytes` UTF-8
bytes including the ellipsis. The clip rule:

1. If `text.toUtf8().size() <= max_match_bytes`, no clip.
2. Else cut UTF-8 to the *largest* prefix whose byte length
   ≤ `max_match_bytes - 3` (the 3 bytes the `…` ellipsis will
   occupy in UTF-8).
3. If the prefix's last byte falls inside a multi-byte UTF-8
   sequence, back up to the prior code-point boundary (no
   half-character emission).
4. Append `…` (one UTF-8 ellipsis, 3 bytes).

The same rule applies to `text` entries inside `context_before` /
`context_after` arrays (bullets-mode) when the caller asked for
context but also passed `max_match_bytes` — context lines carry the
same per-match `text` problem.

The clip-budget floor (50 — the effective clamp floor, distinct from
the schema `minimum` which ANTS-3548 lowered to 0 for the opt-out
sentinel, see §8) guarantees the budget covers at least the ellipsis +
47 bytes of payload, which is roughly one short identifier + surrounding
code — small but not useless.

### 2.3 headline_only — emit `{file, line, headline}` triples

When `headline_only:true`:

- Each match emits as `{file, line, headline}` where `headline` is
  the matched line (same content as `text` would have carried; the
  rename signals "this is the summary shape").
- `also_at` (the dedup-collapsed peers) is retained — it's
  `{file, line}` shape and that's already minimal, so it stays
  unchanged.
- `context_before` / `context_after` are dropped from the per-match
  object even if the caller passed `context > 0`. Rationale: the
  whole point of the flag is to NOT carry context.
- The envelope still carries the existing `dedup` /
  `dedup_collapsed` / `truncated` / `respect_gitignore` /
  `include_hidden` / `timeout_sec` / `elapsed_ms` fields.

### 2.4 Envelope echo

Both args echo back on success when the *effective value activates
the feature* (`max_match_bytes > 0` OR `headline_only == true`) — note
"effective", not "caller-supplied", since a default call activates the
clip without passing the arg (ANTS-3548). **(ANTS-3548 amendment,
2026-07-17)** Since `max_match_bytes` now
defaults to 512 (> 0), the "activated" condition is met on a default
call, so a default `workspace_search` now echoes `max_match_bytes:512`
— the wire shape is intentionally no longer byte-identical to the
pre-3548 default (the echo advertises the now-default clip). Only an
explicit opt-out (`max_match_bytes <= 0`) suppresses the echo.
`headline_only` retains its original "echo iff true" back-compat
behaviour. The below rationale describes the original ANTS-1876
back-compat intent, retained for provenance:

Pre-3548, default-value calls (both args absent or set to their
defaults) emitted no new envelope fields → byte-identical wire shape
for existing callers; post-3548 that holds only for `headline_only`
(the `max_match_bytes:512` echo now fires on a default call). The
original intent deliberately diverged from the existing unconditional
echo of `respect_gitignore` / `include_hidden` / `timeout_sec`
(`cmdWorkspaceSearch:4626-4630`) — those fields shipped from day one
and never needed back-compat protection, but new fields added later
should not silently change envelope shape for old callers. ANTS-3548
consciously accepts that envelope-shape change for `max_match_bytes`
as the transparency signal that the default clip is active.

### 2.5 Composition with dedup

**Load-bearing constraint (dedup key stability).** The existing
dedup step keys on the primary match's `text.simplified()` — the
unclipped, unrenamed matched line. The clip in §2.2 and the
rename to `headline` in §2.3 both happen AFTER dedup, so the
dedup key sees the canonical form. This is what keeps INV-4
true.

**Mechanical (context-entry clip).** Context-array entries (the
`text` field inside each `context_before` / `context_after`
object) participate in NEITHER the dedup key NOR the rename — they
just get clipped to `max_match_bytes` for the same wire-cost
reason as the primary text. This is purely an emission-shape
detail, with no key-stability implication.

Pipeline order (one-shot, top to bottom):

1. rg → `matches[]` (with full `text` + context arrays).
2. Dedup (key on primary `text.simplified()`, no clipping touches
   the key).
3. `max_match_bytes` clip (mutates primary `text` and context-
   array entries' `text`; load-bearing for INV-2/3, not for
   INV-4).
4. `headline_only` rename + context drop (primary `text` →
   `headline`; drop `context_before` / `context_after`).
5. Envelope assembly (INV-6 activation-gated echo).

## 3. Invariants

- **INV-1** — `max_match_bytes` accepted, clamped to `[50, 10000]`.
  **(ANTS-3548 amendment, 2026-07-17)** The default is now **512**
  (`kDefaultMaxMatchBytes`), not 0 — a token-saver default-ON per the
  project's "savers default ON, keep an off switch" rule. Resolution:
  absent → 512; explicit value in `[50, 10000]` → that value; explicit
  `<= 0` → 0 (the off switch — no clip); out-of-range *positive*
  (`1..49` or `> 10000`) clamps to the nearest bound (50 or 10000) so a
  mistyped value still clips predictably rather than silently disabling
  the saver. Type-mismatched values (`isDouble()==false`) leave the
  value at the 512 default. (Supersedes the pre-3548 "default 0 (off);
  out-of-range falls back to 0" behaviour.)
  *Test:*
  `tests/features/workspace_search_payload_knobs/test_workspace_search_payload_knobs.cpp::Inv1MaxMatchBytesParse`,
  `…::Inv20DefaultOnClip`, `…::Inv21ExplicitOptOut`.
- **INV-2** — When `max_match_bytes > 0`, every `text` field in
  `matches[]`, every `headline` field, and every `text` field
  inside any `context_before` / `context_after` entry whose
  unclipped form would exceed `max_match_bytes` UTF-8 bytes is
  clipped to *exactly* `max_match_bytes` bytes (payload prefix +
  3-byte UTF-8 ellipsis). Fields whose unclipped form already fits
  are emitted verbatim (no ellipsis added).
  *Test:* `…::Inv2TextClippedToBudgetExactByteCount`,
  `…::Inv2ContextEntriesClipped`,
  `…::Inv2ShortFieldEmittedVerbatim`.
- **INV-3** — `max_match_bytes` clip does NOT split UTF-8 code-
  points; the clip backs up to the prior boundary before appending
  the ellipsis. Applies to `text`/`headline` AND context-array
  entries.
  *Test:* `…::Inv3ClipDoesNotSplitCodePoints`.
- **INV-4** — Dedup key is unaffected by `max_match_bytes` (key is
  computed on the unclipped `text` per §2.5 ordering — dedup runs
  step 2, clip runs step 3).
  *Test:* `…::Inv4DedupKeyUnaffectedByClip`.
- **INV-5** — When `headline_only:true`, every emitted primary
  match object has the key set `{file, line, headline}` plus
  optional `also_at`; no `text` / `context_before` /
  `context_after` keys present. `also_at` entries remain
  `{file, line}` (unchanged) — they carry no text field so they
  are never clipped, even when `max_match_bytes > 0`.
  *Test:* `…::Inv5HeadlineOnlyKeySet`, `…::Inv5AlsoAtNeverClipped`.
- **INV-6** — Each arg echoes on the envelope when (and only when)
  it *activated the feature*: `max_match_bytes` echoes iff its
  effective value > 0; `headline_only` echoes iff true. Echo
  fires only on the success path (`ok:true`); error envelopes
  (`bad_pattern`, `rg_failed`, etc.) never carry the new fields.
  **(ANTS-3548 amendment, 2026-07-17)** Because the default is now 512
  (> 0), the "effective value > 0" condition is met on a *default* call,
  so `max_match_bytes:512` now echoes by default — a deliberate
  transparency signal that the clip is active (the caller sees the
  effective clip without having to know the default). The echo is
  suppressed only when the caller explicitly opted out (`<= 0` → 0).
  This supersedes the pre-3548 "default calls add neither field"
  clause for `max_match_bytes` (`headline_only` is unchanged — still
  echoes iff true).
  *Test:* `…::Inv6EchoActivationGated`, `…::Inv6NoEchoOnError`.
- **INV-7** — `tools/list` schema lists both new args. The
  `workspace_search` descriptor (`src/claudeintegration.cpp`
  `tools/list` builder) gains `props["max_match_bytes"]` and
  `props["headline_only"]`; the top-level descriptor's prose
  description gets a one-sentence pointer.
  *Test:* `…::Inv7ToolsListEnumerates` — source-grep for the
  literals `"max_match_bytes"` and `"headline_only"` inside the
  block bracketed by the string literal
  `wsTool["name"] = "workspace_search"` and the next tool's
  declaration line — which in current code (`tools/list` builder)
  is the comment marker `// ANTS-` directly preceding the next
  tool's `QJsonObject xTool;` declaration. The grep anchor is the
  literal substring `props["max_match_bytes"]` and
  `props["headline_only"]` (descriptor population sites) — matches
  inside the prose `description` strings, in test fixtures, or in
  comments do NOT count.

## 4. Tests

`tests/features/workspace_search_payload_knobs/`:

- **`spec.md`** — mirrors INV-1..INV-7 in test-anchor form.
- **`test_workspace_search_payload_knobs.cpp`** — source-grep style
  (matches sibling tests). INV-2 / INV-3 exercise the clip helper
  directly with synthetic byte arrays so the UTF-8 boundary and
  ellipsis-inside-budget invariants are pure-function checks.
- Per project convention, each invariant is first verified to FAIL
  against pre-fix code (the literals `"max_match_bytes"` /
  `"headline_only"` and the clip helper are absent), then GREEN
  after the fix.

## 5. RAM / build cost

Zero new state. ~40 LoC in `cmdWorkspaceSearch` (clip helper + two
arg parses + post-dedup mutation + emission branch), ~50 LoC
tool-descriptor schema update (two new prop blocks + description
extension), ~150 LoC test → **~240 LoC** total.

## 6. Out of scope

The ROADMAP bullet (ANTS-1876) lists four "any one closes it"
options; this spec covers (1) `max_match_bytes` and (2)
`headline_only`. The remaining two are deferred:

- **Auto-fallback to summary shape when response would exceed
  `max_bytes`, with `mode_downgraded:"summary"` on the envelope.**
  Composes with this spec — implementable as a second pass after
  envelope assembly. Tracked as a follow-up under ANTS-1876 (this
  spec leaves the door open by emitting the new field set
  cleanly).
- **Persist-hint pre-count `X matches across Y files` so the
  caller can decide narrow-regex vs subagent-slice without opening
  the file.** Composes with the persist path (a separate concern
  from the in-line envelope). Deferred as a follow-up; tracked
  under ANTS-1876.

`headline_only` + `max_match_bytes` together already achieve the
~10× wire reduction Vestige's session needed; the remaining two
options are additive optimisations, not gating.

## 7. Migration

**Original ANTS-1876 (2026-05-26):** None — both args additive with
default-back-compat behaviour; existing `workspace_search{...}` calls
byte-identical (the new envelope echo fields only appeared when the
feature was *activated*, per INV-6).

**ANTS-3548 amendment (2026-07-17) — a deliberate, non-byte-identical
default change:** making `max_match_bytes` default-ON (512) changes the
output of existing default (unchanged) calls in two visible ways: (a)
the envelope now carries `max_match_bytes:512` (no longer byte-identical
— the echo advertises the now-default clip, per the amended INV-6); and
(b) any match `text` / `context_before` / `context_after` line longer
than 512 UTF-8 bytes is now clipped where it previously came back whole.
Only matches whose lines exceed 512 B are affected; shorter matches are
byte-identical. A caller that needs the pre-3548 unclipped behaviour
opts out with `max_match_bytes: 0`. This is intentional per the
project's "token-savers default ON, keep an off switch" rule.

## 8. Tool-descriptor update

Three touchpoints in `src/claudeintegration.cpp`'s
`workspace_search` descriptor:

1. **`props["max_match_bytes"]`** — integer, description naming the
   `[50, 10000]` effective clamp + ellipsis-inside-budget rule.
   **(ANTS-3548 amendment, 2026-07-17)** three coupled schema changes so
   the advertised contract matches the effective default: (a) `default`
   is now `kDefaultMaxMatchBytes` (512), not 0; (b) the schema `minimum`
   is **lowered from 50 to 0** — because `0` is now the opt-out sentinel
   a caller must be able to *pass*, and a `minimum: 50` would advertise
   the off switch as below-range (a bounds-validating client would
   reject it); the server still clamps an explicit `1..49` up to 50, so
   `[50, 10000]` remains the effective *clip* range while `0` stays a
   legal opt-out; (c) the description states "absent → 512 (default-ON
   clip); pass 0 to opt out". Without all three the INV-6 echo of
   `max_match_bytes:512` contradicts a schema that still claims default
   0 / min 50.
2. **`props["headline_only"]`** — boolean, default false,
   description naming the `{file, line, headline}` triple shape
   and the context-drop behaviour.
3. **`wsTool["description"]`** — top-level descriptor gains a
   one-sentence pointer to both args so a caller scanning the
   top-level help sees them.

## 9. Cross-doc impact

- `CLAUDE.md` — no change (the tool-authoring contract list is
  unchanged; only new args are added).
- `CHANGELOG.md` — entry added at next release cut: "Added
  `max_match_bytes` and `headline_only` opt-in args to
  `workspace_search` MCP tool (Vestige feedback ANTS-1876)."

## 10. Cold-eyes loop log

Rows live in [`../reviews/ANTS-1876-workspace-search-payload-knobs-loop-log.md`](../reviews/ANTS-1876-workspace-search-payload-knobs-loop-log.md).
