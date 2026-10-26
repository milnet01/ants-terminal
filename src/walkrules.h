// ANTS-5734 § 2.5 — the directory-walk rules SymbolQuery (src/symbolquery.cpp),
// SimilarCode (src/similarcode.cpp) and the trace engine (src/trace/) share, so
// the walks cannot drift.
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

// The name of a directory a walk does not enter. ANTS-5748 — by name, so a
// path from git's file list can be tested one directory at a time.
inline bool skippedDirName(const QString &name) {
    return name.startsWith(QLatin1Char('.'))
        || name == QLatin1String("build") || name.startsWith(QLatin1String("build-"))
        || name == QLatin1String("node_modules");
}

// A directory a walk does not enter.
inline bool skippedDir(const QFileInfo &fi) { return skippedDirName(fi.fileName()); }

}  // namespace walkrules
