// bench_resize_reflow — ANTS-5134: the cost of a width change, which reflows
// the whole scrollback on the GUI thread. A user dragging a window edge pays
// it on every step of the drag.
//
// Links terminalgrid.cpp + vtparser.cpp, like bench_vt_throughput: the real
// TerminalGrid::resize, no widget. The scrollback is filled through the
// parser with lines longer than the grid is wide, so every stored line is
// soft-wrapped and the reflow has real joining and rewrapping to do. Filling
// is outside the timed region.
//
// Each case times a narrow-then-widen pair (160 -> 120 -> 160 columns) and
// reports the mean of one reflow, in milliseconds.
//
// Cases: the 50k-line default scrollback always. The 1M-line maximum only
// with ANTS_PERF_RESIZE_1M=1: at 160 columns it holds several GiB of cells,
// too much to allocate unasked on a machine other work shares.
//
//   case,scrollback_lines,reflows,mean_ms

#include "terminalgrid.h"
#include "vtparser.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "perf_metric.h"

namespace {

constexpr int kRows = 40;
constexpr int kWide = 160;
constexpr int kNarrow = 120;
constexpr int kPairs = 3;

// 200 printable columns per logical line: two rows at both widths. A line
// needing more rows when narrow would overflow the cap on the narrow pass and
// shrink the scrollback this case is named for.
std::string makeLine() {
    std::string s;
    static const char kWord[] = "reflow-me ";
    while (s.size() < 200) s.append(kWord);
    s.resize(200);
    s.append("\r\n");
    return s;
}

void runCase(const char *name, int lines) {
    TerminalGrid grid(kRows, kWide);
    grid.setMaxScrollback(lines);
    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });

    const std::string line = makeLine();
    // Each logical line occupies two rows at 160 columns; feed enough to fill
    // the scrollback to its cap.
    for (int i = 0; i < lines; ++i)
        parser.feed(line.data(), static_cast<int>(line.size()));

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kPairs; ++i) {
        grid.resize(kRows, kNarrow);
        grid.resize(kRows, kWide);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double meanMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / (2.0 * kPairs);

    std::printf("%s,%d,%d,%.2f\n", name, grid.scrollbackSize(), 2 * kPairs, meanMs);
    AntsPerf::reportLowerBetter((std::string("resize.reflow.") + name).c_str(),
                                meanMs, "ms");
}

}  // namespace

int main() {
    std::printf("case,scrollback_lines,reflows,mean_ms\n");
    runCase("scrollback_50k", 50'000);
    const char *big = std::getenv("ANTS_PERF_RESIZE_1M");
    if (big && std::strcmp(big, "1") == 0)
        runCase("scrollback_1m", 1'000'000);
    else
        std::fprintf(stderr, "bench_resize_reflow: 1M case not run "
                             "(set ANTS_PERF_RESIZE_1M=1)\n");
    return 0;
}
