#include "mcpspill.h"
#include "mcpprojection.h"   // isProtectedCompactKey — ANTS-4692 floor

#include "secureio.h"   // ensurePrivateDir (0700) + setOwnerOnlyPerms (0600)

#include <atomic>
#include <limits>
#include <iterator>   // std::size — ANTS-4474's head ladder

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
#include <QSet>
#include <QStringList>
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

// ANTS-3545 — the dominant array: the root object's own direct **non-empty**
// array member with the most elements; ties break to the lexicographically-
// first key (QJsonObject iterates in sorted key order). No recursion into
// nested objects, so a tabular {__cols__,__rows__} member is never selected.
// Shared by offloadBody's head_rows preview and readSpillRows, so the previewed
// key (head_rows_key) and the paged key are always the same array (INV-13/14).
// Sets *count to the winning array's size (0 ⇒ empty return key, no root array).
QString dominantArrayKey(const QJsonObject &root, int *count) {
    QString domKey;
    int domCount = 0;
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!it.value().isArray()) continue;
        const int n = it.value().toArray().size();
        if (n > domCount) { domCount = n; domKey = it.key(); }
    }
    if (count) *count = domCount;
    return domKey;
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

// ANTS-3552 — the dispatch-site offload size gate, extracted so the boundary
// is behaviourally testable (offloadBody itself has no threshold guard — it
// always spills — so the gate must live at the caller). Offload iff the body
// is at or above the configured threshold (>=, inclusive) AND strictly larger
// than the head budget (> head): the second guard keeps offload a net saving
// when the head clamp meets or exceeds the threshold clamp (both ranges can
// overlap — INV-12). See docs/specs/ANTS-2094.md § INV-1.
bool shouldOffload(qint64 bodyBytes) {
    return shouldOffload(bodyBytes, offloadThresholdBytes());
}

bool shouldOffload(qint64 bodyBytes, qint64 thresholdBytes) {
    return bodyBytes >= thresholdBytes && bodyBytes > offloadHeadBytes();
}

qint64 offloadThresholdFor(const QString &toolName, const QJsonObject &args) {
    // ANTS-5734 § 2.2 — trace pages itself under its own max_bytes, so a
    // spill would only hide rows its next_offset already reaches.
    if (toolName == QLatin1String("trace"))
        return std::numeric_limits<qint64>::max();
    const qint64 base = offloadThresholdBytes();
    const QJsonValue cap = args.value(QStringLiteral("max_body_bytes"));
    if (toolName != QLatin1String("roadmap_query") || !cap.isDouble())
        return base;
    // Clamped as a double: casting an out-of-range double is undefined.
    const double asked = qMin(cap.toDouble() + double(kExplicitSizeHeadroomBytes),
                              double(kExplicitSizeCeilingBytes));
    return qMax(base, qint64(asked));
}

bool offloadRequested(const QJsonObject &args) {
    const QJsonValue v = args.value(QStringLiteral("offload"));
    return v.isBool() ? v.toBool() : offloadDefault();
}

// ANTS-4692 — keys the OFFLOAD or the DISPATCH LAYER owns. A body member of
// the same name describes the offload rather than the result, so it is
// dropped instead of merged.
// `ok` is listed because the envelope emits it, but it is the one exception to
// the skip: the envelope sets it as a placeholder, so a body carrying its own
// `ok` must win — an offloaded body whose `ok` is false must not be reported
// as a success. See docs/specs/ANTS-2094.md § 2.3.2.
static bool isOffloadOwnedKey(const QString &k) {
    static const QSet<QString> kOwned{
        QStringLiteral("ok"),
        QStringLiteral("offloaded"),          QStringLiteral("handle"),
        QStringLiteral("bytes"),              QStringLiteral("head"),
        QStringLiteral("head_truncated"),     QStringLiteral("hint"),
        QStringLiteral("preserved_omitted"),
        QStringLiteral("head_rows"),          QStringLiteral("head_rows_key"),
        QStringLiteral("head_rows_truncated"), QStringLiteral("row_count"),
        QStringLiteral("rows_preview"),       QStringLiteral("rows_preview_key"),
        QStringLiteral("rows_preview_truncated"),
        QStringLiteral("rows_preview_head_chars"),
        QStringLiteral("rows_preview_hint"),
        QStringLiteral("rows_preview_omitted"),
        // ANTS-4626 — `ignored_args` is the DISPATCHER's advisory, attached by
        // mcp::withIgnoredArgs and deliberately re-applied AFTER the offload.
        // That feature's INV-2 pins offloadBody dropping it, and says why: it
        // is the premise that keeps INV-1 -- the re-application -- an
        // assertion about something. Preserving it here would leave INV-1
        // passing even if the dispatcher stopped re-applying, which is the
        // silent weakening ANTS-4626 was written to prevent. The caller loses
        // nothing: the dispatcher puts it back.
        QStringLiteral("ignored_args")};
    return kOwned.contains(k);
}

