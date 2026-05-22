// VerifyTrust implementation — content-trust gate for
// `.ants/verify.json`. See verifytrust.h + docs/specs/ANTS-1337.md.

#include "verifytrust.h"
#include "secureio.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <cstdio>

namespace VerifyTrust {

namespace {

constexpr int kSchemaVersion = 1;

QString defaultTrustFilePath() {
    const QString cfgRoot = QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation);
    if (cfgRoot.isEmpty()) {
        // ANTS-1741 — refuse a predictable /tmp fallback. A
        // world-writable /tmp/ants-verify-trust-<uid>.json can be
        // pre-seeded (or symlinked) by a co-tenant to auto-trust an
        // attacker's `.ants/verify.json` SHA. Return empty instead:
        // loadFromDisk treats an unopenable path as an empty trust set
        // and saveToDisk no-ops on an empty path, so trust simply
        // doesn't persist when there's no config home (rare). A caller
        // that needs persistence can pass an explicit path to the ctor.
        return QString();
    }
    return cfgRoot + QStringLiteral("/verify-trust.json");
}

}  // namespace

FilePersistedTrustClient::FilePersistedTrustClient()
    : m_path(defaultTrustFilePath()) {
    loadFromDisk();
}

FilePersistedTrustClient::FilePersistedTrustClient(
        const QString &trustFilePath)
    : m_path(trustFilePath.isEmpty() ? defaultTrustFilePath()
                                     : trustFilePath) {
    loadFromDisk();
}

QString FilePersistedTrustClient::sha256Hex(const QByteArray &bytes) {
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    hasher.addData(bytes);
    return QString::fromLatin1(hasher.result().toHex());
}

Decision FilePersistedTrustClient::outcomeForConfig(
        const QString &projectPath,
        const QByteArray &configBytes) {
    const QString shaHex = sha256Hex(configBytes);

    // SHA match wins immediately.
    if (m_trustedShas.contains(shaHex)) {
        return {Outcome::Trusted, shaHex};
    }

    // Repo match — honour the SHA-pin contract.
    const auto it = m_trustedRepos.find(projectPath);
    if (it != m_trustedRepos.end()) {
        if (!it->untilShaChanges) {
            // Trust this repo forever, regardless of edits.
            return {Outcome::Trusted, shaHex};
        }
        if (it->shaHex == shaHex) {
            return {Outcome::Trusted, shaHex};
        }
        // Repo trusted but SHA changed — fall through to prompt;
        // user re-confirms the new SHA.
    }

    // Session-cache short-circuit: if the user already declined this
    // SHA earlier in the session, don't re-prompt. (Prevents a
    // misbehaving Claude that retries on every turn from spamming
    // the modal.) The cache is in-RAM only and clears on Ants
    // restart.
    if (m_sessionDenied.contains(shaHex)) {
        return {Outcome::UntrustedFellBack, shaHex};
    }

    // Hand off to the prompt subclass.
    const Decision d = prompt(projectPath, shaHex, configBytes);
    if (d.outcome == Outcome::UntrustedFellBack) {
        m_sessionDenied.insert(shaHex, true);
    }
    return d;
}

Decision FilePersistedTrustClient::prompt(const QString &projectPath,
                                          const QString &shaHex,
                                          const QByteArray &configBytes) {
    Q_UNUSED(projectPath);
    Q_UNUSED(configBytes);
    // Base class has no GUI — Headless outcome; the engine falls
    // back to auto-detect. Subclasses (chrome-layer modal client)
    // override this to show the actual prompt.
    return {Outcome::Headless, shaHex};
}

bool FilePersistedTrustClient::addTrustedSha(const QString &shaHex,
                                              const QString &note) {
    if (shaHex.isEmpty()) return false;
    m_trustedShas.insert(shaHex, note);
    return saveToDisk();
}

