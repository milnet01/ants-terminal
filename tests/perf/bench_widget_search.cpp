// bench_widget_search — wall time of the REAL TerminalWidget::performSearch
// over a full scrollback (ANTS-2000).
//
// bench_search_throughput reproduces the scan loop over a pre-built vector of
// QStrings, so it never pays for building each line's text from the grid's
// cells — which is the part the real search does per line, per query. It
// also cannot catch a regression in the widget. This drives the widget.
//
// performSearch() is private. It is reached the way a user reaches it: text in
// the search bar's line edit, then the regex toggle, whose handler cancels the
// keystroke debounce and scans at once. Each toggle is one full scan.
//
// Cases vary what plausibly costs, so a regression says WHERE:
//
//   literal_term   a needle on 1 line in 8
//   regex_term     a character-class regex on the same lines
//   single_space   one space — matches every blank cell, the unbounded
//                  match-list case ANTS-2000's 2026-09-11 note names
//
// Output is one CSV line per case:
//
//   case,lines,cols,matches,scans,ms_per_scan,scan_lines_per_sec
//
// Scrollback depth: ANTS_PERF_LINES (default 50000, the scrollback default;
// the grid clamps to [1000, 1000000]). Scans per case: ANTS_PERF_SCANS
// (default 5). One case only: ANTS_PERF_CASE=<name>. Exits 2 if a case finds no matches — a search that matched
// nothing measured nothing.

#include "terminalwidget.h"
#include "terminalgrid.h"
#include "vtparser.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "perf_metric.h"

namespace {

int envInt(const char *name, int fallback) {
    const char *v = std::getenv(name);
    if (!v || !*v) return fallback;
    const int n = std::atoi(v);
    return n > 0 ? n : fallback;
}

// Log-shaped lines, 1 in 8 carrying the needle — the same mix
// bench_search_throughput uses, so the two are comparable.
std::string buildCorpus(int lines) {
    static const char *kPlain[] = {
        "[2026-07-09 10:15:03] worker idle, waiting for tasks",
        "  at std::__invoke (functional:61) frame 0x7ffe12ab",
        "the quick brown fox jumps over the lazy dog again",
        "resolved 4096 entries in 12ms, cache hit rate 0.94",
    };
    static const char *kNeedle =
        "[2026-07-09 10:15:04] ERROR connection reset by peer (host.example.com)";
    std::string buf;
    buf.reserve(static_cast<size_t>(lines) * 64);
    for (int i = 0; i < lines; ++i) {
        buf += (i % 8 == 0) ? kNeedle : kPlain[i % 4];
        buf += "\r\n";
    }
    return buf;
}

// The label reads "current/total"; total is the match count.
long matchCount(const QLabel *label) {
    const QString t = label->text();
    const int slash = t.indexOf(QLatin1Char('/'));
    return slash < 0 ? -1 : t.mid(slash + 1).toLong();
}

}  // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    const int lines = envInt("ANTS_PERF_LINES", 50000);
    const int scans = envInt("ANTS_PERF_SCANS", 5);

    // Grid width matters: the scan builds every line's text cell by cell, so
    // cost scales with columns. Default 50x200, matching bench_full_paint.
    const int rows = envInt("ANTS_PERF_ROWS", 50);
    const int cols = envInt("ANTS_PERF_COLS", 200);

    TerminalWidget w;
    w.resize(cols * 10, rows * 20);
    w.grid()->resize(rows, cols);
    w.setMaxScrollback(lines);

    const std::string corpus = buildCorpus(lines + w.grid()->rows());
    VtParser parser([&w](const VtAction &action) { w.grid()->processAction(action); });
    parser.feed(corpus.data(), static_cast<int>(corpus.size()));

    QWidget *bar = w.findChild<QWidget *>(QStringLiteral("searchBar"));
    QLineEdit *input = bar ? bar->findChild<QLineEdit *>() : nullptr;
    QLabel *label = nullptr;
    QPushButton *regexBtn = nullptr;
    if (bar) {
        for (QLabel *l : bar->findChildren<QLabel *>())
            if (l->text().contains(QLatin1Char('/'))) label = l;
        for (QPushButton *b : bar->findChildren<QPushButton *>())
            if (b->text() == QStringLiteral(".*")) regexBtn = b;
    }
    if (!input || !label || !regexBtn) {
        std::fprintf(stderr, "SETUP: search bar widgets not found (input=%p label=%p regex=%p)\n",
                     static_cast<void *>(input), static_cast<void *>(label),
                     static_cast<void *>(regexBtn));
        return 2;
    }

    struct Case { const char *name; const char *text; };
    const Case cases[] = {
        {"literal_term", "ERROR"},
        {"regex_term",   "host\\.[a-z]+\\.com"},
        {"single_space", " "},
    };

    std::printf("case,lines,cols,matches,scans,ms_per_scan,scan_lines_per_sec\n");
    int vacuous = 0;
    const int scrollback = w.grid()->scrollbackSize();
    const int totalLines = scrollback + w.grid()->rows();

    // ANTS_PERF_CASE=<name> runs one case — a profile of all three is
    // dominated by single_space's match-list allocations.
    const char *only = std::getenv("ANTS_PERF_CASE");

    for (const Case &c : cases) {
        if (only && *only && std::string(only) != c.name) continue;
        // Regex mode on for every case: a literal needle is also a valid
        // pattern, and the toggle is the one path that scans synchronously.
        if (!regexBtn->isChecked()) regexBtn->toggle();
        input->setText(QString::fromUtf8(c.text));

        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < scans; ++i) {
            regexBtn->toggle();   // off: scans the escaped literal
            regexBtn->toggle();   // on:  scans the pattern
        }
        const auto t1 = std::chrono::steady_clock::now();

        const long matches = matchCount(label);
        const int scanCount = scans * 2;
        const double msPerScan =
            std::chrono::duration<double, std::milli>(t1 - t0).count() / scanCount;
        const double lps = msPerScan > 0.0 ? totalLines / (msPerScan / 1000.0) : 0.0;

        std::printf("%s,%d,%d,%ld,%d,%.2f,%.0f\n", c.name, totalLines,
                    w.grid()->cols(), matches, scanCount, msPerScan, lps);

        if (matches <= 0) {
            std::fprintf(stderr, "VACUOUS: case '%s' matched nothing (label '%s') — "
                                 "the scan measured nothing.\n",
                         c.name, qPrintable(label->text()));
            ++vacuous;
        }
        AntsPerf::reportHigherBetter(
            (std::string("search.widget.") + c.name + ".scan_lines_per_sec").c_str(),
            lps, "lines/s");
    }
    return vacuous > 0 ? 2 : 0;
}
