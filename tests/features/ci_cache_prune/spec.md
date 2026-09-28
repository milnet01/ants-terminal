# CI keeps one compile cache per job, and never deletes the only one

**Why this exists.** ANTS-5532. Measured 2026-09-28 with `gh cache list`:
the repository held 10,516 MiB in 20 cache entries against GitHub's 10 GB
limit. `build-test` held four `Linux-ccache-release-<sha>` entries (5,149
MiB) and `qt62-baseline` five `Linux-ccache-qt62-<sha>` entries (4,742 MiB).
Each job saves a new entry under its commit's SHA, and a key cannot be
overwritten, so every push adds one. GitHub then evicts the least recently
used entry, which is the nightly `build-asan` cache (387 MiB left that day).
Its cold build overran the 24-minute guard (ANTS-5528).

Only the newest entry is ever restored: `restore-keys` picks the latest
entry with the prefix. The older ones are dead weight.

## The invariants

**INV-1 — every cache save is followed by a prune of the same prefix.** In
`.github/workflows/ci.yml`, each `actions/cache/save` step is followed, in
the same job, by a step running `tools/ci-prune-caches.sh` with that save's
key prefix and its full key. That job grants `actions: write`, which the
delete call needs.

**INV-2 — the prune never deletes the only cache.** When the kept key is not
among the listed entries, which is what a failed save looks like, the script
deletes nothing and says so.

**INV-3 — the prune deletes every other entry of the prefix on this ref.**
When the kept key is listed, every other listed entry is deleted by id, and
the kept one is not.

**INV-4 — a prune failure warns and does not fail the job.** A listing or
delete error prints a `::warning::` annotation and the script exits 0. A
cache problem must not turn a run red that built and tested cleanly.

## Tests

`test_ci_cache_prune.cpp`, in the `test_claude` bundle.

- INV-1: scrapes `ci.yml`. For each save step it finds the key, and requires
  a later step block in the same job that runs the script with the key's
  prefix, and `actions: write` in that job.
- INV-2, INV-3, INV-4: run the script against a fake `gh` placed first on
  `PATH`. The fake prints a canned listing and records each delete.

## What this does NOT cover

- Whether GitHub's API accepts the calls. Only a CI run shows that; the
  first run after this lands is read for `deleted cache` lines.
- The `cache-apt` entries, which `cache-apt-pkgs-action` manages itself.
