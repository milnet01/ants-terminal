<!-- ants-coding-standards: 3 -->
# Coding Standards — Ants Terminal deltas

> **The standard itself is `~/.claude/standards/coding.md`.** This half of the
> file carries only what is specific to *this* project — the C++/Qt spellings
> and the one project-local helper — and must never grow back into a
> restatement of its owner.
>
> **The owner is mirrored verbatim below the divider**, between the
> `MIRROR BEGIN` / `MIRROR END` markers, because this repo is public and an
> outside reader cannot open a path inside a private home directory. **Do not
> edit that half.** A correction goes upstream, then
> `tools/check-standard-mirrors.sh --write` re-copies it down;
> `tools/hooks/pre-commit` refuses a commit whose mirror has drifted from its
> owner (ANTS-4133).

**Why this file is a delta (2026-08-12).** Until today it was a verbatim copy
of the global standard, dropped in by `/start-app` with an instruction to keep
it in sync that nothing checked. It had drifted since 2026-06-02, and by the
time it was reconciled it was instructing three things the current global
standard forbids — most sharply, it told you to add language sections *here*,
which is the exact growth global § 5 exists to prevent. Two copies are two
standards that will disagree; this one lost.

## Where the rules actually live

| You want | Read |
|---|---|
| Principles — shortest correct implementation, no workarounds, reuse before rewriting, the six-month test, latest stable library | global `coding.md` § 1 |
| State an assumption rather than building on it | global `coding.md` § 1.6 |
| Surgical changes — every changed line traces to the reason you are there | global `coding.md` § 1.7 |
| Error handling | global `coding.md` § 2 |
| **When a workaround is permitted** | global `coding.md` § 1.2 — *not* § 2 |
| Comments | global `coding.md` § 3 |
| Naming | global `coding.md` § 4 |
| Performance | global `coding.md` § 6 |
| Anti-patterns | global `coding.md` § 8 |
| Security — validate at the boundary, atomic writes, never log a credential | [`security.md`](security.md) — a verbatim mirror of the global owner, kept in-repo because this repo is public |
| **C++ spellings** — version floor, idioms | global `languages/cpp.md` |
| **Qt spellings** — `tr()`, `QSaveFile`, `Q_OBJECT`, parent-child ownership, new-style `connect` | global `languages/qt.md` |
| **Python spellings** — casing (`snake_case`, *not* the camelCase this file used to mandate), idioms | global `languages/python.md` |

**Adding a language?** Create `~/.claude/standards/languages/<name>.md`. Do
not add a section here, and do not add one to the global `coding.md` either.

## Project-local rules

These are the whole of this project's additions. Everything else is global.

### House style

- **K&R braces.**
- `m_` for member variables, **`s_` for static members**, `PascalCase` types,
  `camelCase` functions. (The `s_` prefix is the project-local part; the rest
  is `languages/qt.md`.)
- **`#pragma once` everywhere; no include guards.**
- **Signals/slots for cross-component communication — never a direct call
  between siblings**, where *sibling* means two components neither of which
  owns the other. A component calling something it owns (`m_grid->…`) is not
  a sibling call and is not a breach — there are 244 such call sites. This is
  the architectural rule behind the module map in
  [`docs/subsystems.md`](../subsystems.md); a direct sibling call couples two
  lanes that are meant to be independently reviewable.

These four were previously stated only in
[`CONTRIBUTING.md`](../../CONTRIBUTING.md) § Code style highlights, which
pointed here "for the full list" — at a file that did not contain them.

### `setOwnerOnlyPerms()` is ours, not Qt's

Global `languages/qt.md` § Idioms lists "`setOwnerOnlyPerms()` on any file
holding config or secrets" beside `tr()` and `QSaveFile`, as though it were a
Qt API. **It is ours**, defined in
[`src/secureio.h`](../../src/secureio.h) — not a Qt built-in, and not
available in any other Qt project. It has two overloads
(`QFileDevice&` and `const QString&`). Reach for it rather than hand-rolling
a `QFile::setPermissions()` call, and do not go looking for it in the Qt
documentation.

## What checks this

Nothing mechanical checks the house-style rules; they are read in review.

`setOwnerOnlyPerms()` is **partially** checked. Hand-rolling the bitmask is
caught: `setPermissions_pair_no_helper` is a hardcoded grep check
(`src/auditdialog.cpp`, not `audit_rules.json`) with fixtures at
`tests/audit_fixtures/setPermissions_pair_no_helper/` and a row in
`tests/audit_self_test.sh`. **Nothing** catches the other half — a config
writer that never sets permissions at all compiles and passes.

