#include "mcpspill.h"

#include "secureio.h"   // ensurePrivateDir (0700) + setOwnerOnlyPerms (0600)

#include <atomic>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>
#include <QStandardPaths>

namespace mcp {

namespace {

// Live config — read on every dispatch, written from the GUI thread
// (config load / Settings Apply). Atomic so the cross-thread publish is a
// relaxed flag flip, no lock (same idiom as mcpprojection's g_terseDefault).
std::atomic<bool> g_offloadEnabled{false};
std::atomic<int>  g_offloadThreshold{16384};
std::atomic<int>  g_offloadHead{2048};

constexpr qint64 kReadDefaultBytes = 512LL * 1024;        // read_spill default
constexpr qint64 kReadCeilingBytes = 4LL * 1024 * 1024;   // read_spill ceiling
constexpr qint64 kSweepMaxAgeSec   = 24 * 60 * 60;        // 24 h

// Test-only override of the spill cache root (ANTS-2154). Empty ⇒ default.
// Written once from a test's SetUp before any dispatch; production never sets
// it, so the dispatch-thread read below races nothing in a real session.
QString g_spillDirOverride;

QString spillDir() {
    if (!g_spillDirOverride.isEmpty())
        return g_spillDirOverride;
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
           + QStringLiteral("/ants-terminal/mcp-spill/");
}

QString spillPath(const QString &handle) {
    return spillDir() + handle + QStringLiteral(".json");
}

// Back `n` down to a UTF-8 character boundary within `utf8` so a slice
// never splits a multi-byte sequence (INV-2/INV-6). `n` is the number of
// bytes we would keep; if the byte at index `n` is a continuation byte
// (10xxxxxx) we are mid-sequence, so retreat until it is a lead/ASCII byte.
qint64 utf8BoundaryLen(const QByteArray &utf8, qint64 n) {
    if (n >= utf8.size()) return utf8.size();
    if (n <= 0) return 0;
    while (n > 0 && (static_cast<unsigned char>(utf8.at(static_cast<int>(n)))
                     & 0xC0) == 0x80)
        --n;
    return n;
}

// Drop spill files oldest-mtime-first until both caps hold, never evicting
// `keepHandle` (the body written this call — keyed on identity, not mtime,
// so an idempotent re-spill that kept an old mtime is still protected).
void evict(const QString &keepHandle) {
    QDir dir(spillDir());
    if (!dir.exists()) return;
    QFileInfoList entries =
        dir.entryInfoList(QStringList{QStringLiteral("*.json")},
                          QDir::Files, QDir::Time | QDir::Reversed);  // oldest first
    qint64 total = 0;
    for (const QFileInfo &fi : entries) total += fi.size();
    int count = static_cast<int>(entries.size());
    const QString keepBase = keepHandle + QStringLiteral(".json");
    for (const QFileInfo &fi : entries) {
        if (count <= kSpillMaxFiles && total <= kSpillMaxBytes) break;
        if (fi.fileName() == keepBase) continue;   // identity carve-out (INV-7)
        const qint64 sz = fi.size();
        if (QFile::remove(fi.absoluteFilePath())) {
            total -= sz;
            --count;
        }
    }
}

}  // namespace

void setOffloadConfig(bool enabled, int thresholdBytes, int headBytes) {
    g_offloadThreshold.store(qBound(4096, thresholdBytes, 1048576),
                             std::memory_order_relaxed);
    g_offloadHead.store(qBound(256, headBytes, 16384),
                        std::memory_order_relaxed);
    g_offloadEnabled.store(enabled, std::memory_order_relaxed);
}

bool offloadDefault()        { return g_offloadEnabled.load(std::memory_order_relaxed); }
int  offloadThresholdBytes() { return g_offloadThreshold.load(std::memory_order_relaxed); }
int  offloadHeadBytes()      { return g_offloadHead.load(std::memory_order_relaxed); }

bool offloadRequested(const QJsonObject &args) {
    const QJsonValue v = args.value(QStringLiteral("offload"));
    return v.isBool() ? v.toBool() : offloadDefault();
}

QString offloadBody(const QString &toolName, const QString &body) {
    Q_UNUSED(toolName);
    const QByteArray utf8 = body.toUtf8();
    const qint64 total = utf8.size();

    const QString handle =
        QString::fromLatin1(QCryptographicHash::hash(
            utf8, QCryptographicHash::Sha256).toHex());

    // Write the full body to the content-addressed cache. Any failure is
    // fail-open: return the original body unchanged (INV-11).
    if (!ensurePrivateDir(spillDir())) return body;
    const QString path = spillPath(handle);
    {
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return body;
        if (f.write(utf8) != utf8.size()) { f.cancelWriting(); return body; }
        if (!f.commit()) return body;   // commit-or-nothing: no orphan temp
    }
    setOwnerOnlyPerms(path);            // 0600 on the final path, post-commit
    evict(handle);

    // Build the head+pointer envelope. The head guard (offload only fires
    // when total > offloadHeadBytes()) makes the head a strict prefix, so
    // head_truncated is always true here (INV-2).
    const qint64 hlen = utf8BoundaryLen(utf8, offloadHeadBytes());
    QJsonObject o;
    o[QStringLiteral("ok")]            = true;
    o[QStringLiteral("offloaded")]     = true;
    o[QStringLiteral("handle")]        = handle;
    o[QStringLiteral("bytes")]         = total;
    o[QStringLiteral("head")]          = QString::fromUtf8(utf8.left(static_cast<int>(hlen)));
    o[QStringLiteral("head_truncated")] = true;
    o[QStringLiteral("hint")] = QStringLiteral(
        "Large result spilled. Re-read the full body via read_spill "
        "{handle:\"%1\"} (optional offset/max_bytes to page).").arg(handle);

    // ANTS-3538 — additive structured preview for array-shaped bodies
    // (§ 2.3.1 / INV-13). Purely additive: the pre-3538 fields above are
    // untouched (INV-2). For a JSON-object body carrying a dominant array
    // member, append the first K complete rows + the total count so a caller
    // that needs only the head rows or the count answers without a read_spill
    // round-trip. INV-9 is airtight by construction: the row budget is
    // measured against the actual serialized envelope (S0) and capped at
    // offloadHeadBytes(), so the envelope always stays strictly < total.
    if (total <= kStructuredParseMaxBytes) {   // parse cap (§ 4 RAM bound)
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(utf8, &perr);
        if (perr.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonObject root = doc.object();
            // Dominant array = the root's own direct non-empty array member
            // with the most elements. QJsonObject iterates in sorted key
            // order, so the first strict-max is the lexicographically-first
            // key on ties (deterministic; no recursion into nested objects,
            // so a tabular {__cols__,__rows__} member is never selected).
            QString domKey;
            int domCount = 0;
            for (auto it = root.begin(); it != root.end(); ++it) {
                if (!it.value().isArray()) continue;
                const int n = it.value().toArray().size();
                if (n > domCount) { domCount = n; domKey = it.key(); }
            }
            if (domCount > 0) {
                // S0 = the full envelope with the three scalar preview fields
                // + head_rows_truncated at its WIDER 'false' form (5 B) + an
                // empty head_rows. Measuring 'false' guarantees the finished
                // field (usually 'true', 4 B) can only shrink the envelope,
                // never grow it past the budget. S0 counts the escaped head
                // and every other byte, so escaping / config extremes are all
                // accounted, not assumed away.
                QJsonObject probe = o;
                probe[QStringLiteral("head_rows_key")]       = domKey;
                probe[QStringLiteral("row_count")]           = domCount;
                probe[QStringLiteral("head_rows_truncated")] = false;
                probe[QStringLiteral("head_rows")]           = QJsonArray();
                const qint64 s0 = QJsonDocument(probe)
                    .toJson(QJsonDocument::Compact).size();
                // Cap the preview at offloadHeadBytes() (keep it a preview,
                // not a bytes-1 near-copy) AND at total - s0 - 1 (INV-9: the
                // -1 supplies the strict inequality).
                const qint64 budget =
                    qMin<qint64>(offloadHeadBytes(), total - s0 - 1);
                // Greedily append complete elements while the head_rows
                // content (its serialized array literal minus the empty "[]")
                // stays within budget and the count stays under kHeadRowsMax.
                const QJsonArray fullArr = root.value(domKey).toArray();
                QJsonArray headRows;
                for (const QJsonValue &el : fullArr) {
                    if (headRows.size() >= kHeadRowsMax) break;
                    QJsonArray probeArr = headRows;
                    probeArr.append(el);
                    const qint64 arrLen = QJsonDocument(probeArr)
                        .toJson(QJsonDocument::Compact).size();
                    if (arrLen - 2 > budget) break;   // "[]" == 2 bytes
                    headRows = probeArr;
                }
                if (!headRows.isEmpty()) {
                    o[QStringLiteral("head_rows_key")] = domKey;
                    o[QStringLiteral("row_count")]     = domCount;
                    o[QStringLiteral("head_rows")]     = headRows;
                    o[QStringLiteral("head_rows_truncated")] =
                        headRows.size() < domCount;
                }
            }
        }
    }
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

SpillSlice readSpill(const QString &handle, qint64 offset, qint64 maxBytes) {
    SpillSlice s;
    QFile f(spillPath(handle));
    if (!f.exists()) { s.code = QStringLiteral("not_found"); return s; }
    if (!f.open(QIODevice::ReadOnly)) { s.code = QStringLiteral("not_found"); return s; }
    const QByteArray full = f.readAll();
    f.close();

    const qint64 total = full.size();
    const qint64 cap = (maxBytes <= 0) ? kReadDefaultBytes
                                       : qMin(maxBytes, kReadCeilingBytes);
    s.ok         = true;
    s.offset     = offset;
    s.totalBytes = total;
    if (offset >= total) {           // at/after end → empty, not truncated
        s.bytes = 0;
        s.truncated = false;
        return s;
    }
    const qint64 want = qMin(cap, total - offset);
    const QByteArray window = full.mid(static_cast<int>(offset),
                                       static_cast<int>(want));
    const qint64 keep = utf8BoundaryLen(window, want);
    s.content   = QString::fromUtf8(window.left(static_cast<int>(keep)));
    s.bytes     = keep;
    s.truncated = (offset + keep) < total;
    return s;
}

void setSpillDirOverride(const QString &dir) {
    if (dir.isEmpty()) {
        g_spillDirOverride.clear();
    } else {
        // spillPath() concatenates dir + handle + ".json", so normalise to a
        // trailing slash regardless of how the caller passed it.
        g_spillDirOverride = dir.endsWith(QLatin1Char('/'))
                                 ? dir
                                 : dir + QLatin1Char('/');
    }
}

void spillSweep() {
    QDir dir(spillDir());
    if (!dir.exists()) return;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QFileInfoList entries =
        dir.entryInfoList(QStringList{QStringLiteral("*.json")}, QDir::Files);
    for (const QFileInfo &fi : entries) {
        if (fi.lastModified().toUTC().secsTo(now) > kSweepMaxAgeSec)
            QFile::remove(fi.absoluteFilePath());
    }
}

}  // namespace mcp
