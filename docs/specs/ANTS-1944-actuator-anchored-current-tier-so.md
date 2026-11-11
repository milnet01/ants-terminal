# ANTS-1944 — Actuator-anchored current-tier so the auto-switcher stops thrashing on a stale transcript

**Status:** ready for implementation (cold-eyes clean 2026-06-02).
**Kind:** fix.
**Source:** user-report-2026-06-02 (observed `/model sonnet` × 8 at 100 % context).
**Pairs with:** ANTS-1916 (the user-`/model` current-model read this must not regress; no standalone spec — landed under the comment block at `modelrecommender.cpp:246-252`).

## 1. Problem

The auto-switcher fired `/model sonnet` eight times in a row while the model was
already Sonnet (user report, 2026-06-02, session at 100 % context used). This is
the ANTS-1916 thrash class re-opening through a path that fix did not cover.

### 1.1 Root cause (confirmed from source)

The gate's anti-re-fire guard is `target_equals_current`
(`src/modelautoswitch.cpp:120-121` — `if (target == g.current)` then the
`blockedBy` append): it blocks a switch only when `target == g.current`.
`g.current` is derived **exclusively** from the transcript:

```
const ModelRecommender::Result rec = ModelRecommender::score(transcriptPath);  // :1463-1464
const ModelRecommender::Tier current =
    ModelRecommender::tierFromModelId(rec.currentModel);                       // :1465-1466
...
gate.current = current;                                                        // :1519
```

`rec.currentModel` is `message.model` from the most-recent assistant turn
(`src/modelrecommender.h:13`), overridden by a newer `/model` command if the
transcript has one (ANTS-1916, `modelrecommender.cpp:246-252`). Both signals
live in the transcript. When the transcript goes **stale relative to what the
switcher actually did** — the dominant case being a long idle / a context
compression that rewrites the transcript and drops the `/model` events ANTS-1916
keys on — `score()` falls back to the last *visible* assistant turn, whose
`message.model` is the session-start tier (e.g. Opus). So `current` reads Opus,
`target` is Sonnet, `target_equals_current` never trips, and the switcher
re-fires every dwell window. End-of-session idle is worst: no new assistant turn
ever arrives to refresh the read.

The actuator already knows the answer: it sets `m_autoSwitchLastTier` (the tier
it last injected, `claudestatuswidgets.cpp:1620`) and `m_autoSwitchLastMs` (when,
`:1615`) on every fire. But `m_autoSwitchLastTier` is recorded "for ledger" only
(`claudestatuswidgets.h:343`) and is **never consulted** when deriving
`g.current`. The one signal that survives transcript compression is thrown away.

**Layman:** After the conversation fills up and gets summarised, the
auto-switcher loses track of which model it already set, reads the model from the
start of the session instead, and keeps issuing the same switch over and over.
The switcher already records what it last set — we just weren't using it. The fix
trusts that record to **suppress a repeat of the switch it just made**, without
ever overriding a model the user picked themselves.

## 2. Surface

The fix anchors `current` to the actuator's own last action **only when doing so
would suppress re-firing the tier the actuator already injected** — never to
change `current` toward a tier the user might be on. This keeps the change purely
thrash-suppressing (§2.2 rationale) and provably cannot revert a user's manual
`/model` (§2.4).

### 2.1 `score()` reports the source + age of its current-model read

`ModelRecommender::Result` (`src/modelrecommender.h:10-18`) gains two fields:

```cpp
struct Result {
    Tier    tier         = Tier::Sonnet;
    QString reason;
    QString currentModel;
    bool    isMechanical = false;
    // ANTS-1944 — provenance of currentModel, so the controller can tell a
    // stale assistant-turn read from an authoritative /model command.
    bool    currentModelFromCommand = false;  // true: read from a /model command
    qint64  currentModelTsMs        = 0;       // ts of the assistant turn it came
                                               // from (0 = none / from command /
                                               // unparseable timestamp)
};
```

In `score()` (`modelrecommender.cpp`), the two fields are populated on the local
`def` during the reverse walk, then **copied forward** into every returned
`Result`:

- When `currentModel` is taken from the newest assistant turn (`:240-245`), parse
  that turn's top-level `"timestamp"` string from the same `turns.last()` JSON
  object (already in memory) and store the epoch-ms in `def.currentModelTsMs`.
  `modelrecommender.cpp` already includes `<QDateTime>` (`:3`), so the parse is
  **inline** — `QDateTime::fromString(ts, Qt::ISODate)`, `0` when invalid. This is
  **byte-identical** to `ModelSwitchLedger::parseIso8601Ms`, whose verified body
  is exactly:

  ```cpp
  qint64 parseIso8601Ms(const QString &ts) {
      const QDateTime dt = QDateTime::fromString(ts, Qt::ISODate);
      return dt.isValid() ? dt.toMSecsSinceEpoch() : 0;
  }
  ```

  so the inline copy cannot disagree with the controller's parse of the same
  field on any input (no fractional-second / offset edge case is handled
  specially — the helper is *just* the bare `Qt::ISODate` call). It is duplicated
  rather than cross-linked so `modelrecommender` stays a leaf translation unit
  with **no new `#include "modelswitchledger.h"`** and no module coupling — which
  also avoids adding `modelswitchledger.cpp` to the test bundles that compile
  `modelrecommender.cpp` directly (CMakeLists `:1578`). It is a pure scorer; the
  2-line `QDateTime` wrapper is not an abstraction worth that build surface. The controller's own parse of the same field
  (`claudestatuswidgets.cpp:2073-2074`) confirms assistant lines carry a
  top-level `"timestamp"`. **Rule-of-three note:** this is technically a third
  site for the `QDateTime::fromString(…, Qt::ISODate)` wrapper (alongside
  `ModelSwitchLedger::parseIso8601Ms` and the controller's call to it). The
  duplication is accepted because the "logic" is a single stdlib call with one
  validity guard — below the bar where extracting a shared helper pays for the
  module coupling it would impose on the leaf scorer. If a fourth site appears,
  promote it to a Qt6::Core-only `isotime.h` leaf header instead.
- When the ANTS-1916 branch overrides `currentModel` with `pendingModelTier`
  (`:251-252`), set `def.currentModelFromCommand = true`. The flag alone marks it
  authoritative — no command timestamp is needed (and `local_command` events are
  not assumed to carry one).
- **Propagation (load-bearing):** `score()` does not return `def`. The main path
  builds a fresh `Result r` at `:337` copying only `currentModel`/`isMechanical`
  (`:338-339`); the commit-intent early return builds another at `:259-263`. Both
  constructions MUST also copy `currentModelFromCommand` + `currentModelTsMs` from
  `def`, or the fields are silently lost before `memoize()`. (`memoize` then
  copies the whole `Result` by value, so propagation past it is automatic.)

### 2.2 A pure reconciliation helper

`ModelAutoSwitch` gains a pure function (table-testable, like
`advanceStability` / `conservatismDwellMultiplier`):

```cpp
// ANTS-1944 — choose the authoritative "current tier" for the gate. Overrides
// the transcript read with the actuator's last-injected tier ONLY when that
// override would block re-firing the SAME tier the actuator just set (i.e. the
// clamped recommendation equals the last-actuated tier) AND the transcript read
// is a (possibly stale) assistant-turn read older than that injection. A /model
// command read is always authoritative. Because the override is gated on
// `actuated == recommendedTarget`, it can never steer `current` toward a tier
// the user manually moved to — it only ever says "you are already on the tier I
// am about to recommend," suppressing the repeat.
ModelRecommender::Tier reconcileCurrentTier(
    ModelRecommender::Tier transcriptTier,
    bool    transcriptFromCommand,
    qint64  transcriptTsMs,
    const QString &lastActuatedTierName,        // "" = no auto-switch this session
    qint64  lastActuatedMs,
    ModelRecommender::Tier recommendedTarget);  // the CLAMPED target (what decide()
                                                // would re-fire)
```

Logic:

```
if (transcriptFromCommand) return transcriptTier;          // ANTS-1916 wins
if (lastActuatedTierName.isEmpty()) return transcriptTier;  // nothing actuated yet
const Tier actuated = ModelRecommender::tierFromModelId(lastActuatedTierName);
if (ModelRecommender::tierName(actuated) != lastActuatedTierName)
    return transcriptTier;                                  // not a clean alias
if (actuated != recommendedTarget) return transcriptTier;   // only suppress a REPEAT
if (lastActuatedMs > transcriptTsMs) return actuated;       // actuator newer → anchor
return transcriptTier;
```

Two facts make the guards safe:

- **Alias round-trip.** The actuator stores its tier as
  `d.tierArg = ModelRecommender::tierName(target)` (`modelautoswitch.cpp:149`,
  read into `m_autoSwitchLastTier` at `claudestatuswidgets.cpp:1620`) — a bare
  alias `"haiku"`/`"sonnet"`/`"opus"`. `tierFromModelId` substring-matches
  the alias (the property ANTS-1916 relies on, `modelrecommender.cpp:247-248`) but
  returns `Tier::Sonnet` for *any* unrecognised string (`:375`). The round-trip
  `tierName(actuated) == lastActuatedTierName` rejects anything that is not
  exactly one canonical alias (`""`, `"bogus"`, or a full model-id like
  `"claude-opus-4-8"` all fall through to the transcript tier). Defensive — the
  stored value is always a `tierName()` output.
- **Repeat-only override.** Gating on `actuated == recommendedTarget` is what
  makes this safe against a user override: see §2.4.

### 2.3 Controller wires the helper in

The controller already computes the clamped target at
`claudestatuswidgets.cpp:1484-1485` (`clampedTarget = clampToFloor(rec.tier,
floor)`). Reorder the block so the final sequence is **`floor` resolution
(`:1469-1473`) → `clampedTarget` (`:1484-1485`) → `reconcileCurrentTier` →
`current` → `advanceStability` (`:1486-1487`)** — i.e. `clampedTarget` and `floor`
move up above the `current` derivation, and `advanceStability` stays below it so
it consumes the reconciled `current`. Replace the direct assignment at
`:1465-1466`:

```cpp
const ModelRecommender::Tier current = ModelAutoSwitch::reconcileCurrentTier(
    ModelRecommender::tierFromModelId(rec.currentModel),
    rec.currentModelFromCommand,
    rec.currentModelTsMs,
    m_autoSwitchLastTier,
    m_autoSwitchLastMs,
    clampedTarget);
```

`m_autoSwitchLastTier` / `m_autoSwitchLastMs` are already controller members set
at the fire site (`:1615`, `:1620`). No new state. The model-state chip
(ANTS-1888) reads the same `current`, so the stale-chip symptom is fixed in the
same place. Update the stale `m_autoSwitchLastTier` comment at
`claudestatuswidgets.h:343` ("for ledger") to note it now also feeds gate
reconciliation.

**`recommendedTarget` must be the clamped target.** `reconcileCurrentTier` is
passed `clampedTarget` so its repeat-suppression compares against the exact tier
`decide()` would re-fire — `decide()` re-clamps internally
(`target = clampToFloor(g.recommended, g.floor)`, `modelautoswitch.cpp:68`), and
the controller computes `clampedTarget` with the *same* `clampToFloor(rec.tier,
floor)` (`:1484-1485`). The two are equal by construction; passing the unclamped
`rec.tier` here would let the anchor suppress (or miss) a tier the gate would not
actually re-fire. INV-10 pins this equivalence.

**`advanceStability` now sees the reconciled `current`.** After the reorder,
`advanceStability` (`:1486-1487`) receives the anchored `current`. This is
intended and reinforces the fix: in the thrash case the stale transcript made
`current != clampedTarget`, so stability accrued toward the repeat and eventually
fired; with `current` anchored to the actuated tier, `current == clampedTarget`,
so no stability accrues toward a switch already made. In the non-repeat case the
helper returns the transcript tier unchanged, so accrual is unaffected.

### 2.4 Why this cannot revert a user's manual `/model`

Consider the adversarial case: the actuator fired Sonnet at `t=2000`; the user
then manually `/model opus` at `t=3000`, and context compression drops that
command before the next tick (so `score()` returns `fromCommand=false`, a stale
assistant-turn tier, `transcriptTsMs < 2000`). Two sub-cases by what the
recommender now wants (clamped):

- **Recommends Sonnet** (`== actuated`): `reconcileCurrentTier` anchors
  `current = Sonnet`, so `target_equals_current` blocks — the switcher declines to
  re-downgrade. The user **stays on Opus**; the switcher simply does not act. No
  revert.
- **Recommends Haiku or Opus** (`!= actuated`): the `actuated != recommendedTarget`
  guard returns the transcript tier unchanged — the anchor does **not** fire, so
  it contributes nothing to that decision. Whatever happens next is the ordinary
  recommendation path on the (independently stale) transcript read, exactly as
  pre-1944; the anchor neither caused nor worsened it.

In neither case does the actuator anchor move `current` toward a tier the user
manually selected. The anchor is provably *repeat-suppression only*. This is a
strict improvement over both pre-1944 (which thrashes) and the naive "always
anchor `current` to the actuator" design (which would revert the user's Opus to
Sonnet in the second sub-case). The residual limitation — an unlogged manual
override is not *detected* during the stale window — is pre-existing and shared
with the ANTS-1890 override cool-down (which also reads the override from the
transcript); ANTS-1944 neither fixes nor worsens it.

