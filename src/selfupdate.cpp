// ANTS-5560 — see selfupdate.h and docs/specs/ANTS-5560-appimage-self-update.md.

#include "selfupdate.h"

#include "ants_update_pubkey.h"  // generated from packaging/update-signing/

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryFile>

#include <cstdio>
#include <utility>
#include <unistd.h>

#ifdef ANTS_HAVE_LIBSODIUM
#include <sodium.h>
#endif

namespace SelfUpdate {

namespace {

constexpr char kAssetPrefix[] = "Ants_Terminal-";
constexpr char kAssetSuffix[] = "-x86_64.AppImage";

// DER SubjectPublicKeyInfo header for an Ed25519 key; the 32-byte key follows.
constexpr unsigned char kEd25519SpkiPrefix[] = {0x30, 0x2a, 0x30, 0x05, 0x06, 0x03,
                                                0x2b, 0x65, 0x70, 0x03, 0x21, 0x00};

// The 32 raw key bytes, or empty when the PEM is not an Ed25519 public key.
QByteArray rawEd25519Key(const QByteArray &pem) {
    QByteArray b64;
    bool inBody = false;
    for (const QByteArray &line : pem.split('\n')) {
        const QByteArray t = line.trimmed();
        if (t == "-----BEGIN PUBLIC KEY-----") { inBody = true; continue; }
        if (t == "-----END PUBLIC KEY-----") break;
        if (inBody) b64 += t;
    }
    const auto decoded = QByteArray::fromBase64Encoding(b64, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) return {};
    const QByteArray &der = *decoded;
    const auto prefix = QByteArray::fromRawData(reinterpret_cast<const char *>(kEd25519SpkiPrefix),
                                                sizeof kEd25519SpkiPrefix);
    if (der.size() != prefix.size() + 32 || !der.startsWith(prefix)) return {};
    return der.mid(prefix.size());
}

bool signatureValid(const QByteArray &message, const QByteArray &signature, const QByteArray &key) {
#ifdef ANTS_HAVE_LIBSODIUM
    if (sodium_init() < 0 || signature.size() != crypto_sign_BYTES
        || key.size() != crypto_sign_PUBLICKEYBYTES)
        return false;
    return crypto_sign_verify_detached(
               reinterpret_cast<const unsigned char *>(signature.constData()),
               reinterpret_cast<const unsigned char *>(message.constData()),
               static_cast<unsigned long long>(message.size()),
               reinterpret_cast<const unsigned char *>(key.constData())) == 0;
#else
    Q_UNUSED(message) Q_UNUSED(signature) Q_UNUSED(key)
    return false;
#endif
}

bool parseManifest(const QByteArray &bytes, Manifest &m) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject o = doc.object();
    static const QRegularExpression hex64(QStringLiteral("^[0-9a-f]{64}$"));
    const QJsonValue size = o.value(QStringLiteral("size"));
    const QString sha = o.value(QStringLiteral("sha256")).toString();
    m.format = o.value(QStringLiteral("format")).toInt(0);
    m.version = o.value(QStringLiteral("version")).toString();
    m.asset = o.value(QStringLiteral("asset")).toString();
    m.size = size.isDouble() ? static_cast<qint64>(size.toDouble(-1)) : -1;
    m.sha256Hex = sha.toLatin1();
    return m.format == 1 && !m.version.isEmpty() && !m.asset.isEmpty() && m.size >= 0
        && hex64.match(sha).hasMatch();
}

// True when `value` names `dir` itself or a path under it.
bool namesDir(const QString &value, const QString &dir) {
    qsizetype at = value.indexOf(dir);
    while (at >= 0) {
        const qsizetype end = at + dir.size();
        if (end == value.size() || value[end] == QLatin1Char('/') || value[end] == QLatin1Char(':'))
            return true;
        at = value.indexOf(dir, at + 1);
    }
    return false;
}

}  // namespace

int compareSemver(const QString &a, const QString &b) {
    const QStringList ap = a.split('.');
    const QStringList bp = b.split('.');
    const qsizetype n = std::max(ap.size(), bp.size());
    for (qsizetype i = 0; i < n; ++i) {
        const QString as = i < ap.size() ? ap[i] : QStringLiteral("0");
        const QString bs = i < bp.size() ? bp[i] : QStringLiteral("0");
        bool aok = false, bok = false;
        const int ai = as.toInt(&aok);
        const int bi = bs.toInt(&bok);
        if (aok && bok) {
            if (ai != bi) return ai > bi ? 1 : -1;
        } else {
            const int c = QString::compare(as, bs);
            if (c != 0) return c > 0 ? 1 : -1;
        }
    }
    return 0;
}

