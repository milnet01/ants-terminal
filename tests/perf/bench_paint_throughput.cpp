// bench_paint_throughput — shaping + draw throughput for the QPainter /
// QTextLayout text path that TerminalWidget::paintEvent runs per frame.
// ANTS-3462 (baseline scaffolding) + ANTS-3453 (shaped-run cache) guard.
//
// The dominant per-frame cost during heavy output is HarfBuzz shaping: every
// styled text run is re-shaped through QTextLayout on every paint, even when
// the run's text has not changed frame-to-frame. This benchmark replays
// representative run corpora through the exact shaping steps paintEvent uses
// (setText → setFont → beginLayout → createLine → setLineWidth(INT_MAX) →
// setPosition → endLayout → draw) onto an offscreen QImage, measuring two
// paths per corpus:
//
//   uncached — a fresh QTextLayout shaped every run every frame (the
//              pre-ANTS-3453 behaviour; the baseline number).
//   cached   — the real ShapedRunCache (src/shapedruncache.cpp): a run whose
//              (text, variant) was seen before is drawn without re-shaping.
//
// The `repeated_frame` corpus is the streaming case — identical runs every
// frame — where the cache turns every frame after the first into pure draws.
//
// Output is one machine-parseable CSV line per corpus:
//
//   corpus,runs,frames,uncached_ms,cached_ms,speedup,hit_rate
//   repeated_frame,2000,50,475.33,12.10,39.28,0.980
//
// Corpus size scales with `ANTS_PERF_FRAMES=<n>` (default 50 frames). Exits 0
// unless `ANTS_PERF_MIN_SPEEDUP=<x>` is set and some corpus's cached speedup
// falls below it — off by default so the bench never flakes on a loaded CI
// runner.

#include "shapedruncache.h"

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
    // "今日は世界" / "テスト" split into per-word CJK runs.
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

const QFont *fontFor(int variant, const QFont &base, const QFont &bold,
                     const QFont &italic, const QFont &boldItalic) {
    if (variant == 3) return &boldItalic;
    if (variant == 1) return &bold;
    if (variant == 2) return &italic;
    return &base;
}

// Uncached: shape + draw every run `frames` times, mirroring pre-cache
// paintEvent. Returns wall ms.
double shapeAndDraw(const std::vector<Run> &runs, int frames, const QFont &base,
                    const QFont &bold, const QFont &italic,
                    const QFont &boldItalic, int fontAscent, QPainter &p) {
    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
        int px_x = 0;
        for (const Run &run : runs) {
            const QFont *drawFont = fontFor(run.variant, base, bold, italic, boldItalic);
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

// Cached: same, through the real ShapedRunCache. Returns wall ms.
double shapeAndDrawCached(const std::vector<Run> &runs, int frames,
                          const QFont &base, const QFont &bold,
                          const QFont &italic, const QFont &boldItalic,
                          int fontAscent, QPainter &p, ShapedRunCache &cache) {
    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
        int px_x = 0;
        for (const Run &run : runs) {
            const QFont *drawFont = fontFor(run.variant, base, bold, italic, boldItalic);
            qreal baselineOff = 0;
            QTextLayout *layout = cache.layoutFor(run.text, run.variant,
                                                  *drawFont, fontAscent, baselineOff);
            layout->draw(&p, QPointF(px_x % 1800, baselineOff));
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
    double minSpeedup = 0.0;
    if (const char *env = std::getenv("ANTS_PERF_MIN_SPEEDUP")) {
        char *end = nullptr;
        double v = std::strtod(env, &end);
        if (end != env && v > 0.0) minSpeedup = v;
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
    corpora.push_back({"repeated_frame", makeWordRuns(true,  false)});

    std::printf("corpus,runs,frames,uncached_ms,cached_ms,speedup,hit_rate\n");
    int belowFloor = 0;
    for (const auto &c : corpora) {
        const double uncachedMs = shapeAndDraw(c.runs, frames, base, bold,
                                               italic, boldItalic, fontAscent, p);
        ShapedRunCache cache;
        const double cachedMs = shapeAndDrawCached(c.runs, frames, base, bold,
                                                   italic, boldItalic,
                                                   fontAscent, p, cache);
        const double speedup = (cachedMs > 0.0) ? uncachedMs / cachedMs : 0.0;
        const double total = static_cast<double>(cache.hits() + cache.misses());
        const double hitRate = (total > 0.0) ? cache.hits() / total : 0.0;
        std::printf("%s,%zu,%d,%.2f,%.2f,%.2f,%.3f\n", c.name, c.runs.size(),
                    frames, uncachedMs, cachedMs, speedup, hitRate);
        if (minSpeedup > 0.0 && speedup < minSpeedup) {
            std::fprintf(stderr,
                         "REGRESSION: corpus '%s' cache speedup %.2fx is below "
                         "the ANTS_PERF_MIN_SPEEDUP floor of %.2fx\n",
                         c.name, speedup, minSpeedup);
            ++belowFloor;
        }
    }
    return belowFloor == 0 ? 0 : 1;
}
