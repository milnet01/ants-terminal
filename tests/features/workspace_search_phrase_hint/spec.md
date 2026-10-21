# workspace_search zero-match advisories

`workspace_search` matches its `query` as one literal or regex pattern. A
zero-match reply used to say nothing about why, so a caller could not tell
an absent string from a pattern that was written wrong or a path that was
never read. Each invariant below adds an advisory to the `ok:true` envelope.
None changes what the search matches.

The directory keeps its first item's name, `phrase_hint` (ANTS-2045). The
name is cited elsewhere, so the contract grew here instead (ANTS-4755).

## Invariants

- **INV-1 (ANTS-2045)** — A zero-match query containing whitespace gets a
  `hint` saying it was matched as one phrase, worded for literal or regex
  mode. A single-token query, or one with matches, gets no such hint.
  *Test:* `WorkspaceSearchPhraseHint.Inv1ZeroMatchPhraseHint`.
- **INV-2 (ANTS-2181)** — A regex whose top-level alternation holds a bare
  term of one to three letters gets a `regex_advisory` suggesting `\b`
  anchors. Anchored or metacharacter-bearing alternatives are exempt.
  *Test:* `WorkspaceSearchPhraseHint.Ants2181RegexShortBareTermAdvisory`.
- **INV-3 (ANTS-5139)** — A pattern that is one group bounded by `\b` on both
  sides, capturing or not, draws no `regex_advisory`. Any other pattern is
  judged per alternative, so `tan|cosine` and `(tan)|\b(TBD)\b` still name
  `tan`. *Test:* `WorkspaceSearchPhraseHint.Ants5139BoundedGroupIsAnchored`.
- **INV-4 (ANTS-3466)** — A zero-match `regex:false` pattern with no
  whitespace that carries regex metacharacters, alternation `|` among them,
  gets a `hint` asking "did you mean regex:true?".
  *Test:* `WorkspaceSearchPhraseHint.Ants3466MetacharRegexFalseHint`.
- **INV-5 (ANTS-4420)** — A zero-match pattern containing a terminated HTML
  entity (`&lt;`, `&#60;` and the like; a bare `&` does not count) gets a
  `hint` pointing at the literal characters.
  *Test:* `WorkspaceSearchPhraseHint.Ants4420HtmlEntityHint`.
- **INV-6 (ANTS-4753)** — With `include_hidden` off, a zero-match search whose
  `glob` names a dot-path sets `hidden_paths_skipped:true` and a `hint` naming
  `include_hidden:true`. This holds on every zero-result path:
  `count_only`, `files_only` and `matches_only` too. A `lane` under a hidden
  directory is searched anyway, so it never triggers this.
  *Test:* `WorkspaceSearchPhraseHint.Ants4753HiddenGlobHint`.
- **INV-7** — `hint` is set at most once. The zero-match branches run in
  order — hidden path (INV-6), HTML entity (INV-5), phrase (INV-1), regex
  metacharacters (INV-4) — and the first that applies wins. `regex_advisory`
  is a separate key and can appear beside any of them. *Tests:* the ordering
  assertions in `WorkspaceSearchPhraseHint.Ants4420HtmlEntityHint` and
  `WorkspaceSearchPhraseHint.Ants4753HiddenGlobHint`.

## How it is tested

The envelope builder needs a live `RemoteControl`, so INV-1, INV-2 and INV-4
to INV-7 read the source as text and pin the wiring. INV-3 calls
`rcShortBareAltTerms` directly.

## Out of scope

An OR-fallback that ranks results by significant-term overlap, kept for a
follow-up if the advisories prove insufficient.