InstallKind installKind(const QProcessEnvironment &env) {
#ifndef ANTS_HAVE_LIBSODIUM
    Q_UNUSED(env)
    return InstallKind::NotAppImage;
#else
    const QString path = env.value(QStringLiteral("APPIMAGE"));
    const QFileInfo fi(path);
    if (path.isEmpty() || !fi.isFile()) return InstallKind::NotAppImage;
    const QByteArray dir = QFile::encodeName(fi.absolutePath());
    return ::access(dir.constData(), W_OK) == 0 ? InstallKind::Updatable
                                                : InstallKind::ReadOnlyDir;
#endif
}

VerifyResult verifyManifest(const QByteArray &manifestBytes, const QByteArray &signature,
                            const QByteArray &publicKeyPem, const QString &releaseTag,
                            const QString &runningVersion, const QStringList &releaseAssets) {
    VerifyResult r;
    auto fail = [&r](VerifyError e, const QString &why) {
        r.error = e;
        r.message = why;
        return r;
    };
    const QByteArray key = rawEd25519Key(publicKeyPem);
    if (key.isEmpty()) return fail(VerifyError::BadKey, QStringLiteral("the public key is not Ed25519"));
    if (!signatureValid(manifestBytes, signature, key))
        return fail(VerifyError::BadSignature, QStringLiteral("the signature does not verify"));
    if (!parseManifest(manifestBytes, r.manifest))
        return fail(VerifyError::BadManifest, QStringLiteral("the manifest is malformed"));
    const QString tag = releaseTag.startsWith(QLatin1Char('v')) ? releaseTag.mid(1) : releaseTag;
    if (r.manifest.version != tag)
        return fail(VerifyError::VersionNotTag,
                    QStringLiteral("manifest version %1 is not the release's %2").arg(r.manifest.version, tag));
    if (compareSemver(r.manifest.version, runningVersion) <= 0)
        return fail(VerifyError::VersionNotNewer,
                    QStringLiteral("%1 is not newer than %2").arg(r.manifest.version, runningVersion));
    const QString expected = QLatin1String(kAssetPrefix) + r.manifest.version + QLatin1String(kAssetSuffix);
    if (r.manifest.asset != expected)
        return fail(VerifyError::AssetName,
                    QStringLiteral("asset %1 is not %2").arg(r.manifest.asset, expected));
    if (!releaseAssets.contains(r.manifest.asset))
        return fail(VerifyError::AssetMissing,
                    QStringLiteral("the release does not carry %1").arg(r.manifest.asset));
    r.error = VerifyError::None;
    return r;
}

QByteArray embeddedPublicKeyPem() {
    return QByteArray(kAntsUpdatePublicKeyPem);
}

bool isAllowedUrl(const QUrl &url) {
    return url.isValid() && !url.host().isEmpty()
        && url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0;
}

QNetworkRequest makeRequest(const QUrl &url) {
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "Ants-Terminal-Updater");
    return req;
}

Relaunch relaunchCommand(qint64 pid, const QString &appImagePath,
                         const QProcessEnvironment &env) {
    Relaunch r;
    r.program = QStringLiteral("/bin/sh");
    r.arguments = {QStringLiteral("-c"),
                   QStringLiteral("while kill -0 \"$1\" 2>/dev/null; do sleep 0.1; done; "
                                  "exec \"$2\""),
                   QStringLiteral("sh"), QString::number(pid), appImagePath};

    const QString appdir = env.value(QStringLiteral("APPDIR"));
    QProcessEnvironment out = env;
    for (const char *k : {"APPDIR", "APPIMAGE", "ARGV0", "OWD"})
        out.remove(QString::fromLatin1(k));
    if (appdir.isEmpty()) {
        r.environment = std::move(out);
        return r;
    }
    static const QStringList pathLists = {
        QStringLiteral("PATH"), QStringLiteral("LD_LIBRARY_PATH"), QStringLiteral("QT_PLUGIN_PATH"),
        QStringLiteral("XDG_DATA_DIRS"), QStringLiteral("XDG_CONFIG_DIRS")};
    for (const QString &k : out.keys()) {
        const QString v = out.value(k);
        if (!namesDir(v, appdir)) continue;
        if (!pathLists.contains(k)) {
            out.remove(k);
            continue;
        }
        QStringList kept;
        for (const QString &entry : v.split(QLatin1Char(':')))
            if (!namesDir(entry, appdir)) kept << entry;
        if (kept.isEmpty()) out.remove(k);
        else out.insert(k, kept.join(QLatin1Char(':')));
    }
    r.environment = std::move(out);
    return r;
}

