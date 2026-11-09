// The GitHub owner/name a checkout's `origin` remote points at, read from
// .git/config without running git. Used by the status bar's repo visibility
// (mainwindow_repo.cpp) and by session_message op:"projects" (ANTS-6720).
#pragma once

#include <QFile>
#include <QString>

namespace GitOrigin {

// Parse `<repoRoot>/.git/config` for the `[remote "origin"] url = ...` line.
// Handles both `https://github.com/owner/repo[.git]` and
// `git@github.com:owner/repo[.git]` forms. Returns "owner/repo" (no `.git`
// suffix) for a GitHub origin; empty for a non-GitHub or missing origin, or
// where .git is not a directory (a worktree's .git file is not followed).
inline QString githubRepo(const QString &repoRoot) {
    QFile f(repoRoot + QStringLiteral("/.git/config"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QString section;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith('[') && line.endsWith(']')) {
            section = line;
            continue;
        }
        if (section != QStringLiteral("[remote \"origin\"]")) continue;
        if (!line.startsWith(QStringLiteral("url"))) continue;
        const int eq = line.indexOf('=');
        if (eq < 0) continue;
        QString url = line.mid(eq + 1).trimmed();
        // strip a trailing .git so the slug compares cleanly.
        if (url.endsWith(QStringLiteral(".git"))) url.chop(4);
        const QString httpsHost = QStringLiteral("https://github.com/");
        const QString sshHost = QStringLiteral("git@github.com:");
        if (url.startsWith(httpsHost)) return url.mid(httpsHost.size());
        if (url.startsWith(sshHost)) return url.mid(sshHost.size());
        return {};  // origin exists but isn't GitHub
    }
    return {};
}

}  // namespace GitOrigin
