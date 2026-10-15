# SIGN-1 token bench

A fixed practice job for comparing what Claude Code costs in Ants against a
plain terminal. It is the bench SIGN-1 names in `docs/design.md`; ANTS-1120
runs the measurement.

**Neither the prompt nor the model changes after the first measurement.**
A change makes earlier results incomparable; it means a new bench.

## What is here

- `project/` — `tally`, a small Python library that splits shared expenses.
  It has one planted bug. Python 3 standard library only.
- `project/tests/test_trip_example.py` — the planted bug's test. It fails
  until the bug is fixed. The other tests pass either way.
- `bench.py` — the runner. Its header says how each arm is set up and where
  the counts come from.
- `pygments/` — a second, larger bench (ANTS-5697), with its own README.

## The fixed job

**Model:** `claude-opus-5-5`, effort `high`, in both arms.

**Bug test:** `python3 -m unittest tests.test_trip_example`

The prompt, word for word:

<!-- bench-prompt -->
```text
Users report that the settlement printed by `python3 -m tally examples/trip.csv` is wrong: the totals do not match the receipts in the CSV. Find the cause and fix it in the library code. Do not change the tests or the example data. When you are done, the whole test suite (`python3 -m unittest`) must pass.
```

## Running it

```bash
tools/token-bench/bench.py run --arm ants --runs 3
tools/token-bench/bench.py run --arm plain --runs 3
tools/token-bench/bench.py summary
```

- Each run copies `project/` to a new folder under
  `/mnt/Emulators/token-bench-runs/tally/` and runs Claude Code there.
- A run counts only when the bug test passes. The runner puts the original
  tests back before it checks.
- Each run costs real money. `--budget` caps one run, in US dollars.
- `summary` prints each arm's runs, their mean cost, and how much cheaper Ants
  was. SIGN-1 asks for at least 20%.
