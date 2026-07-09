// bench_search_throughput — throughput of the terminal find feature's two
// hot paths: the scrollback scan (performSearch) and the per-cell match
// lookup (isCellSearchMatch) run for every painted cell while a search is
// active. ANTS-3462 (baseline scaffolding for ANTS-1115 rows 3/7).
//
// It mirrors the algorithmic core of src/terminalwidget.cpp without the
// widget: `performSearch` runs `QRegularExpression::globalMatch` over every
// scrollback line and appends {globalLine, start, len} matches (already
// sorted by line, since lines are scanned in order); `isCellSearchMatch`
// then binary-searches that sorted vector for the first match at/after a
// line and iterates the matches on it. Both are reproduced faithfully so the
// numbers guide ANTS-3457 (per-cell match) and the row-7 lazy-lower index.
//
// Output is one CSV line per corpus:
//
//   corpus,lines,matches,scan_ms,cells,lookup_ms,scan_lines_per_sec
//
// Scrollback depth scales with `ANTS_PERF_LINES=<n>` (default 50000, the
// scrollback default). Exits 0 unless `ANTS_PERF_MIN_LPS=<floor>` (scan
// lines/sec lower bound) is set and some corpus falls below it.

#include <QRegularExpression>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <vector>

namespace {

struct Match {
    int globalLine{};
    int startCol{};
    int len{};
};

// A representative scrollback: log-ish lines, ~1 in 8 containing the needle.
std::vector<QString> makeScrollback(int lines) {
    static const char *kPlain[] = {
        "[2026-07-09 10:15:03] worker idle, waiting for tasks",
        "  at std::__invoke (functional:61) frame 0x7ffe12ab",
        "the quick brown fox jumps over the lazy dog again",
        "resolved 4096 entries in 12ms, cache hit rate 0.94",
    };
    static const char *kNeedle =
        "[2026-07-09 10:15:04] ERROR connection reset by peer (host.example.com)";
    std::vector<QString> out;
    out.reserve(lines);
    const int n = static_cast<int>(std::size(kPlain));
    for (int i = 0; i < lines; ++i)
        out.push_back(QString::fromUtf8((i % 8 == 0) ? kNeedle : kPlain[i % n]));
    return out;
}

// Faithful reproduction of performSearch's scan loop.
std::vector<Match> scan(const std::vector<QString> &sb, const QRegularExpression &re) {
    std::vector<Match> matches;
    for (int gl = 0; gl < static_cast<int>(sb.size()); ++gl) {
        auto it = re.globalMatch(sb[gl]);
        while (it.hasNext()) {
            auto m = it.next();
            const int len = static_cast<int>(m.capturedLength());
            if (len <= 0) break;  // zero-width guard, as in performSearch
            matches.push_back({gl, static_cast<int>(m.capturedStart()), len});
        }
    }
    return matches;
}

// Faithful reproduction of isCellSearchMatch's lower_bound lookup.
bool cellIsMatch(const std::vector<Match> &matches, int globalLine, int col) {
    auto it = std::lower_bound(matches.begin(), matches.end(), globalLine,
                               [](const Match &m, int line) { return m.globalLine < line; });
    for (; it != matches.end() && it->globalLine == globalLine; ++it) {
        if (col >= it->startCol && col < it->startCol + it->len) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    int lines = 50000;
    if (const char *env = std::getenv("ANTS_PERF_LINES")) {
        char *end = nullptr;
        long v = std::strtol(env, &end, 10);
        if (end != env && v > 0 && v < 5'000'000) lines = static_cast<int>(v);
    }
    double minLps = 0.0;
    if (const char *env = std::getenv("ANTS_PERF_MIN_LPS")) {
        char *end = nullptr;
        double v = std::strtod(env, &end);
        if (end != env && v > 0.0) minLps = v;
    }

    const std::vector<QString> sb = makeScrollback(lines);

    struct Corpus { const char *name; QRegularExpression re; };
    std::vector<Corpus> corpora;
    corpora.push_back({"literal_term", QRegularExpression(QStringLiteral("ERROR"))});
    corpora.push_back({"regex_term",
        QRegularExpression(QStringLiteral("host\\.[a-z]+\\.com"))});
    corpora.push_back({"ci_literal",
        QRegularExpression(QStringLiteral("error"),
                           QRegularExpression::CaseInsensitiveOption)});

    // Emulate a full-viewport per-cell match sweep (40 rows x 160 cols) over
    // the visible window — the cost isCellSearchMatch adds to every frame.
    constexpr int kRows = 40, kCols = 160;

    std::printf("corpus,lines,matches,scan_ms,cells,lookup_ms,scan_lines_per_sec\n");
    int belowFloor = 0;
    for (auto &c : corpora) {
        auto t0 = std::chrono::steady_clock::now();
        std::vector<Match> matches = scan(sb, c.re);
        auto t1 = std::chrono::steady_clock::now();
        const double scanMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        const int firstLine = lines > kRows ? lines - kRows : 0;
        volatile int hits = 0;
        auto t2 = std::chrono::steady_clock::now();
        for (int r = 0; r < kRows; ++r)
            for (int col = 0; col < kCols; ++col)
                if (cellIsMatch(matches, firstLine + r, col)) hits = hits + 1;
        auto t3 = std::chrono::steady_clock::now();
        const double lookupMs = std::chrono::duration<double, std::milli>(t3 - t2).count();

        const double lps = (scanMs > 0.0) ? lines / (scanMs / 1000.0) : 0.0;
        std::printf("%s,%d,%zu,%.2f,%d,%.3f,%.2f\n", c.name, lines,
                    matches.size(), scanMs, kRows * kCols, lookupMs, lps);
        if (minLps > 0.0 && lps < minLps) {
            std::fprintf(stderr,
                         "REGRESSION: corpus '%s' at %.0f lines/s is below the "
                         "ANTS_PERF_MIN_LPS floor of %.0f\n", c.name, lps, minLps);
            ++belowFloor;
        }
    }
    return belowFloor == 0 ? 0 : 1;
}
