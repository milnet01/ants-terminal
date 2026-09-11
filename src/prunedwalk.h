#pragma once

// ANTS-5058 / ANTS-5062 — one directory walk that never descends into a
// directory it excludes. The review engines and the test-audit partition
// each walked every file with QDirIterator and dropped build output
// afterwards, so every build tree was read before the exclusion applied.

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <functional>

namespace PrunedWalk {

struct Stats {
    int  entriesVisited = 0;  // files and directories the walk touched
    bool capped         = false;  // stopped at maxEntries
};

// Calls onFile(path) for each non-hidden file under `root`, never listing a
// directory for which pruneDir(name) is true, and onDir(path) — when given —
// for each directory it will descend into. Symlinked directories are not
// followed, as QDirIterator did not follow them. Stops when a callback
// returns false, or after maxEntries entries (<= 0: unbounded). Paths are
// built from `root` as given, one "/" per level, like QDirIterator's.
inline Stats walkFiles(const QString &root,
                       const std::function<bool(const QString &dirName)> &pruneDir,
                       const std::function<bool(const QString &path)> &onFile,
                       int maxEntries = 0,
                       const std::function<bool(const QString &path)> &onDir = {}) {
    Stats st;
    QStringList stack{root};
    while (!stack.isEmpty()) {
        const QString dir = stack.takeLast();
        const QFileInfoList entries = QDir(dir).entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &fi : entries) {
            if (maxEntries > 0 && st.entriesVisited >= maxEntries) {
                st.capped = true;
                return st;
            }
            ++st.entriesVisited;
            const QString path = dir + QLatin1Char('/') + fi.fileName();
            if (fi.isDir()) {
                if (fi.isSymLink() || pruneDir(fi.fileName())) continue;
                if (onDir && !onDir(path)) return st;
                stack.append(path);
            } else if (!onFile(path)) {
                return st;
            }
        }
    }
    return st;
}

}  // namespace PrunedWalk
