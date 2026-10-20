// bench_selection_copy — ANTS-5134: selecting a large region and copying it.
//
// Drives the REAL TerminalWidget the way a user does: Shift+Home to the top of
// the scrollback, a left-button press at the top-left cell, Shift+End to the
// live screen with the button still held, then a move and release at the
// bottom-right cell. Then it times two things separately:
//
//   extract  TerminalWidget::selectedText(), the walk that builds the string
//   copy     Ctrl+Shift+C, which extracts again and hands it to the clipboard
//
// A selection that did not span the scrollback would time a small copy and
// look fast, so the run fails (exit 2) unless the text covers most of it.
//
//   case,scrollback_lines,chars,extract_ms,copy_ms
//
// Scrollback defaults to 50k; ANTS_PERF_SCROLLBACK overrides.

#include "terminalwidget.h"
#include "terminalgrid.h"
#include "vtparser.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "perf_metric.h"

namespace {

constexpr int kRows = 50;
constexpr int kCols = 200;
constexpr int kLineChars = 180;

int envInt(const char *name, int fallback) {
    const char *v = std::getenv(name);
    const int n = (v && *v) ? std::atoi(v) : 0;
    return n > 0 ? n : fallback;
}

void sendKey(TerminalWidget &w, int key, Qt::KeyboardModifiers mods) {
    QKeyEvent ev(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(&w, &ev);
}

void sendMouse(TerminalWidget &w, QEvent::Type type, QPointF at,
               Qt::MouseButton button, Qt::MouseButtons held) {
    QMouseEvent ev(type, at, w.mapToGlobal(at), button, held, Qt::NoModifier);
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

    std::string line;
    while (static_cast<int>(line.size()) < kLineChars) line += "abcdefghij";
    line.resize(kLineChars);
    line += "\r\n";
    VtParser parser([&w](const VtAction &a) { w.grid()->processAction(a); });
    for (int i = 0; i < lines + kRows; ++i)
        parser.feed(line.data(), static_cast<int>(line.size()));

    const QPointF topLeft(2, 2);
    const QPointF bottomRight(w.width() - 2, w.height() - 2);
    sendKey(w, Qt::Key_Home, Qt::ShiftModifier);
    sendMouse(w, QEvent::MouseButtonPress, topLeft, Qt::LeftButton, Qt::LeftButton);
    sendKey(w, Qt::Key_End, Qt::ShiftModifier);
    sendMouse(w, QEvent::MouseMove, bottomRight, Qt::NoButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, bottomRight, Qt::LeftButton, Qt::NoButton);

    auto t0 = std::chrono::steady_clock::now();
    const QString text = w.selectedText();
    const double extractMs = msSince(t0);

    t0 = std::chrono::steady_clock::now();
    sendKey(w, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    const double copyMs = msSince(t0);

    const qsizetype expected = static_cast<qsizetype>(lines) * kLineChars;
    if (text.size() < expected / 2) {
        std::fprintf(stderr, "VACUOUS: selected %lld chars, expected about %lld — "
                             "the drag did not span the scrollback\n",
                     static_cast<long long>(text.size()),
                     static_cast<long long>(expected));
        return 2;
    }

    std::printf("case,scrollback_lines,chars,extract_ms,copy_ms\n");
    std::printf("full_scrollback,%d,%lld,%.2f,%.2f\n", w.grid()->scrollbackSize(),
                static_cast<long long>(text.size()), extractMs, copyMs);
    AntsPerf::reportLowerBetter("selection.extract_ms", extractMs, "ms");
    AntsPerf::reportLowerBetter("selection.copy_ms", copyMs, "ms");
    return 0;
}
