#pragma once

// ANTS-5560 — the AppImage updates itself
// (docs/specs/ANTS-5560-appimage-self-update.md). The pure parts live here
// so tests call them directly: semver compare, the install-kind check,
// signed-manifest verification, the HTTPS rule, the relaunch command and
// the check's show/skip decision. Session is the download and the swap,
// fed bytes by the caller so a test needs no network.

#include <QByteArray>
#include <QFile>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <cstdint>
#include <memory>

class QCryptographicHash;

namespace SelfUpdate {

// 1 if a > b, -1 if a < b, 0 if equal ("X.Y.Z"; a missing part is 0).
int compareSemver(const QString &a, const QString &b);

// § 2.1. Reads APPIMAGE from `env`.
enum class InstallKind : std::uint8_t { NotAppImage, ReadOnlyDir, Updatable };
InstallKind installKind(
    const QProcessEnvironment &env = QProcessEnvironment::systemEnvironment());

// § 2.2 — the signed manifest's fields.
struct Manifest {
    int        format = 0;
    QString    version;
    QString    asset;
    qint64     size = -1;
    QByteArray sha256Hex;  // 64 lowercase hex characters
};

// § 2.4 step 4's first three checks, each with its own reason.
enum class VerifyError : std::uint8_t {
    None,
    BadKey,           // the public key is not an Ed25519 SubjectPublicKeyInfo
    BadSignature,     // the signature does not verify over the manifest bytes
    BadManifest,      // not JSON of format 1 with every field
    VersionNotTag,    // manifest.version != release tag without its "v"
    VersionNotNewer,  // manifest.version is not newer than the running one
    AssetName,        // asset != Ants_Terminal-<version>-x86_64.AppImage
    AssetMissing,     // asset is not in the release's asset list
};

struct VerifyResult {
    VerifyError error = VerifyError::BadSignature;
    Manifest    manifest;
    QString     message;
    bool ok() const { return error == VerifyError::None; }
};

// `publicKeyPem` is the PEM text of an Ed25519 public key.
VerifyResult verifyManifest(const QByteArray &manifestBytes,
                            const QByteArray &signature,
                            const QByteArray &publicKeyPem,
                            const QString &releaseTag,
                            const QString &runningVersion,
                            const QStringList &releaseAssets);

// The committed packaging/update-signing/ants-update.pub.pem, as built in.
QByteArray embeddedPublicKeyPem();

// § 2.4 step 3 — only https is fetched, and redirects never downgrade.
bool isAllowedUrl(const QUrl &url);
QNetworkRequest makeRequest(const QUrl &url);

// § 2.4 "The relaunch".
struct Relaunch {
    QString             program;
    QStringList         arguments;
    QProcessEnvironment environment;
};
Relaunch relaunchCommand(qint64 pid, const QString &appImagePath,
                         const QProcessEnvironment &env);

// § 2.4 step 1 — does this check result show the indicator?
enum class CheckTrigger : std::uint8_t { Startup, Manual };
bool shouldReport(CheckTrigger trigger, const QString &latestVersion,
                  const QString &runningVersion, const QString &skippedVersion,
                  bool checkOnStartup);

// § 2.4 steps 3–5 for one update into `targetPath` ($APPIMAGE).
// acceptManifest() → begin() → write()… → finish(); any failure removes
// the temporary file and leaves the target untouched.
class Session {
public:
    Session(QByteArray publicKeyPem, QString targetPath);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    VerifyResult acceptManifest(const QByteArray &manifestBytes,
                                const QByteArray &signature,
                                const QString &releaseTag,
                                const QString &runningVersion,
                                const QStringList &releaseAssets);
    bool begin();                           // needs an accepted manifest
    bool write(const QByteArray &chunk);
    bool finish();                          // size + sha256, 0755, rename
    void abort();

    QString temporaryPath() const;          // where begin() wrote
    QString error() const;

private:
    QByteArray m_publicKeyPem;
    QString    m_targetPath;
    Manifest   m_manifest;
    bool       m_accepted = false;
    QString    m_tempPath;
    QString    m_error;
    std::unique_ptr<QFile> m_file;
    std::unique_ptr<QCryptographicHash> m_hash;
    qint64     m_received = 0;
};

}  // namespace SelfUpdate