### 2.5 Missing / unparseable transcript timestamp

If the surviving assistant turn has no parseable `"timestamp"`, the inline
`QDateTime::fromString(…, Qt::ISODate)` yields an invalid datetime → 0, so
`transcriptTsMs == 0` and `lastActuatedMs > 0` holds — the actuator anchor wins
(when `actuated == recommendedTarget`). This is
the intended degradation, not a bug: a turn we cannot date is exactly the stale
case the anchor exists to cover, and the repeat-only guard keeps it safe. INV-5
pins this.

## 3. Invariants

- **INV-1** — `score()` sets `currentModelFromCommand = true` when `currentModel`
  is taken from a `/model` command newer than the last assistant turn.
  (`currentModelTsMs` is don't-care on this path — it may still hold the prior
  assistant turn's parsed ts, but `reconcileCurrentTier` short-circuits on
  `fromCommand` before reading it, INV-3.) *Test:* transcript whose last line is a
  `/model haiku` `local_command` after an `opus` assistant turn → result has
  `currentModelFromCommand == true`, `currentModel` aliases to haiku.
- **INV-2** — `score()` sets `currentModelFromCommand = false` and
  `currentModelTsMs` to the assistant turn's parsed timestamp when `currentModel`
  comes from an assistant turn (no newer `/model` command). *Test:* transcript
  ending in an `opus` assistant turn with a known `"timestamp"` → result carries
  that ts (ms) and `fromCommand == false`. (Guards the §2.1 `def → r` propagation:
  pre-fix the fresh `Result` drops the fields and this fails.)
