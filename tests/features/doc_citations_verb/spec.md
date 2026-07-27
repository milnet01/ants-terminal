# doc_citations_verb — the verb layer and its registration (ANTS-3636)

Contract: [`docs/specs/ANTS-3636.md`](../../../docs/specs/ANTS-3636.md) § 2.1
(refusals, request surface) and § 2.7 (the six registration hooks).

The engine's own invariants are `tests/features/doc_citations/`, in the
`test_core` bundle. Two directories because the two layers live in two bundles:
this one needs `test_claude`, where `claudeintegration.cpp` is linked.

`RemoteControl::cmdDocCitations` needs a live `MainWindow`, which a unit test
does not have. So the behaviour is driven through two **pure statics** —
`docCitationsValidate` (refusals) and `docCitationsClampOptions` (request args →
`Options`) — and the wiring is source-scraped. That is the `rc_get_text_byte_cap`
pattern `mcp-tools.md` § Tests prefers, and the split ANTS-3601 reached for the
same reason.

## Contract

- **INV-18** — `docCitationsValidate` produces exactly five refusals: empty
  `rootCanonical` → `bad_path`; a root-escaping `path` → `bad_path`; a directory
  → `bad_args`; absent or empty `path` → `missing_field`; an existing `path` that
  `QFileInfo` alone rules out → `read_failed`. It returns an empty envelope for
  input it accepts, and never decodes the doc — undecodable-as-UTF-8 is the
  engine's refusal (`doc_citations`' INV-47), because validating it here would
  decode every doc twice. `caller_cwd` is deliberately not in this set: the
  dispatcher refuses it first (INV-48).
  *Test:* `DocCitationsVerb.Inv18ValidateRefusalSet`.
- **INV-21** — `docCitationsClampOptions` clamps and **coerces**: an unrecognised
  `only` → `all`, a wrong-typed numeric → its **default** rather than a clamp
  endpoint, `offset` → `max(0, offset)` with no upper clamp (that bound is
  `count`, which the handler has not computed). The engine trusts what it is
  given, so a direct engine call is never re-clamped.
  *Test:* `DocCitationsVerb.Inv21ClampAndCoerce`.
- **INV-19** — the verb is registered across both files: seven sites plus two
  negative assertions. *Test:* `DocCitationsVerb.Inv19RegisteredAtSevenSites`.
- **INV-22** — the authored descriptor carries `type:"object"`,
  `additionalProperties:false`, an explicit `required[]`, and all nine
  properties; no descriptor comment contains the literal `props["`, and the
  description does not begin with `[`.
  *Test:* `DocCitationsVerb.Inv22DescriptorShape`.
- **INV-48** — the `Required` caller-cwd contract, which is what makes the
  dispatcher refuse `caller_cwd_required` before the handler runs.
  *Test:* `DocCitationsVerb.Inv48CallerCwdRequired`.

## Why INV-22 scrapes the source rather than calling `tools/list`

The emitted description **always** begins with `[`, because the description loop
prepends a `[<kind>] ` tag to any that does not. Asserted against the response,
the "does not begin with `[`" prohibition would be inverted and would fail a
correct implementation. The thing under test is the *authored* string, so the
test reads the source — the same choice `mcp_tools_list_schema` makes.

## Why INV-48 is a contract assertion, not a live call

The dispatcher answers `caller_cwd_required` before `docCitationsValidate` runs,
so the branch exists nowhere in the handler to unit-test. What the test can pin
is the pair that produces the refusal: the `Required` contract in
`callerCwdContractFor` and the same contract at the `registerToolProvider` call,
whose drift is refused in every build configuration (ANTS-1834). This is how
ANTS-3601 INV-10 — the model the spec names — asserts it for `doc_integrity`.

## Test

`tests/features/doc_citations_verb/test_doc_citations_verb.cpp`, compiled into
the `test_claude` bundle per `tests/features/README.md` — not a standalone
`add_executable`. Verified RED against feature-absent code before the
implementation landed.
