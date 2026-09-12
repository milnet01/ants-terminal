// bench_full_paint — wall time of the WHOLE of TerminalWidget::paintEvent
// (ANTS-5134), not the shaping step alone.
//
// bench_paint_throughput measures the QTextLayout shaping that paintEvent
// runs, and it measures it well — but shaping is one part of a frame. The
// paint also walks every visible cell, resolves per-cell foreground and
// background, coalesces background runs (ANTS-1180), tests each cell against
// the URL, highlight and search spans (ANTS-3459 is filed about that linear
// scan), draws the cursor and, when enabled, the overlay. None of that was
// measured, so the number the terminal's smoothness most depends on was the
// one number the suite did not have.
//
// This drives the REAL widget. It builds a TerminalWidget, fills its grid by
// feeding bytes through the real VtParser, and renders it into a QImage with
// QWidget::render(), which dispatches paintEvent synchronously. A
// reproduction of the paint loop would not catch a regression in the widget,
// which is the whole point of having the number.
//
// Corpora vary the axes that plausibly cost, so a regression says WHERE:
//
//   plain          unstyled ASCII — the floor
//   styled         every row carrying SGR colour + bold (the ls/grep shape)
//   cjk            double-width glyphs, the expensive shaping path
//   scrolled_back  the same content viewed from inside the scrollback
//
// Output is one CSV line per corpus:
//
//   corpus,rows,cols,frames,total_ms,ms_per_frame,fps
//
// A frame budget of 16.7 ms is 60 fps. Grid size defaults to 50x200, override
// with ANTS_PERF_ROWS / ANTS_PERF_COLS; frames default to 60, override with
// ANTS_PERF_FRAMES. Exits 0 unless ANTS_PERF_MAX_FRAME_MS=<ms> is set and some
// corpus exceeds it.

#include "terminalwidget.h"
#include "terminalgrid.h"
#include "vtparser.h"

#include <QApplication>
#include <QImage>
#include <QRgb>
#include <QPoint>
#include <QPointF>
#include <QWheelEvent>
#include <QPainter>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "perf_metric.h"

namespace {

int envInt(const char *name, int fallback) {
    const char *v = std::getenv(name);
    if (!v || !*v) return fallback;
    const int n = std::atoi(v);
    return n > 0 ? n : fallback;
}

// Build `rows` lines, each filling roughly `cols` columns by repeating `unit`.
//
// Filling the width matters for comparability: with a fixed-length unit, a
// corpus whose unit is short simply paints less, and then reads as FASTER
// than one whose unit is long. Measured before this was fixed, the CJK corpus
// painted 11,641 ink pixels against plain's 36,564 and duly reported the
// better frame time — the benchmark was comparing content volume, not cost.
// `visualCols` is the unit's width in CELLS, which is not its byte length:
// a Han character is 3 UTF-8 bytes and occupies 2 columns.
std::string buildCorpus(const char *unit, int visualCols, int rows, int cols) {
    const int repeats = std::max(1, cols / std::max(1, visualCols));
    std::string line;
    for (int i = 0; i < repeats; ++i) line += unit;
    line += "\r\n";

    std::string buf;
    buf.reserve(line.size() * static_cast<size_t>(rows));
    for (int i = 0; i < rows; ++i) buf += line;
    return buf;
}

struct Corpus {
    const char *name;
    std::string payload;
    bool scrollBack;
};

struct Result {
    double totalMs;
    double perFrameMs;
};

// Render `frames` full repaints and return the wall time. render() dispatches
// paintEvent synchronously, so this is the real frame cost and not a proxy.
//
// Every frame invalidates the widget's caches the way live output does: a
// paint loop over an unchanged widget would measure the caches, not the paint,
// and would report a frame cost the user never experiences.
Result renderFrames(TerminalWidget &w, QImage &img, int frames) {
    // One warm-up frame, excluded: the first paint builds the font metrics and
    // the initial shaped-run entries, which no later frame pays for.
    w.render(&img);

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        w.update();
        w.render(&img);
    }
    const auto t1 = std::chrono::steady_clock::now();