- **INV-3** — `reconcileCurrentTier` returns the transcript tier unchanged when
  `transcriptFromCommand` is true, regardless of the actuator record. *Test:*
  `reconcileCurrentTier(Haiku, true, 0, "opus", 9e12, Opus) == Haiku`.
- **INV-4** — `reconcileCurrentTier` returns the **actuated** tier only when the
  read is an assistant turn older than the last actuation **and** the clamped
  recommendation equals the actuated tier (repeat suppression). *Test:*
  `reconcileCurrentTier(Opus, false, 1000, "sonnet", 2000, Sonnet) == Sonnet`
  (the thrash repro: stale Opus transcript, Sonnet actuated later, Sonnet
  re-recommended).
- **INV-5** — A missing/unparseable timestamp (`transcriptTsMs == 0`) still lets
  the actuator anchor win on a repeat (`actuated == recommendedTarget`). *Test:*
  `reconcileCurrentTier(Opus, false, 0, "sonnet", 2000, Sonnet) == Sonnet`.
- **INV-6** — The anchor does **not** fire when the clamped recommendation differs
  from the actuated tier — this is the no-user-revert guarantee. *Test:*
  `reconcileCurrentTier(Opus, false, 1000, "sonnet", 2000, Haiku) == Opus` (user
  unlogged-moved off Sonnet; recommender wants Haiku; anchor stays out of it).
