// ANTS-1677 mainwindow piece 8/8 — repository state and updates
#include "mainwindow.h"
#include "mainwindow_internal.h"
#include "terminalwidget.h"
#include "themes.h"             // ANTS-1325: include directly where Themes:: is called
#include "hostexec.h"          // ANTS-5598 — git and gh run on the host inside a Flatpak
#include "themedstylesheet.h"
#include "selfupdate.h"           // ANTS-5560 — the self-updater
#include <QJsonObject>
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>
#include <QPointer>
#include <QStandardPaths>
#include <QDir>
#include <QProcess>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QTimer>
#include <QJsonValue>

using namespace mainwindowdetail;

namespace {

// Walk up `start` looking for a `.git` entry (file or directory).
// Returns the absolute path to the directory containing `.git`, or
// empty if none found.
QString findGitRepoRoot(const QString &start) {
    if (start.isEmpty()) return {};
    QDir d(start);
    while (true) {
        if (QFileInfo::exists(d.filePath(QStringLiteral(".git"))))
            return d.absolutePath();
        if (!d.cdUp()) return {};
    }
}

// Parse `.git/config` for the `[remote "origin"] url = ...` line.
// Handles both `https://github.com/owner/repo[.git]` and
// `git@github.com:owner/repo[.git]` forms. Returns "owner/repo"
// (no `.git` suffix) for GitHub remotes; empty for non-GitHub or
// missing origin.
QString parseGithubOriginSlug(const QString &repoRoot) {
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
        // https://github.com/owner/repo
        const QString httpsHost = QStringLiteral("https://github.com/");
        const QString sshHost = QStringLiteral("git@github.com:");
        if (url.startsWith(httpsHost)) return url.mid(httpsHost.size());
        if (url.startsWith(sshHost)) return url.mid(sshHost.size());
        return {};  // origin exists but isn't GitHub
    }
    return {};
}

}  // namespace