// Compact serialized size of one member, key and value, as it would land in
// the envelope. Measured rather than estimated, so escaping is accounted for.
static qint64 preservedMemberBytes(const QString &k, const QJsonValue &v) {
    return QJsonDocument(QJsonObject{{k, v}})
        .toJson(QJsonDocument::Compact).size();
}

// ANTS-4692 — carry the body's own top-level members onto the envelope.
//
// An offload rebuilds the envelope from scratch, so every root member other
// than the previewed array was discarded. roadmap_query's `file_in_sync`,
// `sync_checked` and `source` are the reported case: a session is told to read
// them BEFORE writing, and they vanished exactly when the same call happened
// to spill. Absent is indistinguishable from never-requested, so a caller
// treating a missing field as its zero value reads "out of sync", or skips the
// check it was told to make.
//
// Runs BEFORE the § 2.3.1 row-budget probe, so S0 counts these members and the
// row budget shrinks to accommodate them — INV-9 then holds by construction,
// with the ANTS-3540 finished-envelope fail-open as the backstop.
static void preserveTopLevelFields(QJsonObject &o, const QJsonObject &root,
                                   const QString &domKey) {
    const qint64 totalBudget = offloadHeadBytes();
    qint64 used = 0;
    QStringList omitted;

    // Protected keys first, so the floor cannot be starved by an ordinary
    // member; then the rest in QJsonObject key order — the same sorted
    // iteration § 2.3.1 relies on for its dominant-array tie-break.
    for (int pass = 0; pass < 2; ++pass) {
        const bool protectedPass = (pass == 0);
        for (auto it = root.begin(); it != root.end(); ++it) {
            const QString k = it.key();
            // The dominant array is the payload being spilled; head_rows_key /
            // rows_preview_key already name it, and it is not an omission.
            if (!domKey.isEmpty() && k == domKey) continue;
            const bool prot = mcp::isProtectedCompactKey(k);
            if (prot != protectedPass) continue;
            // `ok` is the one owned key a body may override.
            if (isOffloadOwnedKey(k) && k != QStringLiteral("ok")) continue;

            const qint64 sz = preservedMemberBytes(k, it.value());
            if (sz > kPreservedFieldMaxBytes) { omitted.append(k); continue; }
            // The floor ignores the running total, never the per-member cap:
            // a protected key large enough to matter to the budget is not a
            // flag.
            if (!prot && used + sz > totalBudget) { omitted.append(k); continue; }
            o[k] = it.value();
            used += sz;
        }
    }

    // A member rejected by the per-member cap or the running total is named,
    // so an absent field is never ambiguous between dropped and
    // never-requested — the reasoning spec_lint's skipped[] and
    // doc_integrity's *_suppressed counters already apply.
    if (!omitted.isEmpty())
        o[QStringLiteral("preserved_omitted")] =
            QJsonArray::fromStringList(omitted);
}

namespace {

// ANTS-5266 — the narrowings this call already applied. A caller who sent a
// projection, a row cap and the columnar encoding and still spilled was told
// to do exactly that; those cut keys and rows, never the width of one row.
QStringList appliedNarrowing(const QJsonObject &args) {
    QStringList out;
    for (const char *k : {"fields", "bullet_fields", "limit", "max_results",
                          "max_bytes", "max_symbols"})
        if (args.contains(QLatin1String(k))) out << QString::fromLatin1(k);
    if (args.value(QStringLiteral("encoding")).toString() == QLatin1String("tabular"))
        out << QStringLiteral("encoding:\"tabular\"");
    return out;
}

// The hint for a caller that already narrowed: name what it applied and point
// at the levers left — which rows come back, or the handle.
QString narrowedHint(const QStringList &applied, const QString &toolName,
                     const QString &readSpill) {
    return QStringLiteral(
        "Large result spilled although this call already narrowed with %1. "
        "Those cut keys and rows, not the width of each row, so more of the "
        "same will not fit. Filter WHICH rows %2 returns (a query, section or "
        "status argument), or %3")
        .arg(applied.join(QStringLiteral(", ")), toolName, readSpill);
}

}  // namespace

