#pragma once

// ANTS-6520 — the lines of each file an audit run reads for snippets and
// inline suppressions, read from disk once per file. A file of
// kMaxFileBytes or more reads as one empty line, and one that cannot be
// opened as no lines. The cache holds at most kMaxChars characters: a file
// that would pass it clears the cache first. Its spec:
// tests/features/audit_line_cache/spec.md.

#include <QFile>
#include <QHash>
#include <QString>
#include <QStringList>

class AuditLineCache {
public:
    static constexpr qint64 kMaxFileBytes = 4LL * 1024 * 1024;
    static constexpr qsizetype kMaxChars = 32LL * 1024 * 1024;

    QStringList lines(const QString &absPath) {
        const auto it = m_files.constFind(absPath);
        if (it != m_files.constEnd()) return it.value();
        QStringList out;
        QFile f(absPath);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray all =
                f.size() < kMaxFileBytes ? f.read(kMaxFileBytes) : QByteArray();
            out = QString::fromUtf8(all).split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        }
        qsizetype chars = 0;
        for (const QString &l : out) chars += l.size();
        if (m_chars + chars > kMaxChars) clear();   // start over at the cap
        m_files.insert(absPath, out);
        m_chars += chars;
        return out;
    }

    void clear() {
        m_files.clear();
        m_chars = 0;
    }

    // Characters held across every cached file.
    qsizetype cachedChars() const { return m_chars; }

private:
    QHash<QString, QStringList> m_files;
    qsizetype m_chars = 0;
};
