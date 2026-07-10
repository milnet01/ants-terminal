#include "roadmapfoldin.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QString>
#include <QStringList>
#include <QThread>

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace RoadmapFoldIn {

namespace {

QString counterPath(const QString &projectPath) {
    return QDir(projectPath).filePath(QStringLiteral(".roadmap-counter"));
}

QString roadmapPath(const QString &projectPath) {
    return QDir(projectPath).filePath(QStringLiteral("ROADMAP.md"));
}

// ANTS-1342: returns true iff resolving counterPath_ (or, if it doesn't
// yet exist, its parent dir) stays inside projectPath's canonical
// form. Catches:
//   - .roadmap-counter is itself a symlink pointing outside the project
//   - the project root or any path component is a symlink to a
//     directory shared with another project (the 2026-05-14 ME-3
//     scenario where two projects collide on a shared counter)
bool counterStaysInProject(const QString &projectPath,
                           const QString &counterPath_) {
    const QString canonProject =
        QFileInfo(projectPath).canonicalFilePath();
    if (canonProject.isEmpty()) return false;

    QString canonCounter = QFileInfo(counterPath_).canonicalFilePath();
    if (canonCounter.isEmpty()) {
        // File doesn't exist yet — canonicalise its parent dir and
        // synthesise the canonical form it WOULD take.
        QFileInfo cfi(counterPath_);
        const QString parentCanon =
            QFileInfo(cfi.absolutePath()).canonicalFilePath();
        if (parentCanon.isEmpty()) return false;
        // FS-root edge case: if parentCanon == "/", direct
        // concatenation yields "//.roadmap-counter". Strip trailing
        // slash before the join.
        const QString joinable = parentCanon.endsWith(QChar('/'))
            ? parentCanon.left(parentCanon.size() - 1)
            : parentCanon;
        canonCounter = joinable + QChar('/') + cfi.fileName();
    }
    return canonCounter ==
        canonProject + QStringLiteral("/.roadmap-counter");
}

// ANTS-1342 sibling: same guard for ROADMAP.md. The file MUST exist
// (insertBlock is meaningless on a non-existent ROADMAP.md), so the
// "doesn't exist yet" branch above doesn't apply here.
bool roadmapStaysInProject(const QString &projectPath,
                           const QString &roadmapPath_) {
    const QString canonProject =
        QFileInfo(projectPath).canonicalFilePath();
    if (canonProject.isEmpty()) return false;
    const QString canon = QFileInfo(roadmapPath_).canonicalFilePath();
    if (canon.isEmpty()) return false;
    return canon == canonProject + QStringLiteral("/ROADMAP.md");
}

// Acquire ::flock(LOCK_EX|LOCK_NB) on `path`, polling 50 ms × 100
// (5 s budget). Returns the open fd on success, -1 on timeout / open
// failure. Caller MUST close + flock(LOCK_UN).
//
// On flock() systemic failure (errno ∈ {ENOLCK, EBADF, EINVAL,
// ENOSYS}) — typically observed on networked filesystems or some FUSE
// drivers — we sleep + retry like a contention wait, since the next
// attempt may still succeed; only "filesystem cannot support flock at
// all" should fall through to the rename-based fallback. We
// distinguish that case via the static-out parameter `*flockBroken`,
// set true when EVERY attempt failed with one of the systemic codes.
int lockExclusive(const QString &path, bool *flockBroken = nullptr) {
    const QByteArray utf8 = path.toUtf8();
    int fd = ::open(utf8.constData(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    bool everSystemic = false;
    bool everContention = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
            if (flockBroken) *flockBroken = false;
            return fd;
        }
        const int err = errno;
        if (err == EWOULDBLOCK || err == EAGAIN || err == EINTR) {
            everContention = true;
        } else if (err == ENOLCK || err == EBADF || err == EINVAL
                   || err == ENOSYS) {
            everSystemic = true;
        } else {
            everSystemic = true;
        }
        QThread::msleep(50);
    }
    ::close(fd);
    // If only systemic errors fired, fallback can try rename-locking.
    if (flockBroken) *flockBroken = everSystemic && !everContention;
    return -1;
}

void unlockAndClose(int fd) {
    if (fd < 0) return;
    ::flock(fd, LOCK_UN);
    ::close(fd);
}

// ANTS-1490 — O_CREAT|O_EXCL rename-based locking fallback. Used when
// flock() returns systemic errors on every retry (NFS, some FUSE
// filesystems, etc.). Creates `.roadmap-counter.lock` exclusively; the
// presence of the lock file is the lock. 5 s budget mirroring
// lockExclusive. Returns true on success; caller MUST call
// releaseRenameLock to remove the file.
bool acquireRenameLock(const QString &counterPath_) {
    const QString lockPath = counterPath_ + QStringLiteral(".lock");
    const QByteArray utf8 = lockPath.toUtf8();
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int fd = ::open(utf8.constData(),
                              O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) { ::close(fd); return true; }
        if (errno != EEXIST) return false;
        QThread::msleep(50);
    }
    return false;
}

void releaseRenameLock(const QString &counterPath_) {
    const QString lockPath = counterPath_ + QStringLiteral(".lock");
    ::unlink(lockPath.toUtf8().constData());
}

// ANTS-1742 — exclusive-lock handle over `.roadmap-counter`. Both
// allocateIds and insertBlock take this so the two halves of a fold-in
// (ID allocation + ROADMAP.md read-modify-write) serialise against
// concurrent fold-ins on a single lock. Wraps lockExclusive() with the
// ANTS-1490 rename-lock fallback. `held()` is false on acquisition
// failure; the caller must releaseCounterLock() (held or not is a no-op).
struct CounterLock {
    int fd = -1;
    bool usingRenameLock = false;
    QString path;
    bool held() const { return fd >= 0; }
};

CounterLock acquireCounterLock(const QString &counterPath_) {
    CounterLock lk;
    lk.path = counterPath_;
    bool flockBroken = false;
    lk.fd = lockExclusive(counterPath_, &flockBroken);
    if (lk.fd < 0) {
        if (!flockBroken) return lk;  // contention timeout → not held
        if (!acquireRenameLock(counterPath_)) return lk;
        lk.usingRenameLock = true;
        lk.fd = ::open(counterPath_.toUtf8().constData(),
                       O_RDWR | O_CREAT, 0644);
        if (lk.fd < 0) {
            releaseRenameLock(counterPath_);
            return lk;
        }
    }
    return lk;
}

void releaseCounterLock(CounterLock &lk) {
    if (lk.fd < 0) return;
    if (lk.usingRenameLock) {
        ::close(lk.fd);
        releaseRenameLock(lk.path);
    } else {
        unlockAndClose(lk.fd);
    }
    lk.fd = -1;
}

} // namespace