namespace {

// The offload envelope's `hint`, chosen in one place. `domKey` is the
// row-pageable array, or empty when the body has none.
// ANTS-4850 — the cheaper route first: re-asking the verb narrower costs one
// ordinary envelope and no handle. Argument names are offered as alternatives
// because verbs do not agree on them (`limit`, `max_results`, `max_symbols`).
// ANTS-5272 — for an array body, `fields` comes first: it drops whole arrays,
// which paging cannot. ANTS-5266 — a caller that already narrowed gets
// narrowedHint instead.
QString spillHint(const QString &toolName, const QString &handle,
                  const QString &domKey, const QStringList &applied) {
    if (domKey.isEmpty()) {
        if (!applied.isEmpty())
            return narrowedHint(applied, toolName,
                QStringLiteral("read the full body via read_spill {handle:\"%1\"} "
                               "(optional offset/max_bytes to page).").arg(handle));
        return QStringLiteral(
            "Large result spilled. Re-read the full body via read_spill "
            "{handle:\"%1\"} (optional offset/max_bytes to page). Often cheaper: "
            "re-ask %2 for LESS instead — most verbs take a size cap (`limit`, "
            "`max_results` or `max_bytes`), and a narrower request comes back as "
            "an ordinary envelope with no handle to page.").arg(handle, toolName);
    }
    if (!applied.isEmpty())
        return narrowedHint(applied, toolName,
            QStringLiteral("read the full body via read_spill "
                           "{handle:\"%1\"}, row-paged "
                           "(row_offset/row_count) over the \"%2\" "
                           "array.").arg(handle, domKey));
    return QStringLiteral(
        "Large result spilled. The cheaper fix, first: re-ask %3 with "
        "`fields:[...]` naming only the top-level fields you need — "
        "that drops whole arrays from the envelope, which paging "
        "cannot do, and a summary object (counts, totals) usually "
        "comes back small enough to need no handle at all. Then, if "
        "you do need the rows, narrow them: most list verbs take a "
        "page size (`limit`, or `max_results`) plus `offset`. "
        "Failing both, read the full body via read_spill "
        "{handle:\"%1\"}: byte-paged (offset/max_bytes), or "
        "row-paged (row_offset/row_count) over the \"%2\" array.")
        .arg(handle, domKey, toolName);
}

// ANTS-3538 — `head_rows`: the first complete rows of the dominant array that
// fit. S0 is the envelope with the preview fields at their WIDEST
// (head_rows_truncated:false, 5 B) and an empty head_rows, so the finished
// envelope can only be smaller. The preview is capped at offloadHeadBytes()
// (a preview, not a near-copy) and at total - S0 - 1, which keeps the
// envelope strictly smaller than the body (INV-9). Returns the rows added.
int addHeadRows(QJsonObject &o, const QJsonArray &fullArr, const QString &domKey,
                int domCount, qint64 total) {
    QJsonObject probe = o;
    probe[QStringLiteral("head_rows_key")]       = domKey;
    probe[QStringLiteral("row_count")]           = domCount;
    probe[QStringLiteral("head_rows_truncated")] = false;
    probe[QStringLiteral("head_rows")]           = QJsonArray();
    const qint64 s0 = QJsonDocument(probe).toJson(QJsonDocument::Compact).size();
    const qint64 budget = qMin<qint64>(offloadHeadBytes(), total - s0 - 1);
    // Greedily append complete elements while the head_rows content (its
    // serialized array literal minus the empty "[]") stays within budget and
    // the count stays under kHeadRowsMax.
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
        o[QStringLiteral("head_rows")]     = headRows;
        o[QStringLiteral("head_rows_truncated")] = headRows.size() < domCount;
    }
    return int(headRows.size());
}

