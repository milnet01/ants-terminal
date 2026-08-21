# ANTS-3771 — Declare a project's id format in `.ants/project.json`

**Status:** spec draft (2026-08-21).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3771 (user-request-2026-08-01); direction chosen
in-session 2026-08-21 (authority = reads *and* writes; a non-matching
lead-in keeps its id and is flagged).
**Blocker for:** ANTS-4491.
**Pairs with:** ANTS-4575.

**Layman:** Let each project write down what its item IDs look like, so the
reader stops guessing which bold heading is an ID and which is just a
sentence — and so a malformed ID is refused when it is written instead of
being discovered years later.

## 1. Problem

Every id rule in the reader is inferred from the text. The live case is the
GFM bold lead-in. `src/roadmapparse.cpp::fillBulletRecord()`'s GFM branch
adopts a leading bold span as the item's id, guarded only by a trailing
colon:

```cpp
if (extractBoldId(head, &boldCand) && !boldCand.endsWith(QLatin1Char(':'))) {
    boldId = boldCand;
}
```

That function's own comment names this item as the fix — *"Nothing in the
text can separate that from a real multi-word id, which is the argument for
ANTS-3771: let a project DECLARE its id format instead of inferring one."*

Three consequences, in the order they cost something.

1. **Prose becomes an identity.** Measured on
   `/mnt/Games/Scripts/Linux/Vestige/ROADMAP.md`, 2026-08-21:
   `grep -cE '^\s*- \[[ x]\] \*\*' ROADMAP.md` → **454** bold lead-ins over
   `grep -cE '^\s*- \[[ x]\]' ROADMAP.md` → **994** GFM bullets. Of those
   454, **192** are the `**AX1. <headline>**` shape
   (`grep -cE '^\s*- \[[ x]\] \*\*[A-Za-z]+[0-9]+\.'`) and **112** carry no
   digit anywhere in the bold span. The store agrees: Vestige holds **427**
   items at `id_origin = 'quarantined'`, of which 323 have a digit and 104
   do not
   (`sqlite3 -readonly ~/.local/share/ants-terminal/roadmap.sqlite "SELECT
   id_origin, COUNT(*) FROM item i JOIN project p USING(project_id) WHERE
   p.export_slug='vestige' GROUP BY 1"`, 2026-08-21).

2. **A whole headline becomes an id.** `extractBoldId()` takes the entire
   bold span, so `**AX1. Geometric / ray-traced audio occlusion**` yields
   the id `AX1. Geometric / ray-traced audio occlusion` rather than `AX1`.
   192 of Vestige's bullets are in that shape and every one of them holds a
   real id it cannot report.

3. **Two bullets sharing a lead-in fold to one identity**, which fails
   ANTS-3756's `UNIQUE (project_id, id_fold)` and refuses that project's
   whole migration (ANTS-3765 § 2.5). ANTS-4575 shipped `id_inferred: true`
   so a caller can *see* the guess; it does not remove it.

**On the write side the prefix is guessed too.**
`src/remotecontrol_roadmap_query.cpp::rcdetail::rlStoreCounterPrefix()`
resolves it as: an explicit `id_prefix` argument, else the store's
`id_prefix` row, else a sniff of the markdown, else the uppercased first
four characters of the project root's leaf directory. Four of the sixteen
registered projects have no `id_prefix` row at all — `mame-curator` (75
items), `contact-list` (62), `rolodex` (36) and `ants-projects-hub-website`
(9) — so for those the answer comes from a sniff or a directory name
(`SELECT p.export_slug FROM project p WHERE NOT EXISTS (SELECT 1 FROM
id_prefix x WHERE x.project_id = p.project_id)`, 2026-08-21). Nothing refuses a malformed id: ANTS-3769 recorded
seven bullets written with a literal `&` in the id and no check fired.

`.ants/project.json` (ANTS-2160) already carries the per-project layout and
is the natural home. It has no id key today, and
`src/projectsettings.cpp::applyWrite()` preserves an unrecognised key
unvalidated (`out[key] = v; // unknown key — preserve (forward-compat)`),
so a hand-added `id_format` is accepted today and read by nothing.

