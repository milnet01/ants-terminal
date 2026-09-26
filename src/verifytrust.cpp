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

#include <optional>
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

QString nowIso() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
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
    // The entry must be in the map for saveToDisk() to write it, but it
    // must not outlive a failed save: outcomeForConfig() reads the map,
    // so a trust that never reached disk would still be honoured for the
    // rest of the session.
    const auto prior = m_trustedShas.constFind(shaHex);
    const std::optional<ShaEntry> previous =
        prior == m_trustedShas.cend() ? std::nullopt : std::optional(*prior);
    ShaEntry &e = m_trustedShas[shaHex];
    e.note = note;
    if (e.firstTrusted.isEmpty()) e.firstTrusted = nowIso();
    if (saveToDisk()) return true;
    if (previous) m_trustedShas[shaHex] = *previous;
    else m_trustedShas.remove(shaHex);
    return false;
}

bool FilePersistedTrustClient::addTrustedRepo(
        const QString &canonicalProjectPath,
        const QString &currentShaHex,
        bool untilShaChanges) {
    if (canonicalProjectPath.isEmpty() || currentShaHex.isEmpty()) {
        return false;
    }
    // Same rule as addTrustedSha: roll the map back if the save fails.
    const auto prior = m_trustedRepos.constFind(canonicalProjectPath);
    const std::optional<RepoEntry> previous =
        prior == m_trustedRepos.cend() ? std::nullopt : std::optional(*prior);
    RepoEntry &e = m_trustedRepos[canonicalProjectPath];
    e.shaHex = currentShaHex;
    e.untilShaChanges = untilShaChanges;
    if (e.firstTrusted.isEmpty()) e.firstTrusted = nowIso();
    if (saveToDisk()) return true;
    if (previous) m_trustedRepos[canonicalProjectPath] = *previous;
    else m_trustedRepos.remove(canonicalProjectPath);
    return false;
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
    // ANTS-5082 — ANTS-1337 § 6: a trust file other users can read or
    // write is still honoured, but said so; saveToDisk narrows it to 0600.
    const QFileDevice::Permissions others =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther;
    if (f.permissions() & others) {
        std::fprintf(stderr,
            "verifytrust: %s can be read or written by other users — "
            "honouring it; the next save narrows it to 0600\n",
            qUtf8Printable(m_path));
    }
    const QByteArray bytes = f.readAll();
    f.close();

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // Corrupt file: log to stderr (caller's logger is out of
        // scope here — this is core_lib, no DebugLog access by
        // design), then start fresh. ANTS-5082 — move the file aside
        // first, so the next write does not destroy the only copy of
        // whatever it still holds.
        // Same naming as the config.cpp / themes.cpp rotation (ANTS-1179).
        const QString aside = m_path
            + QStringLiteral(".corrupt.%1").arg(QDateTime::currentSecsSinceEpoch());
        const bool moved = QFile::rename(m_path, aside);
        std::fprintf(stderr,
            "verifytrust: corrupt %s — starting with empty trust set%s%s\n",
            qUtf8Printable(m_path), moved ? "; moved aside to " : "",
            moved ? qUtf8Printable(aside) : "");
        return false;
    }

    const QJsonObject root = doc.object();

    // ANTS-1825 — schema-version gate. saveToDisk stamps `version`
    // but a v1 reader previously ignored it, so a file written by a
    // future Ants (whose `trusted_shas` / `trusted_repos` field
    // semantics may differ) would be consumed as authoritative —
    // potentially auto-trusting entries this build can't validate.
    // A missing `version` defaults to the current schema (every file
    // we've ever written stamps it, so absence means a hand-edit /
    // pre-history file — treat as compatible, don't regress it). A
    // version GREATER than we understand falls closed: load nothing
    // and flag m_futureSchema so saveToDisk won't clobber the newer
    // file on the next write.
    const int fileVersion =
        root.value(QStringLiteral("version")).toInt(kSchemaVersion);
    if (fileVersion > kSchemaVersion) {
        std::fprintf(stderr,
            "verifytrust: %s is schema v%d but this build understands "
            "only v%d — refusing to load (trust falls closed) and "
            "leaving the file untouched.\n",
            qUtf8Printable(m_path), fileVersion, kSchemaVersion);
        m_futureSchema = true;
        return false;
    }

    const QJsonObject shas = root.value(QStringLiteral("trusted_shas"))
                                .toObject();
    for (auto it = shas.constBegin(); it != shas.constEnd(); ++it) {
        const QJsonObject meta = it.value().toObject();
        ShaEntry e;
        e.note = meta.value(QStringLiteral("note")).toString();
        e.firstTrusted = meta.value(QStringLiteral("first_trusted")).toString();
        m_trustedShas.insert(it.key(), e);
    }
    const QJsonObject repos = root.value(QStringLiteral("trusted_repos"))
                                 .toObject();
    for (auto it = repos.constBegin(); it != repos.constEnd(); ++it) {
        const QJsonObject meta = it.value().toObject();
        RepoEntry e;
        e.shaHex = meta.value(QStringLiteral("sha")).toString();
        e.untilShaChanges =
            meta.value(QStringLiteral("until_sha_changes")).toBool(true);
        e.firstTrusted = meta.value(QStringLiteral("first_trusted")).toString();
        m_trustedRepos.insert(it.key(), e);
    }
    return true;
}