- **INV-7** — The anchor does not fire when the assistant-turn read is **newer**
  than the last actuation (the transcript has caught up / the user changed it).
  *Test:* `reconcileCurrentTier(Opus, false, 3000, "sonnet", 2000, Sonnet)
  == Opus`.
- **INV-8** — No override when no switch was actuated this session
  (`lastActuatedTierName` empty) or the name is not a canonical alias. *Test (8a):*
  `reconcileCurrentTier(Haiku, false, 1000, "", 9e12, Sonnet) == Haiku` (empty
  name → transcript tier; chosen ≠ Sonnet so it is observable). *Test (8b):*
  `reconcileCurrentTier(Haiku, false, 1000, "bogus", 9e12, Sonnet) == Haiku`
  (round-trip reject) and *Test (8c):*
  `reconcileCurrentTier(Haiku, false, 1000, "claude-opus-4-8", 9e12, Opus)
  == Haiku` (a model-id-shaped name round-trips to `"opus" != "claude-opus-4-8"`
  → reject; transcript tier Haiku is distinct from both the model-id's Opus and
  the recommendation, so the reject is observable — not masked by an equal tier).
- **INV-9** — End-to-end anti-thrash through the gate: an anchored `current` makes
  `decide()` block the repeat with **only** `target_equals_current`. *Test:*
  compute `current = reconcileCurrentTier(Opus, false, 1000, "sonnet", 2000,
  Sonnet)` (== Sonnet). The Gate splits into two groups: (a) **the pair that
  produces the `target` under test** — `recommended=Sonnet`, `floor=Haiku` (so
  `target = clampToFloor(Sonnet, Haiku) = Sonnet == current` → the guard fires);
  and (b) **guard-pass sentinels for every other guard** — `enabled=true`,
  `focusedState=Idle`, `composerEmpty=true`, `ticksTargetStable=kStableTicks`
  (the named constant, currently 2), `configuredMinDwellMs=90'000` (default),
  `msSinceLastSwitch=9'000'000` (a concrete value far above the dwell floor so
  `dwell_time_insufficient` cannot fire), `msSinceLastOverride=-1`,
  `idleElapsedMs=-1`, `idleCeilingMs=-1`, and `composerStaleMs`/
  `composerStaleThresholdMs=-1` (inert here because `composerEmpty=true` skips the
  composer block, listed only for an explicit, default-free fixture). Assert
  `decide(gate).blockedBy == {"target_equals_current"}` (singleton: the thrash
  guard is the *sole* blocker,
  not an incidental co-veto) and `decide(gate).act == false`. A second row with
  the *unreconciled* stale `current` (Opus) asserts `act == true` (the pre-1944
  thrash), proving the anchor is what suppresses it.
- **INV-10** — The controller passes `reconcileCurrentTier` a `recommendedTarget`
  equal to `clampToFloor(rec.tier, floor)` — the same clamp `decide()` applies to
  `g.recommended` (`modelautoswitch.cpp:68`), since `gate.recommended = rec.tier`
  (`:1520`) and `gate.floor = floor` (`:1521`) feed that internal clamp the
  identical inputs. *Check (review/grep rule, like INV-7 of ANTS-1941 — no runtime
  oracle, the two `clampToFloor` calls are textually identical):* grep that the
  6th argument of the `reconcileCurrentTier(` call in `claudestatuswidgets.cpp` is
  the token `clampedTarget`, **not** `rec.tier`; a mismatch means the anchor
  compares against an unclamped tier the gate would not re-fire.

## 4. RAM / build cost

Two scalar fields on `Result` (one `bool`, one `qint64`; `Result` is a
short-lived stack value). One pure free function. One inline `QDateTime`
ISO-8601 parse of the `turns.last()` JSON object already in memory inside
`score()`'s existing reverse walk (no new file read; `<QDateTime>` is already
included at `modelrecommender.cpp:3`). No new `#include`, no new module coupling
(`modelrecommender` stays free of `modelswitchledger.h`), no new build target, no
new external library.

## 5. Out of scope

- Changing how Claude Code compresses the transcript — not ours to change; the
  fix is robust to *whatever* drops the events because it anchors on the actuator.