// ANTS-4397 — when the row BODIES do not all fit, `rows_preview` gives one
// shape row per row, {index, bytes, head}, so a caller can see which rows are
// large and pick what to page. It must cover EVERY row: a summary of some
// rows hides the same thing a prefix does. So the head narrows down a ladder
// before it is dropped (ANTS-4474: a row's identity lives in its first ~40
// characters), and a shape that covers every row only with no heads at all is
// omitted, with a hint, because a list of row lengths says no more than
// `row_count` + `bytes` (ANTS-4519).
void addRowsPreview(QJsonObject &o, const QJsonArray &fullArr, const QString &domKey,
                    int domCount, qint64 total) {
    const qint64 usedSoFar = QJsonDocument(o).toJson(QJsonDocument::Compact).size();
    const qint64 shapeBudget =
        qMin<qint64>(offloadHeadBytes() * 2, total - usedSoFar - 1);
    // ANTS-5104 — each row's JSON is built once for every rung, and a rung
    // sums row lengths instead of re-serialising the growing array for each
    // row, which made every rung quadratic in the row count. Compact JSON puts
    // only a comma between elements, so the array is 2 + rows + (rows - 1)
    // bytes long.
    QList<QByteArray> elBytesByRow;
    if (shapeBudget > 0) {
        elBytesByRow.reserve(fullArr.size());
        for (int i = 0; i < fullArr.size(); ++i)
            elBytesByRow.append(QJsonDocument(QJsonObject{
                {QStringLiteral("v"), fullArr.at(i)}})
                .toJson(QJsonDocument::Compact));
    }
    // `headChars` 0 means no head; that rung only decides the omission.
    const auto buildShape = [&](int headChars) {
        QJsonArray acc;
        if (shapeBudget <= 0) return acc;
        qint64 contentLen = 0;   // the array's length minus "[]"
        for (int i = 0; i < fullArr.size(); ++i) {
            const QByteArray &elBytes = elBytesByRow.at(i);
            QJsonObject r;
            r[QStringLiteral("index")] = i;
            r[QStringLiteral("bytes")] = static_cast<int>(elBytes.size());
            if (headChars > 0) {
                // ANTS-5153 — elBytes is the {"v":row} wrapper built for
                // sizing, so sample the row's own JSON.
                QString firstChars =
                    fullArr.at(i).isString()
                        ? fullArr.at(i).toString()
                        : QString::fromUtf8(elBytes.mid(5, elBytes.size() - 6));
                if (firstChars.size() > headChars) {
                    firstChars.truncate(headChars);
                    firstChars += QChar(0x2026);
                }
                r[QStringLiteral("head")] = firstChars;
            }
            const qint64 next = contentLen
                + (acc.isEmpty() ? 0 : 1)
                + QJsonDocument(r).toJson(QJsonDocument::Compact).size();
            if (next > shapeBudget) break;
            contentLen = next;
            acc.append(r);
        }
        return acc;
    };
    // The ladder. Each rung is tried only because the one above it could not
    // cover every row, and the walk stops the moment coverage is complete.
    static constexpr int kHeadLadder[] = { 60, 40, 24, 12 };
    QJsonArray shape = buildShape(kHeadLadder[0]);
    int headChars = kHeadLadder[0];
    for (size_t li = 1;
         li < std::size(kHeadLadder) && shape.size() < fullArr.size(); ++li) {
        const QJsonArray narrower = buildShape(kHeadLadder[li]);
        // Strictly more rows: a narrower head that buys no extra row is a
        // worse preview of the same set.
        if (narrower.size() > shape.size()) {
            shape     = narrower;
            headChars = kHeadLadder[li];
        }
    }
    bool headsDropped = false;
    if (shape.size() < fullArr.size()) {
        const QJsonArray lean = buildShape(0);
        if (lean.size() > shape.size()) {
            shape = lean;
            headsDropped = true;
        }
    }
    if (headsDropped) {
        o[QStringLiteral("rows_preview_omitted")] = true;
        o[QStringLiteral("rows_preview_hint")] = QStringLiteral(
            "no per-row preview: the row bodies do not fit and "
            "neither do per-row text samples, and a bare list "
            "of row LENGTHS says no more than `row_count` + "
            "`bytes` already do. Page the \"%1\" array with "
            "read_spill row_offset/row_count.").arg(domKey);
    } else if (!shape.isEmpty()) {
        o[QStringLiteral("rows_preview_key")] = domKey;
        o[QStringLiteral("rows_preview")]     = shape;
        o[QStringLiteral("rows_preview_truncated")] = shape.size() < domCount;
        // ANTS-4474 — only on the narrowed arm, where it explains why a head
        // is shorter than usual.
        if (headChars < kHeadLadder[0])
            o[QStringLiteral("rows_preview_head_chars")] = headChars;
        o[QStringLiteral("rows_preview_hint")] = QStringLiteral(
            "one SHAPE row per row — {index, bytes, head} — "
            "because the row BODIES do not all fit. Use "
            "`bytes` to pick which rows to fetch with "
            "read_spill row_offset/row_count; a prefix of "
            "bodies would show you the first row (possibly "
            "cut off, and duplicating `head`) and say nothing "
            "about the rest.");
    }
}

}  // namespace

