// bench_scrollback_memory — ANTS-4534: what a full scrollback costs in heap,
// and which part of each line pays for it.
//
// Fills a real TerminalGrid through the VT parser with a mix of output shapes
// (short `ls`-style lines, coloured compiler lines, full-width lines), then
// reads the heap growth from glibc's mallinfo2 and splits it into the terms
// the roadmap item names:
//
//   cells        sizeof(Cell) x each scrollback row's capacity
//   content      the cells up to each row's last non-blank one: what a row
//                trimmed of trailing blanks would keep (lever b)
//   line_headers sizeof(TermLine) per line, plus the hyperlink side table
//
//   case,cols,scrollback_lines,sizeof_cell,heap_mib,heap_bytes_per_line,
//   cells_mib,content_cells_pct,line_header_mib
//
// Scrollback defaults to the 50k default; ANTS_PERF_SCROLLBACK overrides.

#include "terminalgrid.h"
#include "vtparser.h"

#include <QCoreApplication>

#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <string>
#include <vector>

#include "perf_metric.h"

namespace {

constexpr int kRows = 50;
constexpr int kCols = 200;

int envInt(const char *name, int fallback) {
    const char *v = std::getenv(name);
    const int n = (v && *v) ? std::atoi(v) : 0;
    return n > 0 ? n : fallback;
}

size_t heapInUse() {
    const struct mallinfo2 mi = mallinfo2();
    return mi.uordblks + mi.hblkhd;  // arena chunks in use + mmapped blocks
}

// One cycle of output: three short lines, two coloured compiler lines, one
// full-width line. Six lines per cycle.
std::string cycle() {
    std::string s;
    s += "total 48\r\n";
    s += "-rw-r--r--. 1 ants ants  4096 Nov  6 13:30 CMakeLists.txt\r\n";
    s += "drwxr-xr-x. 1 ants ants   512 Nov  6 13:30 src\r\n";
    s += "\x1b[1m/src/terminalgrid.cpp:2095:12:\x1b[0m \x1b[35mwarning:\x1b[0m "
         "unused variable 'cols' [\x1b[35m-Wunused-variable\x1b[0m]\r\n";
    s += "[ 42%] \x1b[32mBuilding CXX object CMakeFiles/ants.dir/src/vtparser.cpp.o\x1b[0m\r\n";
    std::string full;
    while (static_cast<int>(full.size()) < kCols) full += "0123456789abcdef";
    full.resize(kCols - 1);
    s += full + "\r\n";
    return s;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const int lines = envInt("ANTS_PERF_SCROLLBACK", 50'000);

    TerminalGrid grid(kRows, kCols);
    grid.setMaxScrollback(lines);
    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });
    const std::string chunk = cycle();

    const size_t before = heapInUse();
    for (int i = 0; i < (lines + kRows) / 6 + 1; ++i)
        parser.feed(chunk.data(), static_cast<int>(chunk.size()));
    const size_t after = heapInUse();

    const int n = grid.scrollbackSize();
    if (n < lines / 2) {
        std::fprintf(stderr, "VACUOUS: only %d of %d scrollback lines filled\n", n, lines);
        return 2;
    }

    double cellBytes = 0, contentCells = 0, allCells = 0;
    for (int i = 0; i < n; ++i) {
        const std::vector<Cell> &row = grid.scrollbackLine(i);
        cellBytes += double(row.capacity()) * sizeof(Cell);
        allCells += double(row.size());
        int last = static_cast<int>(row.size()) - 1;
        while (last >= 0 && row[last].codepoint == ' ') --last;
        contentCells += last + 1;
    }
    // TermLine itself sits in the deque; the hyperlink side table holds one
    // empty vector per line (std::deque<std::vector<HyperlinkSpan>>).
    const double headerBytes = double(n) * (sizeof(TermLine) + sizeof(std::vector<int>));

    const double mib = 1024.0 * 1024.0;
    const double heap = double(after - before);
    std::printf("case,cols,scrollback_lines,sizeof_cell,heap_mib,heap_bytes_per_line,"
                "cells_mib,content_cells_pct,line_header_mib\n");
    std::printf("mixed,%d,%d,%zu,%.1f,%.0f,%.1f,%.1f,%.1f\n", kCols, n, sizeof(Cell),
                heap / mib, heap / n, cellBytes / mib, 100.0 * contentCells / allCells,
                headerBytes / mib);
    AntsPerf::reportLowerBetter("scrollback.memory.bytes_per_line", heap / n, "B");
    return 0;
}
