# Feature spec: `roadmap_query` narrator-bullet filter v2 (ANTS-1425)

ANTS-1398 v1 dropped section-rollup bullets (empty id AND empty
headline). A second class of ID-less bullets still leaked through:
**narrator** bullets — non-empty headline but empty id, e.g.
"Trust-model gaps in IPC sockets." These are section-summary prose
not tracked by the project's stable `[PROJ-NNNN]` ID system.

`docs/standards/roadmap-format.md` § 3.5.1 makes the stable ID
mandatory for every actionable bullet, so `id.isEmpty()` alone is a
sufficient non-actionable marker. v2 widens the drop predicate to
cover narrators, behind a new `include_narrator_bullets` opt-in that
mirrors the v1 `include_section_headers` design.

## Invariants

- **INV-1 / `include_narrator_bullets` opt-in.** Default false.
  When false (or absent), narrator bullets (empty id, non-empty
  headline) are dropped from `bullets[]` server-side. When true, the
  legacy shape is preserved for back-compat callers. Anchor:
  `ANTS-1425` in `src/remotecontrol.cpp`.
- **INV-2 / narrator predicate is the complement of rollup.** Both
  share `id.isEmpty()`; rollups have `headline.isEmpty()` while
  narrators have non-empty headline. The two predicates are
  disjoint by construction. Anchor: `isNarratorBullet` lambda in
  `src/remotecontrol.cpp`.
- **INV-3 / single drop helper composes both opt-ins.** A
  `shouldDropUnnumbered` lambda combines the two predicates with
  their respective opt-in flags so the two filter loops (section-
  mode emission + full-file emission) call one helper instead of
  re-deriving the logic. Anchor: `shouldDropUnnumbered` in
  `src/remotecontrol.cpp`.
- **INV-4 / schema advertises `include_narrator_bullets`.** The
  `roadmap_query` `tools/list` descriptor in
  `src/claudeintegration.cpp` declares the new boolean property
  with a `default: false` and a description referencing ANTS-1425.
- **INV-5 / dispatch forwards the flag.** `mainwindow.cpp`'s
  `roadmap_query` provider forwards `include_narrator_bullets` to the
  handler. ANTS-3422 replaced the per-arg forward (whose omissions were
  the silent-drop bug class the ANTS-1437 forward-fix belonged to) with a
  verbatim `rcDelegate(&RemoteControl::cmdRoadmapQuery)` forward that
  passes the whole args object through, so the flag reaches the handler by
  construction. Source anchor: `rcDelegate(&RemoteControl::cmdRoadmapQuery)`
  + `ANTS-3422`.
- **INV-6 / echo-only-when-set discipline.** Envelope only carries
  `include_narrator_bullets` when the caller explicitly passed
  it — mirrors `include_section_headers`. Keeps the default
  envelope shape trim.
- **INV-7 / orthogonal with `include_section_headers`.** Passing
  `include_section_headers:true` alone keeps rollups but still
  drops narrators; passing `include_narrator_bullets:true` alone
  keeps narrators but still drops rollups; passing both keeps
  everything.

## Test scope

Source-scrape against `src/remotecontrol.cpp`,
`src/claudeintegration.cpp`, and `src/mainwindow.cpp` for the
anchor strings + key code patterns. INV-7 orthogonality is implicit
in the `shouldDropUnnumbered` shape (two-flag conjunction); a
runtime test would require a RemoteControl + Roadmap fixture which
is out of scope here.
