# Feature: the shared caller_cwd description matches what the dispatcher does

## Problem

Every tool that anchors on `caller_cwd` advertises the same property
description, built by one helper in `tools/list`. It said:

> The terminal-state verbs (`get_*`) still accept it as an Optional
> tab-routing anchor and fall back to the focused Ants tab when absent.

There is no such fallback. ANTS-1415 Phase 3b removed it: a
`CallerCwdContract::TabSpecific` tool called with no `caller_cwd` and no
usable `tab` index is refused with `tab_or_cwd_required`, before the
rate limit and the cache, and the refusal text says in full why —
falling back to the focused tab is the cross-tenant leak ANTS-1404
closed for the Required tools.

So the schema promises a behaviour the dispatcher refuses. A session
that believes it omits `caller_cwd`, and gets an error it was told could
not happen. A wrong description costs more than a missing one, because
it is acted on.

Two smaller errors sit in the same place. The `get_*` shorthand does not
cover the classification: `recent_errors` and `last_selection` are
`TabSpecific` and neither begins with `get_`. And the code comment above
the helper enumerates five of the seven classified verbs.

## Contract

The shared `caller_cwd` description MUST state the refusal, not a
fallback, and MUST NOT describe the per-tab verbs by a `get_*` prefix
that does not match the classification.

It stays short. The property is attached to nearly every tool in the
catalogue, so its length is paid once per tool on the wire; the seven
verb names belong in the source comment, which is not shipped.

The comment above the helper MUST name every verb the classifier
returns `TabSpecific` for, so it cannot silently under-report again.

## Invariants

**INV-1 — the description does not promise a focused-tab fallback.**
Source-grep the helper's description string: no "fall back to the
focused" phrasing.

**INV-2 — the description names the refusal code.** It contains
`tab_or_cwd_required`, which is what a caller actually meets.

**INV-3 — the description does not describe the per-tab set as
`get_*`.** Two of the seven do not carry that prefix.

**INV-4 — every `TabSpecific` verb is named in the helper's comment.**
Extracted from the classifier, checked against the comment block. This
is the invariant that stops the enumeration drifting from the code
again; the others pin one wording each.

**INV-5 — the classifier still has `TabSpecific` verbs.** Guards INV-4
against passing vacuously if the extraction finds nothing.

## Scope

### In scope
- Source-grep over `src/claudeintegration.cpp`.

### Out of scope
- Whether the fallback should come back. ANTS-1415 decided that and
  its spec owns it; this is about the description matching it.
- The three verbs that accept a `tab` index. Named in the description
  because a caller needs to know an alternative exists, but which three
  is `tabSpecificAcceptsTabIndex`'s contract, not this one's.
- The per-tool `description` fields. This is the shared property
  description only.

## Regression history

- **ANTS-1520:** introduced the shared property so consumers would see
  one canonical statement instead of many variants.
- **ANTS-1415 Phase 3b:** removed the focused-tab fallback and added
  the `tab_or_cwd_required` refusal. The shared description was not
  updated, so the canonical statement became the canonically wrong one.
- **ANTS-4457 (cold sweep 2026-08-18, verified 2026-09-08):** reported
  as "shared `caller_cwd` schema promises a focused-tab fallback
  ANTS-1415 removed (six verbs)". Verified against source; the count is
  seven, not six. Fixed. Locked by this spec.
