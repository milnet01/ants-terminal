// The GitHub owner/name a checkout's remotes point at, read from .git/config
// without running git. Used by the status bar's repo visibility
// (mainwindow_repo.cpp) and by session_message op:"projects" (ANTS-6720,
// ANTS-6726).
#pragma once

#include <QFile>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace GitOrigin {

// "owner/repo" for a GitHub remote url, in the `https://github.com/owner/repo`
// or `git@github.com:owner/repo` form with or without `.git`; empty otherwise.
inline QString githubSlugOf(QString url) {
    if (url.endsWith(QStringLiteral(".git"))) url.chop(4);
    const QString httpsHost = QStringLiteral("https://github.com/");
    const QString sshHost = QStringLiteral("git@github.com:");
    if (url.startsWith(httpsHost)) return url.mid(httpsHost.size());
    if (url.startsWith(sshHost)) return url.mid(sshHost.size());
    return {};
}

// Every `[remote "<name>"] url = ...` in `<repoRoot>/.git/config`, in file
// order, as {name, url}. Empty where .git is not a directory (a worktree's
// .git file is not followed) or has no config.
inline QVector<QPair<QString, QString>> remoteUrls(const QString &repoRoot) {
    QVector<QPair<QString, QString>> out;
    QFile f(repoRoot + QStringLiteral("/.git/config"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    const QString prefix = QStringLiteral("[remote \"");
    QString remote;  // empty outside a [remote "..."] section
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith('[') && line.endsWith(']')) {
            remote = line.startsWith(prefix) && line.endsWith(QStringLiteral("\"]"))
                ? line.mid(prefix.size(), line.size() - prefix.size() - 2)
                : QString();
            continue;
        }
        if (remote.isEmpty() || !line.startsWith(QStringLiteral("url"))) continue;
        const int eq = line.indexOf('=');
        if (eq < 0) continue;
        out.append({remote, line.mid(eq + 1).trimmed()});
    }
    return out;
}

// The `origin` remote's GitHub owner/name; empty for a non-GitHub or missing
// origin.
inline QString githubRepo(const QString &repoRoot) {
    for (const auto &[name, url] : remoteUrls(repoRoot))
        if (name == QStringLiteral("origin")) return githubSlugOf(url);
    return {};
}

// Every GitHub remote's owner/name: origin's first, then the rest in file
// order, each once. A fork whose origin is the upstream lists both.
inline QStringList githubRepos(const QString &repoRoot) {
    QStringList out;
    if (const QString origin = githubRepo(repoRoot); !origin.isEmpty())
        out.append(origin);
    for (const auto &[name, url] : remoteUrls(repoRoot)) {
        const QString slug = githubSlugOf(url);
        if (!slug.isEmpty() && !out.contains(slug)) out.append(slug);
    }
    return out;
}

}  // namespace GitOrigin
