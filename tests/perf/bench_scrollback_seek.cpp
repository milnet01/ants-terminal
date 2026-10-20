// bench_scrollback_seek — ANTS-5134: jumping to the top of a full scrollback
// and paging back down, the way a reader looks for something that scrolled
// away.
//
// Drives the REAL TerminalWidget through its own keys: Shift+Home jumps to the
// top, Shift+PageDown pages down. Each key is followed by a synchronous
// render(), so a step costs what the user waits for: the key handling plus the
// frame that shows the new position. The fill is outside the timed region.
//
//   case,scrollback_lines,steps,jump_ms,page_ms_mean
//
// Scrollback defaults to the 50k default; ANTS_PERF_SCROLLBACK overrides.

#include "terminalwidget.h"
#include "terminalgrid.h"
#include "vtparser.h"

#include <QApplication>
#include <QImage>
#include <QKeyEvent>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "perf_metric.h"

namespace {

constexpr int kRows = 50;
constexpr int kCols = 200;
constexpr int kPages = 40;

int envInt(const char *name, int fallback) {
    const char *v = std::getenv(name);
    const int n = (v && *v) ? std::atoi(v) : 0;
    return n > 0 ? n : fallback;
}

void sendKey(TerminalWidget &w, int key) {
    QKeyEvent ev(QEvent::KeyPress, key, Qt::ShiftModifier);
    QApplication::sendEvent(&w, &ev);
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const int lines = envInt("ANTS_PERF_SCROLLBACK", 50'000);

    TerminalWidget w;
    w.resize(kCols * 10, kRows * 20);
    w.setMaxScrollback(lines);
    w.grid()->resize(kRows, kCols);

    // Coloured, full-width lines, so each page paints real content.
    std::string line = "\x1b[1;32m[INFO]\x1b[0m row with \x1b[36msome colour\x1b[0m ";
    while (line.size() < 180) line += "abcdefghij";
    line += "\r\n";
    VtParser parser([&w](const VtAction &a) { w.grid()->processAction(a); });
    for (int i = 0; i < lines + kRows; ++i)
        parser.feed(line.data(), static_cast<int>(line.size()));

    QImage img(w.size(), QImage::Format_ARGB32_Premultiplied);
    w.render(&img);  // warm-up frame: font metrics and first shaping, excluded

    auto t0 = std::chrono::steady_clock::now();
    sendKey(w, Qt::Key_Home);
    w.render(&img);
    const double jumpMs = msSince(t0);
    const int topOffset = w.scrollOffset();

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kPages; ++i) {
        sendKey(w, Qt::Key_PageDown);
        w.render(&img);
    }
    const double pageMean = msSince(t0) / kPages;

    // A jump that did not move the view measures a repaint, not a seek.
    if (topOffset < lines / 2) {
        std::fprintf(stderr, "VACUOUS: Shift+Home left the view at offset %d of "
                             "%d scrollback lines\n", topOffset,
                     w.grid()->scrollbackSize());
        return 2;
    }

    std::printf("case,scrollback_lines,steps,jump_ms,page_ms_mean\n");
    std::printf("scrollback,%d,%d,%.2f,%.3f\n", w.grid()->scrollbackSize(),
                kPages, jumpMs, pageMean);
    AntsPerf::reportLowerBetter("scrollback.seek.jump_to_top_ms", jumpMs, "ms");
    AntsPerf::reportLowerBetter("scrollback.seek.page_down_ms", pageMean, "ms");
    return 0;
}
