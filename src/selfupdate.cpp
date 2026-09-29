// ANTS-5560 — see selfupdate.h. INTERFACE STUB: every function refuses or
// returns nothing, so the conformance tests compile and fail on their
// assertions. The implementation replaces this file.

#include "selfupdate.h"

#include <QCryptographicHash>

namespace SelfUpdate {

int compareSemver(const QString &, const QString &) { return 0; }

InstallKind installKind(const QProcessEnvironment &) {
    return InstallKind::NotAppImage;
}

VerifyResult verifyManifest(const QByteArray &, const QByteArray &,
                            const QByteArray &, const QString &,
                            const QString &, const QStringList &) {
    return {};
}

QByteArray embeddedPublicKeyPem() { return {}; }

bool isAllowedUrl(const QUrl &) { return false; }

QNetworkRequest makeRequest(const QUrl &url) { return QNetworkRequest(url); }

Relaunch relaunchCommand(qint64, const QString &, const QProcessEnvironment &) {
    return {};
}

bool shouldReport(CheckTrigger, const QString &, const QString &,
                  const QString &, bool) {
    return false;
}

Session::Session(QByteArray publicKeyPem, QString targetPath)
    : m_publicKeyPem(std::move(publicKeyPem)),
      m_targetPath(std::move(targetPath)) {}

Session::~Session() = default;

VerifyResult Session::acceptManifest(const QByteArray &, const QByteArray &,
                                     const QString &, const QString &,
                                     const QStringList &) {
    return {};
}

bool Session::begin() { return false; }
bool Session::write(const QByteArray &) { return false; }
bool Session::finish() { return false; }
void Session::abort() {}
QString Session::temporaryPath() const { return m_tempPath; }
QString Session::error() const { return m_error; }

}  // namespace SelfUpdate
