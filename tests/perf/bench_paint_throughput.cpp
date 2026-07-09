// bench_paint_throughput — shaping + draw throughput for the QPainter /
// QTextLayout text path that TerminalWidget::paintEvent runs per frame.
// ANTS-3462 (baseline scaffolding for ANTS-1115 row 3 / ANTS-3453).
//
// The dominant per-frame cost during heavy output is HarfBuzz shaping: every
// styled text run is re-shaped through QTextLayout on every paint, even when
// the run's text has not changed frame-to-frame. This benchmark isolates that
// cost by replaying representative run corpora through the exact shaping steps
// paintEvent uses (setText → setFont → beginLayout → createLine →
// setLineWidth(INT_MAX) → setPosition → endLayout → draw), drawing onto an
// offscreen QImage so the draw cost is included.
//
// It links no Ants sources — it is a pure Qt micro-benchmark that mirrors the
// current (uncached) paint path, establishing the baseline number that
// ANTS-3453's shaped-run cache is measured against. When the cache lands, a
// second column compares cached vs uncached over the `repeated_frame` corpus
// (the streaming case, where the same runs are drawn every frame).
//
// Output is one machine-parseable CSV line per corpus:
//
//   corpus,runs,frames,wall_ms,runs_per_sec
//   ascii_words,2000,50,123.4,810372.00
//
// Corpus size scales with `ANTS_PERF_FRAMES=<n>` (default 50 frames). Like
// bench_vt_throughput, the process exits 0 unless `ANTS_PERF_MIN_RPS=<floor>`
// (a runs/sec lower bound) is set and some corpus falls below it — off by
// default so the bench never flakes on a loaded CI runner.

#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QTextLayout>
#include <QTextLine>

#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <vector>

namespace {

// One styled run: the text plus the font variant it shapes in.
struct Run {
    QString text;
    int variant{};  // bit0 = bold, bit1 = italic
};

// Build a corpus of runs. `styled` cycles the variant so bold/italic faces
// (which fall back to different shaping) are exercised; otherwise all-regular.
std::vector<Run> makeWordRuns(bool styled, bool cjk) {
    static const char *kAsciiWords[] = {
        "the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog",
        "INFO", "ERROR", "connection", "established", "0123456789",
        "src/terminalwidget.cpp:1187", "->", "==", "std::vector<int>",
    };
    // "今日は世界" split into per-word CJK runs.
    static const char *kCjkWords[] = {
        "\xe4\xbb\x8a\xe6\x97\xa5",       // 今日
        "\xe3\x81\xaf",                   // は
        "\xe4\xb8\x96\xe7\x95\x8c",       // 世界
        "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88",  // テスト
    };
    std::vector<Run> runs;
    const int perFrame = 2000;
    runs.reserve(perFrame);
    if (cjk) {
        const int n = static_cast<int>(std::size(kCjkWords));
        for (int i = 0; i < perFrame; ++i)
            runs.push_back({QString::fromUtf8(kCjkWords[i % n]), 0});
    } else {
        const int n = static_cast<int>(std::size(kAsciiWords));
        for (int i = 0; i < perFrame; ++i) {
            const int variant = styled ? (i % 4) : 0;
            runs.push_back({QString::fromUtf8(kAsciiWords[i % n]), variant});
        }
    }
    return runs;
}

// Shape + draw every run `frames` times, mirroring paintEvent. Returns wall ms.
double shapeAndDraw(const std::vector<Run> &runs, int frames,
                    const QFont &base, const QFont &bold, const QFont &italic,
                    const QFont &boldItalic, int fontAscent, QPainter &p) {
    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
        int px_x = 0;
        for (const Run &run : runs) {
            const QFont *drawFont = &base;
            if (run.variant == 3) drawFont = &boldItalic;
            else if (run.variant == 1) drawFont = &bold;
            else if (run.variant == 2) drawFont = &italic;

            QTextLayout layout;
            layout.setText(run.text);
            layout.setFont(*drawFont);
            layout.beginLayout();
            QTextLine line = layout.createLine();
            qreal baselineOff = 0;
            if (line.isValid()) {
                line.setLineWidth(static_cast<qreal>(INT_MAX));
                line.setPosition(QPointF(0, 0));
                baselineOff = static_cast<qreal>(fontAscent) - line.ascent();
            }
            layout.endLayout();
            layout.draw(&p, QPointF(px_x % 1800, baselineOff));
            px_x += 10;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace

int main(int argc, char *argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    int frames = 50;
    if (const char *env = std::getenv("ANTS_PERF_FRAMES")) {
        char *end = nullptr;
        long v = std::strtol(env, &end, 10);
        if (end != env && v > 0 && v < 100000) frames = static_cast<int>(v);
    }

    double minRps = 0.0;
    if (const char *env = std::getenv("ANTS_PERF_MIN_RPS")) {
        char *end = nullptr;
        double v = std::strtod(env, &end);
        if (end != env && v > 0.0) minRps = v;
    }

    QFont base(QStringLiteral("monospace"), 11);
    base.setStyleHint(QFont::Monospace);
    QFont bold = base;         bold.setBold(true);
    QFont italic = base;       italic.setItalic(true);
    QFont boldItalic = base;   boldItalic.setBold(true); boldItalic.setItalic(true);
    const int fontAscent = QFontMetrics(base).ascent();

    QImage canvas(1920, 32, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::black);
    QPainter p(&canvas);
    p.setFont(base);
    p.setPen(Qt::white);

    struct Corpus { const char *name; std::vector<Run> runs; };
    std::vector<Corpus> corpora;
    corpora.push_back({"ascii_words",    makeWordRuns(false, false)});
    corpora.push_back({"sgr_styled",     makeWordRuns(true,  false)});
    corpora.push_back({"cjk_words",      makeWordRuns(false, true)});
    // repeated_frame is the streaming case: identical runs every frame, so a
    // shaped-run cache (ANTS-3453) turns every frame after the first into pure
    // draws. At this baseline it re-shapes each time — the number to beat.
    corpora.push_back({"repeated_frame", makeWordRuns(true,  false)});

    std::printf("corpus,runs,frames,wall_ms,runs_per_sec\n");
    int belowFloor = 0;
    for (const auto &c : corpora) {
        const double wallMs = shapeAndDraw(c.runs, frames, base, bold, italic,
                                           boldItalic, fontAscent, p);
        const double totalRuns = static_cast<double>(c.runs.size()) * frames;
        const double rps = (wallMs > 0.0) ? totalRuns / (wallMs / 1000.0) : 0.0;
        std::printf("%s,%zu,%d,%.2f,%.2f\n", c.name, c.runs.size(), frames,
                    wallMs, rps);
        if (minRps > 0.0 && rps < minRps) {
            std::fprintf(stderr,
                         "REGRESSION: corpus '%s' at %.0f runs/s is below the "
                         "ANTS_PERF_MIN_RPS floor of %.0f\n", c.name, rps, minRps);
            ++belowFloor;
        }
    }
    return belowFloor == 0 ? 0 : 1;
}
