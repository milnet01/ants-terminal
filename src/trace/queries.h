// ANTS-5734 § 2.4 — the trace query files: one per language, compiled into
// ants-mcpd as Qt resources (:/trace-queries/<lang>.scm). A file of the same
// name in ${XDG_DATA_HOME:-~/.local/share}/ants-terminal/trace-queries/
// replaces the built-in copy, and is re-read when its mtime or size changes.

#pragma once

#include <QByteArray>
#include <QString>

#include <memory>

struct TSQuery;

namespace trace {

struct Query {
    // Null when it failed to compile. The query lives while any copy of this
    // Query does, so a call may keep using it after a newer text replaces it.
    std::shared_ptr<const TSQuery> query;
    QByteArray hash;                 // of the text compiled; part of the cache key
    QString limits;                  // the `;; limits:` sentence, if any
    QString error;                   // why `query` is null
    bool userCopy = false;           // read from the user directory
};

// The compiled query for a vendored language: one per language, compiled again
// when its text changes. A compiled query is safe to share between threads.
// Thread-safe.
Query queryFor(const QString &langId);

// The built-in query text for `langId`, empty when there is none.
QByteArray builtinQueryText(const QString &langId);

// The user-override directory, from XDG_DATA_HOME read at each call.
QString userQueryDir();

}  // namespace trace
