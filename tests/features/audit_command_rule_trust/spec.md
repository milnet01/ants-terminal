# audit_command_rule_trust — project-scoped trust for command-bearing rules

## Why

`<project>/audit_rules.json` may carry rules with a `command` field. Those
strings are bash-exec'd verbatim when the Audit dialog runs. A cloned-but-
untrusted repo with a hostile rule pack is therefore a local-RCE chain the
moment the user opens the Audit dialog. 0.7.12 gated this on a single
global bool (`audit_trust_command_rules`), which meant once flipped for
Ants's own repo, every cloned repo inherited trust — an over-granted
permission. 0.7.13 scopes trust per **(canonical projectPath, sha256 of
rule-pack bytes)** so trust is narrow and self-invalidating.

## Contract

`Config` exposes three methods:

- `isAuditRulePackTrusted(projectPath, rulesBytes) const → bool`
- `trustAuditRulePack(projectPath, rulesBytes)` — records the hash
- `untrustAuditRulePack(projectPath)` — forgets the entry

### Invariants

1. **Default-untrusted.** A freshly-constructed `Config` with no prior
   calls returns `false` for any `(projectPath, rulesBytes)` pair.
2. **Trust record round-trips.** After `trustAuditRulePack(P, B)`,
   `isAuditRulePackTrusted(P, B)` returns `true`.
3. **Hash-bound.** If the rule-pack bytes change
   (`B → B'`, B' ≠ B), `isAuditRulePackTrusted(P, B')` returns `false`
   even though `P` was previously trusted under `B`. This enforces
   re-prompt on any rule-pack edit.
4. **Project-scoped.** Trust for project `P1` does **not** extend to
   project `P2`. Trusting `(P1, B)` leaves `isAuditRulePackTrusted(P2, B)`
   at `false` — a cloned-untrusted repo can't inherit trust from a
   trusted sibling project even when their rule packs are byte-identical.
5. **Canonicalization.** `projectPath` is canonicalized via
   `QFileInfo::canonicalFilePath()`. `/path/to/proj`, `/path/to/proj/`,
   and a symlink pointing at it all resolve to the same trust record.
6. **Untrust is idempotent.** `untrustAuditRulePack(P)` against an
   already-untrusted project is a no-op; subsequent checks return
   `false`.
7. **Persistence.** Trust records survive across `Config` instances
   (i.e. across process restarts) because they round-trip through
   `~/.config/ants-terminal/config.json`.

## What the test proves

The test exercises each invariant inside a sandboxed `XDG_CONFIG_HOME`
and a temporary project tree. Failures signal a regression in the trust
gate's semantics. A broken gate is a silent RCE re-introduction, so the
test is mandatory on the fast lane.

## What the test does NOT cover

- The loader branch in `AuditDialog::loadUserRules` that calls
  `isAuditRulePackTrusted` — it's a one-liner `if (!trusted) skip`, and
  exercising it requires a `QApplication` round-trip that doesn't
  provide meaningful signal beyond this Config-level test.
- The env-var escape hatch `ANTS_AUDIT_TRUST_UNSAFE=1` — orthogonal to
  the persisted trust store.
- The UI badge / tooltip — visual.
