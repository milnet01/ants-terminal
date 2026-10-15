# SIGN-1 token bench: pygments

The second SIGN-1 bench (ANTS-5697). The first bench's job is too small to
need code search, so no run used an Ants tool (ANTS-1120). This one plants a
bug in Pygments, a real library far too large to read whole.

**Neither the prompt nor the model changes after the first measurement.**

## What is here

- `setup.sh` — builds a run's copy: Pygments 2.21.0 at commit
  `a43b45dcf081b6010c6ab4428f149f7f6d2499c4`, without `tests/contrast` (it
  needs a package that is not installed), with the bug planted and the bug
  test added. The copy carries no upstream history, so the plant cannot be
  read back out of git.
- `plant.patch` — the planted bug: short hex colours such as `#0a0` swap
  their second and third digits.
- `test_style_colours.py` — the bug test. All four of its tests pass on
  Pygments as released and fail with the bug planted. One upstream test,
  `tests/test_groff_formatter.py::test_basic`, also fails until the fix.

## The fixed job

**Model:** `claude-opus-5-5`, effort `high`, in both arms.

**Bug test:** `python3 -m pytest -q -p no:cacheprovider tests/test_style_colours.py`

The prompt, word for word:

<!-- bench-prompt -->
```text
Users report that Pygments gets some style colours wrong: a style that colours keywords `#0a0` (green) comes out a different colour in LaTeX and groff output. Find the cause and fix it in the library code. Do not change the tests. When you are done, the test suite (`python3 -m pytest -q tests`) must pass.
```

## Running it

```bash
tools/token-bench/bench.py run --bench pygments --arm ants --runs 3
tools/token-bench/bench.py run --bench pygments --arm plain --runs 3
tools/token-bench/bench.py summary --bench pygments
```

The first run clones Pygments once into
`/mnt/Emulators/token-bench-runs/cache/`; later runs copy from there.