bool shouldReport(CheckTrigger trigger, const QString &latestVersion,
                  const QString &runningVersion, const QString &skippedVersion,
                  bool checkOnStartup) {
    if (compareSemver(latestVersion, runningVersion) <= 0) return false;
    if (trigger == CheckTrigger::Manual) return true;
    return checkOnStartup && skippedVersion != latestVersion;
}

// ---- Session ---------------------------------------------------------------

Session::Session(QByteArray publicKeyPem, QString targetPath)
    : m_publicKeyPem(std::move(publicKeyPem)),
      m_targetPath(std::move(targetPath)) {}

Session::~Session() { abort(); }

VerifyResult Session::acceptManifest(const QByteArray &manifestBytes, const QByteArray &signature,
                                     const QString &releaseTag, const QString &runningVersion,
                                     const QStringList &releaseAssets) {
    const VerifyResult r = verifyManifest(manifestBytes, signature, m_publicKeyPem, releaseTag,
                                          runningVersion, releaseAssets);
    m_accepted = r.ok();
    if (m_accepted) m_manifest = r.manifest;
    else m_error = r.message;
    return r;
}

bool Session::begin() {
    if (!m_accepted) {
        m_error = QStringLiteral("no verified manifest");
        return false;
    }
    const QFileInfo target(m_targetPath);
    // Same directory as the target, so the final rename cannot cross filesystems.
    m_file = std::make_unique<QTemporaryFile>(
        target.absolutePath() + QLatin1String("/.") + target.fileName()
        + QLatin1String(".update-XXXXXX"));
    if (!m_file->open()) {
        m_error = QStringLiteral("cannot create a temporary file beside %1: %2")
                      .arg(m_targetPath, m_file->errorString());
        m_file.reset();
        return false;
    }
    m_tempPath = m_file->fileName();
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    m_received = 0;
    return true;
}

bool Session::write(const QByteArray &chunk) {
    if (!m_file) return false;
    m_received += chunk.size();
    if (m_received > m_manifest.size) {
        m_error = QStringLiteral("download is larger than the manifest's %1 bytes").arg(m_manifest.size);
        abort();
        return false;
    }
    if (m_file->write(chunk) != chunk.size()) {
        m_error = QStringLiteral("write failed: %1").arg(m_file->errorString());
        abort();
        return false;
    }
    m_hash->addData(chunk);
    return true;
}

bool Session::finish() {
    if (!m_file) {
        if (m_error.isEmpty()) m_error = QStringLiteral("no download in progress");
        return false;
    }
    if (m_received != m_manifest.size) {
        m_error = QStringLiteral("received %1 bytes, the manifest says %2")
                      .arg(m_received).arg(m_manifest.size);
        abort();
        return false;
    }
    if (m_hash->result().toHex() != m_manifest.sha256Hex) {
        m_error = QStringLiteral("the download's SHA-256 does not match the manifest");
        abort();
        return false;
    }
    if (!m_file->flush()) {
        m_error = QStringLiteral("flush failed: %1").arg(m_file->errorString());
        abort();
        return false;
    }
    m_file->close();
    const auto exec = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                    | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                    | QFileDevice::ReadOther | QFileDevice::ExeOther;
    if (!QFile::setPermissions(m_tempPath, exec)
        || std::rename(QFile::encodeName(m_tempPath).constData(),
                       QFile::encodeName(m_targetPath).constData()) != 0) {
        m_error = QStringLiteral("cannot replace %1").arg(m_targetPath);
        abort();
        return false;
    }
    m_file->setAutoRemove(false);  // the name now belongs to the target
    m_file.reset();
    return true;
}

void Session::abort() {
    if (!m_file) return;
    m_file->close();
    m_file->remove();
    m_file.reset();
}

QString Session::temporaryPath() const { return m_tempPath; }
QString Session::error() const { return m_error; }

}  // namespace SelfUpdate
