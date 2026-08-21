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
| `prefix` | `string` | generative | `rlStoreCounterPrefix()`, `roadmap_log op:append`, and the migration allocator's chain at ANTS-3765 § 2.8 step 1 (§ 2.3) | today's four-step resolution, unchanged |
| `pattern` | `string` (PCRE2) | recognitional | `fillBulletRecord()`'s GFM bold branch; `RoadmapMigrate::isGrammaticalId()`, on an explicit parameter (§ 2.4) | today's trailing-colon guard, unchanged |

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
and gains **no** dependency on `ProjectSettings`: the declaration arrives as
a value, the same way text already arrives rather than a path.

```cpp
namespace RoadmapParse {

// ANTS-3771 — a project's DECLARED id format, as read from
// .ants/project.json's `id_format`. Default-constructed means undeclared,
// which is every project that ships no key and is the zero-change path.
struct IdFormat {
    QString prefix;    // canonical prefix grammar; empty = undeclared
    QString pattern;   // PCRE2, matched against extractBoldId()'s output
                       // (see below); empty = undeclared
    bool isDeclared() const { return !prefix.isEmpty() || !pattern.isEmpty(); }
};

QVector<BulletRecord> parseBullets(const QString &markdownText,
                                   const IdFormat &fmt = {});
}
```

**The default argument keeps every existing call site compiling, and that is
exactly the hazard.** A caller that omits the argument silently reads a
project's roadmap as undeclared, so one bullet resolves to two different ids
depending on which path reached it. There are 31 `parseBullets` mentions
across `src/` outside `roadmapparse.*` (`grep -rn 'parseBullets(' src/`,
2026-08-21), which is far too many to audit per change.

**`RoadmapSource::bulletsFor()` cannot load it, and the build graph is why.**
It lives in `ants_roadmapstore_lib`, whose `target_link_libraries` names
`Qt6::Core Qt6::Sql ants_roadmapparse_lib` and carries the comment *"the
reader, NOT ants_core_lib: this surface stays headless for ANTS-3794's
publish path"*. `ProjectSettings` is in `ants_core_lib`, which links
`ants_roadmapstore_lib` PRIVATE — so the edge runs core → store, and reading
`.ants/project.json` inside the seam would invert it.

**So `bulletsFor()` takes an `IdFormat` parameter, exactly as `parseBullets()`
does, and the load happens core-side.** That is this section's own stated
principle applied consistently: the declaration arrives as a value, never as
a path the callee resolves.

**Which core-side helper owns that single load is an OPEN DESIGN DECISION and
is deliberately not settled here** — see § 8. What this spec does fix is the
requirement: exactly one, not one per entry point.

**The bare one-argument overload survives only for a caller that holds no
project root** — a test fixture, an ad-hoc string. That is the test, and it
is not "the text came from somewhere else": `remotecontrol_roadmap_backfill.cpp`
parses added lines *of a project's own roadmap* and therefore takes the
declaration like any other project-scoped caller. **A caller holding a project
root and taking the bare overload is a defect, and INV-13 is what catches
it.**

**Inside the parser the pattern governs one branch:** `fillBulletRecord()`'s
GFM bold-lead-in branch, quoted in § 1. Outside it, exactly one other
function consults the declaration — `RoadmapMigrate::isGrammaticalId()`,
which takes it as a parameter (§ 2.4). Those two are the whole consumer set
for `pattern`. The ants-v1 native branch (`rxIdShaped`),
the head-anchored bracket branch (`rxLeadBracketId`), the body-wide `rxId`
and `rxLeadToken` are all unchanged. That boundary is measured, not
stylistic: all 427 quarantined ids in the store are Vestige's, and Vestige
is `github-task-list`; the native branch's `rxIdShaped` already demands a
single whitespace-free token and adopts no prose.

With a `pattern` declared, that branch becomes:

**The pattern is matched against `extractBoldId()`'s output — the bold span
with its trailing `.` already dropped — not against the raw span.** Named
because the two build differently: against the stripped candidate
`^([A-Za-z0-9]+)$` matches `**AX1.**`, and against the raw span it does not,
so every project's pattern is authored to whichever this sentence picks.
`extractBoldId()` chops **one** trailing `.` and then trims, and its own
regex is `^\*\*(.{1,80}?)\*\*` — so a bold run over 80 characters yields no
candidate at all and a declaration cannot reach it. That is today's
behaviour and this spec does not change it; it is stated because a project
authors its pattern against this input.