## 2. Surface

### 2.1 The key

One new optional object under the existing root object, both of whose
members are themselves optional:

```json
{
  "roadmap": "ROADMAP.md",
  "id_format": {
    "prefix":  "ANTS",
    "pattern": "^([A-Za-z]{1,4}\\d+)(?:\\.|$)"
  }
}
```

| Member | Type | Half | Consumed by | When absent |
|---|---|---|---|---|
| `prefix` | `string` | generative | `rlStoreCounterPrefix()`, `roadmap_log op:append` | today's four-step resolution, unchanged |
| `pattern` | `string` (PCRE2) | recognitional | `fillBulletRecord()`'s GFM bold branch | today's trailing-colon guard, unchanged |

**There is no `digits` member, and its absence is a measurement rather than
an omission.** `RoadmapFoldIn::renderId()` already renders
`<prefix>-<suffix zero-padded to four digits>`, which ANTS-3765 § 2.8 step 3
fixes deliberately. Across the whole store every id that is not a Vestige
quarantine is four-wide: 4,676 of 5,103 items match `*-[0-9][0-9][0-9][0-9]`,
the remaining 427 are Vestige's quarantines, and **0** ids carry a numeric
suffix at any other width (`SELECT COUNT(*) FROM item WHERE id GLOB
'*-[0-9]*' AND NOT id GLOB '*-[0-9][0-9][0-9][0-9]'` → 0, 2026-08-21). A width member would be a second
statement of a settled fact with nothing to say.

**The two halves are independent.** A project may declare `prefix` alone
(the expected case for the fifteen counter-based projects), `pattern` alone,
or both. `prefix` never affects reading; `pattern` never affects allocation.

### 2.2 The read half

`RoadmapParse` gains a value type and an overload. It stays Qt6::Core-only
and gains **no** dependency on `ProjectSettings` — the caller loads the
declaration and passes it in, the same way it already passes text rather
than a path.

```cpp
namespace RoadmapParse {

// ANTS-3771 — a project's DECLARED id format, as read from
// .ants/project.json's `id_format`. Default-constructed means undeclared,
// which is every project that ships no key and is the zero-change path.
struct IdFormat {
    QString prefix;    // canonical prefix grammar; empty = undeclared
    QString pattern;   // PCRE2, matched against the bold span; empty = undeclared
    bool isDeclared() const { return !prefix.isEmpty() || !pattern.isEmpty(); }
};

QVector<BulletRecord> parseBullets(const QString &markdownText,
                                   const IdFormat &fmt = {});
}
```

The default argument is what keeps every existing call site untouched.

**The pattern governs one branch and no other:** `fillBulletRecord()`'s GFM
bold-lead-in branch, quoted in § 1. The ants-v1 native branch (`rxIdShaped`),
the head-anchored bracket branch (`rxLeadBracketId`), the body-wide `rxId`
and `rxLeadToken` are all unchanged. That boundary is measured, not
stylistic: all 427 quarantined ids in the store are Vestige's, and Vestige
is `github-task-list`; the native branch's `rxIdShaped` already demands a
single whitespace-free token and adopts no prose.

With a `pattern` declared, that branch becomes:

- **Match** → the id is **capture group 1**, or the whole match when the
  pattern has no capturing group. So `**AX1. Geometric / ray-traced audio
  occlusion**` under `^([A-Za-z]{1,4}\d+)(?:\.|$)` yields `AX1`, and the
  remaining text stays the headline. Search semantics, not anchored unless
  the author anchors it — the same rule `spec_conformance` uses for a
  pattern invariant (`specs.md` § 3.5.1).