    const double total = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return {total, frames > 0 ? total / frames : 0.0};
}

}  // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    const int rows   = envInt("ANTS_PERF_ROWS", 50);
    const int cols   = envInt("ANTS_PERF_COLS", 200);
    const int frames = envInt("ANTS_PERF_FRAMES", 60);

    // Enough rows to fill the view several times over, so the scrolled-back
    // corpus has somewhere to scroll to.
    const int fillRows = rows * 4;

    // Each unit is annotated with its width in CELLS, so every corpus fills the
    // same grid width and the frame times are comparable.
    std::vector<Corpus> corpora;
    corpora.push_back({"plain",
        buildCorpus("The quick brown fox jumps over the lazy dog 01234. ",
                    50, fillRows, cols),
        false});
    corpora.push_back({"styled",
        // 50 printing cells; the SGR bytes occupy no columns.
        buildCorpus("\x1b[1;32m[INFO]\x1b[0m conn \x1b[36mhost.example.com\x1b[0m "
                    "port \x1b[33m443\x1b[0m up \x1b[1;31mOK\x1b[0m  ",
                    50, fillRows, cols),
        false});
    corpora.push_back({"cjk",
        // Five Han characters at 2 columns each = 10 cells.
        buildCorpus("\xe4\xbb\x8a\xe6\x97\xa5\xe3\x81\xaf\xe4\xb8\x96\xe7\x95\x8c",
                    10, fillRows, cols),
        false});
    corpora.push_back({"scrolled_back",
        buildCorpus("\x1b[1;32m[INFO]\x1b[0m scrollback with \x1b[36msome\x1b[0m "
                    "\x1b[33mcolour\x1b[0m  ",
                    50, fillRows, cols),
        true});

    std::printf("corpus,rows,cols,frames,total_ms,ms_per_frame,fps,ink_px\n");

    const int maxFrameMs = envInt("ANTS_PERF_MAX_FRAME_MS", 0);
    int overBudget = 0;
    int vacuous = 0;

    for (const Corpus &c : corpora) {
        TerminalWidget w;
        w.resize(cols * 10, rows * 20);   // roughly the requested grid
        w.grid()->resize(rows, cols);

        VtParser parser([&w](const VtAction &action) {
            w.grid()->processAction(action);
        });
        parser.feed(c.payload.data(), static_cast<int>(c.payload.size()));

        if (c.scrollBack) {
            // Scroll the way a user does — a real wheel event — rather than by
            // poking state. There is no public scroll setter, and the wheel
            // path is what a reader actually takes, so it is also the path
            // worth measuring. Several notches to get clear of the live screen.
            for (int i = 0; i < 8; ++i) {
                QWheelEvent ev(QPointF(10, 10), w.mapToGlobal(QPointF(10, 10)),
                               QPoint(0, 0), QPoint(0, 120),
                               Qt::NoButton, Qt::NoModifier,
                               Qt::NoScrollPhase, false);
                QApplication::sendEvent(&w, &ev);
            }
        }

        QImage img(w.size(), QImage::Format_ARGB32_Premultiplied);
        const Result r = renderFrames(w, img, frames);

        // A benchmark that paints an empty widget reports a fast frame and
        // measures nothing, and it looks identical to a fast one. Count the
        // pixels that differ from the most common colour: a grid with text in
        // it has many, a blank one has essentially none. Reported in the CSV
        // so the check cannot be satisfied by nobody looking at it.
        long inkPixels = 0;
        {
            const QRgb bg = img.pixel(img.width() - 2, img.height() - 2);
            for (int y = 0; y < img.height(); y += 2)
                for (int x = 0; x < img.width(); x += 2)
                    if (img.pixel(x, y) != bg) ++inkPixels;
        }
        if (inkPixels < 500) {
            std::fprintf(stderr,
                         "VACUOUS: corpus '%s' rendered only %ld non-background "
                         "pixels — the widget painted no content, so the frame "
                         "time measures nothing.\n", c.name, inkPixels);
            ++vacuous;
        }
        const double fps = r.perFrameMs > 0.0 ? 1000.0 / r.perFrameMs : 0.0;

        // The grid's ACTUAL size, not the requested one: the widget resizes
        // its own grid from its pixel geometry, so a requested 50x200 that
        // became something else would otherwise be reported as 50x200 and the
        // per-frame time read against the wrong amount of work.
        std::printf("%s,%d,%d,%d,%.2f,%.3f,%.1f,%ld\n",
                    c.name, w.grid()->rows(), w.grid()->cols(), frames,
                    r.totalMs, r.perFrameMs, fps, inkPixels);

        // ANTS-5133 — per-frame time is the metric: it is the number a frame
        // budget is stated in, and it does not move when ANTS_PERF_FRAMES does.
        AntsPerf::reportLowerBetter(
            (std::string("paint.full.") + c.name + ".ms_per_frame").c_str(),
            r.perFrameMs, "ms");

        if (maxFrameMs > 0 && r.perFrameMs > double(maxFrameMs)) {
            std::fprintf(stderr,
                         "REGRESSION: corpus '%s' at %.2f ms/frame exceeds the "
                         "ANTS_PERF_MAX_FRAME_MS ceiling of %d\n",
                         c.name, r.perFrameMs, maxFrameMs);
            ++overBudget;
        }
    }

    // A vacuous run fails REGARDLESS of any budget: a frame time from a
    // widget that painted nothing is worse than no number, because it looks
    // like a good one.
    if (vacuous > 0) return 2;
    return overBudget > 0 ? 1 : 0;
}