bool FilePersistedTrustClient::saveToDisk() const {
    if (m_path.isEmpty()) return false;

    // ANTS-1825 — never downgrade-clobber a future-schema file. If
    // loadFromDisk saw a `version` newer than we understand, the file
    // belongs to a newer Ants; rewriting it as v1 would silently strip
    // whatever v2 added. Refuse the write (the in-RAM add is lost,
    // which is correct: this build can't safely persist into a schema
    // it doesn't understand).
    if (m_futureSchema) return false;

    // Ensure the parent directory exists. AppConfigLocation under
    // XDG is `$XDG_CONFIG_HOME/ants-terminal` on Linux; first call
    // may need the dir created.
    const QString parentDir = QFileInfo(m_path).absolutePath();
    if (!parentDir.isEmpty()) QDir().mkpath(parentDir);

    QJsonObject root;
    root[QStringLiteral("version")] = kSchemaVersion;

    QJsonObject shas;
    for (auto it = m_trustedShas.constBegin();
         it != m_trustedShas.constEnd(); ++it) {
        QJsonObject meta;
        if (!it->firstTrusted.isEmpty()) {
            meta[QStringLiteral("first_trusted")] = it->firstTrusted;
        }
        if (!it->note.isEmpty()) {
            meta[QStringLiteral("note")] = it->note;
        }
        shas[it.key()] = meta;
    }
    root[QStringLiteral("trusted_shas")] = shas;

    QJsonObject repos;
    for (auto it = m_trustedRepos.constBegin();
         it != m_trustedRepos.constEnd(); ++it) {
        QJsonObject meta;
        if (!it->firstTrusted.isEmpty()) {
            meta[QStringLiteral("first_trusted")] = it->firstTrusted;
        }
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
    // ANTS-5082 — a short write, a failed flush or close, or permissions that
    // cannot be narrowed must not reach the rename: on a full disk that
    // replaced the trust store with truncated JSON.
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    const bool written = f.write(bytes) == bytes.size() && f.flush();
    f.close();
    if (!written || f.error() != QFileDevice::NoError || !setOwnerOnlyPerms(tmp)) {
        QFile::remove(tmp);
        return false;
    }

    if (std::rename(tmp.toLocal8Bit().constData(),
                    m_path.toLocal8Bit().constData()) != 0) {
        QFile::remove(tmp);
        return false;
    }
    fsyncParentDir(m_path);
    return true;
}

}  // namespace VerifyTrust