- **Match** → the id is **capture group 1**, or the whole match when the
  pattern has no capturing group. So `**AX1. Geometric / ray-traced audio
  occlusion**` under `^([A-Za-z]{1,4}\d+)(?:\.|$)` yields `AX1`, and the
  remaining text stays the headline. Search semantics, not anchored unless
  the author anchors it — the same rule `spec_conformance` uses for a
  pattern invariant (`specs.md` § 3.5.1).

  **A match wins the id outright, including over the body-wide `rxId`.**
  Today `fillBulletRecord()` assigns `rec.id` from the first
  `[<PREFIX>-NNNN]` found ANYWHERE in the body and only falls back to the
  lead-in — so `- [ ] **AX1. foo** … see [ANTS-9999]` reports the citation.
  That is a known wart the parser's own comment records; under a declaration
  the project has said which text names its items, so the lead-in is
  authoritative and the citation is a citation. **This is the one place the
  declaration changes a branch other than the bold fallback**, and INV-2 is
  scoped to say so.

  **A match sets all three id fields, and leaving any of them to the
  implementer breaks something downstream.** `rec.id` and `rec.idToken` both
  take the captured value; **`rec.boldId` is left EMPTY.** `boldId` means
  *the reader adopted this bold span as an id by inference*, and under a
  declaration it did not — the project wrote the rule down. `idToken` follows
  because `RoadmapMigrate` classifies on the leading-slot token (§ 2.4), and
  leaving it as the whole bold run would quarantine the very bullet the
  declaration just resolved.

  **`boldId` has a second consumer, and emptying it is a visible change.**
  `roadmap_query` emits `bold_id` only when the field is set — five sites,
  `grep -c 'o\["bold_id"\] = b.boldId' src/remotecontrol_roadmap_query.cpp`
  → 5 (2026-08-21) — and ANTS-1438 has a shipped test at
  `tests/features/gfm_adapter_bold_id_multitoken`. So on a
  matched bullet of a declaring project `bold_id` is **absent**. That is
  intended and ANTS-1438's field keeps its meaning — it reports a bold span
  the reader adopted as an id, and under a declaration it adopted nothing.
  INV-1's byte-identity promise does not cover it, because INV-1 is about
  undeclared projects; INV-3 carries it instead.
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
**is** emitted on a non-match.

**That outcome comes from the emptied `boldId` above, and from nothing
else.** `RoadmapParse::idWasInferred()` is
`!rec.boldId.isEmpty() && rec.id == rec.boldId`, so an empty `boldId` makes
it false on every match, and the predicate keeps its signature with no new
field and no new state. **Reasoning instead that the captured id "no longer
equals `boldId`" is wrong**, and the worked table in § 3.1 is the
counter-example: `A1` captures the *whole* span, so `id == boldId` holds and
an unchanged `boldId` would fire the flag on a match. A pattern whose
capture spans its whole input is the ordinary case for a project whose ids
are bare tokens, not a corner.

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

**The migration allocates from its own chain, and it takes the declaration at
the same rank.** ANTS-3765 § 2.8 step 1 resolves a prefix as: the stored
`id_prefix` row, else the most frequent prefix among parsed ids, else the
uppercased first four characters of the leaf directory. A declared `prefix`
is inserted **above all three**. Without that, one declaring project gets its
declared prefix from `roadmap_log` and a sniffed or directory-derived one from
a migration allocation — two id families in one store, which is the outcome
`UNIQUE (project_id, id_fold)` cannot even detect.

**Refusal, and the predicate is pinned here because "the declaration
rejects" has three readings that build three different refusal surfaces.**
A caller-supplied written id — `stable_id`, or the id an `id_hint` renders —
is refused when **either** test fails:

| Test | Applies | Code |
|---|---|---|
| `roadmap-format.md` § 3.5.1's universal grammar, as `RoadmapMigrate::isGrammaticalId()` already spells it | always, declaration or not | `bad_id_format` |
| the text before the id's final `-` equals the declared `prefix` | only when `prefix` is declared | `id_format_mismatch` |

**`pattern` takes no part in a written id.** It is matched against a GFM
bold span and nothing else, so running it over a bracket id would reject a
conforming `ANTS-0042` under § 3.1's own example — which is why § 2.1 calls
it recognitional and this table does not name it.

**ANTS-3769 is caught by the first row, not by the declaration**, and the
distinction matters because that row fires on projects that declare nothing.
`ANTS-119&` has the prefix `ANTS` and would pass a prefix test; what refuses
it is the universal grammar, whose suffix must be `\d+`. What this spec adds
there is the **check point** — that gate does not reach `op:append`'s written
ids today, which is why those seven were written unchallenged.

**Not `bad_id_format`, which is taken and means something else.**
`mcp-error-codes.md` gives that code to a token that is id-*shaped* but
fails the **universal** canonical gate of `roadmap-format.md` § 3.5.1, and
`tests/features/roadmap_id_format_guard` locks it (ANTS-3387, ANTS-3492).
A token can satisfy that universal gate and still breach *this project's*
declaration; folding the two into one code would make the two refusals
indistinguishable to a caller, and the fix for each is different. **Allocation is unaffected** — an allocated id is rendered
by `renderId()` and conforms by construction.

### 2.4 What the declaration does to a migration

`RoadmapMigrate` classifies a bullet's leading-slot token with
`isGrammaticalId()`, which tests `RoadmapParse::idTokenPattern()` —
`PREFIX-\d+`. `AX1` has no `-`, so it fails, and the bullet is quarantined.
That is why Vestige holds 427 quarantined items today.

**Under a declared `pattern`, a bullet whose lead-in matched is `parsed`,
not `quarantined`.** The declaration is the project's own grammar, and
recording a declared id as off-grammar would say the opposite.

**The mechanism is pinned here because nothing on the record carries it.**
`isGrammaticalId(const QString &token)` sees a token and nothing else, and
after § 2.2 a declared `AX1` is byte-indistinguishable from an off-grammar
`Cl9`: both arrive as `idToken` with an empty `boldId`. Reasoning that the
two "agree by construction" is therefore wrong — `isGrammaticalId("AX1")` is
false however the migration got there.

**So `isGrammaticalId()` gains an `IdFormat` parameter** and accepts a token
when the universal grammar accepts it **or** the declared pattern matches it
whole. Two consequences worth stating:

- **It never narrows.** A project that declares a pattern does not thereby
  make `ANTS-0042` off-grammar; the universal arm is unconditional.
- **`BulletRecord` gains no member.** A `bool idDeclared` was the obvious
  alternative and is rejected: it would be a 23rd member against ANTS-3793's
  census, which that spec states as a count and whose INV-2 requires both
  backends to derive every member identically. Passing the declaration to the
  one function that asks the question costs nothing there.

**This is why the item exists.** Leaving the migration gate alone would let
a project declare a format and change no `id_origin` at all — the
declaration would be inert exactly where ANTS-4491's converter reads.

**Re-migrating a project that already migrated is § 5's, and excluded.**
This section governs the classification a migration performs *when it runs*,
never a repass over rows already stored.

### 2.5 Validation, and the untrusted-input boundary

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
  over-long pattern, or a prefix `isValidIdPrefix()` rejects. **`applyWrite()`
  gains an `id_format` branch above its unknown-key passthrough**; without
  one the key keeps falling into `out[key] = v` and INV-9 cannot pass. The
  branch owes no new *refusal code* — `bad_args` is what that function
  already returns for a wrong-typed or invalid value, alongside `bad_path`
  for the path keys, which `id_format` is not.
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
- **INV-2** — Inside `fillBulletRecord()` a declared `pattern` changes only
  the GFM bold-lead-in branch and the `rec.id` precedence § 2.2 pins: on a
  match the lead-in beats the body-wide `rxId`. The ants-v1 native branch,
  `rxLeadBracketId` and `rxLeadToken` assign what they assign today, and on a
  non-match every branch including `rxId` does. *Test:*
  `tests/features/roadmap_id_format_declared`, with a GFM bullet whose body
  cites an unrelated `[ANTS-9999]` parsed under a matching and a non-matching
  declaration. *Test:*
  `tests/features/roadmap_id_format_declared`, a fixture carrying one bullet of each
  shape parsed under a pattern that would match all of them.
