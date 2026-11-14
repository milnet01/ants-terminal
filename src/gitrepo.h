// Finding a checkout's git folder without running git: the repo root above a
// directory, the git directory a `.git` entry names, and the checked-out
// branch. Used by the status bar's branch chip (mainwindow.cpp), its
// repo-visibility badge (mainwindow_repo.cpp) and rcGitDirOf (ANTS-6187).
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

namespace GitRepo {

// The nearest directory at or above `start` holding a `.git` entry, a
// directory or a worktree's file; empty when there is none.
inline QString rootOf(const QString &start) {
    if (start.isEmpty()) return {};
    QDir d(start);
    while (true) {
        if (QFileInfo::exists(d.filePath(QStringLiteral(".git"))))
            return d.absolutePath();
        if (!d.cdUp()) return {};
    }
}

// The git directory a `.git` path names: the path itself for a directory; for
// a worktree's or submodule's `.git` FILE, its `gitdir:` line, resolved against
// the file's own directory. Empty when the file holds no gitdir.
inline QString gitDirOf(const QString &dotGit) {
    const QFileInfo gi(dotGit);
    if (!gi.isFile()) return dotGit;
    QFile f(dotGit);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    const QString first = QString::fromUtf8(f.readLine()).trimmed();
    static const QLatin1String kGitdir("gitdir:");
    if (!first.startsWith(kGitdir)) return QString();
    const QString gitDir = first.mid(kGitdir.size()).trimmed();
    if (gitDir.isEmpty()) return QString();
    // git may write it relative to the file's directory.
    return QDir(gi.absolutePath()).absoluteFilePath(gitDir);
}

// The branch checked out at or above `start`, or the first seven characters
// of a detached HEAD's commit; empty outside a repository.
inline QString branchOf(const QString &start) {
    const QString root = rootOf(start);
    if (root.isEmpty()) return {};
    const QString gitDir = gitDirOf(root + QStringLiteral("/.git"));
    if (gitDir.isEmpty()) return {};
    QFile head(gitDir + QStringLiteral("/HEAD"));
    if (!head.open(QIODevice::ReadOnly)) return {};
    const QString ref = QString::fromUtf8(head.readAll()).trimmed();
    if (ref.startsWith(QLatin1String("ref: refs/heads/"))) return ref.mid(16);
    if (ref.length() >= 7) return ref.left(7);   // detached HEAD
    return {};
}

}  // namespace GitRepo
