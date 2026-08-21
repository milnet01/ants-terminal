# roadmap_query_no_roadmap_refusal — the no_roadmap_loaded refusal says where it looked

**Item:** ANTS-4611 · **Bundle:** `test_claude` ·
**Suite:** `roadmap_query_no_roadmap_refusal` · **Label:** `features`

## Why this exists

`roadmap_query` with an explicit `caller_cwd`, against a project with no
`ROADMAP.md`, returned:

```
{ok:false, code:"no_roadmap_loaded", error:"no ROADMAP.md detected for the active tab"}
```

The **verdict is right**; the wording misdirects. The caller named a root and
the verb answered about a browser tab, so nothing in the envelope says where
it actually looked. Two wrong conclusions are available and both are
reasonable: read it as tab misrouting and retry / switch tab / abandon, or
read `ok:false` as failure and stop — when for some callers the refusal *is*
the answer. The `adopt-project` skill asks exactly this question and calls a
missing roadmap "an answer and not an error", which the envelope contradicted.

Reproduced byte-identically on two projects. 9 of the 24 projects on this
machine keep no `ROADMAP.md`, so this is the common case.

## Invariants

- **INV-1 — an explicit `caller_cwd` is never blamed on the tab.** The tab is
  named only where `m_main->roadmapPathForRemote()` was what supplied the path.
- **INV-2 — the refusal echoes `resolved_root`,** and the message names it.
  A caller can then distinguish "this project keeps no roadmap" from "the root
  resolved somewhere I did not mean".
- **INV-3 — the tab wording survives where the tab really was the source.**
  The fix narrows the claim; it does not delete a true one.
- **INV-4 — the verdict is unchanged:** still `ok:false`, still
  `no_roadmap_loaded`. Only what the caller can conclude from it moves.

**INV-3 carries no case.** The tab branch needs a live `MainWindow`, and this
bundle constructs `RemoteControl(nullptr)`; a case that drove it would be
asserting against a stub. It is held by the branch structure instead — the tab
wording sits under the `callerRaw.isEmpty()` arm and nothing else can reach it.
Saying so is cheaper than a case that passes for the wrong reason.