- **INV-3** — On a match, `rec.id` **and** `rec.idToken` are capture group 1
  when the pattern has a capturing group and the whole match otherwise,
  `rec.boldId` is empty, and the text the pattern did not consume remains in
  the headline. The pattern is applied to `extractBoldId()`'s output, not to
  the raw bold span. *Test:*
  `tests/features/roadmap_id_format_declared`, including a bullet whose bold
  span ends in `.` so the stripped-vs-raw input is distinguishable.
- **INV-4** — On a non-match, `rec.id` is exactly the value the undeclared
  path assigns. **No bullet loses an id it has today**, so no bullet becomes
  a narrator bullet by declaring a format. *Test:*
  `tests/features/roadmap_id_format_declared`, asserting id equality across a
  declared / undeclared pair over the same fixture.
- **INV-5** — `roadmap_query` emits `id_inferred: true` for a bullet whose
  id the GFM branch adopted and the declared pattern did **not** match, and
  omits it when the pattern matched — **including when the capture spans the
  whole bold span**, which is the case an unchanged `boldId` would get wrong.
  *Test:* `tests/features/roadmap_query_id_inferred` (extended), with a
  `**A1**`-shaped fixture as well as an `**AX1. …**` one.
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
  caller-supplied written id by § 2.3's two-row table and write nothing: the
  universal grammar yields `bad_id_format` with or without a declaration, a
  prefix disagreeing with a declared `prefix` yields `id_format_mismatch`,
  and `pattern` is applied to neither. *Test:* `tests/features/roadmap_id_format_declared`.
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
- **INV-12** — Under a declared `pattern`, a migration records a bullet whose
  lead-in matched as `id_origin = 'parsed'`, not `'quarantined'`, and
  `isGrammaticalId()` still accepts every token the universal grammar accepts.
  *Test:* `tests/features/roadmap_id_format_declared`, migrating a
  Vestige-shaped fixture with and without a declaration and comparing the
  `id_origin` census.
- **INV-13** — Every path that parses a project's roadmap resolves the same
  id for the same bullet. `bulletsFor()` and `parseBullets()` both take the
  `IdFormat` as a parameter; **a caller that holds a project root and takes a
  bare overload is a defect**, and a caller that holds none is the only
  legitimate user of one. *Test:*
  `tests/features/roadmap_id_format_declared`, asserting `roadmap_query`,
  `roadmap_log`'s markdown read, `RoadmapDialog` and the backfill path return
  one id for one declared-project bullet; plus a source-grep over the bare
  overload's call sites checked against whether each holds a root.

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
- **Backfilling or re-migrating rows a migration already stored.** A
  migration that RUNS under a declaration honours it (§ 2.4); what is excluded
  is going back over rows written before the project declared one. Re-deciding
  a stored `id_origin` is a repass, and ANTS-4585 phase 3 owns those.
- **Anything that empties an id.** Permanently excluded — § 2.2 gives the
  reason and ANTS-4575's body forbids it by name.

## 6. Tests

Feature test: `tests/features/roadmap_id_format_declared/`. Covers INV-1
through INV-4, INV-7 through INV-10, and INV-12 / INV-13. Label
`features;fast`. **INV-11 is a source-grep and has no case here** — its claim
is that no second id renderer appeared, which a passing test cannot show.
**INV-13 is half source-grep**: its behavioural half has cases, its
no-caller-takes-the-bare-overload half does not. Ask
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
- `docs/specs/ANTS-3793-roadmap-consumer-cutover.md` — no change to INV-2 or
  to the 22-member census; § 2.1.1 gains a note that the declared pattern is
  unreachable on the store path.