void MainWindow::refreshRepoVisibility() {
    if (!m_repoVisibilityLabel) return;

    // Probe `gh` once per session — caching the result avoids a
    // shell-out on every tab switch when the binary is missing.
    if (!m_ghAvailableProbed) {
        m_ghAvailable = !QStandardPaths::findExecutable(
            QStringLiteral("gh")).isEmpty();
        m_ghAvailableProbed = true;
    }
    if (!m_ghAvailable) { m_repoVisibilityLabel->hide(); return; }

    QString cwd;
    if (auto *t = focusedTerminal()) cwd = t->shellCwd();
    if (cwd.isEmpty()) { m_repoVisibilityLabel->hide(); return; }

    const QString repoRoot = findGitRepoRoot(cwd);
    if (repoRoot.isEmpty()) { m_repoVisibilityLabel->hide(); return; }

    const QString slug = parseGithubOriginSlug(repoRoot);
    if (slug.isEmpty()) { m_repoVisibilityLabel->hide(); return; }

    // ANTS-1137 — in-flight guard mirroring m_reviewProbeInFlight.
    // Without this, a fast tab-switch could race two `gh repo view`
    // QProcesses against the same repoRoot, both writing into the
    // same cache slot — winner is order-dependent. Drop redundant
    // probes; cached visibility is still rendered if available.
    if (m_repoVisibilityProbeInFlight.value(repoRoot, false)) return;

    auto applyVisibility = [this](const QString &visibility,
                                  const QString &repoSlug) {
        if (!m_repoVisibilityLabel) return;
        if (visibility.isEmpty()) { m_repoVisibilityLabel->hide(); return; }
        const bool isPublic =
            visibility.compare(QStringLiteral("PUBLIC"),
                               Qt::CaseInsensitive) == 0;
        const QString label = isPublic ? tr("Public") : tr("Private");
        const Theme &th = Themes::byName(m_currentTheme);
        const QColor &col = isPublic ? th.ansi[2] : th.ansi[3];
        m_repoVisibilityLabel->setText(label);
        // ANTS-1147 — chip QSS shared with the git-branch label via
        // themedstylesheet::buildChipStylesheet. The only delta from
        // the branch chip is the left margin: 0 here so the badge sits
        // flush against the title bar (the branch chip uses 4 px to
        // pair with the git separator). Foreground colour stays
        // public-green / private-red for the at-a-glance visibility
        // cue from 0.7.45.
        m_repoVisibilityLabel->setStyleSheet(
            themedstylesheet::buildChipStylesheet(th, col, /*leftMarginPx=*/0));
        m_repoVisibilityLabel->setToolTip(tr("%1 on GitHub").arg(repoSlug));
        m_repoVisibilityLabel->show();
    };

    // Cache hit (10 min TTL) → render immediately, skip the shell-out.
    constexpr qint64 kCacheTtlMs = 10 * 60 * 1000;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    auto it = m_repoVisibilityCache.find(repoRoot);
    if (it != m_repoVisibilityCache.end() &&
            (nowMs - it->fetchedAt) < kCacheTtlMs) {
        applyVisibility(it->visibility, slug);
        return;
    }

    // Miss → hide until the async query lands. Avoids flashing a
    // stale value from a different repo (the previous tab's).
    m_repoVisibilityLabel->hide();

    auto *proc = new QProcess(this);
    proc->setProgram(QStringLiteral("gh"));
    // `slug` is read verbatim from the repo's .git/config origin URL, so a
    // hostile clone with an origin like `https://github.com/-x/y` could hand
    // `gh` a leading-dash arg parsed as a flag. Guard with a trailing `--`
    // end-of-options sentinel AFTER the flags (gh/Cobra treats everything past
    // `--` as positional, so the `--json`/`-q` flags must precede it; slug then
    // parses as the positional repo even with a leading dash).
    proc->setArguments({QStringLiteral("repo"), QStringLiteral("view"),
                        QStringLiteral("--json"),
                        QStringLiteral("visibility"),
                        QStringLiteral("-q"),
                        QStringLiteral(".visibility"),
                        QStringLiteral("--"), slug});
    QPointer<MainWindow> self(this);
    connect(proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [self, proc, repoRoot, slug, applyVisibility](
                int exitCode, QProcess::ExitStatus status) {
                proc->deleteLater();
                if (!self) return;
                QString visibility;
                if (status == QProcess::NormalExit && exitCode == 0) {
                    visibility = QString::fromUtf8(
                        proc->readAllStandardOutput()).trimmed();
                }
                // Cache both hits and negative results — a 60 s
                // negative TTL avoids hammering on every tab switch
                // when `gh` is unauthenticated. The full TTL applies
                // to positive results; we encode "negative" by storing
                // an empty visibility with the same fetchedAt so the
                // hit-branch sees an empty string and hides.
                self->m_repoVisibilityCache[repoRoot] = {
                    visibility,
                    QDateTime::currentMSecsSinceEpoch()};
                // ANTS-1137 — clear in-flight on completion regardless
                // of success/failure path. ANTS-1554: tightly-scoped
                // pragma suppresses a GCC -Wnull-dereference false
                // positive — the warning fires inside QHash::isEmpty()
                // (qhash.h:966 `!d || d->size == 0`) when the
                // `QHash::remove` → `removeImpl` → `isEmpty` inline
                // chain is reached from this lambda-via-signal callsite;
                // the short-circuit is logically safe but the
                // template-instantiation context defeats GCC's value
                // tracking.
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wnull-dereference"
#endif
                self->m_repoVisibilityProbeInFlight.remove(repoRoot);
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif
                applyVisibility(visibility, slug);
            });
    // ANTS-1137 — mark in-flight before start so a re-entry within
    // this 2 s tick (or any fast tab-switch before the QProcess
    // completes) drops the redundant probe.
    // ANTS-5080 — a gh that fails to start never emits finished, which left
    // the in-flight flag set for the session. Clear it here (assignment, not
    // remove(): the remove() above sits in ANTS-1554's pragma block) and
    // cache the negative result like any other failure.
    connect(proc, &QProcess::errorOccurred, this,
            [self, proc, repoRoot](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;  // finished follows otherwise
        proc->deleteLater();
        if (!self) return;
        self->m_repoVisibilityCache[repoRoot] = {
            QString(), QDateTime::currentMSecsSinceEpoch()};
        self->m_repoVisibilityProbeInFlight[repoRoot] = false;
    });
    m_repoVisibilityProbeInFlight[repoRoot] = true;
    HostExec::start(*proc);
    // ANTS-5080 — a gh that never exits (no network, a prompt) is killed;
    // kill() delivers finished, which caches the failure and clears the flag.
    QTimer::singleShot(15000, proc, [proc]() {
        if (proc->state() != QProcess::NotRunning) proc->kill();
    });
}