**The mirrored half is checked.** `tools/hooks/pre-commit` runs
`tools/check-standard-mirrors.sh`, which fails the commit if the text between
the MIRROR markers no longer matches `~/.claude/standards/coding.md`. It skips
on a checkout with no global standards tree — an outside contributor's, or
CI's — since there is then nothing to compare against.

## Review loop log

| Loop | Date | Lanes | Q1/Q2/Q3/Q4 | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-12 | 1 (cold, general-purpose) | Q1 2 · Q2 2 · Q3 1 · Q4 n/a | 5 verified, 5 fixed, 0 dismissed. casing routed to cpp.md where cpp.md itself defers Qt projects to qt.md; `What checks this` claimed nothing covers `setOwnerOnlyPerms()` when `setPermissions_pair_no_helper` is a fixture-backed grep check; the qt.md delta was stated backwards; "sibling" was undefined against 244 owner-to-owned call sites; CONTRIBUTING's workaround pointer landed on § 2 rather than § 1.2 (fixed there). |

---

<!-- MIRROR BEGIN ~/.claude/standards/coding.md -->
# Coding Standards — v1

**Purpose: so that code written now is still readable and changeable by
someone who was not there when it was written.**

Every rule here traces to that sentence. A rule that does not is either
a language detail (`languages/`), another standard's job, or nobody's.

**This file holds concepts only.** How a concept is spelled in a given
language — casing, idioms, the API that replaced the one you remember —
lives in `languages/<name>.md`, because spellings go stale and concepts
do not. See the [index](README.md) for the full set of standards.

Governs ROADMAP bullets with `Kind: implement`, `fix`, `refactor`,
`audit-fix`, or `review-fix`.


## 1. Principles

### 1.1 Shortest correct implementation

50 lines beats 250. No scaffolding for hypothetical futures, no
abstractions where a direct call works, no error paths for
scenarios that can't happen at the call site. Every line pays rent
in legibility or function.

### 1.2 No workarounds without a root-cause fix

Silencing warnings, `try/except: pass`, `--no-verify`, commenting
out broken code, disabling checks — last resort, not default.
Applies to build, test, runtime, and lint failures alike. When a
workaround is genuinely the only option, leave a comment naming
the underlying constraint so it reads as deliberate, not neglect.

**A disabled test is the one case where the comment is not enough — it
needs a tracked item as well.** `testing.md` §7 requires one and owns
the rule. The reason is that this workaround is invisible: every other
one leaves something failing, or noisy, or obviously commented out,
while a skipped test leaves a green run that looks exactly like a
passing one. A comment in a file nobody opens is not a reminder.
Reconciled 2026-08-14 (ROADMAP CFG-0098) — this section accepted a bare
comment for every failure class including tests, and the two standards
disagreed.

### 1.3 Reuse before rewriting

Before writing new code, look for existing code that does the same
or similar thing, in order of preference:

1. Call it directly.
2. Refactor it to cover the new case, then call it — existing
   call-sites benefit.
3. Only if neither fits, write new code and justify the
   duplication in the commit body.

**Rule of Three:** where nothing suitable exists and the choice is
whether to *extract* a helper, extract on the third call-site, not
the first or second. Premature DRY costs more than duplication.

### 1.4 Six-month test

If someone opens this file six months from now, can they read the
change and understand *why* the code looks this way without the
author? If not, it's too clever or too long.

### 1.5 Use latest stable library + current idioms

Prefer the latest stable release of an external library. When
calling into it, use the idiom current for *that* version — not the
one you remember. `dependencies.md` §1 owns when a project may hold
one back.

**Check before writing, rather than recalling.** A stale idiom
still compiles, which is exactly why it survives: nothing fails,
and the codebase ages a little. Where a language has aged visibly,
`languages/<name>.md` lists its retired spellings; the library's own
current documentation covers the rest.

**A bump and the idiom refresh ship together.** Upgrading a
dependency and leaving the calling code in the previous era's
style means the refresh never happens. Where a bump genuinely
needs no caller change, say so in the commit rather than leaving
it unsaid.


### 1.6 State an assumption; do not build on it

When the code could reasonably go two ways and the answer is not on disk,
**say so before writing** — in the conversation, not in a comment inside the
diff.