- `docs/specs/ANTS-3757-*` / `docs/specs/ANTS-3765-roadmap-migration-load.md`
  — § 2.5/2.6's parsed-vs-quarantined gate now has a second accepting rule
  (§ 2.4) and `isGrammaticalId()` gains an `IdFormat` parameter; ANTS-3765
  § 2.8 step 1's prefix chain gains the declaration above all three of its
  existing terms.
- `docs/specs/ANTS-3808-*` — § 4's library-layering note is the reason the
  declaration cannot load inside the read seam (§ 2.2); no change owed, cited
  so the constraint is findable.
- `docs/standards/mcp-config-keys.md` — `id_format` catalogued.
- `docs/standards/mcp-error-codes.md` — one new row, `id_format_mismatch`,
  stating how it differs from the neighbouring `bad_id_format`.
- `docs/standards/roadmap-format.md` — § 3.5.1's grammar gains a pointer
  saying a project may declare its own.
- ROADMAP.md ANTS-4491 — its remaining blocker clears when this ships.
- CHANGELOG.md — one entry.

## 8. Open questions

- **Which core-side helper owns the single load of `id_format`.** § 2.2
  establishes that `RoadmapSource::bulletsFor()` cannot do it — the edge runs
  `ants_core_lib` → `ants_roadmapstore_lib` and ANTS-3808 § 4 keeps the store
  surface headless on purpose — and that exactly one core-side owner is
  required rather than one per entry point. **Which one is not settled here.**
  Three candidates, and the choice has build-graph consequences a document
  review should not make alone: a new helper in `ants_core_lib` that every
  project-scoped roadmap read funnels through; moving `ProjectSettings` to a
  leaf library, which drags `pathvalidation` and `codebaseindex` with it; or
  splitting only the JSON-reading half out. **This blocks implementation of
  INV-13 and nothing else** — the rest of the spec is independent of it.
- **Whether the backfill path's records reach the store.** § 2.2 puts
  `remotecontrol_roadmap_backfill.cpp` on the project-scoped side because it
  parses a project's own roadmap lines. If those records are never stored, the
  cost of exempting it is lower than the cost of wiring it, and the exemption
  becomes defensible. Not measured.

## Cold-eyes loop log