void MainWindow::checkForUpdates(bool userInitiated) {
    if (!m_updateAvailableAction) return;
    if (!m_updateNam) m_updateNam = new QNetworkAccessManager(this);

    QNetworkRequest req(QUrl(QStringLiteral(
        "https://api.github.com/repos/milnet01/ants-terminal/releases/latest")));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "Ants-Terminal-Updater");

    QNetworkReply *reply = m_updateNam->get(req);
    QPointer<MainWindow> self(this);
    connect(reply, &QNetworkReply::finished, this,
            [self, reply, userInitiated]() {
        reply->deleteLater();
        MainWindow *win = self.data();
        if (!win) return;
        if (!win->m_updateAvailableAction) return;
        if (reply->error() != QNetworkReply::NoError) {
            if (userInitiated) {
                win->showStatusMessage(
                    win->tr("Update check failed: %1")
                        .arg(reply->errorString()),
                    5000);
            }
            return;
        }
        const QByteArray body = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) return;
        const QJsonObject rel = doc.object();
        const QString rawTag = rel.value("tag_name").toString();
        QString tag = rawTag;
        if (tag.startsWith('v')) tag.remove(0, 1);
        if (tag.isEmpty()) return;
        win->m_latestRemoteVersion = tag;
        const QString current = QString::fromUtf8(ANTS_VERSION);
        // ANTS-5560 — while a swapped-in update waits for a restart, no check
        // replaces the "Restart to finish" indicator.
        if (!win->m_updateInstalledVersion.isEmpty()) {
            if (userInitiated)
                win->showStatusMessage(
                    win->tr("v%1 is installed — restart to finish updating")
                        .arg(win->m_updateInstalledVersion), 4000);
            return;
        }
        const auto trigger = userInitiated ? SelfUpdate::CheckTrigger::Manual
                                           : SelfUpdate::CheckTrigger::Startup;
        if (!SelfUpdate::shouldReport(trigger, tag, current,
                                      win->m_config.updateSkippedVersion(),
                                      win->m_config.updateCheckOnStartup())) {
            // Up to date, a newer dev build, or the skipped version on startup.
            win->m_updateAvailableAction->setVisible(false);
            if (userInitiated) {
                win->showStatusMessage(
                    win->tr("Up to date — running v%1 (latest)")
                        .arg(current),
                    4000);
            }
            return;
        }
        const QString url = QStringLiteral(
            "https://github.com/milnet01/ants-terminal/releases/tag/v%1").arg(tag);
        UpdateDialog::Release release;
        release.tag = rawTag;
        release.notes = rel.value("body").toString();
        release.pageUrl = QUrl(url);
        for (const QJsonValue &a : rel.value("assets").toArray()) {
            const QJsonObject o = a.toObject();
            release.assets.insert(o.value("name").toString(),
                                  QUrl(o.value("browser_download_url").toString()));
        }
        win->m_latestRelease = release;
        // Plain QAction text (no rich-text — menu bars render the
        // string verbatim). The leading ↗ keeps the call-to-action
        // glyph the user is used to from the status-bar variant.
        win->m_updateAvailableAction->setText(
            win->tr("↗ Update v%1 available").arg(tag));
        win->m_updateAvailableAction->setToolTip(
            win->tr("Click to update to v%1. Currently running v%2.").arg(tag, current));
        win->m_updateAvailableAction->setData(url);
        win->m_updateAvailableAction->setVisible(true);
    });
}

void MainWindow::handleUpdateClicked(const QString &url) {
    // ANTS-5560 § 2.4 — an update already swapped in: offer the restart again.
    if (m_updateDialog) {
        m_updateDialog->raise();
        m_updateDialog->activateWindow();
        return;
    }
    UpdateDialog *dlg = nullptr;
    if (!m_updateInstalledVersion.isEmpty()) {
        dlg = new UpdateDialog(m_config.theme(), m_updateInstalledVersion, this);
    } else {
        // Only an AppImage updates itself ($APPIMAGE names it); any other
        // install is updated by its package manager, so open the release page.
        const SelfUpdate::InstallKind kind = SelfUpdate::installKind();
        if (kind == SelfUpdate::InstallKind::NotAppImage || m_latestRelease.tag.isEmpty()) {
            QDesktopServices::openUrl(QUrl(url));
            return;
        }
        dlg = new UpdateDialog(m_config.theme(), m_latestRelease, kind, this);
    }
    m_updateDialog = dlg;
    connect(dlg, &UpdateDialog::skipRequested, this, [this](const QString &version) {
        m_config.setUpdateSkippedVersion(version);
        m_updateAvailableAction->setVisible(false);
    });
    connect(dlg, &UpdateDialog::updateInstalled, this, [this](const QString &version) {
        m_updateInstalledVersion = version;
        m_updateAvailableAction->setText(tr("↻ Restart to finish updating to v%1").arg(version));
        m_updateAvailableAction->setToolTip(
            tr("v%1 is installed. It starts the next time Ants Terminal opens, "
               "or click to restart now.").arg(version));
        m_updateAvailableAction->setVisible(true);
    });
    connect(dlg, &UpdateDialog::restartNowRequested, this, &MainWindow::restartForUpdate);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}
