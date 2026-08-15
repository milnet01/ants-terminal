# Roadmap migration — the brief to hand each project's session

**ANTS-3807.** One template, plus a filled instance per project. Copy the block
for the project you are cutting over and paste it into that project's Claude
Code session. You do not need to read anything else in this repo to use it.

Measured 2026-08-15 against the live corpus. Every number below came from
`tools/roadmap-corpus-survey.py` and a per-project scan, not from recall — but
they are a **snapshot**, and each brief tells the session to re-measure before
it writes anything.

---

## Before you send any of these

**1. Ants must be running a build that includes `ANTS-4404`** (2026-08-15,
commit `2fcea0fc`). Before that fix, a roadmap whose *prose* quotes fence
syntax — a line beginning ```` ```` ```lang ```` ```` — opened a code fence
that never closed, and every bullet below it refused `anchor_unsafe_context`
on write. On this project that was 391 of 2,032 bullets. Check with
`session_orient` → `server_build.build_commit`.

**2. One project at a time, and verify each before starting the next.** The
store is shared. A project that imports badly is a project whose reads are now
wrong, and `roadmap_query` will answer from the store while naming the file.

**3. Expect the first render to rewrite the whole file.** It is a one-time
normalisation — wrapping, blank lines, table separators — and it materialises
any store-allocated ids into the text. `ANTS-3758 § 2.6` accepts this as *not*
data loss. **Verify it by id-set difference, not by reading the diff**; the
diff is thousands of lines and the only question that matters is whether an id
disappeared.

---

## The three groups

| | Projects | What to do |
|---|---|---|
| **Ready** | AI_Prompts, Ants_Projects_Hub_Website, Contact_List, DOOM_Ants, Games_Hub, LocalWebServerManager, LottoTracker, MAME_Curator, Music_Production, OneUp, RetroArch, Rolodex, finbreak | Send the brief. 13 projects. |
| **Blocked on dialect** | Vestige (GFM task lists), RetroDB (`#### Pass N.M` headings) | **Do not cut over.** Only the emoji-bullet form renders back today (`ANTS-3758 § 5`). Migrating them makes their reads store-backed with no way to write the file again. |
| **Done** | Ants_Terminal | Migrated 2026-08-15. |

That is where the plan's "remaining 13" comes from: 15 projects hold a
roadmap, less the two whose dialect cannot round-trip.

---

## Per-project facts

`no-id` items get ids **allocated** at import (`ANTS-3765 § 2.6.1`) — a real
change to that project's roadmap, which the session should expect rather than
discover. `gate` is open items with no `Layman:` line; each one refuses every
store-backed write on that project until it is filled.

| Project | Roadmap file | Prefix | Items | No id | Gate |
|---|---|---|---|---|---|
| AI_Prompts | `ROADMAP.md` | `AIPR` | 27 | 0 | 0 |
| Ants_Projects_Hub_Website | `ROADMAP.md` | `APHW` | 9 | 0 | 0 |
| Contact_List | `ROADMAP.md` | `CL` | 62 | 0 | 0 |
| DOOM_Ants | `ROADMAP.md` | `DOOM` | 345 | 0 | 0 |
| Games_Hub | `ROADMAP.md` | `GHUB` | 31 | 0 | 0 |
| LocalWebServerManager | `ROADMAP.md` | `LWSM` | 115 | 0 | **3** |
| LottoTracker | `ROADMAP.md` | `LOTTO` | 31 | **24** | 0 |
| MAME_Curator | `ROADMAP.md` | `mame-curator` | 75 | 0 | **2** |
| Music_Production | `ROADMAP.md` | *none* | 366 | **366** | **4** |
| OneUp | `ROADMAP.md` | `ONEUP` | 107 | 0 | 0 |
| RetroArch | `docs/private/ROADMAP.md` | *none* | 162 † | **all 162** | **20** |
| Rolodex | `ROADMAP.md` | `ROLO` | 36 | 0 | 0 |
| finbreak | `ROADMAP.md` | `FIBR` | 268 | 0 | **16** |

† **RetroArch's row was measured directly, because the survey tool cannot see
that project.** `tools/roadmap-corpus-survey.py` probes for `ROADMAP.md` at a
project's root; RetroArch keeps its roadmap at `docs/private/ROADMAP.md`, so
the tool reports "no ROADMAP.md found" and the project is absent from every
corpus figure it produces — including the ones `roadmap-data-model.md` quotes.
Filed as [ANTS-4409]. The 162 items and 0 ids here come from a direct scan.

