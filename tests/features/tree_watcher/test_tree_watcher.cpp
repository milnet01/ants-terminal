// ANTS-3509 — DirTreeWatcher (raw-inotify directory-tree change watcher).
// Behavioral tests (real temp dir + inotify + a pumped QCoreApplication event
// loop) plus the pure directoriesContaining() helper.

#include "treewatcher.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>

namespace {

// Write `content` to `path`, truncating. Returns true on success.
bool writeFile(const QString &path, const QByteArray &content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(content);
    f.close();
    return true;
}

// Pump the event loop until `pred()` is true or `budgetMs` elapses. Returns
// the final pred() value. Needed because inotify delivery + the 300 ms
// debounce are asynchronous.
template <typename Pred>
bool pumpUntil(Pred pred, int budgetMs) {
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < budgetMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return pred();
}

}  // namespace

// INV-1 — a content edit of an existing file in a watched dir fires changed().
// This is the case QFileSystemWatcher dropped (the whole reason for ANTS-3509).
TEST(DirTreeWatcher, ContentEditFires) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString file = tmp.path() + "/existing.txt";
    ASSERT_TRUE(writeFile(file, "v1\n"));

    DirTreeWatcher w;
    ASSERT_TRUE(w.ok());
    int fired = 0;
    QObject::connect(&w, &DirTreeWatcher::changed, [&fired] { ++fired; });
    ASSERT_EQ(w.addDirs({tmp.path()}), 1);

    ASSERT_TRUE(writeFile(file, "v2 edited — different length\n"));
    EXPECT_TRUE(pumpUntil([&fired] { return fired > 0; }, 3000))
        << "content edit of a watched dir's child did not fire changed()";
}

// INV-2 — creating a new file in a watched dir fires changed().
TEST(DirTreeWatcher, NewFileFires) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    DirTreeWatcher w;
    ASSERT_TRUE(w.ok());
    int fired = 0;
    QObject::connect(&w, &DirTreeWatcher::changed, [&fired] { ++fired; });
    ASSERT_EQ(w.addDirs({tmp.path()}), 1);

    ASSERT_TRUE(writeFile(tmp.path() + "/brand_new.txt", "hi\n"));
    EXPECT_TRUE(pumpUntil([&fired] { return fired > 0; }, 3000))
        << "new file in a watched dir did not fire changed()";
}

// INV-4 — addDirs is idempotent (re-adding does not grow the watch count) and
// a non-existent path is skipped, not fatal.
TEST(DirTreeWatcher, AddDirsIdempotentAndSafe) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    DirTreeWatcher w;
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(w.addDirs({tmp.path()}), 1);
    EXPECT_EQ(w.addDirs({tmp.path()}), 1);   // re-add: no growth
    EXPECT_EQ(w.addDirs({tmp.path() + "/does/not/exist"}), 1);  // skipped
    EXPECT_EQ(w.watchCount(), 1);
}

// INV-5 — directoriesContaining() maps ls-files -z output to the sorted,
// de-duplicated set of absolute directories holding those files.
TEST(DirTreeWatcher, DirectoriesContainingPure) {
    // Two files share src/, one is at the root, one is nested deeper.
    QByteArray z;
    z += "README.md";          z += '\0';
    z += "src/a.cpp";          z += '\0';
    z += "src/b.cpp";          z += '\0';
    z += "src/ui/w.cpp";       z += '\0';
    const QStringList dirs =
        DirTreeWatcher::directoriesContaining("/repo", z);

    // Root + src + src/ui, each once, sorted, absolute.
    ASSERT_EQ(dirs.size(), 3);
    EXPECT_EQ(dirs.at(0), QStringLiteral("/repo"));
    EXPECT_EQ(dirs.at(1), QStringLiteral("/repo/src"));
    EXPECT_EQ(dirs.at(2), QStringLiteral("/repo/src/ui"));
}

// INV-5 (edge) — empty input yields an empty list, not a crash.
TEST(DirTreeWatcher, DirectoriesContainingEmpty) {
    EXPECT_TRUE(DirTreeWatcher::directoriesContaining("/repo", QByteArray()).isEmpty());
    EXPECT_TRUE(DirTreeWatcher::directoriesContaining("/repo", QByteArray("\0", 1)).isEmpty());
}
