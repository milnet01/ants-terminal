// ANTS-5560 — see updatedialog.h. Every network fetch goes through
// SelfUpdate::isAllowedUrl and SelfUpdate::makeRequest (https only, redirects
// never downgrade). The AppImage is streamed into SelfUpdate::Session, so
// memory holds one network buffer, never the file (spec § 4).

#include "updatedialog.h"

#include "dialogchrome.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

QLabel *wrappedLabel(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    l->setWordWrap(true);
    return l;
}

QString runningVersion() { return QString::fromLatin1(ANTS_VERSION); }

}  // namespace

QString UpdateDialog::Release::version() const {
    return tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag;
}

UpdateDialog::UpdateDialog(const QString &themeName, const Release &release,
                           SelfUpdate::InstallKind kind, QWidget *parent)
    : QDialog(parent), m_release(release) {
    buildChrome(themeName);
    buildOfferPage(kind);
    buildProgressPage();
    buildRestartPage();
    m_pages->setCurrentWidget(m_offerPage);
}

UpdateDialog::UpdateDialog(const QString &themeName, const QString &installedVersion,
                           QWidget *parent)
    : QDialog(parent) {
    buildChrome(themeName);
    buildRestartPage();
    showRestartPage(installedVersion);
}

UpdateDialog::~UpdateDialog() {
    if (QNetworkReply *reply = m_download.data()) reply->abort();
    // m_session's destructor removes any half-written temporary file.
}

void UpdateDialog::buildChrome(const QString &themeName) {
    setObjectName(QStringLiteral("UpdateDialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Update Ants Terminal"));
    auto chrome = DialogChrome::install(this, themeName, /*resizable=*/true,
                                        QStringLiteral("UpdateDialog"));
    auto *outer = new QVBoxLayout(chrome.contentArea);
    m_pages = new QStackedWidget(chrome.contentArea);
    outer->addWidget(m_pages);
    setMinimumSize(460, 320);
    resize(560, 440);
}

void UpdateDialog::buildOfferPage(SelfUpdate::InstallKind kind) {
    m_offerPage = new QWidget(m_pages);
    auto *v = new QVBoxLayout(m_offerPage);
    const QString version = m_release.version();
    auto *headline = wrappedLabel(tr("<b>Version %1 is available</b> (you have %2).")
                                      .arg(version, runningVersion()), m_offerPage);
    headline->setTextFormat(Qt::RichText);
    v->addWidget(headline);

    auto *notes = new QPlainTextEdit(m_release.notes, m_offerPage);
    notes->setReadOnly(true);
    notes->setObjectName(QStringLiteral("updateNotes"));
    v->addWidget(notes, 1);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    if (kind == SelfUpdate::InstallKind::ReadOnlyDir) {
        const QString dir = QFileInfo(qEnvironmentVariable("APPIMAGE")).absolutePath();
        v->insertWidget(1, wrappedLabel(
            tr("Ants Terminal cannot update itself here, because it cannot write "
               "to %1. Download the new version from the release page instead.")
                .arg(dir), m_offerPage));
        auto *page = new QPushButton(tr("Open release page"), m_offerPage);
        auto *closeBtn = new QPushButton(tr("Close"), m_offerPage);
        connect(page, &QPushButton::clicked, this, [this] {
            QDesktopServices::openUrl(m_release.pageUrl);
            close();
        });
        connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
        buttons->addWidget(page);
        buttons->addWidget(closeBtn);
    } else {
        auto *skip = new QPushButton(tr("Skip this version"), m_offerPage);
        skip->setObjectName(QStringLiteral("updateSkipButton"));
        auto *later = new QPushButton(tr("Later"), m_offerPage);
        later->setObjectName(QStringLiteral("updateLaterButton"));
        auto *now = new QPushButton(tr("Update now"), m_offerPage);
        now->setObjectName(QStringLiteral("updateNowButton"));
        now->setDefault(true);
        connect(skip, &QPushButton::clicked, this, [this, version] {
            emit skipRequested(version);
            close();
        });
        connect(later, &QPushButton::clicked, this, &QDialog::close);
        connect(now, &QPushButton::clicked, this, &UpdateDialog::startUpdate);
        buttons->addWidget(skip);
        buttons->addWidget(later);
        buttons->addWidget(now);
    }
    v->addLayout(buttons);
    m_pages->addWidget(m_offerPage);
}

void UpdateDialog::buildProgressPage() {
    m_progressPage = new QWidget(m_pages);
    auto *v = new QVBoxLayout(m_progressPage);
    m_progressLabel = wrappedLabel(tr("Checking the release signature…"), m_progressPage);
    m_progress = new QProgressBar(m_progressPage);
    m_progress->setRange(0, 0);
    v->addWidget(m_progressLabel);
    v->addWidget(m_progress);
    v->addStretch();
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *cancel = new QPushButton(tr("Cancel"), m_progressPage);
    connect(cancel, &QPushButton::clicked, this, &QDialog::close);
    buttons->addWidget(cancel);
    v->addLayout(buttons);
    m_pages->addWidget(m_progressPage);
}