QString offloadBody(const QString &toolName, const QString &body,
                    const QJsonObject &args) {
    const QStringList applied = appliedNarrowing(args);
    const QByteArray utf8 = body.toUtf8();
    const qint64 total = utf8.size();

    // ANTS-5104 — a body over the spill directory's whole byte cap is not
    // spilled (user decision 2026-09-14). evict() spares the handle just
    // written, so spilling it emptied the directory of every other body and
    // still broke INV-7. Refuse instead, writing and evicting nothing.
    if (total > kSpillMaxBytes) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("code")]  = QStringLiteral("result_too_large");
        o[QStringLiteral("error")] = QStringLiteral(
            "%1's result is %2 bytes, over the %3-byte spill cap, so it can "
            "be neither returned inline nor spilled").arg(toolName)
                .arg(total).arg(kSpillMaxBytes);
        o[QStringLiteral("bytes")] = total;
        o[QStringLiteral("hint")]  = QStringLiteral(
            "Narrow the call: most verbs take a filter (query, section, path) "
            "and a size cap (limit, max_results or max_bytes).");
        return QString::fromUtf8(
            QJsonDocument(o).toJson(QJsonDocument::Compact));
    }

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
    // 0600 on the final path, post-commit. ANTS-5104 — the spill holds the
    // response body, so one that cannot be made owner-only is removed and
    // the call fails open (INV-11) rather than leaving it readable.
    if (!setOwnerOnlyPerms(path)) { QFile::remove(path); return body; }
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

    // ANTS-3538 — for a JSON-object body, its dominant array (the direct
    // non-empty array member with the most elements; ANTS-3545's shared
    // helper, so the preview and read_spill row-paging agree) and its other
    // top-level fields (ANTS-4692, before the row budget so S0 counts them).
    QJsonObject root;
    QString domKey;
    int domCount = 0;
    if (total <= kStructuredParseMaxBytes) {   // parse cap (§ 4 RAM bound)
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(utf8, &perr);
        if (perr.error == QJsonParseError::NoError && doc.isObject()) {
            root = doc.object();
            domKey = dominantArrayKey(root, &domCount);
            preserveTopLevelFields(o, root, domKey);
        }
    }
    o[QStringLiteral("hint")] =
        spillHint(toolName, handle, domCount > 0 ? domKey : QString(), applied);

    // A body with a dominant array is row-pageable, and says so here, where
    // the caller decides how to continue (ANTS-3545). `row_count` is set
    // whenever the count is knowable (ANTS-4705); the previews are additive.
    if (domCount > 0) {
        o[QStringLiteral("row_count")] = domCount;
        const QJsonArray fullArr = root.value(domKey).toArray();
        if (addHeadRows(o, fullArr, domKey, domCount, total) < domCount)
            addRowsPreview(o, fullArr, domKey, domCount, total);
    }
    // INV-9 (ANTS-3540) — the offload must be a strict net saving. The base
    // head+pointer envelope's fixed overhead (handle + hint + keys ≈ 330 B)
    // plus a full head can exceed a body that only just clears the § 2.1 head
    // guard (reachable at a head≈threshold config). The structured path above
    // already bounds itself against `total`, but the base envelope did not.
    // Fail open when the finished envelope is not strictly smaller than the
    // body: the untrimmed body is both correct and smaller (same posture as the
    // INV-11 write-failure fail-open). Guarantees out_bytes < bytes for every
    // offloaded envelope, at every config.
    const QByteArray envelope = QJsonDocument(o).toJson(QJsonDocument::Compact);
    if (envelope.size() >= total) return body;
    return QString::fromUtf8(envelope);
}

