#pragma once

#include <QFont>
#include <QString>
#include <QTextLayout>

#include <cstdint>
#include <memory>
#include <unordered_map>

// ShapedRunCache (ANTS-3453) — caches the HarfBuzz-shaped layout of a text
// run so TerminalWidget::paintEvent skips re-shaping runs whose text has not
// changed between frames.
//
// The shape pass (QTextLayout::beginLayout/endLayout) is the dominant
// per-frame cost during heavy output: Claude Code's styled stream produces
// dozens of runs per row, and pre-cache every one was re-shaped on every
// paint even when the content was identical frame-to-frame (spinners, stable
// status lines, unchanged scrollback). This is the root of the multi-second
// typing freeze. A cache hit reduces a run to a single QTextLayout::draw().
//
// Key: (run text, font variant). Colour and pixel position are applied by the
// caller at draw time, so one cached layout serves any colour/position — only
// the glyph geometry is cached. The cached baselineOff reproduces the
// ANTS-2100 per-run baseline correction (m_fontAscent - line.ascent()) exactly,
// so the draw output is byte-identical to the uncached path.
//
// Eviction is generational (two-map clock): a lookup checks the `hot` map,
// then the `cold` map (promoting a cold hit into hot). When `hot` reaches the
// capacity, `cold` is dropped and `hot` becomes the new `cold`. O(1),
// approximate-LRU, no per-node bookkeeping. Live entries are bounded to
// 2 x capacity.
//
// RAM budget: worst case 2 x capacity QTextLayouts. At the default 4096 cap
// and ~1 KB per short-run layout that is ~8 MB worst case; a real terminal
// working set (visible rows x runs/row plus recent history) sits far below.
//
// Not thread-safe — constructed and used only on the GUI thread (paintEvent).
class ShapedRunCache {
public:
    explicit ShapedRunCache(std::size_t capacity = 4096) : m_capacity(capacity) {}

    // Return a laid-out layout for (text, variant) shaped in `font`. Shapes
    // and stores on a miss; reuses on a hit. `fontAscent` is the widget's
    // shared cell ascent; `baselineOffOut` receives fontAscent - line.ascent()
    // (the ANTS-2100 correction), cached alongside the layout. The returned
    // pointer is owned by the cache and stays valid until the entry is evicted
    // (i.e. for the duration of the current draw).
    QTextLayout *layoutFor(const QString &text, int variant, const QFont &font,
                           int fontAscent, qreal &baselineOffOut);

    // Drop everything. Call on font / DPI / theme change — the shaping inputs
    // changed, so the cached geometry is stale.
    void clear();

    std::size_t size() const { return m_hot.size() + m_cold.size(); }
    std::size_t capacity() const { return m_capacity; }
    std::uint64_t hits() const { return m_hits; }
    std::uint64_t misses() const { return m_misses; }

private:
    struct Key {
        QString text;
        int variant;
        bool operator==(const Key &o) const {
            return variant == o.variant && text == o.text;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key &k) const {
            return qHash(k.text) ^ (static_cast<std::size_t>(k.variant) * 0x9e3779b9u);
        }
    };
    struct Entry {
        std::unique_ptr<QTextLayout> layout;
        qreal baselineOff = 0;
    };
    using Map = std::unordered_map<Key, Entry, KeyHash>;

    // Shape a fresh layout for `key` into `map`, returning the inserted entry.
    Entry &shapeInto(Map &map, const Key &key, const QFont &font, int fontAscent);
    // If hot is at capacity, rotate it into cold (dropping the old cold).
    void rotateIfFull();

    Map m_hot;
    Map m_cold;
    std::size_t m_capacity;
    std::uint64_t m_hits = 0;
    std::uint64_t m_misses = 0;
};
