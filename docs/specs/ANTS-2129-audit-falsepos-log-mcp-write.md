# ANTS-2129 — `audit_falsepos_log` MCP write verb (false-positive ledger)

**Status:** implemented + shipped (2026-06-15); accepted after cold-eyes loops 1–6. See tests/features/audit_falsepos_log/ + the audit-false-positives.md / audit-skill updates.
**Kind:** feature.
**Source:** ROADMAP.md ANTS-2129 (cross-session-2026-06-12 DOOM; carve-out of ANTS-2106 idea 3).
**Composes with:** ANTS-1457 (the `.ants_review_falsepos.jsonl` read side + standard whose model this mirrors); ANTS-1962 (`feedback_log`, the nearest write-verb sibling — its `QSaveFile` mechanism is deliberately **not** reused; § 2.4).

(ANTS-1687 — the `/audit` step-10.5 recipe this verb replaces — and ANTS-1701 — writer-side validation the read path assumed — are cited inline in § 1 / § 5.)

## 1. Problem

The false-positive ledger `.ants_review_falsepos.jsonl` (ANTS-1457) has a
**read** side only. `src/falseposledger.cpp` exposes `loadEntries` /
`filter` / `formatForBrief` / `formatForJsonArray`, and the brief-assembly
tools (`cold_eyes_brief`, `indie_review_brief`, `test_audit_brief`) inject
prior dismissals so a re-run does not re-litigate them. There is **no MCP
write verb**.

A CC session that confirms a false positive (the `/audit` step-10.5 fold-in,
ANTS-1687) appends to the ledger by hand. The standard
(`docs/standards/audit-false-positives.md` § *Atomic append*) **forbids the
`Write` tool** — read-modify-write interleaves catastrophically when two CC
sessions append concurrently — and hands over a raw `jq` + `printf '\n%s\n'
>>` shell recipe instead. DOOM re-confirmed 2026-06-12 (audit-fix bundle):
the recipe was hand-rolled twice in one session for two cppcheck FPs.

