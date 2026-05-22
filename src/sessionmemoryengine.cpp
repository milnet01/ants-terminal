// ANTS-1283 — implementation. See header and docs/specs/ANTS-1283.md.

#include "sessionmemoryengine.h"

#include "secureio.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace SessionMemoryEngine {

namespace {

constexpr int kHashHexLen = 16;   // INV-4 — 16 hex chars = 64 bits

QString envBaseDir() {
    const QByteArray e = qgetenv("ANTS_SESSION_MEMORY_DIR");
    return QString::fromUtf8(e);
}

}  // namespace

QString cwdHash(const QString &absoluteCwd) {
    const QByteArray sum = QCryptographicHash::hash(
        absoluteCwd.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromUtf8(sum.toHex().left(kHashHexLen));
}

QString defaultBaseDir() {
    const QString cacheRoot = QStandardPaths::writableLocation(
        QStandardPaths::GenericCacheLocation);
    return cacheRoot + QStringLiteral("/ants-terminal/mcp-state");
}

QString storePathFor(const QString &cwd, const QString &baseDirOverride) {
    QString base = baseDirOverride;
    if (base.isEmpty()) base = envBaseDir();
    if (base.isEmpty()) base = defaultBaseDir();
    return base + QStringLiteral("/") + cwdHash(cwd)
        + QStringLiteral(".json");
}

bool parseOp(const QString &raw, Op *out) {
    if (!out) return false;
    if (raw == QStringLiteral("get"))    { *out = Op::Get;    return true; }
    if (raw == QStringLiteral("set"))    { *out = Op::Set;    return true; }
    if (raw == QStringLiteral("delete")) { *out = Op::Delete; return true; }
    if (raw == QStringLiteral("list"))   { *out = Op::List;   return true; }
    return false;
}

bool isValidKey(const QString &key) {
    // INV-7 — ^[A-Za-z0-9._-]{1,64}$ inlined as a hand-loop. Cheaper
    // than QRegularExpression for a per-call check, and we already
    // know the alphabet.
    if (key.isEmpty() || key.size() > kMaxKeyLength) return false;
    for (QChar c : key) {
        const ushort u = c.unicode();
        const bool ok =
            (u >= 'A' && u <= 'Z') ||
            (u >= 'a' && u <= 'z') ||
            (u >= '0' && u <= '9') ||
            u == '.' || u == '_' || u == '-';
        if (!ok) return false;
    }
    return true;
}

QJsonObject loadStore(const QString &path, bool *corruptOut) {
    if (corruptOut) *corruptOut = false;
    QFile f(path);
    if (!f.exists()) return QJsonObject();
    if (!f.open(QIODevice::ReadOnly)) return QJsonObject();
    const QByteArray body = f.readAll();
    f.close();
    if (body.isEmpty()) return QJsonObject();

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // INV-11 — corrupt store treated as empty, warn once.
        qWarning() << "[SessionMemoryEngine] corrupt store at" << path
                   << "— treating as empty (" << err.errorString() << ")";
        if (corruptOut) *corruptOut = true;
        return QJsonObject();
    }
    return doc.object();
}

QString saveStore(const QString &path, const QByteArray &body) {
    // ANTS-1364 — body is pre-serialized by the caller. INV-3 + INV-10
    // logic (atomic temp+rename, parent-dir 0700, leaf 0600) is
    // unchanged byte-for-byte; only the serialize moved up the stack.
    // INV-10 — parent dir exists at mode 0700. ANTS-1821 — create it
    // privately (no mkpath-then-chmod-0755 window) and re-tighten an
    // already-existing 0755 dir (subsumes the old ANTS-1801 chmod).
    const QString parent = QFileInfo(path).absolutePath();
    if (!ensurePrivateDir(parent)) {
        return QStringLiteral("session_memory: failed to secure "
                              "directory ") + parent;
    }

    // INV-3 — QSaveFile = temp + rename, atomic on commit.
    QSaveFile sf(path);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QStringLiteral("session_memory: failed to open ")
            + path + QStringLiteral(" for write: ") + sf.errorString();
    }
    if (sf.write(body) != body.size()) {
        sf.cancelWriting();
        return QStringLiteral("session_memory: short write to ") + path;
    }
    if (!sf.commit()) {
        return QStringLiteral("session_memory: failed to commit ")
            + path + QStringLiteral(": ") + sf.errorString();
    }
    // INV-10 — mode 0600 on the leaf file.
    QFile::setPermissions(path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return QString();
}

