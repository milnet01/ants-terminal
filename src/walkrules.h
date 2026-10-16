// ANTS-5734 § 2.5 — the directory-walk rules SymbolQuery (src/symbolquery.cpp)
// and the trace engine (src/trace/) share, so the two walks cannot drift.
// Header-only: ants_trace_lib uses them without linking ants_mcpcore_lib.

#pragma once

#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QString>

namespace walkrules {

// The entries a walk visits in one directory, in the order it visits them.
inline QFileInfoList entries(const QString &dirPath) {
    return QDir(dirPath).entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable,
        QDir::Name);
}

// A walk never follows a symlink, to a file or a directory: it may point out
// of the project root.
inline bool followed(const QFileInfo &fi) { return !fi.isSymLink(); }

// A directory a walk does not enter.
inline bool skippedDir(const QFileInfo &fi) {
    const QString name = fi.fileName();
    return name.startsWith(QLatin1Char('.'))
        || name == QLatin1String("build") || name.startsWith(QLatin1String("build-"))
        || name == QLatin1String("node_modules");
}

}  // namespace walkrules