**Four need attention before or during cutover:**

- **Music_Production** and **RetroArch** carry no ids at all. Every item gets
  one allocated. Decide the prefix deliberately — pass `id_prefix` to
  `roadmap_migrate`, or it is derived from the directory name and you live
  with it.
- **RetroArch** additionally has a non-standard path and **every** open item
  missing `Layman:`. It is the most work; do it last.
- **finbreak** has the largest real gate debt at 16.

---

## The brief — template

Replace `<PROJECT>`, `<ROADMAP PATH>` and the facts line from the table above.

```text
Please migrate this project's roadmap into the Ants roadmap store.

Context: Ants Terminal now keeps roadmaps in a SQLite store rather than
parsing ROADMAP.md on every read. Ants_Terminal itself cut over on
2026-08-15. This project is next. After cutover, `roadmap_query` answers
from the store and `roadmap_log` REGENERATES the roadmap file from it —
so hand edits to the file stop being visible and get overwritten.

Facts I measured for this project on 2026-08-15 (re-measure, do not trust):
  roadmap file : <ROADMAP PATH>
  dialect      : ants-v1 (emoji bullets)
  id prefix    : <PREFIX or "none — ids will be allocated">
  items        : <N>
  no id        : <N>   <- these get ids ALLOCATED, changing the file
  render gate  : <N> open items with no `Layman:` line

Steps:

1. Commit or stash everything. The tree must be clean — a later step
   rewrites the roadmap file and `git checkout` is the undo.

2. Dry run, and read it rather than skimming:
      roadmap_migrate {caller_cwd: "<abs path>", dry_run: true}
   Check: `items_orphaned` is 0. `sources[]` lists every roadmap file you
   expect INCLUDING archives. `ids_allocated` matches the "no id" count
   above — if it is unexpectedly non-zero, stop and ask, because those ids
   become permanent.

3. Clear the render gate if step 2 or a write reports `render_gate_unmet`.
   The refusal names the failing ids in `gate_failures[]`. Fill ONLY those,
   with a one-sentence plain-English `Layman:` line each, via
   `roadmap_log op:"annotate"`. Do not bulk-fill the whole roadmap.

4. Migrate for real:
      roadmap_migrate {caller_cwd: "<abs path>"}
   Record `items_inserted`, `items_orphaned`, `ids_allocated`.

5. Prove it is idempotent — this is the check that says it worked, and it
   is `ANTS-3765` INV-2:
      roadmap_migrate {caller_cwd: "<abs path>", dry_run: true}
   Must now report 0 inserted and 0 orphaned. Anything else means the
   import is not stable; stop and report it.

6. Prove the render is lossless BEFORE you trust it. Note the id set,
   make any small `roadmap_log` write, then compare:
      git show HEAD:<ROADMAP PATH> | grep -oE '\[[A-Za-z0-9_-]+-[0-9]+\]' | sort -u > /tmp/before.ids
      grep -oE '\[[A-Za-z0-9_-]+-[0-9]+\]' <ROADMAP PATH> | sort -u > /tmp/after.ids
      comm -23 /tmp/before.ids /tmp/after.ids     # MUST be empty
   Ids GAINED are fine (allocated ids being written into the text). Any id
   LOST means stop and `git checkout` immediately.

7. Report back: the step-4 counts, the step-5 confirmation, and whether
   step 6 was empty.

Two things to expect so they do not alarm you:
  - The first render produces a very large diff. It is a one-time
    normalisation and is documented as not being data loss.
  - Items that had no id now have one, written into the file.

If anything refuses with a code you do not recognise, the taxonomy is in
Ants_Terminal's `docs/standards/mcp-error-codes.md` — report the code
rather than working around it.
```

---

## What this brief deliberately does not cover

- **The round-trip drift check** (`ANTS-4065` Phase D3). It is a diagnostic for
  the import contract, not a per-project cutover gate, and its procedure is
  destructive — it drives a render, then restores with `git checkout`. Step 5's
  idempotence check is what a cutover actually needs.
- **Rotation** of closed sections into `docs/roadmap/`. Independent of cutover.
- **The two blocked dialects.** They need `ANTS-3758 § 5` to grow a renderer
  for their form, or a conversion; neither is a brief.