QString counterFilePath(const QString &projectPath) {
    const QString canon = QFileInfo(projectPath).canonicalFilePath();
    if (canon.isEmpty()) return {};
    return canon + QStringLiteral("/.roadmap-counter");
}

// ANTS-3450 — see the doc-comment in roadmapfoldin.h. The counter is now a
// derived cache; this recomputes the true high-water mark from committed
// content so allocation never trusts a stale/absent counter blindly.
// ANTS-3473 — dominant `[PREFIX-NNNN]` prefix in a roadmap's text (the
// most frequent bracketed counter-style token; dominance-by-count shrugs
// off strays like "[UTF-8]"). Returns `fallback` when none is found.
static QString sniffPrefixFromText(const QString &roadmap,
                                   const QString &fallback) {
    static const QRegularExpression sniffRe(
        QStringLiteral("\\[([A-Za-z][A-Za-z0-9_-]*)-[0-9]{1,8}\\]"));
    QHash<QString, int> counts;
    int best = 0;
    QString pfx;
    auto it = sniffRe.globalMatch(roadmap);
    while (it.hasNext()) {
        const QString p = it.next().captured(1);
        const int c = ++counts[p];
        if (c > best) { best = c; pfx = p; }
    }
    return pfx.isEmpty() ? fallback : pfx;
}

