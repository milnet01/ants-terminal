# Feature: `similar_code` MCP shape matcher (ANTS-1305)

Human contract for the regression test in this directory. Full design:
`docs/specs/ANTS-1305.md`.

`similar_code` walks a project's C++/Python source tree, extracts
class/function signatures by reusing the `FileOutline` extractor, and
ranks them by token-set Jaccard similarity to a free-text `shape`
query. It surfaces the project's existing examples of a shape before
Claude writes a new one.

## Invariants under test

**Tokeniser (`SimilarCode::tokenize`)**
- Lowercases, splits on runs of non-`[A-Za-z0-9]`, drops tokens < 2
  chars. `"class FooDialog : public QDialog"` →
  `[class, foodialog, public, qdialog]`; `"cmd_bar"` → `[cmd, bar]`;
  `"a + b"` → `[]`.

**Similarity (`SimilarCode::jaccard`)**
- `|A∩B| / |A∪B|`; 0 when the union is empty.

**Ranking (`SimilarCode::findSimilar`)**
- Returns matches ordered by descending `score`, tie-broken by
  `(file, line)` ascending.
- A `QDialog`-subclass query ranks `class … : public QDialog` lines
  above an unrelated `class` (distinctive token `qdialog` dominates).
- `kind` / `lang` come from `FileOutline` (`class` / `func`, `cpp` /
  `py`); paths are project-relative.
- Candidates with `score == 0` are excluded.
- `build` / `build-*` / `node_modules` / dot-dirs are skipped; symlinks
  not followed.
- `lang=py` scans only Python; `lang=cpp` only C++.
- `maxResults` caps `matches[]`; `matches_count` is the pre-cap total;
  `truncated` flips when the cap drops entries.
- A caller `max_results` above the hard cap (20) is clamped to 20.
- `bad_args` on empty / over-512-char / token-empty `shape`.
- A shape with no token-overlapping signature → `ok:true`, empty
  `matches`, `matches_count:0`.

**Wiring contract (source-grep)**
- `cmdSimilarCode` declared in `remotecontrol.h`, defined in
  `remotecontrol.cpp`, IPC verb `similar-code` dispatched.
- `similar_code` registered in `mainwindow.cpp`.
- `claudeintegration.cpp` carries the descriptor, the `{600,2500}`
  token-cost entry, the `pattern` `kindForName` bucket, and the
  `Required` caller-cwd contract.
