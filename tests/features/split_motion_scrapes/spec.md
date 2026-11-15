# split-motion-check --scrapes: windows a commit did not cut

`tools/split-motion-check.py --scrapes` lists every scrape window whose text
differs between a split commit and its parent. Two window shapes were listed
although the commit cut nothing the test reads (ANTS-6423, ANTS-6424).

- **INV-1 — an untaken ternary arm is not cut.** A window inside one arm of
  `cond ? a : b` whose condition picks the other arm at both revisions is not
  judged.
- **INV-2 — text appended after an open-ended window's subject is not a
  cut.** For `substr(pos)` or `mid(pos)` with no length, the window is
  unchanged when the commit's window begins with the parent's.
- **INV-3 — a real change still fails.** A commit that changes the text a
  window reads exits 1.

Test: `test_split_motion_scrapes.py` builds a throwaway git repository per
case, commits a parent and a child, and runs the tool on the pair.
