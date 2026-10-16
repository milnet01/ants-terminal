// ANTS-5734 § 2.4, § 2.5 — parse one file with its language's query and keep
// the rows trace needs, never the tree. Rows are cached per file, keyed by
// path, mtime, size and the query's hash, in a 64 MiB least-recently-used
// cache (§ 5) that lives as long as the process.

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <memory>

struct TSQuery;

namespace trace {

struct ByteRange {
    uint32_t start = 0;
    uint32_t end = 0;
    bool contains(uint32_t b) const { return b >= start && b < end; }
};

struct DefRow {
    QString name;
    QString kind;            // function, method, class, type, module, macro, constant, field
    bool isDeclaration = false;
    int line = 0;            // 1-based, the whole node
    int endLine = 0;
    int column = 0;          // 0-based byte column of the node's start
    ByteRange node;          // the whole definition
    ByteRange nameRange;     // the @name node
    QString signature;       // node text up to its body, one line, <= 200 bytes
    QStringList scope;       // enclosing scope names, outermost first
    QString qualifier;       // `Foo` of `void Foo::bar()`, already in scope
};

struct CallRow {
    QString name;
    int line = 0;
    int column = 0;
    ByteRange nameRange;
};

struct FileRows {
    QVector<DefRow> defs;      // in source order
    QVector<CallRow> calls;    // in source order
    QVector<ByteRange> imports;
    QVector<ByteRange> comments;
    QVector<ByteRange> strings;
    bool hasErrors = false;
    bool timedOut = false;     // the parse was abandoned; nothing else is set
    qint64 bytes() const;      // the cache's size estimate
};

// Parses `text` as `langId` with `query` (compiled for that language) and
// extracts its rows. `timeoutMs` bounds the parse (§ 2.5: 2000).
std::shared_ptr<const FileRows> extractRows(const QString &langId,
                                            const TSQuery *query,
                                            const QByteArray &text, int timeoutMs);

// The rows for a file, from the cache when its key still matches, else
// parsed now and cached. Thread-safe.
std::shared_ptr<const FileRows> rowsFor(const QString &absPath, qint64 mtimeMs,
                                        qint64 size, const QString &langId,
                                        const TSQuery *query,
                                        const QByteArray &queryHash,
                                        const QByteArray &text);

// The cache's byte cap and current use, for tests.
qint64 rowCacheBytes();
void clearRowCache();

}  // namespace trace