- **No match** → **the record keeps exactly the id today's heuristic
  assigns.** Nothing is emptied. An emptied id makes the bullet a narrator
  bullet, which `roadmap_query` drops from its default result set, so a
  project declaring a format would silently hide items — and ANTS-4575's
  body already forbids this route by name, because ANTS-1438 ships a Vestige
  fixture whose ids are `Terrain System` and `JustBoldNoSeparator` and both
  are addressable today.

**`id_inferred` reports the declaration, once there is one.** ANTS-4575
defines the flag as *"did the reader ADOPT this record's id from a bold
prose lead-in, rather than read it from a bracket token the author wrote?"*
A pattern match is no longer an adoption the reader invented — it is the
project's own written rule — so the flag is **not** emitted on a match and
**is** emitted on a non-match. `RoadmapParse::idWasInferred()` keeps its
signature and its `id == boldId` test; what changes is that a matched id is
capture group 1 and therefore no longer equals `boldId`, so the existing
predicate reports the new answer with no new field and no new state.

That is the field ANTS-4491's converter needs: after Vestige declares a
pattern, `id_inferred: true` means *this one still needs a decision*, and
its absence means *this id is the project's own*.

### 2.3 The write half

`prefix`, when declared and valid, is inserted into `rlStoreCounterPrefix()`
as a new step **2**, between the explicit argument and the store row:

| Step | Source | Status |
|---|---|---|
| 1 | explicit `id_prefix` argument | unchanged — still wins |
| **2** | **`.ants/project.json` `id_format.prefix`** | **new** |
| 3 | store's `id_prefix` row (`idPrefixFor()`) | unchanged, demoted one place |
| 4 | markdown sniff (`rlResolveCounterPrefix()`) | unchanged |
| 5 | leaf directory, uppercased first four characters | unchanged |

It sits above the store row because the store row records what *this store*
has allocated, while the declaration records what the *project* has decided;
where they disagree the project is right and the store row is a stale
artefact of a migration.

**Refusal.** `roadmap_log op:append` / `op:append_batch` refuse a
caller-supplied `id_hint`-derived or `stable_id` id that the declaration
rejects, with a **new** code `id_format_mismatch`, naming the declared
form. This is the check that would have refused ANTS-3769's seven
`&`-bearing ids.

**Not `bad_id_format`, which is taken and means something else.**
`mcp-error-codes.md` gives that code to a token that is id-*shaped* but
fails the **universal** canonical gate of `roadmap-format.md` § 3.5.1, and
`tests/features/roadmap_id_format_guard` locks it (ANTS-3387, ANTS-3492).
A token can satisfy that universal gate and still breach *this project's*
declaration; folding the two into one code would make the two refusals
indistinguishable to a caller, and the fix for each is different. **Allocation is unaffected** — an allocated id is rendered
by `renderId()` and conforms by construction.

### 2.4 Validation, and the untrusted-input boundary

`prefix` is validated by the **existing** `RoadmapFoldIn::isValidIdPrefix()`
— 1-16 characters of `[A-Za-z0-9_-]` containing at least one ASCII letter
(ANTS-3492). No second grammar is written; the digit-led `3D_E` case is
already correct there.

`pattern` crosses a trust boundary: it is author-supplied text compiled into
a regex and run against every bullet of a file that can be a megabyte
(`specs.md` § 5.4). Three limits, all refusals rather than clamps:

- **512 bytes maximum**, matching the cap `spec_conformance` already applies
  to a pattern (ANTS-4108 § 2.4), so the two agree on what an over-long
  pattern is.
- **It must compile.** `QRegularExpression::isValid()` is checked once, at
  load; a pattern that does not compile is not a pattern.
- **It is compiled once per `parseBullets()` call**, never per bullet.

Failure is asymmetric on purpose, and it follows ANTS-2160's shipped shape:

- **At write time** (`project_settings op:set`), an invalid `id_format` is
  **refused** with `bad_args` — a wrong-typed member, an uncompilable or
  over-long pattern, or a prefix `isValidIdPrefix()` rejects. That is the
  code `applyWrite()` already returns for every invalid value it checks, so
  no new code is owed here. This closes the forward-compat hole quoted in
  § 1, where the key is accepted unexamined today.
