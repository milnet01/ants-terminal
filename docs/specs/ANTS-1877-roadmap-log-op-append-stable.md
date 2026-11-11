# ANTS-1877 — `roadmap_log op:append` stable-prefix project hint

**Status:** spec draft, cold-eyes loops 1 + 2 folded (2026-05-26).
**Kind:** enhancement.
**Source:** ROADMAP.md ANTS-1877 (vestige-feedback-2026-05-26 Obs
#20).

**Layman:** When a project uses stable string IDs (`Sh4`, `MT8`)
instead of numeric `[PROJ-NNNN]` IDs, the existing
`roadmap_log op:append` can't help — it requires `.roadmap-counter`
to allocate a new ID. Today the refusal envelope just says
"counter read failed", leaving the caller to guess what's wrong.
This change splits the failure mode: when the counter is genuinely
absent (file doesn't exist) AND the existing roadmap uses
stable-prefix IDs, surface a clear hint explaining the limitation
and pointing at the Edit-direct workaround.

## 1. Problem

Vestige's CC session created 30+ new bullets via Edit because
`roadmap_log op:append` requires a `.roadmap-counter` (absent on
stable-string-ID projects). The existing failure code
`counter_read_failed` covers both "file missing" and "file
corrupted" — the caller can't tell which, and the hint doesn't
mention that some projects don't use `.roadmap-counter` at all by
design.

## 2. Surface

### 2.1 Refusal-envelope split

The existing block in `cmdRoadmapLogAppend` is:

```cpp
QFile cf(counterPath);
if (!cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return rlErr("counter_read_failed",
        "roadmap_log: could not read .roadmap-counter at \"%1\"" …);
}
```

After this change, when the file doesn't exist (`!QFile::exists(counterPath)`),
sniff the roadmap markdown for stable-prefix bullet IDs and pick a
more specific refusal:

| Condition                                                       | code                              |
|-----------------------------------------------------------------|-----------------------------------|
| `.roadmap-counter` exists but unreadable (permissions)          | `counter_read_failed` (unchanged) |
| `.roadmap-counter` exists but not a number (non-empty corrupt)  | `counter_read_failed` (unchanged) |
| `.roadmap-counter` exists but empty (zero bytes / whitespace)   | `counter_missing` (new — treat empty as "not initialised") |
| `.roadmap-counter` missing AND existing bullets use stable IDs  | `stable_prefix_unsupported`       |
| `.roadmap-counter` missing AND existing bullets carry any id    | `counter_missing` (lost high-water mark — real desync) |
| `.roadmap-counter` missing AND no bullet ids of any kind        | **auto-init at 0** (ANTS-3397; see below) |

> **ANTS-3397 amendment.** The last row above originally refused with
> `counter_missing` too. It now **auto-creates `.roadmap-counter` at 0
> and proceeds**: a counter-strategy append on a roadmap with neither a
> counter file nor any existing bullet ids is an unambiguous greenfield
> project, and forcing a shell `echo 0 > .roadmap-counter` side-step
> broke the all-MCP workflow. The discriminator is the
> `rlRoadmapHasAnyBulletId(markdown)` helper (any non-empty id in the
> first 50 parsed bullets → NOT greenfield). The "existing bullets carry
> any id" row keeps refusing — re-allocating from 0 over an already-used
> id space would mint duplicates. Applies to both `op:append` and
> `op:append_batch` (parity, ANTS-1879). If the auto-create write fails,
> the call refuses with `counter_write_failed`.

The new envelope shapes:

```jsonc
// stable_prefix_unsupported
{
  "ok": false,
  "code": "stable_prefix_unsupported",
  "error": "roadmap_log op:append needs .roadmap-counter; this
            project uses stable-string IDs (e.g. \"Sh4\", \"MT8\")
            which the allocator doesn't currently support.",
  "detected_prefix_example": "Sh4",
  "hint": "Edit ROADMAP.md directly to add bullets; the verb
           handles ANTS-NNNN-style projects only today.",
  "follow_up": "ANTS-1877"
}

// counter_missing
{
  "ok": false,
  "code": "counter_missing",
  "error": "roadmap_log: .roadmap-counter does not exist at
            \"<absolute path>\" — touch the file with the current
            high-water mark to enable op:append.",
  "hint": "echo 0 > <repo-root>/.roadmap-counter (or the highest
           existing [PROJ-NNNN] number) — then re-run op:append."
}
```

### 2.2 Stable-prefix sniffer

A new helper `rlDetectStablePrefixId(const QString &markdown) →
QString` walks the first 50 parsed bullets (per `parseBullets`) and
returns the first bullet `id` that:

- Is non-empty.
- Does NOT match `^ANTS-[0-9]+$` (the canonical numeric pattern).
- Matches `^[A-Za-z][A-Za-z0-9_-]+$` (looks like a stable
  identifier — `Sh4`, `MT8`, `phase_10_audio_design`, etc.).

Returns the matching id (used as `detected_prefix_example`), or an
empty string if no stable-prefix bullet is found within the cap.
Cap chosen for cheap O(n) scan on cold cache.

### 2.3 Out of scope

Actually implementing stable-prefix ID allocation (option (b) in
the ROADMAP bullet) is deferred. The diagnostic hint closes
Vestige's option (a) — "Sniff the last 50 bullets for ID shape;
surface a hint when counter is empty" — which is the practical
unblocker. Full allocator support tracked as a follow-up under the
same ID once a project actually adopts it.

## 3. Invariants

- **INV-1** — `cmdRoadmapLogAppend` distinguishes
  `.roadmap-counter` missing from `.roadmap-counter` unreadable.
  The existing-and-unreadable path keeps `code:"counter_read_failed"`;
  the missing-file path uses one of `counter_missing` or
  `stable_prefix_unsupported` depending on the sniffer's result.
  *Test:*
  `tests/features/roadmap_log_stable_prefix_hint/test_roadmap_log_stable_prefix_hint.cpp::Inv1FileMissingVsUnreadable`.
- **INV-2** — `rlDetectStablePrefixId` returns the first
  non-ANTS-NNNN bullet id that matches
  `^[A-Za-z][A-Za-z0-9_-]+$` in the first 50 parsed bullets, or
  empty string when none. *Test:* `…::Inv2SnifferContract`.
- **INV-3** — `stable_prefix_unsupported` envelope carries
  `detected_prefix_example` (the sniffed id) and `follow_up`
  (string `"ANTS-1877"`). *Test:* `…::Inv3StablePrefixEnvelope`.
- **INV-4** — `counter_missing` envelope carries a `hint`
  pointing at the `echo 0 > <path>/.roadmap-counter` recipe.
  *Test:* `…::Inv4CounterMissingHint`.
- **INV-5** (ANTS-3397) — a counter-strategy `op:append` /
  `op:append_batch` on a roadmap with NO `.roadmap-counter` AND no
  bullet ids of any kind auto-creates `.roadmap-counter` at 0 and
  succeeds (first id allocates as `<prefix>-0001`); the same call on a
  roadmap that already carries ids still refuses with `counter_missing`
  (the lost-high-water-mark desync). Discriminator:
  `rlRoadmapHasAnyBulletId`. *Tests:*
  `tests/features/roadmap_log_greenfield_counter/test_roadmap_log_greenfield_counter.cpp::{Inv1SingleAppendGreenfieldAutoInit, Inv2BatchGreenfieldAutoInit, Inv3ExistingIdsNoCounterStillRefuses}`.

## 4. Tests

`tests/features/roadmap_log_stable_prefix_hint/` (added to the
`test_claude` bundle, label `features;fast`):

- **`spec.md`** — mirrors INV-1..INV-4 in test-anchor form.
- **`test_roadmap_log_stable_prefix_hint.cpp`** — source-grep style
  against `cmdRoadmapLogAppend` for the new code paths + envelope
  fields. Per project convention, each invariant is first verified
  to FAIL against pre-fix code (the literals
  `stable_prefix_unsupported`, `counter_missing`,
  `detected_prefix_example`, `follow_up` are absent), then GREEN
  after the fix.

| INV | Test                                  | Anchor                                       |
|-----|---------------------------------------|----------------------------------------------|
| 1   | `Inv1FileMissingVsUnreadable`         | both new codes appear in cmdRoadmapLogAppend |
| 2   | `Inv2SnifferContract`                 | `rlDetectStablePrefixId` helper signature    |
| 3   | `Inv3StablePrefixEnvelope`            | `detected_prefix_example` + `follow_up` literals |
| 4   | `Inv4CounterMissingHint`              | `echo 0 >` recipe hint literal              |

## 5. RAM / build cost

Zero new state. ~30 LoC helper, ~20 LoC dispatch split, ~80 LoC
test → **~130 LoC** total.

## 6. Migration

None. The existing `counter_read_failed` code stays in use for
the "file exists but unreadable" / "file exists but not a number"
cases. Callers that currently branch on `counter_read_failed`
will continue to see it on those paths; the new codes only fire
on the missing-file path that previously emitted
`counter_read_failed` unconditionally.

## 7. Cross-doc impact

- `docs/standards/mcp-error-codes.md` — add `counter_missing` +
  `stable_prefix_unsupported` under category § 2 *Resource state*
  (matches the existing `no_roadmap`, `counter_read_failed`
  neighbours).

## 8. Cold-eyes loop log

- **Loop 1 (2026-05-26).** Spec drafted with the diagnostic
  splits + sniffer contract. Verified against
  `src/remotecontrol.cpp` `cmdRoadmapLogAppend` (line 3102 — the
  block whose refusal needs splitting). Scope deliberately limited
  to Vestige's option (a); allocator support deferred (option b).
- **Loop 2 (2026-05-26).** Reviewer flagged 2 HIGH (§4 Tests
  missing INV-mapping / label / fail-first sentence per
  `specs.md:131-138`; decision table didn't address zero-byte
  `.roadmap-counter` case — recipe `echo 0 >` works but
  `touch` doesn't), 3 MEDIUM (mcp-error-codes category unstated;
  sniffer regex ≥2 char minimum undocumented; `follow_up`
  parity between the two new codes), 4 LOW/INFO polish. All
  verified against `cmdRoadmapLogAppend:3109-3116` (empty-file
  parse) + `docs/standards/mcp-error-codes.md:47-104`. Fixes:
  expand §4 with INV-mapping table + label + fail-first
  convention; add zero-byte-counter row to §2.1 table routing
  to `counter_missing`; name mcp-error-codes category § 2
  *Resource state*.
