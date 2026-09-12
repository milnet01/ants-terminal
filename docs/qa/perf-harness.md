# Perf harness

One command that runs every benchmark and reports it against a saved
baseline, so a performance change can be shown rather than asserted.

```bash
tools/perf-report.sh                  # run everything, compare to the baseline
tools/perf-report.sh --save-baseline  # record the current numbers as the baseline
tools/perf-report.sh -R vt            # only benchmarks whose name matches
tools/perf-report.sh --repeat 5       # 5 runs each, keep the best
tools/perf-report.sh --json           # machine-readable
```

Exit code is 0 when everything is within the threshold, 1 when a metric
regressed, 2 on a setup problem. Nothing here runs in CI — the numbers are
wall-clock on a developer desktop and a shared runner cannot reproduce them.

## Running it

The benchmarks are ctest label `perf` and are excluded from the `default` and
`fast` presets, so an ordinary test run never pays for them. Build them first:

```bash
cmake --build build --target bench_vt_throughput bench_paint_throughput \
                             bench_search_throughput bench_partition_walk \
                             bench_drift_lanes
```

A benchmark whose source exists but whose executable does not is reported as
**not built**, with the command to build it. It is not skipped silently: "you
did not build it" and "it produced no numbers" are different problems.

## Reading the report

Each row is one metric, its current value, and its change against the baseline.

```
metric                                            value unit         vs base
------------------------------------------------------------------------------
vt.throughput.ascii_print                       28.8257 MB/s           +3.1%
audit.drift.contract_doc_specs                 750.0839 ms            -95.7%  improved
search.literal_term.lookup_ms                    0.0524 ms            +12.1%  REGRESSION
```

A metric carries its own direction, because the suite mixes both senses: more
MB/s is better, more milliseconds is worse. The percentage is always the raw
change in the value; the verdict accounts for the direction.

**A baseline from a different machine is not compared.** The report says so and
prints the values with no percentages. These are wall-clock measurements, and a
percentage across two machines measures the machines.

## Adding a benchmark

1. Write `tests/perf/bench_<name>.cpp`.
2. Print whatever CSV is useful to a human reading one run directly.
3. Emit each headline number through `tests/perf/perf_metric.h`:

```cpp
#include "perf_metric.h"

AntsPerf::reportLowerBetter("grid.reflow_1M_ms", ms, "ms");
AntsPerf::reportHigherBetter("vt.throughput.ascii", mbPerSec, "MB/s");
```

4. Register it in `CMakeLists.txt` beside the others: an `add_executable`, a
   `target_include_directories(... ${CMAKE_SOURCE_DIR}/tests/perf)` so the
   header resolves, and an `add_test` with `LABELS "perf"`.

It then appears in the report. There is no list of benchmarks and no list of
metrics kept anywhere — the runner discovers sources from `tests/perf/` and
reads whatever metric lines each one emits. That is deliberate: a runner
holding its own copy of either is a hand-maintained parallel description of the
suite, which is the arrangement ANTS-4392 records the cost of. The two drift,
and the drift is invisible exactly when something that should have been
measured was not.

### Choosing a metric

Prefer a **rate** where the workload size is an env knob, and a **time** where
it is fixed. `bench_vt_throughput` reports MB/s rather than milliseconds
because `ANTS_PERF_MB` changes how much it parses; a run at a different size
would otherwise compare against the baseline and read as an enormous
regression.

Report the number that describes the cost, and any number that would explain
it moving. `bench_paint_throughput` reports the cached frame time (what a frame
costs) alongside the cache speedup and hit rate, because the frame time can
improve while the cache quietly stops working.

## What is covered today

| Benchmark | Covers |
|---|---|
| `bench_vt_throughput` | PTY → VtParser → TerminalGrid parse and apply, four corpora |
| `bench_paint_throughput` | the QTextLayout shaping `paintEvent` runs, cached vs not |
| `bench_full_paint` | the WHOLE of `TerminalWidget::paintEvent`, real widget rendered offscreen |
| `bench_search_throughput` | scrollback scan and per-cell match lookup |
| `bench_partition_walk` | the test-audit tree walk run on the GUI thread |
| `bench_drift_lanes` | the four in-process audit lanes run on the GUI thread |

Known gaps, not yet covered: scrollback seek, resize reflow, startup time,
and selection/copy over a large region.

## A benchmark that measures nothing

`bench_full_paint` counts the non-background pixels it rendered and fails
outright if there are almost none. A paint benchmark pointed at an empty
widget reports an excellent frame time, and an excellent frame time is exactly
what it would report if everything were fine — the two are indistinguishable
from the number alone, so the number has to be defended.

The same care applies to the corpora. Each is built to fill the grid width,
because a corpus whose lines are short simply paints less and then reads as
faster. Measured while writing it: the CJK corpus painted 11,641 ink pixels
against plain's 36,564 and duly reported the better frame time. It was
comparing content volume, not cost. Widths are stated in **cells**, not bytes
— a Han character is 3 UTF-8 bytes and 2 columns.

And report what actually happened rather than what was asked for:
`bench_full_paint` prints the grid size the widget ended up with, not the one
requested, because the widget sizes its own grid from its pixel geometry. A
request for 50x200 becomes 52x210 here.
