// ANTS-5133 — uniform, self-describing metric lines for the perf harness.
//
// Every benchmark under tests/perf keeps printing its own CSV, which is what a
// human reads when running one directly. Alongside it, each emits one line per
// headline number in the fixed form below, which is what tools/perf-report.sh
// reads:
//
//   ANTSPERF<TAB>name<TAB>value<TAB>unit<TAB>lower_is_better|higher_is_better
//
// The point of the indirection is that the runner knows no benchmark's CSV
// schema. The five benchmarks emit five different shapes — MB/s, a speedup
// ratio, a cache hit rate, milliseconds per lane — and a runner that parsed
// each one would be a hand-maintained parallel description of them, the exact
// arrangement ANTS-4392 records the cost of: the recipes drift and the drift is
// invisible until something that should have been measured silently was not.
// Here a benchmark that emits no metric line is reported as emitting none.
//
// The direction travels WITH the metric because the suite genuinely mixes both
// senses, and a comparison that assumes one is wrong for half the suite: 8%
// more MB/s is an improvement, 8% more milliseconds is a regression.
//
// Written to stdout, so a benchmark run by hand shows both. The prefix is
// deliberately not a CSV-looking token, so it cannot be confused with a data
// row by anything reading the CSV.

#ifndef ANTS_TESTS_PERF_METRIC_H
#define ANTS_TESTS_PERF_METRIC_H

#include <cstdio>

namespace AntsPerf {

// Lower values better (milliseconds, seconds, bytes allocated).
inline void reportLowerBetter(const char *name, double value, const char *unit) {
    std::printf("ANTSPERF\t%s\t%.4f\t%s\tlower_is_better\n", name, value, unit);
}

// Higher values better (throughput, hit rate, speedup).
inline void reportHigherBetter(const char *name, double value, const char *unit) {
    std::printf("ANTSPERF\t%s\t%.4f\t%s\thigher_is_better\n", name, value, unit);
}

}  // namespace AntsPerf

#endif  // ANTS_TESTS_PERF_METRIC_H