- **At read time** (`ProjectSettings::load()`), an invalid member is
  **dropped**, not fatal, exactly as every existing key behaves — an
  unreadable settings file must never take the roadmap down with it. A
  dropped member reads as undeclared, so the fallback in § 2.2 / § 2.3
  applies.

## 3. Invariants

- **INV-1** — A project with no `id_format`, an empty one, or one whose
  every member was dropped by validation parses **byte-identically** to
  today, on every dialect. *Test:* `tests/features/roadmap_id_format_declared`,
  parsing a fixture with and without the key and comparing the full
  `BulletRecord` vector.
- **INV-2** — A declared `pattern` changes the id assigned by
  `fillBulletRecord()`'s **GFM bold-lead-in branch only**. The ants-v1
  native branch, `rxLeadBracketId`, `rxLeadToken` and the body-wide `rxId`
  assign what they assign today. *Test:*
  `tests/features/roadmap_id_format_declared`, a fixture carrying one bullet of each
  shape parsed under a pattern that would match all of them.
- **INV-3** — On a match, `rec.id` is capture group 1 when the pattern has
  a capturing group and the whole match otherwise; the text the pattern did
  not consume remains in the headline. *Test:*
  `tests/features/roadmap_id_format_declared`.
- **INV-4** — On a non-match, `rec.id` is exactly the value the undeclared
  path assigns. **No bullet loses an id it has today**, so no bullet becomes
  a narrator bullet by declaring a format. *Test:*
  `tests/features/roadmap_id_format_declared`, asserting id equality across a
  declared / undeclared pair over the same fixture.
- **INV-5** — `roadmap_query` emits `id_inferred: true` for a bullet whose
  id the GFM branch adopted and the declared pattern did **not** match, and
  omits it when the pattern matched. *Test:*
  `tests/features/roadmap_query_id_inferred` (extended).
- **INV-6** — ANTS-3793 INV-2 still holds: the store backend and the
  markdown backend return identical `BulletRecord`s. The declared pattern
  cannot break it, because `bulletsFromStore()` derives every record from
  `RoadmapRender::bulletText()`, which emits ants-v1, so `gfmHere` is false
  and the branch this spec changes never runs on that path. *Test:*
  `tests/features/roadmap_read_seam`, run against a project carrying
  an `id_format`.
- **INV-7** — A valid declared `prefix` wins over the store's `id_prefix`
  row and the markdown sniff, and loses to an explicit `id_prefix` argument.
  *Test:* `tests/features/roadmap_id_format_declared`, over a fixture project whose
  store row and declaration disagree.
- **INV-8** — `roadmap_log op:append` / `op:append_batch` refuse a
  caller-supplied id the declaration rejects, with code
  `id_format_mismatch` — never `bad_id_format`, which stays the universal
  gate's — and write nothing. *Test:* `tests/features/roadmap_id_format_declared`.
- **INV-9** — `project_settings op:set` refuses an `id_format` whose
  `prefix` fails `isValidIdPrefix()`, or whose `pattern` exceeds 512 bytes,
  fails to compile, or is not a string — and the file on disk is unchanged.
  *Test:* `tests/features/roadmap_id_format_declared`.
- **INV-10** — `ProjectSettings::load()` drops an invalid member rather than
  failing, leaving the rest of the settings object intact. *Test:*
  `tests/features/roadmap_id_format_declared`, loading a file whose `pattern` does
  not compile and asserting `roadmap` still resolves.
- **INV-11** — Allocation still renders `<prefix>-<four-digit suffix>` via
  `RoadmapFoldIn::renderId()`; this spec introduces no width of its own.
  *Test:* source-grep — `renderId()` remains the only id renderer on the
  allocation path.

### 3.1 The worked pattern, stated so it can be run

The example § 2.1 shows, in the runnable form `specs.md` § 3.5.1 defines —
so `spec_conformance` executes it rather than a reader checking it:

```regex pcre2
^([A-Za-z]{1,4}\d+)(?:\.|$)
```

| input | expected |
|---|---|
| `AX1. Geometric / ray-traced audio occlusion` | `AX1` |
| `A1` | `A1` |
| `Aerodynamics` | no match |
| `Photo mode` | no match |

**This example is illustrative and is not Vestige's declaration.** It
deliberately fails on `FW W5`, a real id in that corpus, to make the point
that the pattern is each project's to author and to measure against its own
file — this spec fixes the mechanism, never the pattern.

## 4. RAM / build cost

No new state, no new build target, no new external library, no new library
dependency edge — `RoadmapParse` takes `IdFormat` by value and never reaches
for `ProjectSettings`.

Per `parseBullets()` call: one `QRegularExpression` compiled once from a
string capped at 512 bytes, plus the `IdFormat`'s two `QString`s. Bounded by
the cap, not by the corpus. The existing per-bullet regexes are `static
const` and unaffected.

`ProjectSettings::load()` is uncached by design (ANTS-2160 § 2.2) and gains
one nested-object read; the file is tiny and already read at points that
touch the filesystem.

## 5. Out of scope

- **Converting a `github-task-list` roadmap to ants-v1** — ANTS-4491. This
  spec gives that converter the input it needs and performs no rewrite.
- **Declaring the id format for the ants-v1 native branch or the pass-headings
  dialect.** Not deferred, excluded: measured above, all 427 off-grammar ids
  in the store are on the GFM branch, and widening the change to branches
  with no measured defect adds risk for nothing. A future project that needs
  it can file for it.
- **A `digits` / width member** — permanently excluded, see § 2.1. Width is
  ANTS-3765 § 2.8's and is uniform across the corpus.
- **Backfilling or re-migrating existing store rows** once a project declares
  a format. The declaration governs reads and future writes; re-deciding
  `id_origin` for rows already stored is a migration, and ANTS-4585 phase 3
  owns the store-side repasses.
- **Anything that empties an id.** Permanently excluded — § 2.2 gives the
  reason and ANTS-4575's body forbids it by name.

## 6. Tests

Feature test: `tests/features/roadmap_id_format_declared/`. Covers INV-1
through INV-4 and INV-7 through INV-10. Label `features;fast`. **INV-11 is a
source-grep and has no case here** — its claim is that no second id renderer
appeared, which a passing test cannot show. Ask
`build_target_for` which bundle owns the new source rather than guessing —
bundles are not derivable from the path — and check that `ctest -N -R` moves
before and after.

Extended: `tests/features/roadmap_query_id_inferred/` for INV-5 (the
declared-match case, which that suite has no fixture for today), and
`tests/features/roadmap_read_seam/` for INV-6.

Per the project test convention, verify each new assertion **fails against
pre-change source**, on assertions rather than on a compile error — stub the
new predicate to the undeclared answer and confirm the red run before
implementing.

## 7. Cross-doc impact

- § 2.1's key table in `docs/specs/ANTS-2160.md` gains `id_format`. It is the
  first non-path key there, so that section's sentence
  "Every value is a repo-root-relative path." needs qualifying.
- `docs/specs/ANTS-3765-roadmap-migration-load.md` — § 2.8 step 1's prefix
  resolution gains the declaration above the store row.
- `docs/specs/ANTS-3793-roadmap-consumer-cutover.md` — no change to INV-2 or
  to the 22-member census; § 2.1.1 gains a note that the declared pattern is
  unreachable on the store path.
- `docs/standards/mcp-config-keys.md` — `id_format` catalogued.
- `docs/standards/mcp-error-codes.md` — one new row, `id_format_mismatch`,
  stating how it differs from the neighbouring `bad_id_format`.
- `docs/standards/roadmap-format.md` — § 3.5.1's grammar gains a pointer
  saying a project may declare its own.
- ROADMAP.md ANTS-4491 — its remaining blocker clears when this ships.
- CHANGELOG.md — one entry.

## Cold-eyes loop log