void UpdateDialog::buildRestartPage() {
    m_restartPage = new QWidget(m_pages);
    auto *v = new QVBoxLayout(m_restartPage);
    m_restartLabel = wrappedLabel(QString(), m_restartPage);
    m_restartLabel->setTextFormat(Qt::RichText);
    v->addWidget(m_restartLabel);
    v->addStretch();
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *later = new QPushButton(tr("Restart later"), m_restartPage);
    later->setObjectName(QStringLiteral("updateRestartLaterButton"));
    auto *now = new QPushButton(tr("Restart now"), m_restartPage);
    now->setObjectName(QStringLiteral("updateRestartNowButton"));
    connect(later, &QPushButton::clicked, this, &QDialog::close);
    connect(now, &QPushButton::clicked, this, [this] {
        close();
        emit restartNowRequested();
    });
    buttons->addWidget(later);
    buttons->addWidget(now);
    v->addLayout(buttons);
    m_pages->addWidget(m_restartPage);
}

void UpdateDialog::showRestartPage(const QString &version) {
    m_restartLabel->setText(
        tr("<b>Ants Terminal %1 is installed.</b><br><br>"
           "<b>Restart now</b> closes every tab, and every Claude Code session "
           "running in them. Your tabs are saved and come back after the restart.<br><br>"
           "<b>Restart later</b> keeps you working; the new version starts the next "
           "time you open Ants Terminal.").arg(version.toHtmlEscaped()));
    m_pages->setCurrentWidget(m_restartPage);
}

void UpdateDialog::startUpdate() {
    m_pages->setCurrentWidget(m_progressPage);
    m_nam = new QNetworkAccessManager(this);
    const QString target = qEnvironmentVariable("APPIMAGE");
    m_session = std::make_unique<SelfUpdate::Session>(SelfUpdate::embeddedPublicKeyPem(), target);
    const QString base = QStringLiteral("Ants_Terminal-%1-x86_64.AppImage.manifest")
                             .arg(m_release.version());
    m_smallPending = 2;
    fetchSmall(base, &m_manifest, &m_smallPending);
    fetchSmall(base + QStringLiteral(".sig"), &m_signature, &m_smallPending);
}

void UpdateDialog::fetchSmall(const QString &name, QByteArray *into, int *pending) {
    const QUrl url = m_release.assets.value(name);
    if (!SelfUpdate::isAllowedUrl(url)) {
        fail(tr("This release carries no signed update (%1). Download it from the "
                "release page instead.").arg(name));
        return;
    }
    QNetworkReply *reply = m_nam->get(SelfUpdate::makeRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, into, pending] {
        reply->deleteLater();
        if (m_failed) return;
        if (reply->error() != QNetworkReply::NoError) {
            fail(tr("Download failed: %1").arg(reply->errorString()));
            return;
        }
        *into = reply->readAll();
        if (--*pending == 0) onSmallFetched();
    });
}

void UpdateDialog::onSmallFetched() {
    const SelfUpdate::VerifyResult r = m_session->acceptManifest(
        m_manifest, m_signature, m_release.tag, runningVersion(), m_release.assets.keys());
    if (!r.ok()) {
        fail(tr("The update was refused: %1.").arg(r.message));
        return;
    }
    fetchAppImage();
}

void UpdateDialog::fetchAppImage() {
    const QString asset = QStringLiteral("Ants_Terminal-%1-x86_64.AppImage").arg(m_release.version());
    const QUrl url = m_release.assets.value(asset);
    if (!SelfUpdate::isAllowedUrl(url) || !m_session->begin()) {
        fail(tr("The download could not start: %1").arg(m_session->error()));
        return;
    }
    m_progressLabel->setText(tr("Downloading version %1…").arg(m_release.version()));
    m_download = m_nam->get(SelfUpdate::makeRequest(url));
    connect(m_download, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        if (total > 0) {
            m_progress->setRange(0, 1000);
            m_progress->setValue(static_cast<int>(got * 1000 / total));
        }
    });
    connect(m_download, &QNetworkReply::readyRead, this, [this] {
        if (!m_session->write(m_download->readAll())) {
            m_download->abort();
            fail(tr("The download was refused: %1").arg(m_session->error()));
        }
    });
    connect(m_download, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_download;
        reply->deleteLater();
        if (m_failed) return;
        if (reply->error() != QNetworkReply::NoError) {
            m_session->abort();
            fail(tr("Download failed: %1").arg(reply->errorString()));
            return;
        }
        if (!m_session->write(reply->readAll()) || !m_session->finish()) {
            fail(tr("The download was refused: %1").arg(m_session->error()));
            return;
        }
        const QString version = m_release.version();
        emit updateInstalled(version);
        showRestartPage(version);
    });
}

void UpdateDialog::fail(const QString &why) {
    if (m_failed) return;
    m_failed = true;
    if (m_session) m_session->abort();
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progressLabel->setText(why + QLatin1Char(' ')
                             + tr("Nothing was changed; your current version still runs."));
}