bool FilePersistedTrustClient::addTrustedRepo(
        const QString &canonicalProjectPath,
        const QString &currentShaHex,
        bool untilShaChanges) {
    if (canonicalProjectPath.isEmpty() || currentShaHex.isEmpty()) {
        return false;
    }
    RepoEntry e;
    e.shaHex = currentShaHex;
    e.untilShaChanges = untilShaChanges;
    m_trustedRepos.insert(canonicalProjectPath, e);
    return saveToDisk();
}

void FilePersistedTrustClient::clearSessionCache() {
    m_sessionDenied.clear();
}

bool FilePersistedTrustClient::loadFromDisk() {
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly)) {
        // Absent or unreadable — start with an empty trust set.
        // Next addTrusted* call will create the file.
        return true;
    }
    const QByteArray bytes = f.readAll();
    f.close();

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // Corrupt file: log to stderr (caller's logger is out of
        // scope here — this is core_lib, no DebugLog access by
        // design), then start fresh. Next write replaces the
        // corrupt file.
        std::fprintf(stderr,
            "verifytrust: corrupt %s — starting with empty trust set\n",
            qUtf8Printable(m_path));
        return false;
    }

    const QJsonObject root = doc.object();
    const QJsonObject shas = root.value(QStringLiteral("trusted_shas"))
                                .toObject();
    for (auto it = shas.constBegin(); it != shas.constEnd(); ++it) {
        const QJsonObject meta = it.value().toObject();
        m_trustedShas.insert(it.key(),
            meta.value(QStringLiteral("note")).toString());
    }
    const QJsonObject repos = root.value(QStringLiteral("trusted_repos"))
                                 .toObject();
    for (auto it = repos.constBegin(); it != repos.constEnd(); ++it) {
        const QJsonObject meta = it.value().toObject();
        RepoEntry e;
        e.shaHex = meta.value(QStringLiteral("sha")).toString();
        e.untilShaChanges =
            meta.value(QStringLiteral("until_sha_changes")).toBool(true);
        m_trustedRepos.insert(it.key(), e);
    }
    return true;
}

bool FilePersistedTrustClient::saveToDisk() const {
    if (m_path.isEmpty()) return false;

    // Ensure the parent directory exists. AppConfigLocation under
    // XDG is `$XDG_CONFIG_HOME/ants-terminal` on Linux; first call
    // may need the dir created.
    const QString parentDir = QFileInfo(m_path).absolutePath();
    if (!parentDir.isEmpty()) QDir().mkpath(parentDir);

    QJsonObject root;
    root[QStringLiteral("version")] = kSchemaVersion;

    QJsonObject shas;
    const QString now =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    for (auto it = m_trustedShas.constBegin();
         it != m_trustedShas.constEnd(); ++it) {
        QJsonObject meta;
        meta[QStringLiteral("first_trusted")] = now;
        if (!it.value().isEmpty()) {
            meta[QStringLiteral("note")] = it.value();
        }
        shas[it.key()] = meta;
    }
    root[QStringLiteral("trusted_shas")] = shas;

    QJsonObject repos;
    for (auto it = m_trustedRepos.constBegin();
         it != m_trustedRepos.constEnd(); ++it) {
        QJsonObject meta;
        meta[QStringLiteral("first_trusted")] = now;
        meta[QStringLiteral("sha")] = it->shaHex;
        meta[QStringLiteral("until_sha_changes")] = it->untilShaChanges;
        repos[it.key()] = meta;
    }
    root[QStringLiteral("trusted_repos")] = repos;

    // Atomic write: write to `<path>.tmp`, set 0600, rename, fsync
    // the parent dir for crash-safety (ext4 journal can drop the
    // rename otherwise — ANTS-1141 pattern).
    const QString tmp = m_path + QStringLiteral(".tmp");
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();

    setOwnerOnlyPerms(tmp);

    if (std::rename(tmp.toLocal8Bit().constData(),
                    m_path.toLocal8Bit().constData()) != 0) {
        QFile::remove(tmp);
        return false;
    }
    fsyncParentDir(m_path);
    return true;
}

}  // namespace VerifyTrust
