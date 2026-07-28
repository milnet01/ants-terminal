# doc_dedup — verb-layer conformance

Contract for `tests/features/doc_dedup_verb/test_doc_dedup_verb.cpp`. Owning
spec: [`docs/specs/ANTS-3660.md`](../../../docs/specs/ANTS-3660.md).

`RemoteControl::cmdDocDedup` needs a live `MainWindow`, so behavioural rows drive
the pure helper (`RemoteControl::docDedupBuildResponse`) and wiring rows
source-scrape the registration sites — the pattern `doc_integrity`'s and
`doc_symbols`' verb lanes already use.

## What each row locks

| Row | Invariant | Claim |
|---|---|---|
| `Inv5ReportOnly` | INV-5 | No finding is `autoFixable`, `auto_fixable` never reaches the wire, and the engine has no write path at all. |
| `Inv8RefusalMinimums` | INV-8 | `caller_cwd` Required at both declaration sites; a supplied `path` is validated **before** the walk; nothing to scan is `ok:true`, not a refusal. |
| `PairAndClusterShapeReachTheWire` | § 2.5 | `pairs[]` carries both ends and `clusters[]` collapses them — the two arrays `DocFinding::Finding` cannot express and ANTS-3663 hoists whole. |

## Why two of these are scrapes

- **"The engine writes nothing"** is a claim about code that did *not* run. No
  behavioural row can hold it, because the engine passing without writing and
  the engine having no write path look identical from outside. Hence
  `SRC_DOCDEDUP_CPP_PATH` and a ban on `WriteOnly` / `QSaveFile` / `QTextStream`
  / `ReadWrite` / `Append`.
- **`caller_cwd` Required** is asserted twice because it is declared twice —
  at `registerToolProvider` in `mainwindow.cpp` and in
  `callerCwdContractFor` in `claudeintegration.cpp` — and the two can disagree.

`Inv8`'s ordering assertion (`validatePath` appears before
`docIntegrityEnumerate` in the handler body) is the one that would otherwise be
missed: a handler that validates *after* enumerating still refuses a
root-escaping path, but has already walked the tree it was supposed to refuse.

## Verified RED before the implementation landed

The engine lane's mutation run (10 mutations, table in
[`../doc_dedup/spec.md`](../doc_dedup/spec.md)) reached this lane once, and that
is the honest summary rather than a claim of independent proof:

| # | Mutation | Result here |
|---|---|---|
| M7 | Cluster only directly-scored pairs (drop the transitive union) | RED — `PairAndClusterShapeReachTheWire`: three docs sharing one stanza return 3 clusters where 1 is expected. |

**The other two rows are not mutation-provable, by construction, and saying so
is more useful than inventing a mutation that appears to prove them.**

- `Inv5ReportOnly`'s scrape half asserts the *absence* of a write path. Mutating
  the engine to add one would turn it red, but that mutation is circular — it
  tests that the scrape greps, not that the engine is report-only. Its
  behavioural half **is** load-bearing and does have a live guard: the
  `ASSERT_FALSE(r.findings.isEmpty())` on the fixture, without which the
  `autoFixable` loop would iterate zero times and pass against an engine
  producing nothing.
- `Inv8RefusalMinimums` is entirely a scrape of wiring that lives in three other
  files. Deleting any of the three registration sites turns it red — which is
  what it is for — but that is a wiring check, not a behavioural one, and it
  cannot be proved by mutating `docdedup.cpp`.

Its ordering assertion was worth writing for a concrete reason: `cmdDocDedup`'s
first draft is the only place in this change where validate-after-enumerate
would still have refused correctly while having already walked the tree.

## Not covered here

- **Driving `cmdDocDedup` end-to-end** — it resolves the project root through a
  live `MainWindow`, which this bundle has none of. The engine lane covers the
  behaviour; these rows cover the wiring.
- **The engine's segmentation, thresholds and clustering** —
  `tests/features/doc_dedup/`.