qint64 serializedSize(const QJsonObject &store) {
    return QJsonDocument(store).toJson(QJsonDocument::Compact).size();
}

qint64 serializedSize(const QJsonValue &value) {
    // QJsonDocument needs an object or array root; wrap and subtract
    // the wrapper bytes to get the value-only size. Wrapper:
    //   {"v":<value>}
    // wrapper-overhead = 6 (`{"v":}` without the value).
    QJsonObject wrap;
    wrap[QStringLiteral("v")] = value;
    const qint64 total = QJsonDocument(wrap).toJson(QJsonDocument::Compact).size();
    return total > 6 ? total - 6 : 0;
}

OpResult mutateLocked(const QString &path,
                      const std::function<bool(QJsonObject &)> &mutator) {
    OpResult r;
    r.ok   = true;
    r.path = path;

    // ANTS-1823 — the lock file lives beside the store, so the store dir
    // must exist before we can take the lock. ensurePrivateDir is
    // idempotent and saveStore re-checks, so this is cheap and safe.
    ensurePrivateDir(QFileInfo(path).absolutePath());

    // Advisory cross-process lock spanning load → mutate → save. Best
    // effort: a crashed holder is reclaimed after the stale window, and a
    // timeout falls through to an unlocked cycle (the pre-fix behaviour)
    // rather than failing the op — correctness degrades to last-writer-
    // wins, the tool never wedges.
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(30 * 1000);   // 30 s — generous for a JSON RMW
    lock.tryLock(5000);                 // 5 s budget; ignore the result

    QJsonObject store = loadStore(path);
    const bool needWrite = mutator(store);
    if (!needWrite) {
        r.totalBytes = serializedSize(store);
        return r;
    }

    // INV-2 / HI-4 — store-level caps, identical codes to execute().
    if (store.size() > kMaxStoreKeys) {
        r.ok   = false;
        r.code = QStringLiteral("cap_exceeded");
        r.error = QStringLiteral("session_memory: store would exceed "
                                 "%1-key cap (%2 keys)")
                      .arg(kMaxStoreKeys).arg(store.size());
        return r;
    }
    const QByteArray body = QJsonDocument(store).toJson(QJsonDocument::Compact);
    if (body.size() > kMaxStoreBytes) {
        r.ok   = false;
        r.code = QStringLiteral("cap_exceeded");
        r.error = QStringLiteral("session_memory: store would exceed "
                                 "100 KiB cap (%1 bytes)").arg(body.size());
        return r;
    }
    const QString saveErr = saveStore(path, body);
    if (!saveErr.isEmpty()) {
        r.ok   = false;
        r.code = QStringLiteral("io_error");
        r.error = saveErr;
        return r;
    }
    r.totalBytes = body.size();
    return r;
}

namespace {

OpResult makeErr(Op op, const QString &key, const QString &code,
                 const QString &msg, const QString &path) {
    OpResult r;
    r.ok    = false;
    r.code  = code;
    r.error = msg;
    r.path  = path;
    r.key   = key;
    switch (op) {
        case Op::Get:    r.op = QStringLiteral("get");    break;
        case Op::Set:    r.op = QStringLiteral("set");    break;
        case Op::Delete: r.op = QStringLiteral("delete"); break;
        case Op::List:   r.op = QStringLiteral("list");   break;
    }
    return r;
}

QString opName(Op op) {
    switch (op) {
        case Op::Get:    return QStringLiteral("get");
        case Op::Set:    return QStringLiteral("set");
        case Op::Delete: return QStringLiteral("delete");
        case Op::List:   return QStringLiteral("list");
    }
    return QString();
}

}  // namespace