SpillSlice readSpill(const QString &handle, qint64 offset, qint64 maxBytes) {
    SpillSlice s;
    QFile f(spillPath(handle));
    if (!f.exists()) { s.code = QStringLiteral("not_found"); return s; }
    if (!f.open(QIODevice::ReadOnly)) { s.code = QStringLiteral("not_found"); return s; }

    const qint64 total = f.size();
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
    // ANTS-5104 — read only this page, not the whole spill, and cut on a
    // character boundary of the BODY. A window exactly `want` bytes long left
    // utf8BoundaryLen nothing past its end to inspect, so a page could end
    // mid-character. Three more bytes show whether it does, and finish a
    // character that alone outruns a tiny max_bytes.
    if (!f.seek(offset)) { s = SpillSlice{}; s.code = QStringLiteral("not_found"); return s; }
    const QByteArray window = f.read(want + 3);
    qint64 keep = utf8BoundaryLen(window, want);
    if (keep == 0) {
        // max_bytes is smaller than the character at `offset`: return that
        // one character rather than an empty page the caller cannot advance past.
        keep = 1;
        while (keep < window.size() &&
               (static_cast<unsigned char>(window.at(static_cast<int>(keep))) & 0xC0) == 0x80)
            ++keep;
    }
    s.content   = QString::fromUtf8(window.left(static_cast<int>(keep)));
    s.bytes     = keep;
    s.truncated = (offset + keep) < total;
    return s;
}

SpillRows readSpillRows(const QString &handle, qint64 rowOffset, qint64 rowCount) {
    SpillRows r;
    // ANTS-3545 — negative-arg gate lives HERE (not cmdReadSpill) so it is
    // directly unit-testable; only 0/omitted maps to the default, never a
    // negative (§ 2.4.1 / INV-14).
    if (rowOffset < 0 || rowCount < 0) { r.code = QStringLiteral("bad_args"); return r; }

    const QString path = spillPath(handle);
    const QFileInfo fi(path);
    if (!fi.exists()) { r.code = QStringLiteral("not_found"); return r; }
    // Stat BEFORE reading: refuse an over-cap body without loading it into RAM
    // (§ 4 — do not mirror readSpill's unconditional readAll on the row path).
    if (fi.size() > kStructuredParseMaxBytes) { r.code = QStringLiteral("too_large"); return r; }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { r.code = QStringLiteral("not_found"); return r; }
    const QByteArray full = f.readAll();
    f.close();

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(full, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        r.code = QStringLiteral("not_array"); return r;
    }
    int domCount = 0;
    const QString domKey = dominantArrayKey(doc.object(), &domCount);
    if (domCount == 0) { r.code = QStringLiteral("not_array"); return r; }

    const QJsonArray arr = doc.object().value(domKey).toArray();
    const qint64 totalRows = arr.size();
    r.ok        = true;
    r.key       = domKey;
    r.rowOffset = rowOffset;
    r.totalRows = totalRows;
    // ANTS-4375 — compare the array length against the population the
    // producing verb reported, when it reported one. A silent short file is
    // the failure mode this exists for: a truncation that announces itself
    // costs a page, this one read as completeness.
    for (const char *k : {"total", "total_count", "count_total"}) {
        const QJsonValue tv = doc.object().value(QLatin1String(k));
        if (!tv.isDouble()) continue;
        r.population = static_cast<qint64>(tv.toDouble());
        break;
    }
    r.rowsArePartial = (r.population > totalRows);
    if (rowOffset >= totalRows) {          // past end → empty, not truncated
        r.truncated = false;
        return r;
    }
    const qint64 count = (rowCount <= 0) ? kSpillRowsDefault : rowCount;
    // Overflow-safe: `totalRows - rowOffset` is >= 0 after the past-end guard,
    // never `rowOffset + count` (two caller-controlled values could overflow).
    const qint64 take = qMin(count, totalRows - rowOffset);
    for (qint64 i = 0; i < take; ++i)
        r.rows.append(arr.at(static_cast<int>(rowOffset + i)));
    r.truncated = (rowOffset + take) < totalRows;
    return r;
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
