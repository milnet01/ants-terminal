#include "shapedruncache.h"

#include <QPointF>
#include <QTextLine>

#include <climits>
#include <utility>

void ShapedRunCache::clear() {
    m_hot.clear();
    m_cold.clear();
    m_hotUnits = 0;
    m_coldUnits = 0;
    m_uncached.reset();
    // hits/misses are lifetime counters — left intact across a clear so a
    // font-change mid-session does not reset the observed hit rate.
}

void ShapedRunCache::rotateIfFull(std::size_t incomingUnits) {
    if (m_hot.size() < m_capacity &&
        m_hotUnits + incomingUnits <= static_cast<std::size_t>(kTextUnitBudget))
        return;
    m_cold = std::move(m_hot);
    m_coldUnits = m_hotUnits;
    m_hot.clear();
    m_hotUnits = 0;
}

ShapedRunCache::Entry &ShapedRunCache::shapeInto(Map &map, const Key &key,
                                                 const QFont &font, int fontAscent) {
    // Mirror the exact shaping paintEvent used before the cache: a fresh
    // QTextLayout (no clearFormats needed — a new layout carries no formats),
    // an unbounded line width so a monospace run is never wrapped, and the
    // ANTS-2100 baseline offset so every run sits on the shared cell baseline.
    auto layout = std::make_unique<QTextLayout>();
    layout->setText(key.text);
    layout->setFont(font);
    layout->beginLayout();
    QTextLine line = layout->createLine();
    qreal baselineOff = 0;
    if (line.isValid()) {
        line.setLineWidth(static_cast<qreal>(INT_MAX));
        line.setPosition(QPointF(0, 0));
        baselineOff = static_cast<qreal>(fontAscent) - line.ascent();
    }
    layout->endLayout();

    Entry entry;
    entry.layout = std::move(layout);
    entry.baselineOff = baselineOff;
    auto [it, _] = map.emplace(key, std::move(entry));
    return it->second;
}

QTextLayout *ShapedRunCache::layoutFor(const QString &text, int variant,
                                       const QFont &font, int fontAscent,
                                       qreal &baselineOffOut) {
    const Key key{text, variant};
    const auto units = static_cast<std::size_t>(text.size());

    // ANTS-5077 — a run this long is shaped into the one-slot scratch layout
    // and never stored, so it cannot hold its text in the cache.
    if (text.size() > kMaxCachedRunUnits) {
        ++m_misses;
        Map scratch;
        Entry &entry = shapeInto(scratch, key, font, fontAscent);
        baselineOffOut = entry.baselineOff;
        m_uncached = std::move(entry.layout);
        return m_uncached.get();
    }

    if (auto it = m_hot.find(key); it != m_hot.end()) {
        ++m_hits;
        baselineOffOut = it->second.baselineOff;
        return it->second.layout.get();
    }

    if (auto it = m_cold.find(key); it != m_cold.end()) {
        ++m_hits;
        // Promote cold -> hot so a still-referenced run survives the next
        // rotation. Extract first, then rotate, then re-insert.
        Entry entry = std::move(it->second);
        m_cold.erase(it);
        m_coldUnits -= units;
        baselineOffOut = entry.baselineOff;
        QTextLayout *layout = entry.layout.get();
        rotateIfFull(units);
        m_hot.emplace(key, std::move(entry));
        m_hotUnits += units;
        return layout;
    }

    ++m_misses;
    rotateIfFull(units);
    Entry &entry = shapeInto(m_hot, key, font, fontAscent);
    m_hotUnits += units;
    baselineOffOut = entry.baselineOff;
    return entry.layout.get();
}
