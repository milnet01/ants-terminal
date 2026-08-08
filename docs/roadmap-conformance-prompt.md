# Prompt: bring this project's ROADMAP.md up to standard

Paste the block below into a Claude Code session **in the project being fixed**.
It is deliberately short: the checks are mechanical and already written, so the
prompt points at the tool rather than describing the defects and inviting each
session to re-derive them.

**Why this is being asked.** Every project's roadmap is moving from markdown
into a SQLite store. Once a project is migrated the store becomes the source and
the markdown becomes generated output — so a defect that survives into the
import becomes permanent, and the file can no longer correct it. This pass is
the last chance to fix the text while the text is still authoritative.

---

## The prompt

```text
This project's ROADMAP.md needs to conform to the shared roadmap format before
it can be imported into the roadmap store. Please bring it up to standard.

1. Run the checker (it reports, it never edits):

     python3 /mnt/Games/Scripts/Linux/Ants_Terminal/tools/roadmap-conformance.py .
     python3 /mnt/Games/Scripts/Linux/Ants_Terminal/tools/roadmap-conformance.py . --verbose

2. The contract is
   /mnt/Games/Scripts/Linux/Ants_Terminal/docs/standards/roadmap-format.md
   — section 3.5 for bullet structure, 3.5.3 for the 21 valid Kind values.
   Read the relevant section before fixing a class of finding; do not guess the
   rule from the checker's one-line summary.

3. Fix in the order the checker prints. The first class blocks all the others.

4. Re-run the checker until only "DO NOT EDIT" findings remain, then commit.

Four things that will cost you a rewrite if you skip them:

- DO NOT EDIT the "headlines the CURRENT parser truncates" findings. Those
  headlines are CORRECT. They quote a bold marker inside backticks — one of
  them quotes the C signature `char **argv`, where the asterisks are pointer
  syntax. The reader has the bug, not the text, and it is being fixed centrally
  as ANTS-4066. Editing them corrupts correct content to work around a defect
  that is about to disappear.

- DO NOT blindly append a period to headlines missing one. Three different
  defects wear that label: the period may already be there but sitting OUTSIDE
  the `**`, or the bullet may use a `**Headline**: body` label form where a
  period is simply wrong. Appending one produces `**Headline.**. Body` or
  `**Headline.**: body`. Look at each before changing it.

- DO NOT lose text when shortening an over-long headline. Split it at a clause
  boundary and move the remainder into the body. Do not summarise it away.

- DO NOT invent a Kind value or a Priority. If a bullet declares no Kind, leave
  it — the standard says an absent Kind means `implement` and the import notes
  the default. If it declares no Priority, leave it empty: priority is encoded
  by POSITION in the file (roadmap-format.md 3.5.2), and writing a number
  nobody chose is the exact failure this migration exists to stop.

When you fix a whole class mechanically, verify the result rather than trusting
the rule — print the first few rewritten lines and read them. Both mechanical
passes on the largest roadmap in the corpus produced malformed output on the
first attempt, and both were caught this way.

If the checker says SKIPPED, this project's roadmap is in a dialect the store
does not govern yet. Nothing to do — reply saying so.
```

---

## What to expect

Measured 2026-08-08 across the corpus, after Ants Terminal's own pass:

| Project | Findings | Dominated by |
|---|---|---|
| AI_Prompts, Contact_List, DOOM_Ants, OneUp, Rolodex | 0 | already clean |
| Ants_Projects_Hub_Website | 5 | wrapped headlines |
| 3D_Engine | 5 | missing `Layman:` |
| Music_Production | 5 | missing `Layman:` (4) + 1 DO-NOT-EDIT |
| finbreak | 27 | wrapped headlines (18) |
| MAME_Curator | 31 | wrapped headlines (30) |
| LocalWebServerManager | 36 | wrapped headlines (36) |
| LottoTracker | 11 | missing periods |
| RetroDB | — | skipped, pass-headings dialect |

89 of the 119 findings are wrapped headlines, which is a join with no wording
changes. This is a much smaller job than it looks.

**The `Layman:` count is the one that matters.** A public open item with no
`Layman:` line refuses *every* write on a migrated project — not just
publication — so those 17 are what actually gate each rollout. Ants Terminal
hit this first and had to write 101 of them before any store write succeeded.

## Why a checker and not a written checklist

The defects are deterministic, so a tool finds them exactly and a human
description finds them approximately. More to the point, this session made two
mechanical passes over the largest roadmap in the corpus and **both produced
malformed output on the first attempt** — periods appended onto headlines that
already had one, and a split that cut inside `e.g.`. Both were caught by reading
the output rather than trusting the rule. A prompt that merely described the
rules would reproduce those failures once per project; the tool encodes what was
learned from getting them wrong.