OpResult execute(const QString &cwd, Op op,
                 const QString &key, const QJsonValue &value,
                 const QString &baseDirOverride) {
    // INV-13 — caller is expected to have canonicalised cwd already.
    // Bail with no_project if empty.
    if (cwd.isEmpty()) {
        return makeErr(op, key, QStringLiteral("no_project"),
                       QStringLiteral("session_memory: cwd is empty"),
                       QString());
    }

    const QString path = storePathFor(cwd, baseDirOverride);

    // INV-7 — key validation. List doesn't need a key; others do.
    const bool needsKey =
        (op == Op::Get || op == Op::Set || op == Op::Delete);
    if (needsKey && !isValidKey(key)) {
        return makeErr(op, key, QStringLiteral("bad_key"),
                       QStringLiteral("session_memory: key must match "
                                      "^[A-Za-z0-9._-]{1,64}$"),
                       path);
    }

    OpResult r;
    r.ok   = true;
    r.op   = opName(op);
    r.key  = key;
    r.path = path;

    switch (op) {
        case Op::Get: {
            // Read op — no lock; QSaveFile's atomic rename means a
            // concurrent write is seen whole or not at all (ANTS-1823).
            const QJsonObject store = loadStore(path);
            if (store.contains(key)) {
                r.found = true;
                r.value = store.value(key);
            } else {
                r.found = false;
            }
            r.totalBytes = serializedSize(store);
            return r;
        }
        case Op::Set: {
            // INV-8 — per-value cap. Checked pre-load: it depends only on
            // the incoming value, and yields the specific bad_value code.
            const qint64 valBytes = serializedSize(value);
            if (valBytes > kMaxValueBytes) {
                return makeErr(op, key, QStringLiteral("bad_value"),
                    QStringLiteral("session_memory: value exceeds "
                                   "16 KiB cap (%1 bytes)")
                        .arg(valBytes),
                    path);
            }
            // ANTS-1823 — locked RMW. mutateLocked loads the store under
            // an advisory lock, applies the insert, enforces the store-
            // level caps (HI-4 key count + INV-2 total bytes), and writes
            // once — so a concurrent same-cwd set can't drop this key.
            const OpResult mr = mutateLocked(path,
                [&](QJsonObject &store) {
                    store[key] = value;
                    return true;
                });
            if (!mr.ok) return makeErr(op, key, mr.code, mr.error, path);
            r.bytesWritten = valBytes;
            r.totalBytes   = mr.totalBytes;
            return r;
        }
        case Op::Delete: {
            // ANTS-1823 — locked RMW; only writes when the key existed.
            bool had = false;
            const OpResult mr = mutateLocked(path,
                [&](QJsonObject &store) {
                    had = store.contains(key);
                    if (had) store.remove(key);
                    return had;
                });
            if (!mr.ok) return makeErr(op, key, mr.code, mr.error, path);
            r.found      = had;
            r.totalBytes = mr.totalBytes;
            return r;
        }
        case Op::List: {
            const QJsonObject store = loadStore(path);
            // INV-5 — keys + per-key bytes, no inline values.
            QList<QPair<QString, qint64>> rows;
            rows.reserve(store.size());
            for (auto it = store.begin(); it != store.end(); ++it) {
                rows.append({it.key(), serializedSize(it.value())});
            }
            std::sort(rows.begin(), rows.end(),
                [](const auto &a, const auto &b) {
                    return a.first < b.first;
                });
            for (const auto &row : rows) {
                QJsonObject k;
                k[QStringLiteral("key")]   = row.first;
                k[QStringLiteral("bytes")] = row.second;
                r.keys.append(k);
            }
            r.totalBytes = serializedSize(store);
            return r;
        }
    }
    return r;
}

}  // namespace SessionMemoryEngine
