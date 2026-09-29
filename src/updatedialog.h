#pragma once

// ANTS-5560 — the update dialog (docs/specs/ANTS-5560-appimage-self-update.md
// § 2.4 steps 2–6). Non-modal, like every dialog here (dialogs.md). It offers
// the update, downloads and verifies it through SelfUpdate::Session, swaps it
// in, then offers Restart now / Restart later. MainWindow owns the check, the
// indicator and the restart itself; this dialog only reports what happened.

#include "selfupdate.h"

#include <QDialog>
#include <QMap>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <memory>

class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QProgressBar;
class QPushButton;
class QStackedWidget;

class UpdateDialog : public QDialog {
    Q_OBJECT
public:
    // What the release check found: the tag, its notes, and each asset's
    // download URL by file name.
    struct Release {
        QString tag;
        QString notes;
        QUrl pageUrl;
        QMap<QString, QUrl> assets;
        QString version() const;
    };

    // Offers `release` (or, for ReadOnlyDir, explains why it cannot).
    UpdateDialog(const QString &themeName, const Release &release,
                 SelfUpdate::InstallKind kind, QWidget *parent = nullptr);
    // Opens straight on the restart choice, for an update already installed.
    UpdateDialog(const QString &themeName, const QString &installedVersion,
                 QWidget *parent = nullptr);
    ~UpdateDialog() override;

signals:
    void skipRequested(const QString &version);
    void updateInstalled(const QString &version);
    void restartNowRequested();

private:
    void buildChrome(const QString &themeName);
    void buildOfferPage(SelfUpdate::InstallKind kind);
    void buildProgressPage();
    void buildRestartPage();
    void showRestartPage(const QString &version);
    void startUpdate();
    void fetchSmall(const QString &name, QByteArray *into, int *pending);
    void onSmallFetched();
    void fetchAppImage();
    void fail(const QString &why);

    Release m_release;
    QStackedWidget *m_pages = nullptr;
    QWidget *m_offerPage = nullptr;
    QWidget *m_progressPage = nullptr;
    QWidget *m_restartPage = nullptr;
    QLabel *m_progressLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_restartLabel = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_download;
    std::unique_ptr<SelfUpdate::Session> m_session;
    QByteArray m_manifest;
    QByteArray m_signature;
    int m_smallPending = 0;
    bool m_failed = false;
};