- Whether the auto-injected `/model` is logged as a `local_command` at all — the
  fix does not depend on it (if logged, the ANTS-1916 `fromCommand` path catches
  it; if not, the actuator anchor catches the repeat).
- *Detecting* an unlogged manual override during the stale window — pre-existing
  limitation shared with the ANTS-1890 override cool-down (§2.4); ANTS-1944
  neither fixes nor worsens it.
- Persisting the actuator's last tier across process restarts — a fresh process
  has empty `m_autoSwitchLastTier`, so reconciliation falls back to the transcript
  (correct: a new process re-reads the live model from the transcript head).
- A TTL on `m_autoSwitchLastMs` — unnecessary because the anchor only ever
  suppresses re-firing the already-actuated tier (§2.4), so even an hours-old
  record can do nothing but decline a redundant repeat.

## 6. Tests

Feature tests split by unit:
- `score()` provenance (INV-1, INV-2) → `tests/features/model_recommender/`
  (the recommender's existing dir), driving `score()` over `QTemporaryDir`
  transcript fixtures.
- The pure `reconcileCurrentTier` table (INV-3..INV-8) and the `decide()`
  end-to-end (INV-9) → `tests/features/model_auto_switch/`.

Label `features;fast`. Verify the reconciliation + provenance cases **fail
against pre-1944 source** (no `reconcileCurrentTier`, `Result` has no provenance
fields → the gate reads the stale transcript tier and the switch fires) before
restoring the fix.

## 7. Cross-doc impact

- **CLAUDE.md** — note under the model-switcher section that `g.current` is
  actuator-anchored (ANTS-1944, repeat-suppression only) so a stale/compressed
  transcript no longer thrashes the switcher.
- **docs/subsystems.md** — `modelrecommender` / `modelautoswitch` rows: note the
  `Result` provenance fields + `reconcileCurrentTier`.
- **`src/modelrecommender.h`** — the `Result` struct comment block (`:10-18`) and
  `score()`'s contract comment gain the two provenance fields (in-code doc, same
  commit).
- **CHANGELOG.md** — one entry (Fixed; Layman line).
- **ROADMAP.md** — flip ANTS-1944 to ✅ with a resolution note.

## Cold-eyes loop log

Two independent cold reviewers per loop (no prior-loop briefing); every verified
finding fixed in-place, then re-read cold.

- **Loop 1 (2026-06-02)** — 0 CRITICAL, 4 HIGH. The load-bearing catch: the naive
  "always anchor `current` to the actuator" design would **revert an unlogged
  manual `/model` to a different tier**. Redesigned to the *repeat-only* anchor
  (override only when `actuated == recommendedTarget`, §2.4). Also: the `def → r`
  field-propagation gap at `modelrecommender.cpp:337-339`/`259-263`; the
  "already-read timestamp" overclaim; INV-7 didn't exercise the helper.
- **Loop 2** — 0 CRITICAL, 2 HIGH. `parseIso8601Ms` would add a cross-module
  dependency to a leaf scorer → switched to an inline `QDateTime` parse (no new
  include, no test-bundle build surgery). INV-9 under-specified the gate fixture.
  Pinned the clamped-target equivalence (INV-10). Citation fix `d.tierArg`@149.
- **Loop 3** — 0 CRITICAL, 2/1 HIGH. §2.5 still named `parseIso8601Ms` (contradicted
  the inline decision) — reworded. INV-1's `currentModelTsMs == 0` claim was wrong
  given the pre-override populate order — relaxed to don't-care. §2.3 reorder
  ordering pinned; INV-9 added `configuredMinDwellMs`; INV-10 given a concrete grep.
- **Loop 4** — 0 CRITICAL; lone HIGH was INV-9 field-list framing (guard-pass
  sentinels vs the recommended/floor pair under test) — split. Quoted the verified
  `parseIso8601Ms` body to prove the inline parse is byte-identical (settling the
  reviewers' equivalence concern). Pinned INV-9 literals (`msSinceLastSwitch`,
  `kStableTicks`). Both code-side open questions (does CC emit ISO-parseable
  assistant-line timestamps; `<QDateTime>`@3) verified during grounding.