QString sniffIdPrefix(const QString &projectPath, const QString &fallback) {
    const QString root = QFileInfo(projectPath).canonicalFilePath();
    if (root.isEmpty()) return fallback;
    QFile f(root + QStringLiteral("/ROADMAP.md"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return fallback;
    return sniffPrefixFromText(QString::fromUtf8(f.readAll()), fallback);
}

qint64 corpusHighWater(const QString &projectPath, const QString &prefix) {
    const QString root = QFileInfo(projectPath).canonicalFilePath();
    if (root.isEmpty()) return 0;

    auto readFile = [](const QString &path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        return QString::fromUtf8(f.readAll());
    };
    const QString roadmap = readFile(root + QStringLiteral("/ROADMAP.md"));

    // Resolve the prefix: sniff the dominant bracketed counter-style id
    // (e.g. "[ANTS-3450]" → "ANTS") from ROADMAP.md when the caller didn't
    // pin one. Dominance-by-count shrugs off stray tokens like "[UTF-8]".
    // ANTS-3473 — reuse the shared text-sniff (empty fallback → "" when the
    // roadmap has no counter-style id, preserving the "return 0" contract).
    QString pfx = prefix;
    if (pfx.isEmpty()) {
        pfx = sniffPrefixFromText(roadmap, QString());
        if (pfx.isEmpty()) return 0;  // no counter-style ids anywhere
    }

    // Max numeric suffix of `pfx-NNNN` across the corpus.
    const QRegularExpression idRe(
        QStringLiteral("\\b") + QRegularExpression::escape(pfx) +
        QStringLiteral("-([0-9]{1,8})\\b"));
    qint64 maxN = 0;
    auto scan = [&](const QString &text) {
        auto it = idRe.globalMatch(text);
        while (it.hasNext()) {
            bool ok = false;
            const qint64 n = it.next().captured(1).toLongLong(&ok);
            if (ok && n > maxN) maxN = n;
        }
    };
    scan(roadmap);
    scan(readFile(root + QStringLiteral("/CHANGELOG.md")));
    QDir archive(root + QStringLiteral("/docs/roadmap"));
    if (archive.exists()) {
        const auto entries =
            archive.entryList({QStringLiteral("*.md")}, QDir::Files);
        for (const QString &e : entries)
            scan(readFile(archive.absoluteFilePath(e)));
    }
    return maxN;
}

// ANTS-1618 — post-failure inspection. Callers invoke this AFTER
// allocateIds has returned an empty list so they can compose a
// state-specific error message instead of the generic "flock/IO
// failure" hint that misled cross-session reports when the real
// cause was an empty / corrupt counter file.
CounterInspection inspectCounter(const QString &projectPath) {
    CounterInspection ins;
    const QString path = counterPath(projectPath);
    if (!counterStaysInProject(projectPath, path)) {
        ins.state = CounterState::PathRefused;
        return ins;
    }
    QFile f(path);
    if (!f.exists()) {
        ins.state = CounterState::EmptyOrAbsent;
        return ins;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        ins.state = CounterState::PermissionDenied;
        return ins;
    }
    const QByteArray raw = f.readAll();
    f.close();
    const QByteArray trimmed = raw.trimmed();
    if (trimmed.isEmpty()) {
        ins.state = CounterState::EmptyOrAbsent;
        return ins;
    }
    bool ok = false;
    const int v = trimmed.toInt(&ok);
    if (!ok) {
        ins.state = CounterState::Corrupt;
        ins.preview = QString::fromUtf8(raw.left(80));
        return ins;
    }
    ins.state = CounterState::Ok;
    ins.value = v;
    return ins;
}

QString renderId(const QString &prefix, int n) {
    // ANTS-3480 — width matches roadmap_log op:append (fixed min-4 pad).
    return QStringLiteral("%1-%2").arg(prefix).arg(n, 4, 10, QLatin1Char('0'));
}

QList<int> allocateIds(const QString &projectPath, int n) {
    if (n <= 0) return {};
    const QString path = counterPath(projectPath);

    // ANTS-1342: refuse if the counter path escapes the project root
    // via symlink. Catches the shared-counter-via-symlink case before
    // any flock / read / write happens.
    if (!counterStaysInProject(projectPath, path)) return {};

    // ANTS-1490 fallback to rename-based locking is folded into
    // acquireCounterLock (shared with insertBlock — ANTS-1742).
    CounterLock lock = acquireCounterLock(path);
    if (!lock.held()) return {};
    auto cleanup = [&](){ releaseCounterLock(lock); };

    // Read current value.
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        cleanup();
        return {};
    }
    const QByteArray raw = f.readAll();
    f.close();

    // ANTS-1618 — empty/whitespace counter is a valid INITIAL state
    // (the file was just created — `flock` + `open(O_CREAT)` above
    // doesn't write any content). Treat as current=0 rather than
    // failing the toInt() parse. Cross-session reports observed
    // 0-byte `.roadmap-counter` files returning misleading "stale
    // .lock sibling" hints when no .lock existed.
    int current = 0;
    const QByteArray trimmed = raw.trimmed();
    if (!trimmed.isEmpty()) {
        bool ok = false;
        current = trimmed.toInt(&ok);
        if (!ok) {
            cleanup();
            return {};
        }
    }

    // ANTS-3450 — floor to the committed high-water mark. The counter is
    // now an untracked cache; a stale-low (or fresh-clone absent → 0) value
    // must never let the batch path reissue a live id. corpusHighWater
    // scans ROADMAP + CHANGELOG + docs/roadmap/*.md, so it also respects ids
    // that have already migrated out of ROADMAP.md. Sniffs the project
    // prefix itself, so callers need not thread one through.
    current = static_cast<int>(
        qMax<qint64>(current, corpusHighWater(projectPath)));

    QList<int> ids;
    ids.reserve(n);
    for (int i = 1; i <= n; ++i) ids.append(current + i);

    // Write the new counter atomically.
    QSaveFile save(path);
    if (!save.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        cleanup();
        return {};
    }
    const QByteArray newVal = QByteArray::number(current + n) + '\n';
    if (save.write(newVal) != newVal.size() || !save.commit()) {
        cleanup();
        return {};
    }

    cleanup();
    return ids;
}

QList<int> peekIds(const QString &projectPath, int n) {
    // ANTS-2227 — would-be allocation without the counter bump. inspectCounter
    // does the path-guard + read + parse; we mirror allocateIds' id math but
    // never write. Empty on any non-Ok state (lets callers reuse their
    // allocateIds "counter_failed" path).
    if (n <= 0) return {};
    const CounterInspection insp = inspectCounter(projectPath);
    // Ok → parsed value; EmptyOrAbsent → value stays 0, exactly as allocateIds
    // treats a fresh/empty counter (current=0 → ids 1…n). Any other state
    // (Corrupt / PermissionDenied / PathRefused) → empty, mirroring allocateIds'
    // refusal so the caller's counter_failed path fires identically.
    if (insp.state != CounterState::Ok &&
        insp.state != CounterState::EmptyOrAbsent) return {};
    // ANTS-3450 — floor to the committed high-water mark, exactly as
    // allocateIds does, so a dry_run preview reports the same first id the
    // real allocate would hand out (mcp_dry_run_parity).
    const int base = static_cast<int>(
        qMax<qint64>(insp.value, corpusHighWater(projectPath)));
    QList<int> ids;
    ids.reserve(n);
    for (int i = 1; i <= n; ++i) ids.append(base + i);
    return ids;
}

QString findActiveReleaseHeading(const QString &projectPath) {
    QFile f(roadmapPath(projectPath));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QString body = QString::fromUtf8(f.readAll());
    f.close();

    static const QRegularExpression reInflight(
        QStringLiteral(R"(\(target:\s*\d{4}-\d+\))"));
    static const QRegularExpression reShipped(
        QStringLiteral(R"(^\d+\.\d+(?:\.\d+)?\s*[—-])"));

    QString firstShipped;
    const QStringList lines = body.split(QChar('\n'));
    for (const QString &line : lines) {
        if (!line.startsWith(QStringLiteral("## "))) continue;
        const QString headingBody = line.mid(3).trimmed();
        if (reInflight.match(line).hasMatch()) {
            return line;
        }
        if (firstShipped.isEmpty()
            && reShipped.match(headingBody).hasMatch()) {
            firstShipped = line;
        }
    }
    return firstShipped;
}

bool insertBlock(const QString &projectPath,
                 const QString &releaseBlockHeading,
                 const QString &block) {
    if (releaseBlockHeading.isEmpty() || block.isEmpty()) return false;

    const QString path = roadmapPath(projectPath);
    // ANTS-1342: refuse if ROADMAP.md escapes the project root via
    // symlink. Same threat shape as the counter case above.
    if (!roadmapStaysInProject(projectPath, path)) return false;

    // ANTS-1742 — serialise this read-modify-write against concurrent
    // fold-ins on the same `.roadmap-counter` lock allocateIds takes.
    // Without it two inserts both read the pre-insert ROADMAP, and the
    // second commit clobbers the first — silently dropping a block and
    // orphaning its already-allocated IDs.
    const QString cpath = counterPath(projectPath);
    if (!counterStaysInProject(projectPath, cpath)) return false;
    CounterLock lock = acquireCounterLock(cpath);
    if (!lock.held()) return false;
    const auto unlock = qScopeGuard([&]{ releaseCounterLock(lock); });

    QFileInfo srcInfo(path);
    QFile::Permissions origPerms = srcInfo.permissions();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QString body = QString::fromUtf8(f.readAll());
    f.close();

    // Normalise the caller's heading: rstrip any trailing newline.
    QString needle = releaseBlockHeading;
    while (needle.endsWith(QChar('\n'))) needle.chop(1);
    while (needle.endsWith(QChar('\r'))) needle.chop(1);

    // Search line-by-line for an exact match.
    const QStringList lines = body.split(QChar('\n'));
    int matchIdx = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i) == needle) {
            matchIdx = i;
            break;
        }
    }
    if (matchIdx < 0) return false;

    // Build the rewritten body: lines[0..matchIdx], then the block,
    // then lines[matchIdx+1..end]. Block ends with a newline so the
    // following line stays a separate paragraph.
    QString out;
    out.reserve(body.size() + block.size() + 4);
    for (int i = 0; i <= matchIdx; ++i) {
        out += lines.at(i);
        out += QChar('\n');
    }
    // Ensure the block has a trailing newline.
    QString blockOut = block;
    if (!blockOut.endsWith(QChar('\n'))) blockOut += QChar('\n');
    out += QChar('\n');     // blank line between heading and block
    out += blockOut;
    if (!blockOut.endsWith(QStringLiteral("\n\n"))) out += QChar('\n');
    for (int i = matchIdx + 1; i < lines.size(); ++i) {
        out += lines.at(i);
        if (i + 1 < lines.size()) out += QChar('\n');
    }

    QSaveFile save(path);
    if (!save.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray utf8 = out.toUtf8();
    if (save.write(utf8) != utf8.size() || !save.commit()) return false;

    // Restore original perms (QSaveFile may have written 0600).
    QFile::setPermissions(path, origPerms);
    return true;
}

} // namespace RoadmapFoldIn
