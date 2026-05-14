#include "roadmapfoldin.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QString>
#include <QStringList>
#include <QThread>

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
int lockExclusive(const QString &path) {
    const QByteArray utf8 = path.toUtf8();
    int fd = ::open(utf8.constData(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return fd;
        QThread::msleep(50);
    }
    ::close(fd);
    return -1;
}

void unlockAndClose(int fd) {
    if (fd < 0) return;
    ::flock(fd, LOCK_UN);
    ::close(fd);
}

} // namespace

QList<int> allocateIds(const QString &projectPath, int n) {
    if (n <= 0) return {};
    const QString path = counterPath(projectPath);

    // ANTS-1342: refuse if the counter path escapes the project root
    // via symlink. Catches the shared-counter-via-symlink case before
    // any flock / read / write happens.
    if (!counterStaysInProject(projectPath, path)) return {};

    int fd = lockExclusive(path);
    if (fd < 0) return {};

    // Read current value.
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        unlockAndClose(fd);
        return {};
    }
    const QByteArray raw = f.readAll();
    f.close();

    bool ok = false;
    int current = raw.trimmed().toInt(&ok);
    if (!ok) {
        unlockAndClose(fd);
        return {};
    }

    QList<int> ids;
    ids.reserve(n);
    for (int i = 1; i <= n; ++i) ids.append(current + i);

    // Write the new counter atomically.
    QSaveFile save(path);
    if (!save.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        unlockAndClose(fd);
        return {};
    }
    const QByteArray newVal = QByteArray::number(current + n) + '\n';
    if (save.write(newVal) != newVal.size() || !save.commit()) {
        unlockAndClose(fd);
        return {};
    }

    unlockAndClose(fd);
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
