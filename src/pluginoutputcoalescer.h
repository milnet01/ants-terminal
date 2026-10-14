#pragma once

// ANTS-4272 — the text behind the `output` and `line` plugin events.
// docs/specs/ANTS-4272-plugin-io-events.md § 2.3 and § 2.4.

#include <QByteArray>
#include <QString>
#include <QStringDecoder>

#include <optional>

// Gathers PTY bytes between two `output` events. One per terminal.
class PluginOutputCoalescer {
public:
    static constexpr int kMaxChars = 16384;

    // Stateful UTF-8 decode: a character split across two appends waits for
    // its second half. Past kMaxChars only the newest text is kept.
    void append(const QByteArray &bytes) {
        m_pending += m_decoder.decode(bytes);
        if (m_pending.size() > kMaxChars) {
            m_pending = m_pending.right(kMaxChars);
            if (!m_pending.isEmpty() && m_pending.front().isLowSurrogate())
                m_pending.remove(0, 1);
        }
    }

    // Pending text; empties the buffer.
    QString take() {
        QString out = std::move(m_pending);
        m_pending.clear();
        return out;
    }

    bool hasPending() const { return !m_pending.isEmpty(); }

private:
    QStringDecoder m_decoder{QStringDecoder::Utf8};
    QString m_pending;
};

// Joins soft-wrapped screen rows into one logical line. One per terminal.
class PluginLineJoiner {
public:
    // Holds a soft-wrapped row; on a row that is not, returns the held
    // rows plus this one. Held text past PluginOutputCoalescer::kMaxChars
    // keeps its head.
    std::optional<QString> addRow(const QString &rowText, bool softWrapped) {
        m_held += rowText;
        if (m_held.size() > PluginOutputCoalescer::kMaxChars) {
            m_held.truncate(PluginOutputCoalescer::kMaxChars);
            if (m_held.back().isHighSurrogate()) m_held.chop(1);
        }
        if (softWrapped) return std::nullopt;
        QString out = std::move(m_held);
        m_held.clear();
        return out;
    }

private:
    QString m_held;
};