| Loop | Date | Lanes | Q-count | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-21 | 3, cold — genre pinned `spec`; one byte-stable shared packet carrying 8 code windows, the ANTS-3793 § 2.1.1 / ANTS-3765 § 2.8 / ANTS-2160 § 2.1 passages, and the store + Vestige measurements | **Q1 1 · Q2 0 · Q3 5** (6 verified / 1 dismissed) | **Six verified, six fixed; one dismissed.** **All three lanes independently found the same two**, which is the strongest signal the run produced. **[Q1] The `id_inferred` reasoning was false, and the spec's own worked table was the counter-example.** § 2.2 claimed a matched id "is capture group 1 and therefore no longer equals `boldId`", so `idWasInferred()` — `!rec.boldId.isEmpty() && rec.id == rec.boldId` — would report the new answer unchanged. The § 3.1 row `` \| `A1` \| `A1` \| `` captures the *whole* bold span, so `id == boldId` holds and the flag fires **on a match**, breaching INV-5 and feeding ANTS-4491's converter "still needs a decision" for ids the project declared. A whole-span capture is the ordinary case for bare-token ids, not a corner. Fixed by having a match set `boldId` empty, which is also the truer statement: an adoption by declaration is not an adoption by inference. **[Q3] The write-time refusal predicate was never defined** — "an id the declaration rejects" had three readings (prefix equality, `pattern` re-purposed against a bracket id, or the universal grammar), each producing a different `id_format_mismatch` surface for callers to bind to. The ANTS-3769 claim was false under two of them: `ANTS-119&` has the prefix `ANTS` and passes a prefix test. Now a two-row table, with that item re-grounded on the universal grammar and on this spec adding the *check point* rather than the check. **Three more Q3s, each an invention something else binds to.** The pattern's input was "the bold span", which has two readings — `extractBoldId()` chops a trailing `.` and trims, so `^([A-Za-z0-9]+)$` matches `**AX1.**` against one and not the other, and every project authors its pattern to whichever. `rec.idToken` was unstated on a match, so one builder leaves the whole bold run there and `isGrammaticalId()` quarantines the bullet the declaration just resolved. And the migration's parsed-vs-quarantined gate was neither in scope nor excluded — which would have made a declaration inert exactly where ANTS-4491 reads it; § 2.4 now owns it. **The best finding came from an Open question, not a Findings section:** § 2.2 said "the caller loads the declaration and passes it in" and named no caller, while the overload's default argument silently reads a project as undeclared — 31 `parseBullets` mentions in `src/` outside `roadmapparse.*`, so one bullet could resolve to two ids depending on the path. The load point is now pinned at `RoadmapSource::bulletsFor()`, which already receives the `projectRoot`. **Dismissed as unverified:** one lane read INV-6 as false on the ground that "Vestige is in the store, so the rendered text for a migrated project is GFM there". `renderBullet()` writes `"- " + emojiFor(it.status)` and `roadmaprender.cpp` contains no checkbox emission at all, so the store path renders ants-v1 whatever the project's source dialect — matching ANTS-3793 § 2.1.2's own table. **The other two lanes raised the same point as an Open question rather than a finding, and that was a packet gap of mine**: no window covered `bulletText()`. **Also fixed, as this loop's own collateral:** the sentence "The default argument is what keeps every existing call site untouched" survived the seam fix and directly contradicted the new INV-13. |
| 2 | 2026-08-21 | 3, cold — identical brief, packet rebuilt from disk and given the three windows loop 1 lacked (`renderBullet()`, `extractBoldId()`, `bulletsFor()`) | **Q1 2 · Q2 4 · Q3 1** (7 verified / 0 dismissed) | **Seven verified, seven fixed. Cap reached (2 for a spec); the run files its tail and exits.** **This is a VIOLENT cap, and the measurement says so plainly: six of the seven findings landed on text loop 1 wrote.** Each was a mechanism loop 1 pinned without tracing that field's consumers — the sweep covered the document and not the code that reads it. **The worst was found by resolving a lane's Open question, not by a Findings section, and it was unbuildable:** loop 1 put the single load of `id_format` inside `RoadmapSource::bulletsFor()`, which lives in `ants_roadmapstore_lib` — whose link list is `Qt6::Core Qt6::Sql ants_roadmapparse_lib` under the comment *"the reader, NOT ants_core_lib: this surface stays headless for ANTS-3794's publish path"*, while `ants_core_lib` links the store PRIVATE. `ProjectSettings` is in core, so the seam cannot read `.ants/project.json` without inverting a deliberate edge. `bulletsFor()` now takes the declaration as a parameter, and **which core-side helper owns the load is surfaced as § 8 rather than invented here** — it has build-graph consequences a document review should not settle alone. **All three lanes found the migration mechanism, and it was genuinely unbuildable too:** § 2.4 asserted `isGrammaticalId()` "accepts a token the declared pattern resolved" and justified it as agreeing "by construction", but that function sees a token and nothing else, and after loop 1 emptied `boldId` a declared `AX1` is byte-indistinguishable from an off-grammar `Cl9`. It now takes an `IdFormat` parameter — chosen over a `BulletRecord::idDeclared` flag, which would be a 23rd member against ANTS-3793's stated 22-member census. **Two lanes found the `rxId` precedence**, which loop 1 created by pinning `rec.id`: the body-wide `rxId` runs first and wins, so `**AX1. foo** … see [ANTS-9999]` reported the citation. A declared match now wins outright and INV-2 is scoped to say so. **Three more:** `bold_id` silently disappears from the envelope on a matched bullet (five emit sites, plus a shipped ANTS-1438 test) — intended, now stated; "no new code is owed here" read as *no new source code*, leaving `applyWrite()`'s forward-compat passthrough intact and INV-9 unpassable; and § 2.1's `prefix` row omitted the migration allocator, so a declaring project would take its declared prefix from `roadmap_log` and a sniffed one from a migration — two id families in one store. **One count of my own was wrong for the second run running**: `bold_id` has five emit sites, not three, because the grep that found them was truncated. **Why the cap binds, since that is evidence about the run:** 6/7 collateral means this gate was repairing itself rather than the draft. A third loop is not indicated — the document goes to implementation, which exercises the contract against real code and is the better third reviewer. |
