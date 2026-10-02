#pragma once
// ANTS-5096 — `mutation_probe`'s crash-recovery journal.
//
// While a mutant is on disk, the only copy of the original file is in the
// probing process's memory. ants-mcpd runs one process per session, so a
// session that ends or reconnects mid-probe kills it between the write and the
// restore, and the mutant stays in the source tree. The journal is that
// original, written outside the project before the first mutant and removed
// after a clean restore. The next probe, in any session, puts back a file
// still holding the mutant it recorded.
//
// One journal per source file, named by the SHA-256 of its path, holding the
// path, the original bytes, the SHA-256 of the mutant last written and the
// writer's pid. Kept apart from mutationprobe.cpp, which is pure by design.

#include <QByteArray>
#include <QList>
#include <QString>

namespace MutationJournal {

// GenericDataLocation/ants-terminal/mutation-journal — outside every project,
// so a journal never shows in `git status`.
QString defaultDir();

// Record `original` for `path` and the mutant about to be written. Rewritten
// before each mutant, so it always names the bytes on disk. False when it
// could not be written; the caller must not write the mutant then.
bool write(const QString &dir, const QString &path, const QByteArray &original,
           const QByteArray &mutant);

// Drop `path`'s journal after its file is back to the original, or after a
// concurrent edit made the journal meaningless.
void clear(const QString &dir, const QString &path);

struct Recovered {
    QString path;
    // "restored": the file held the recorded mutant and now holds the original.
    // "left_edited": the file changed since, so it is someone's edit; untouched.
    // "left_missing": the file is gone; nothing to restore.
    // "restore_failed": the write back failed; the journal is kept.
    QString outcome;
};

// Settle every journal whose writer is no longer running. A journal whose pid
// is alive belongs to a probe in progress and is skipped, since restoring its
// file would revert a mutant its tests are running against.
QList<Recovered> recover(const QString &dir);

}  // namespace MutationJournal