**The asymmetry is the whole reason.** Someone who reaches an unspecified
case asks. An agent invents one, confidently, and the result is
indistinguishable from a correct one until something checks — so the gap does
not surface as a question, it surfaces as work built on a guess nobody made
deliberately. By then it is in several files rather than one.

The trigger is noticing yourself write *"I'll assume"*, *"presumably"*,
*"this is probably how it works"*. Each is the signal to verify or to ask —
never to continue. `~/.claude/workflow.md` § 2 states the same asymmetry for the design
gates; this is it at the moment of writing a line.

Skip for the obvious: a typo, a one-liner, a case where either reading
produces the same code.

### 1.7 Surgical changes — every changed line traces to the reason you are here

A diff should contain the change that was asked for and nothing else.

- **No drive-by reformatting** while fixing something unrelated — quote
  style, whitespace, import order, added type hints.
- **No rewriting working code into your preferred idiom.** Match the file's
  existing style even where you would write it differently. (Refactoring *to
  satisfy* the request is the request; refactoring *adjacent* to it is not.)
- **Do clean up orphans your own change creates** — a now-unused import,
  variable or helper.
- **Do not delete pre-existing dead code without being asked.** Surface it
  instead: *"`legacy_foo()` is unreferenced — leave or remove?"*

**The cost is not tidiness, it is review.** An unrelated line in a diff has to
be read, understood and cleared by whoever reviews it, and it dilutes the
change that mattered. On a fix pass it is worse: a repair that also reformats
is a repair nobody can verify at a glance, and unverifiable repairs are how a
review loop stops converging.

## 2. Error handling

- **Validate at boundaries, trust inside them.** Which points are
  boundaries, and what validating one means, is `security.md` §1
  and §3 — stated there rather than twice.
- **Catch the failure you can name, not everything.** A handler
  that catches every failure catches the ones you did not predict
  and hides them. Catch the specific one you can actually do
  something about.
- **Surface what you did not expect.** A swallowed error is a
  defect that will be reported later as something else entirely,
  by a user, with no trace of where it began.
- **Handle where you can act; propagate where you cannot.** Code
  that catches an error it has no answer for is deciding on behalf
  of a caller that knew better.
- **Do not write paths that cannot happen.** A guard against a
  state the call site makes impossible reads as though that state
  is possible, and the next person preserves it forever.
- **An error a user sees names what failed and what they can do.**
  A message that only names the internal operation is a message
  they cannot act on.


## 3. Comments

Default to **no comments**. Only add one when the WHY is
non-obvious:

- A hidden constraint (`// gpg is single-threaded; serialise here`).
- A subtle invariant (`// must run before m_grid is freed`).
- A workaround for a specific bug (`// QTBUG-79126: frameless +
  modal drops clicks on Wayland — fall back to event filter`).
- Behaviour that would surprise a reader.

Don't:

- Explain WHAT the code does — well-named identifiers do that.
- Reference the current task / fix / callers ("used by X", "added
  for Y") — those belong in the commit body.
- Write multi-line block comments or paragraph docstrings.

**A documentation comment is not a comment, and this section is not
about it.** A docstring or `///` block that a tool extracts is part of
the published interface: one line saying what a caller gets is right,
because that line is the contract, not an explanation of the body. The
prohibition above is on narrating a body. Anything past that one line is
the paragraph form, which is still the anti-pattern. Whether the
language has such a comment, and how it is written, is
`languages/<name>.md`.


## 4. Naming

A name's job is to tell the next reader what a thing **is**, or what it
**does**, without opening it. That job is identical in every language.

**Casing is not.** `snake_case`, `camelCase`, `PascalCase` and the rest
are settled by each language's own ecosystem — or, where a language has
no single one, by the project, recorded once and held. Code that fights
its ecosystem reads as foreign in its own community. So this section says
*what a name must communicate*; `languages/<name>.md` says *how to spell
it*. The examples here are written as words rather than in a casing,
deliberately.

| Thing | Name it as | Test |
|---|---|---|
| **Function / method** | an action — a verb phrase (*parse colour*, *apply theme*) | Does it read as an instruction? |
| **Class / type** | a thing or a role, never an action (*colour parser*, *retry policy* — not *parse colour*) | Can you put "a" or "the" in front of it? |
| **Variable** | what it holds — a noun phrase (*current tab*, *grid size*) | Would the name still fit if you printed the value? |
| **Boolean** | a claim that is true or false (*is ready*, *has focus*, *can retry*) | Does it answer a yes/no question? |
| **Constant** | its meaning, never its value (*max retries*, not *three*) | Would the name still be right if the value changed? |
| **File** | its main export — a file holding one class takes that class's name | |
| **Directory / module** | the subject it groups | Singular, unless it holds a collection of peers |

Rules that hold in every language:

- **Avoid abbreviations** except universally-known ones (`url`, `id`,
  `db`). Prefer *temperature* over *temp* anywhere *temp* could be read
  as *temporary*.
- **No type prefixes.** `strName`, `iCount` and their kin encode what
  the type system already knows, and start lying the moment the type
  changes. A member-field marker like `m_` is a *scope* marker, not a
  type prefix — fine where a project uses one, provided it uses it
  everywhere.
- **Filler words are a design smell, not a style one.** `Helper`,
  `Util`, `Data`, `Info`, `Object`, and often `Manager`, usually mean
  the thing has no single responsibility yet. Rename the concept, not
  the identifier.
- **Match the file you are in.** A file that has used one convention for
  400 lines keeps it, even where this section would have chosen
  otherwise. Consistency within a file beats correctness across the
  repo; raise the mismatch separately rather than fixing it inside an
  unrelated change. **Except a database schema, which is read as one
  object and not file by file** — `domains/database.md` §3a owns it and
  requires one choice held across the whole schema. Without the
  exception a schema split over several files can end up half singular
  and half plural with both halves compliant, which is exactly the
  confusion that rule exists to stop. Added 2026-08-14 (ROADMAP
  CFG-0098): `database.md` deferred naming here, and this bullet routed
  it straight back to the opposite answer.
- **Name for someone who has never seen the file.** The test is not "is
  this name accurate" — it is "could a stranger guess what this does
  from the name alone".


## 5. Language and framework notes

**They are not in this file.** Each language has its own:

```
languages/cpp.md      languages/python.md      languages/qt.md
```

Read `coding.md` always; read a language file only when the project uses
that language. A Python project has no reason to load C++ rules.

A language file carries the version floor, the casing convention, the
current idioms, the spellings of this file's and `testing.md`'s general
rules, and — where the language has aged
visibly — the retired spellings that still compile. That last part
matters: an idiom that no longer errors is how a codebase quietly
becomes a museum. Write a retirement as *this over that* in the idiom
list; give it a section of its own only where there are enough of them
to be worth listing (`qt.md`).

A framework with its own conventions gets a file too (`qt.md`). It
carries only what differs from its language's file, and wins over that
file for code written in the framework.

**Adding a language:** copy the shape of `cpp.md` or `python.md`. Do not
add a section here.


## 6. Performance

- **Profile before optimising.** Make it work, make it right, make
  it fast — in that order. An optimisation with no measurement
  behind it is a guess that costs legibility either way.
- **Algorithmic shape beats micro-optimisation.** A nested scan
  over the same collection will outgrow every constant-factor
  saving; look there first.
- **No cache without a measured hit rate.** A cache adds a second
  source of truth and a class of staleness bug, and pays for
  neither unless it is actually being hit.
- **Do not pessimise, either.** Where the cheaper form is equally
  clear — avoiding a needless copy, reserving a known size,
  batching I/O instead of looping it — take it. That is not
  premature optimisation; it is not being wasteful for no reason.
  How each is spelled is in `languages/<name>.md`.
- **State the cost where it is not obvious.** An operation that is
  fine for a hundred items and hopeless for a million should say
  so, next to the code, not in someone's memory.


## 7. Security

**Not in this file — see `security.md`.**

Security is not a coding topic. Most of it happens outside a source
file: in the repository, the release, the config, the dependency
manifest, the log stream. A handful of code-level bullets here could
never cover it, and projects that needed more wrote their own.

The code-level rules that used to sit here — validate at the boundary,
argument lists never shell strings, atomic writes, owner-only
permissions on secret-bearing files, resolve paths before opening,
never log a credential — are in `security.md`, stated once and in
context. **`security.md` states them in a way that does not need a
per-language spelling, and no language file carries one** — checked
2026-08-14 (ROADMAP CFG-0108). This paragraph claimed §5's split applied
to them, which told a C++ author to look in `cpp.md` for how a boundary
check is written and find nothing. Where a language genuinely needs the
spelling — an API that is the safe one, a flag that must be set — add it
to that language file and say so here.


## 8. Anti-patterns

Each of these is something that looks like care and is not.

- ❌ **"Just in case" handlers that swallow every failure.** Not
  caution — a way to be told about a bug months later, by a user.
- ❌ **Dead branches and compatibility shims kept for callers that
  no longer exist.** The next reader cannot tell them from live
  code and will maintain them. Delete the one your own change
  orphaned; one you merely found is §1.7's — surface it.
- ❌ **Half-finished work behind a flag.** Two code paths, one of
  which nobody exercises, and no record of which is real.
- ❌ **Renaming an unused variable your own change orphaned instead
  of deleting it.**
- ❌ **A deferral comment with nothing tracking it.** If it matters
  it is a roadmap item; if it does not, delete the line.
- ❌ **Hardcoded paths and unexplained numbers.** A literal with no
  name cannot be searched for, and nobody dares change it.
- ❌ **Explaining what the code does** where a better name would.
- ❌ **Wildcard imports and namespace-wide inclusions**, which make
  it impossible to see where a name came from. Each language's
  spelling of this is in `languages/<name>.md`.

## What checks this

| Rule | What catches a breach |
|------|----------------------|
| §1.1 shortest correct implementation | **nothing** — "shorter would have worked" is a judgement about an alternative that was never written. A code review is the only reader that can raise it |
| §1.2 no workarounds without a root-cause fix | Partial: a linter suppression, a bare `except: pass` and a disabled check are all greppable, and static analysis flags several. **Nothing** catches the commented-out branch or the silently loosened condition |
| §1.3 reuse before rewriting | **nothing** mechanical for the general case. A near-duplicate detector finds copied *text*, which is the weakest form of the rule and the one least worth catching |
| §1.4 six-month test | **nothing** — by construction. It asks whether a stranger will understand this later, which nobody present can answer |
| §1.6 an assumption is stated, not built on | **nothing** — the assumption is invisible once the code exists, which is the failure. Caught only by a reviewer asking "how was this decided?", or by the user recognising a choice they never made |
| §1.7 every changed line traces to the request | The diff itself, read before committing — the cheapest check in this table and the one most often skipped. **Nothing** automates it: a reformat and a fix are the same kind of edit to any tool |
| §1.5 latest stable library, current idioms | Two halves, and only one is checked. The **version** half: a dependency-currency scan reports what is behind. The **idiom** half is **nothing** — a retired spelling compiles, so nothing objects until someone reads it |
| §2 error handling | Partial: static analysis flags swallowed exceptions and ignored return values in the languages that support it. **Nothing** checks that an error a user sees names what they can do |
| §3 comments explain why, not what | **nothing** — and the failure mode is a comment that was true when written and is now describing code that changed underneath it |
| §4 naming | **nothing** — a naming convention is checkable per language and no linter here is configured for it. `languages/` holds the per-language spellings |
| §5 language notes stay in `languages/` | **nothing** — a language-specific section added to this file reads exactly like the rest of it, and only a reader who knows the split will object |
| §6 performance | Partial: static analysis catches some allocation-in-loop and copy-by-value cases. **Nothing** catches a cache nobody measured, which is the rule here most often broken with good intentions |
| §7 security | The security analysers — secret scanning, taint and pattern rules — plus `security.md`'s own table. The strongest-checked section here |
| §8 anti-patterns | Partial and per-item; each anti-pattern is checked by whichever analyser covers it, and several by nothing |

**The pattern is worth naming: what is checked is what a tool can decide
without judgement.** Every rule about *relationships* — is this the shortest form,
does this duplicate something elsewhere, will this read in six months —
is uncheckable, and those are the rules this standard exists for. That
is an argument for spending review attention there and not on the rest.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-08-12 | 3 | 3 | 3 | 1 | 0 | 8 findings, 7 verified / 1 dismissed. 6 fixed here, 2 of them in `README.md` and `languages/`-facing prose; 1 surfaced to the user (§3 versus `languages/python.md` on docstrings). Loop 2 dispatched. |
| 2 | 2026-08-12 | 3 | 1 | 3 | 0 | 0 | 4 findings, all 4 verified and fixed. One was loop 1's own repair (§5's language-file shape); the other three predate the run. Loop 3 dispatched. |
| 3 | 2026-08-12 | 3 | 1 | 3 | 1 | 0 | 7 findings, 5 verified / 2 dismissed. All 5 fixed; 1 was loop 2's own repair (§8's second dead-code bullet, left unscoped when its neighbour was scoped). **Cap reached, not converged** — tail in `~/.claude/docs/reviews/coding-md-review-2026-08-12.md`. |
<!-- MIRROR END -->