The standard already **names** this verb as anticipated future work
(§ *Cross-references*, the ANTS-1353 row: *"a hypothetical future
`audit_falsepos_log` write tool would use `read_failed` / `write_failed` per
the taxonomy"*) and § *Limitations* notes the v1 write-side is the
CC-shell-recipe only.

**Layman:** When an audit flags something that's actually fine, a session has
to hand-write a fiddly one-line shell command to record "known false alarm,
here's why". This adds a single helper that does it safely in one call, so
future audits skip re-flagging it.

## 2. Surface

A new single-purpose MCP write verb `audit_falsepos_log`. One operation
(append one ledger record); no `op` arg — the verb name *is* the operation.

### 2.1 Arguments

| Arg | Required | Shape / default |
|-----|----------|-----------------|
| `caller_cwd` | yes | Dispatcher `CallerCwdContract::Required`. Resolves the project root (§ 2.3). |
| `claim` | yes | One-line FP summary. Trimmed to 280 UTF-16 units on write (§ 2.5). Absent or empty → `bad_args`. |
| `rationale` | yes | Why it's an FP (the load-bearing field). Trimmed to 1024 UTF-16 units on write. Absent or empty → `bad_args`. |
| `review_kind` | yes | One of `audit` \| `cold-eyes` \| `indie-review` \| `test-audit`. Any other value, including absent or empty-string → `bad_args` (§ 2.2 / INV-10). |
| `timestamp` | no | `YYYY-MM-DD`. Default: today. A present value failing `QDate::fromString(s,"yyyy-MM-dd").isValid()` → `bad_args`. |
| `lane` | no | Filter tag; default empty (matches all lanes on read). |
| `topic` | no | Short grouping tag; default empty. |
| `logged_by` | no | Audit-trail tag; default `"cc-session"`. |

`kind` / `source` / `headline` (roadmap_log-style args) are not part of this
verb; unknown args are ignored, not refused (forward-compat parity with the
ledger's read-side "ignore unknown fields" rule).

**Refusal-code choice — `bad_args`, not `missing_field`.** The absent and the
malformed cases for `claim` / `rationale` / `review_kind` / `timestamp` are all
caught by a *single* guard each (`LedgerEntry::isValid` for claim/rationale/
timestamp; the canonical-set membership check for `review_kind`), so there is
no presence-vs-value branch to distinguish — absent and empty-string take the
identical code path. The taxonomy explicitly permits folding both into
`bad_args` when one guard spans absent-and-malformed
(`docs/standards/mcp-error-codes.md:36`: *"a verb is free to fold both into
`bad_args` … as `roadmap_log`'s pass-headings path does"* — ANTS-2126). This
verb takes that fold rather than emitting the absent-arg-only `missing_field`
that `feedback_log` / `spec_log` use, because it has no absent-only guard.

### 2.2 Why `review_kind` is required-and-canonical

The read-side schema (ANTS-1457 § Schema) marks `review_kind` *required:
yes* but allows empty to mean "broadcast to all sweeps — use sparingly". The
**write verb** tightens this to required-and-canonical: a confirmed FP almost
always belongs to exactly one sweep, and forcing the caller to name it keeps
the ledger disciplined and the brief-injection filters (`filter()`,
`falseposledger.cpp:198`) precise. Writing a broadcast (empty-`review_kind`)
entry is the rare case the standard says to use sparingly; it stays a
hand-edit and is **out of scope** for v1 (§ 5, OQ-1). The read path still
honours hand-written empty-`review_kind` entries — this verb just won't
*emit* them.

### 2.3 Root resolution + ledger path

The ledger path is **fixed** at `<root>/.ants_review_falsepos.jsonl`. `<root>`
is resolved **directly** from `caller_cwd` via
`QFileInfo(caller_cwd).canonicalFilePath()` — the same `m_main`-independent
pattern the sibling write verbs use (`roadmap_log`,
`remotecontrol_roadmap_log.cpp`; `feedback_log`; `spec_log`), which keeps the
handler unit-testable with a `RemoteControl(nullptr)`. For a present
`caller_cwd` this yields the identical canonical, isDir-checked root that
`ants::resolveCallerCwdRoot` (ANTS-1401) would return — its Case-2
(ExplicitMatch) and Case-3 (NoMatch) both return
`QFileInfo(callerCwd).canonicalFilePath()`, and the `Required` contract
guarantees `caller_cwd` is present so the empty-fallback branch never fires —
so the write root matches the root the brief-assembly tools pass to
`loadEntries`. An empty result (caller_cwd absent or not an existing
directory) → `no_project`. No `PathValidation::validatePath` is applied — the
basename is a fixed literal, not caller-supplied (ANTS-1457 § Cross-references
confirms this posture).

### 2.4 Append mechanism — `O_APPEND`, NOT `QSaveFile`

`feedback_log`'s `QSaveFile` read-whole-file → rewrite-whole-file approach
(`remotecontrol_feedback.cpp`) is **wrong for this ledger** and is deliberately
not reused. The standard mandates true append: `open(O_APPEND)` + a single
`write()` of the complete record. In Qt that is
`QFile::open(QIODevice::WriteOnly | QIODevice::Append)` →
`O_WRONLY | O_CREAT | O_APPEND` (confirmed by the standard's § Cross-references
"Future C++ writer" note). The record bytes are:

```
"\n" + <compact-json> + "\n"
```

- **Leading `\n`** is the self-healing prefix (§ Atomic append): on a clean
  file it produces a skippable blank line; on a torn-mid-write file it
  terminates the orphan line so the new record starts clean (INV-2).
- **Trailing `\n`** ends the record so the file always terminates in a
  newline. Both newlines are deliberate (the standard's recipe is
  `printf '\n%s\n'`): adjacent records are therefore **blank-line-separated**
  (record N's trailing `\n` + record N+1's leading `\n` → one blank line),
  which `loadEntries` reads line-by-line (`falseposledger.cpp:186-189`,
  chomping the trailing `\n`) and skips as an empty line (`parseLine`,
  `falseposledger.cpp:100`). `bytes_appended` (§ 2.8 / INV-11) counts both
  newlines.
- **`<compact-json>`** is `QJsonDocument(obj).toJson(QJsonDocument::Compact)`
  — one physical line. `Compact` escapes embedded control chars (`\n`, `\r`,
  …) as JSON escapes in **every** string field — `claim`, `rationale`, and
  also `lane` / `topic` / `logged_by` — so no field can break the
  one-physical-line JSONL framing (INV-3). No write-side control-char
  stripping is needed (the read side's `stripLineBreaks`,
  `falseposledger.cpp:67`, handles brief-injection sanitisation on the way
  out). `QJsonObject` serialises keys in sorted order; readers are key-keyed,
  so order is irrelevant.
- Mode **0644** on create (review metadata, not a secret — explicitly NOT
  `setOwnerOnlyPerms()`, which is for the 0600 config files). Set
  deterministically via `QFile::setPermissions(ReadOwner|WriteOwner|ReadGroup|
  ReadOther)` after a create so a restrictive umask can't leave it 0600 (only
  on create — an existing file's perms are left as the user set them).

### 2.5 Trim-then-bound

1. **Trim** `claim` to 280 and `rationale` to 1024 UTF-16 code units via the
   existing `surrogateAwareTruncate` (`falseposledger.cpp:42`, anonymous
   namespace — `appendEntry`'s **definition** lives in the same TU, so it
   calls the helper directly; the header only declares `appendEntry`). The
   standard requires writers to trim before append. `surrogateAwareTruncate`
   appends the `…` ellipsis **only when the input exceeds the cap**
   (`falseposledger.cpp:43` — `if (in.size() <= cap) return in;`); an
   at-or-under-cap value (including one that already ends in `…`) is written
   unchanged.
2. **Bound-check** the *final* encoded record (`"\n" + json + "\n"`, UTF-8
   bytes). The read caps do **not** mathematically guarantee the < 3.5 KiB
   on-disk bound: 1024 UTF-16 units of BMP-range CJK encode to ~3 KB of UTF-8,
   plus `claim`, JSON structure, and the optional tags can exceed 3584 bytes.
   If the encoded record is ≥ 3.5 KiB after trim → `bad_args`
   ("record exceeds 3.5 KiB after trim — shorten rationale"); the file is
   **not** touched (INV-7). The 3.5 KiB ceiling is the Linux atomic-`write(2)`
   bound the append-atomicity contract relies on.

### 2.6 Pre-write file-type guard

Before opening for append, `lstat` the ledger path. If it exists and is **not**
a regular file (symlink, FIFO, directory, device) → `write_failed`
("not a regular file"); no append through a symlink. This mirrors the read
side's `isRegularFile` S_ISREG posture (`falseposledger.cpp:87`). If the path
does not exist, `O_APPEND | O_CREAT` creates it (INV-8).

The `lstat`→`open` window is a TOCTOU (a symlink could be swapped in between
the check and the open). This is **accepted** under the same-UID trust model
([ADR-0004](../decisions/0004-same-uid-trust-model-for-mcp-audit-suite.md)) —
the read side carries the identical window and accepts it for the same reason;
no flock/`O_NOFOLLOW` hardening in v1.

### 2.7 Implementation shape

A new pure helper joins the existing `ants::falsepos` namespace:

```cpp
// src/falseposledger.h
struct AppendResult {
    bool    ok = false;
    QString code;          // "" on success; else bad_args / write_failed
                           // (no_project is handler-side, not emitted here)
    QString message;
    qint64  bytesAppended = 0;   // on-disk bytes ("\n"+json+"\n")
    bool    created = false;     // file did not exist before
    QString timestamp;           // the effective (possibly defaulted) date
};

// Validates — claim/rationale non-empty + timestamp valid via
// LedgerEntry::isValid (falseposledger.cpp:133-141), AND review_kind canonical
// via a SEPARATE canonicalReviewKinds() membership check (isValid does NOT
// check review_kind) — then trims to the read caps, bound-checks < 3.5 KiB,
// and appends one record via O_APPEND. Never throws. projectPath is the
// resolved root.
AppendResult appendEntry(const QString &projectPath, const LedgerEntry &e);
```

`cmdAuditFalseposLog` in `remotecontrol.cpp` resolves the root (§ 2.3), builds
a `LedgerEntry` from the args (defaulting `timestamp`/`logged_by`), calls
`appendEntry`, and wraps the result in the response envelope (§ 2.8). Putting
the file I/O + validation in the helper keeps it unit-testable without the
`m_main` guard (mirrors `changeloglog.{h,cpp}` / `passheadingwrite.{h,cpp}`)
and reuses the read side's `surrogateAwareTruncate` + `canonicalReviewKinds` +
`LedgerEntry::isValid` rather than duplicating them.

**Name parallel (deliberate).** `ants::auditfp::appendEntry(const QString&,
Entry)` already exists (`auditfpledger.h:62`) — the writer for the *sibling*
`.audit_suppress` (code-grain) ledger. `ants::falsepos::appendEntry` mirrors
it on purpose: the two ledgers "coexist" (ANTS-1457 standard) and a parallel
writer name across the two namespaces signals the parallel role. The
namespace qualifies them; a bare `appendEntry` grep returning both is expected,
not a clash.

**Naming split.** `LedgerEntry`'s fields are **camelCase**
(`reviewKind`, `loggedBy`; `falseposledger.h:25-32`); the on-disk JSON keys
and this verb's request args are **snake_case** (`review_kind`, `logged_by`).
`cmdAuditFalseposLog` reads the snake_case args, sets the camelCase struct
fields, and `appendEntry` emits the snake_case JSON keys (the same keys
`parseLine` reads, `falseposledger.cpp:109-115`).

### 2.8 Response envelope

Success: `{ok:true, path, bytes_appended, created, timestamp, review_kind}`.
`path` is the **absolute** resolved ledger path
`<root>/.ants_review_falsepos.jsonl` (parity with `feedback_log`, which
returns the absolute resolved `path`). `bytes_appended` is the on-disk byte
count (`"\n"+json+"\n"`, including both newlines); `created` is true when the
file was absent before; `timestamp` echoes the effective (possibly defaulted)
date so the caller need not re-derive it. Only `review_kind` and `timestamp`
are echoed (not `lane`/`topic`/`logged_by`): those two are the fields the verb
may *transform* — `timestamp` defaults to today, `review_kind` is the
canonicalised gate value — while the rest pass through verbatim, so the caller
already knows them.

Refusal: `{ok:false, code, error}` with `code` ∈ {`bad_args`, `no_project`,
`write_failed`} (+ the dispatcher's `caller_cwd_required`).

### 2.9 Wiring sites (parity with `feedback_log`)

- `src/mainwindow.cpp` — `registerToolProvider("audit_falsepos_log",
  CallerCwdContract::Required, rcDelegate(&RemoteControl::cmdAuditFalseposLog))`
  (next to the feedback verbs, `:4897`).
- `src/claudeintegration.cpp` — schema descriptor block (model on the
  `feedback_log` block `:3330`); `CallerCwdContract::Required` map entry
  (`:8797` sibling). State-routing (the `name==…` ladder whose
  `feedback_log` branch is at `:7866`): the verb has no read sibling
  registered as an in-app verb (the ledger read is internal to briefs) and
  the read cache self-invalidates on mtime/size change
  (`falseposledger.cpp:170-173`, the comparison — not `:163`, the cache
  declaration),
  so **no explicit `name==…` branch is added** — the verb falls through to the
  chain's default `return QStringLiteral("other")`
  (`claudeintegration.cpp:7903`), the same bucket every uncategorised verb
  uses; no shadowing of another verb's state category.
- `src/remotecontrol.h` / `.cpp` — `cmdAuditFalseposLog` declaration + body.

## 3. Invariants

- **INV-1** — appending to a pre-populated ledger leaves every prior byte
  unchanged and writes the new record as a strict byte-**suffix** of the file
  (the observable contract of the `QIODevice::Append` /
  `O_WRONLY|O_CREAT|O_APPEND` mechanism in § 2.4 — the "no read-modify-write"
  property is the *mechanism*, stated there, not a black-box-testable claim
  here). *Test:* append to a pre-populated ledger; `bytes_before` of the file
  are byte-identical and the appended record is the remaining suffix
  (`audit_falsepos_log` INV-1).
- **INV-2** — each appended record is bracketed by a leading `\n` and a
  trailing `\n` (so adjacent records are blank-line-separated). Appending to a
  file whose last line lacks a trailing newline (torn write) terminates the
  orphan and the new record parses cleanly via `loadEntries`. *Test:* seed a
  file ending in a partial JSON line with no `\n`; after append, `loadEntries`
  returns the new entry (orphan skipped) and the on-disk bytes show the
  leading + trailing `\n` (INV-2).
- **INV-3** — the JSON record is a single physical line; an embedded control
  char (`\n`/`\r`/…) in **any** string field (`claim`, `rationale`, `lane`,
  `topic`, `logged_by`) is JSON-escaped by `toJson(Compact)`, not written raw,
  so no field can break the JSONL framing. *Test:* a `rationale` AND a `lane`
  each containing a literal `\n`; the on-disk record spans one physical line
  and `loadEntries` round-trips both (INV-3).
- **INV-4** — required-field validation refuses `bad_args` with the file
  byte-untouched when `claim` or `rationale` is absent/empty. (Malformed
  `timestamp` is INV-5; non-canonical `review_kind` is INV-10 — each refusal
  lives in exactly one invariant.) *Test:* empty-`claim` and empty-`rationale`
  cases; ledger byte-identical before/after (INV-4).
- **INV-5** — `timestamp` defaults to today (`QDate::currentDate()`,
  `yyyy-MM-dd`) when absent; a present value is validated. *Test:* absent →
  written entry's timestamp == today; `"2026-13-40"` → `bad_args` (INV-5).
- **INV-6** — `claim` is trimmed to 280 and `rationale` to 1024 UTF-16 code
  units (surrogate-aware) before encoding. Per `surrogateAwareTruncate`
  (`falseposledger.cpp:42-59`, `left(cut) + "…"` with `cut ≤ cap` — `cut` may
  retreat *below* `cap` past a trailing high-surrogate or combining mark,
  `:44-57`), the on-disk value is **at most `cap` sliced units followed by a
  trailing `…`** (so ≤ 281 / ≤ 1025 units total). *Test:* a 2000-char
  `rationale` → on-disk `rationale` ends in `…`, its sliced portion is `≤
  1024` units (assert `≤`, never `==`), and no surrogate pair is split
  (INV-6).
- **INV-7** — when the encoded record (`"\n"+json+"\n"`, UTF-8) is ≥ 3.5 KiB
  (3584 bytes) after trim, the verb refuses `bad_args` and the file is
  untouched. *Test:* a deterministic worst-case seed whose multibyte content
  pushes the post-trim record over the bound — e.g. `rationale` = 1024×
  `U+4E00` + `claim` = 280× `U+4E00` (each `U+4E00` is 3 UTF-8 bytes, so the
  trimmed CJK payload alone runs several hundred bytes past 3584, before tags
  or the framing newlines — the refusal does not depend on `lane`/`topic`).
  The test asserts the *behaviour* (encoded record ≥ 3584 ⇒ refuse `bad_args`
  + byte-identical file), not an exact byte count, so a Qt-encoding delta
  won't break it. A same-shape all-ASCII record (well under the bound) is the
  passing control (INV-7).
- **INV-8** — appending when the ledger is absent creates it (mode 0644),
  returns `created:true`, and the leading-`\n` blank first line is skipped by
  `loadEntries`, which returns the one written entry. *Test:* append into an
  empty `QTemporaryDir`; file exists 0644, `loadEntries` size == 1,
  `created:true` (INV-8).
- **INV-9** — if the ledger path exists and is not a regular file (symlink /
  directory / FIFO), the verb refuses `write_failed` and writes nothing — the
  `lstat`/`S_ISREG` guard's load-bearing case is the symlink (no append
  through a symlink). *Test:* two seeds — (a) `mkdir
  .ants_review_falsepos.jsonl` (directory) and (b) `symlink("/tmp/elsewhere",
  ".ants_review_falsepos.jsonl")` — each append → `write_failed`, and for (b)
  the symlink target is left untouched (INV-9).
- **INV-10** — `review_kind` must be one of `audit` / `cold-eyes` /
  `indie-review` / `test-audit`; any other value (including empty/absent) →
  `bad_args`. *Test:* `"frobnicate"` and `""` refuse; `"audit"` succeeds
  (INV-10).
- **INV-11** — success envelope carries `{ok, path, bytes_appended, created,
  timestamp, review_kind}`; `bytes_appended` is the byte length of the record
  *this call* wrote (`"\n"+json+"\n"`), computed in-process — NOT a
  before/after file-size diff (which a concurrent appender could inflate).
  *Test:* envelope fields present and `bytes_appended` equals the length of
  the record string the call built (the primary oracle). As a secondary check
  *valid only in this single-writer `QTemporaryDir`*, it also equals
  `size_after - size_before` (INV-11).
- **INV-12** — a round-tripped record re-parses through `loadEntries` to a
  `LedgerEntry` whose fields `claim` / `rationale` / `reviewKind` / `lane` /
  `topic` / `timestamp` / `loggedBy` match what was written (post-trim).
  *Test:* write then `loadEntries`; field-equality — seed `lane`/`topic`/
  `logged_by` with **control-char-free** values, since `parseLine` runs
  `stripLineBreaks` on those three on read (`falseposledger.cpp:124-126`), so
  a tag containing C0/DEL would not round-trip byte-for-byte (that read-side
  strip is by design — those fields land outside the brief's data fence;
  `claim`/`rationale` are unaffected). (`LedgerEntry` fields are camelCase per
  `falseposledger.h:25-32`; the on-disk JSON keys are snake_case —
  `review_kind` / `logged_by` — per § 2.1 / ANTS-1457 § Schema.)

## 4. RAM / build cost

No held state — `appendEntry` is a transient open-write-close; the only
allocation is the single record string (≤ 3.5 KiB) plus the `LedgerEntry`. No
new TU (the helper joins the existing `falseposledger.{h,cpp}` in
`ants_core_lib`); `remotecontrol.cpp` gains one handler (~60 LoC). No new
external library (Qt6::Core `QFile`/`QJsonDocument`, already linked). The read
path's process-wide parse cache is untouched and invalidates on the mtime/size
change the append produces (`falseposledger.cpp:170-173`).

## 5. Out of scope

- **Broadcast (empty-`review_kind`) entries** — the rare "applies to all
  sweeps" case stays a hand-edit; the verb requires a canonical kind (§ 2.2,
  OQ-1).
- **`audit_falsepos_query` read verb** — briefs already inject the ledger via
  `cold_eyes_brief` / `indie_review_brief` / `test_audit_brief`; no confirmed
  second consumer for a standalone reader yet (Rule of Three). Deferred (OQ-2).
- **`audit_falsepos_verify`** (flag entries whose `rationale` cites deleted
  paths) — the standard's § Pruning / ANTS-1701 anticipates it; separate item
  (OQ-3).
- **`.audit_suppress` (the code-grain ledger) write surface** — ANTS-2106
  idea 3's "fingerprint dedup via `auditfpledger`" clause concerns that
  *other* ledger; this verb scopes only the prose `.ants_review_falsepos.jsonl`
  (OQ-4).
- **Pruning / rotation** — append-only by contract; the verb never rewrites or
  prunes (ANTS-1457 § Pruning is a user responsibility).

## 6. Migration / compatibility

Net-new verb — no existing behaviour changes. The read side, its cache, the
brief-injection, and all four sweep skills are untouched. The standard's two
forward-looking notes are reconciled to "shipped" (§ 8). No feature test is
narrowed or repurposed.

## 7. Tests

New feature test `tests/features/audit_falsepos_log/` (`spec.md` + a
`test_audit_falsepos_log.cpp` driving `cmdAuditFalseposLog` against a seeded
`.ants_review_falsepos.jsonl` in a `QTemporaryDir`, plus pure-helper unit
cases for `appendEntry`). Label `features;fast`. Bundle assignment: the bundle
that already carries the falsepos read tests / the feedback_log test (verify
at implementation — likely `test_claude`; do NOT `add_executable`). Per the
project convention each new write assertion is verified to **fail against
pre-fix source** (the verb does not exist) before the handler is wired.

## 8. Cross-doc impact

- `docs/standards/audit-false-positives.md` — § Atomic append gains "or the
  `audit_falsepos_log` MCP verb, which performs the atomic append for you";
  § Limitations / § Cross-references "future write tool" notes flip from
  hypothetical to shipped (verb name, `bad_args`/`write_failed` codes) — and
  **strike the stale `read_failed` mention** from that anticipatory note (the
  verb is append-only and never reads, so only `write_failed` applies). Also
  **soften the § File-location "the read-side caps … keep records well under"
  claim** (line ~76): the spec proves multibyte content can exceed 3.5 KiB
  (§ 2.5), so the standard should read "usually under — writers MUST
  bound-check the encoded record (ANTS-2129 § 2.5)". The
  § Schema `review_kind` row (which today reads "Empty (or omitted) means 'all
  kinds'") gains a one-line annotation: *the `audit_falsepos_log` verb
  requires a canonical kind and will not emit an empty `review_kind`;
  broadcast entries remain a hand-edit (ANTS-2129 § 2.2)* — so the standard
  doesn't describe an omittable field with no pointer to the verb that refuses
  it.
- `.claude/skills` — the `/audit` skill step 10.5 (ANTS-1687) gains a line:
  prefer `audit_falsepos_log` over the hand-rolled `jq`+`printf` recipe (the
  recipe stays documented as the no-MCP fallback). **Doc-only edit, no INV /
  test**; in-scope for the ship commit (not deferred).
- `CLAUDE.md` — the MCP-tool authoring section's verb list / `tool_info`
  catalog gains the verb (the catalog is generated from the descriptors, so
  the descriptor in § 2.9 is the source of truth).
- `CHANGELOG.md` — one `Added` entry on ship.
- `docs/standards/mcp-error-codes.md` — no new code (reuses `bad_args` /
  `no_project` / `write_failed`); no edit required.

## 9. Open questions

- **OQ-1** — Support a broadcast (empty-`review_kind`) write path, or keep
  required-and-canonical? *Proposed:* required-and-canonical; broadcast stays
  a hand-edit (§ 2.2).
- **OQ-2** — Ship `audit_falsepos_query` now as a pair, or defer? *Proposed:*
  defer — briefs already read the ledger; no second consumer yet.
- **OQ-3** — `audit_falsepos_verify` (stale-rationale detector). *Proposed:*
  separate future item.
- **OQ-4** — Should `logged_by` be enum-validated (`cc-session` /
  `user-confirmed` / `external`) or free-text? *Proposed:* free-text with a
  `cc-session` default — the read side treats it as informational and strips
  control chars; an enum adds a refusal path for a non-load-bearing field.

## Cold-eyes loop log

Rows live in [`../reviews/ANTS-2129-audit-falsepos-log-mcp-write-loop-log.md`](../reviews/ANTS-2129-audit-falsepos-log-mcp-write-loop-log.md).
