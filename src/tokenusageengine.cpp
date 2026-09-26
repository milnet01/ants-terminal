// ANTS-1284 — implementation. See header and docs/specs/ANTS-1284.md.

#include "tokenusageengine.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>

namespace TokenUsageEngine {

namespace {

// Per-tool baselines (bytes/call) — the estimated cost of doing the
// same work via Bash + Read instead of the MCP tool. See spec § 2.4
// for the anchors behind each number. Adding an entry here is a
// one-line change that flows directly through estTokensSaved math.
const QHash<QString, qint64> &baselineTable() {
    static const QHash<QString, qint64> kBaselines = {
        {QStringLiteral("roadmap_query"),  594000},   // ROADMAP.md size
        {QStringLiteral("verify_changes"),   8192},   // skill 4.1 KiB + ~4 KiB bash overhead
        {QStringLiteral("plan_template"),    8192},   // skill 6.0 KiB + ~2 KiB template echo
        // ANTS-3361 — the read/search verbs each REPLACE a full-file Read
        // or a grep, so they carry a real saving the meter previously
        // credited at ~0 (no baseline → estTokensSaved 0). Unlike
        // roadmap_query's exact file size, these are DELIBERATELY
        // CONSERVATIVE per-call estimates of the naive alternative's cost
        // (a modest source file / grep output) — a rough order-of-magnitude
        // model, matching the metric's existing precision. Under-shooting is
        // intentional: the estTokensSaved floor-at-0 (see buildReport)
        // means a call whose own output exceeds the baseline credits 0
        // rather than over-claiming, so we bias low and never inflate the
        // "tokens saved" headline. A precise per-call baseline (file size
        // threaded from each verb) is the larger cross-verb estimation task
        // tracked separately; these constants are the proportionate fix.
        {QStringLiteral("file_outline"),     8192},   // vs a full-file Read
        {QStringLiteral("read_region"),      8192},   // vs Read-ing the file to slice it
        {QStringLiteral("read_regions"),    12288},   // multiple slices / files
        {QStringLiteral("workspace_search"), 4096},   // vs grep -r output
        {QStringLiteral("codebase_index"),  12288},   // vs a project-wide grep/find
        {QStringLiteral("find_definition"),  4096},   // vs multi-grep for a def
        {QStringLiteral("find_sources"),     4096},   // vs multi-grep for callers
        {QStringLiteral("find_caller"),      4096},   // vs multi-grep for callers
        {QStringLiteral("build_status"),     3072},   // vs reading build-log tail
    };
    return kBaselines;
}

// ANTS-3579 — kCharsPerToken moved to the public header (tokenusageengine.h) so
// the per-project display path + tests reference it by symbol.

}  // namespace

Tracker::Tracker() {
    m_sinceUnixMs = QDateTime::currentMSecsSinceEpoch();
}

void Tracker::recordCall(const QString &toolName,
                         qint64         bytesIn,
                         qint64         bytesOut,
                         qint64         wrapBytes,
                         qint64         durationUs,
                         bool           success) {
    auto &c = m_counters[toolName];
    // ANTS-1432 — failed-call branch is mutually exclusive with the
    // success accumulator. We deliberately do NOT update nCalls /
    // durations / wrapBytes on failure: those are "what did this
    // tool cost when it worked" metrics; mixing failure bytes into
    // estTokensSaved arithmetic would muddy the saved figure.
    if (!success) {
        c.failedCalls    += 1;
        c.failedBytesIn  += bytesIn;
        c.failedBytesOut += bytesOut;
        return;
    }
    // ANTS-1355 INV-4: sentinel handling for min/max — overwrite
    // unconditionally on the first record, then min/max thereafter.
    if (c.nCalls == 0) {
        c.durationUsMin = durationUs;
        c.durationUsMax = durationUs;
    } else {
        if (durationUs < c.durationUsMin) c.durationUsMin = durationUs;
        if (durationUs > c.durationUsMax) c.durationUsMax = durationUs;
    }
    c.nCalls        += 1;
    c.bytesIn       += bytesIn;
    c.bytesOut      += bytesOut;
    c.wrapBytes     += wrapBytes;
    c.durationUsSum += durationUs;
}

void Tracker::reset() {
    m_counters.clear();
    m_sinceUnixMs = QDateTime::currentMSecsSinceEpoch();
}

qint64 Tracker::baselineFor(const QString &toolName) {
    const auto &t = baselineTable();
    auto it = t.find(toolName);
    return it == t.end() ? 0 : it.value();
}

Snapshot Tracker::buildReport(bool includeZero) const {
    Snapshot snap;
    snap.sinceUnixMs = m_sinceUnixMs;
    snap.toolsCalled = m_counters.size();

    QList<ToolReport> all;
    all.reserve(m_counters.size());
    for (auto it = m_counters.cbegin(); it != m_counters.cend(); ++it) {
        ToolReport r;
        r.tool          = it.key();
        r.nCalls        = it.value().nCalls;
        r.bytesIn       = it.value().bytesIn;
        r.bytesOut      = it.value().bytesOut;
        // ANTS-1355 v2 fields.
        r.wrapBytes     = it.value().wrapBytes;
        r.durationUsMin = it.value().durationUsMin;
        r.durationUsMax = it.value().durationUsMax;
        r.durationUsMean = (it.value().nCalls > 0)
            ? (it.value().durationUsSum / it.value().nCalls)
            : 0;
        // ANTS-1432 v3 fields.
        r.failedCalls    = it.value().failedCalls;
        r.failedBytesIn  = it.value().failedBytesIn;
        r.failedBytesOut = it.value().failedBytesOut;

        const qint64 baseline = baselineFor(r.tool);
        // Per-call baseline × n_calls is the modelled "would-have-spent"
        // cost; subtract observed wire bytes for the same period; floor
        // at 0 per INV-4 (negative would mean response exceeded model).
        const qint64 totalBaseline = baseline * r.nCalls;
        const qint64 totalActual   = r.bytesIn + r.bytesOut;
        const qint64 savedBytes    = std::max<qint64>(0, totalBaseline - totalActual);
        r.estTokensSaved = savedBytes / kCharsPerToken;

        snap.totalSaved        += r.estTokensSaved;
        snap.totalWrapBytes    += r.wrapBytes;     // ANTS-1355 — across ALL tools
        // ANTS-1432 — sum across ALL tools (even ones filtered out
        // of calls[] by include_zero) so the envelope summary stays
        // truthful regardless of the include_zero filter.
        snap.totalFailedBytes  += r.failedBytesIn + r.failedBytesOut;
        all.append(r);
    }

    // Sort by estTokensSaved desc, tiebreak by tool name asc — stable
    // ordering matters for the consumer (deterministic for tests, and
    // for diffing successive reports).
    std::sort(all.begin(), all.end(),
              [](const ToolReport &a, const ToolReport &b) {
        if (a.estTokensSaved != b.estTokensSaved) {
            return a.estTokensSaved > b.estTokensSaved;
        }
        return a.tool < b.tool;
    });

    if (includeZero) {
        snap.calls = std::move(all);
    } else {
        snap.calls.reserve(all.size());
        for (auto &r : all) {
            // ANTS-1432 — also retain tools with only failed calls
            // (estTokensSaved == 0 but failedCalls > 0). Surfacing
            // failure-only tools is the whole point of the metric.
            if (r.estTokensSaved > 0 || r.failedCalls > 0) {
                snap.calls.append(std::move(r));
            }
        }
    }
    return snap;
}

qint64 Tracker::totalSaved() const {
    // ANTS-5104 — buildReport(false).totalSaved without the per-tool list or
    // the sort: the same floor-at-0 arithmetic, summed.
    return totalSavedOf(m_counters);
}

qint64 totalSavedOf(const QHash<QString, ToolCounter> &counters) {
    qint64 total = 0;
    for (auto it = counters.cbegin(); it != counters.cend(); ++it) {
        const qint64 saved = Tracker::baselineFor(it.key()) * it.value().nCalls
                             - (it.value().bytesIn + it.value().bytesOut);
        if (saved > 0) total += saved / kCharsPerToken;
    }
    return total;
}

// ---- ANTS-3572 pure persistence helpers ---------------------------------

QJsonObject foldMonthlyBucket(QJsonObject monthly, const QString &monthKey,
                              qint64 add, int keepMonths) {
    // Values are stored as JSON numbers (double), exact-integer to 2^53 —
    // far beyond any real token total (INV-9).
    const qint64 prev = static_cast<qint64>(monthly.value(monthKey).toDouble(0));
    monthly[monthKey] = static_cast<double>(prev + add);

    // Retain only the keepMonths lexicographically-greatest keys. QJsonObject
    // keys() is ascending, so the oldest are at the front (INV-4).
    if (keepMonths > 0 && monthly.size() > keepMonths) {
        const QStringList keys = monthly.keys();  // ascending
        const int drop = monthly.size() - keepMonths;
        for (int i = 0; i < drop; ++i) monthly.remove(keys.at(i));
    }
    return monthly;
}

// ANTS-3579 — see header. Folds one root; does NOT evict (pruneProjectBuckets does).
QJsonObject foldProjectBucket(QJsonObject byProject, const QString &root,
                              qint64 addTokens, const QString &monthKey,
                              const QString &nowIso, int keepMonths) {
    QJsonObject bucket = byProject.value(root).toObject();
    const qint64 prevLife = static_cast<qint64>(
        bucket.value(QStringLiteral("lifetime")).toDouble(0));
    bucket[QStringLiteral("lifetime")] =
        static_cast<double>(prevLife + addTokens);
    bucket[QStringLiteral("monthly")] = foldMonthlyBucket(
        bucket.value(QStringLiteral("monthly")).toObject(), monthKey, addTokens,
        keepMonths);
    if (bucket.value(QStringLiteral("since")).toString().isEmpty())
        bucket[QStringLiteral("since")] = nowIso.left(10);  // date portion
    bucket[QStringLiteral("updated")] = nowIso;
    byProject[root] = bucket;
    return byProject;
}

// ANTS-3579 — evict to keepProjects roots, oldest `updated` first; ties broken by
// root string (larger evicted first) so the result is a pure function of the map.
QJsonObject pruneProjectBuckets(QJsonObject byProject, int keepProjects) {
    if (keepProjects <= 0 || byProject.size() <= keepProjects) return byProject;
    QStringList roots = byProject.keys();
    std::sort(roots.begin(), roots.end(),
              [&](const QString &a, const QString &b) {
                  const QString ua = byProject.value(a).toObject()
                                         .value(QStringLiteral("updated")).toString();
                  const QString ub = byProject.value(b).toObject()
                                         .value(QStringLiteral("updated")).toString();
                  if (ua != ub) return ua < ub;  // oldest updated evicted first
                  return a > b;                  // tie: larger root evicted first
              });
    const int drop = byProject.size() - keepProjects;
    for (int i = 0; i < drop; ++i) byProject.remove(roots.at(i));
    return byProject;
}

qint64 sumYear(const QJsonObject &monthly, const QString &yearPrefix) {
    qint64 sum = 0;
    for (auto it = monthly.begin(); it != monthly.end(); ++it) {
        if (it.key().startsWith(yearPrefix))
            sum += static_cast<qint64>(it.value().toDouble(0));
    }
    return sum;
}

QString humanizeCount(qint64 n) {
    if (n < 1000) return QString::number(n);
    double v;
    QLatin1Char suffix('K');
    if (n < 1000000) {
        v = n / 1000.0;
    } else if (n < 1000000000) {
        v = n / 1000000.0;
        suffix = QLatin1Char('M');
    } else {
        v = n / 1000000000.0;
        suffix = QLatin1Char('B');
    }
    QString s = QString::number(v, 'f', 1);
    // ANTS-5104 — a value that rounds up to 1000 belongs to the next unit:
    // 999,950 reads "1M", not "1000K".
    if (s == QLatin1String("1000.0") && suffix != QLatin1Char('B')) {
        s = QStringLiteral("1.0");
        suffix = QLatin1Char(suffix == QLatin1Char('K') ? 'M' : 'B');
    }
    if (s.endsWith(QLatin1String(".0"))) s.chop(2);  // "1.0K" → "1K"
    return s + suffix;
}

// ---- ANTS-5311 — ants-mcpd usage snapshots --------------------------------

namespace {

constexpr qint64 kMaxSnapshotBytes = qint64{256} * 1024;   // § 2.6
constexpr qint64 kLoneLockAgeSecs  = 60;           // § 2.5
constexpr int    kSnapshotFormat   = 1;

const char *const kCounterKeys[] = {
    "n_calls", "bytes_in", "bytes_out", "wrap_bytes", "duration_us_min",
    "duration_us_max", "duration_us_sum", "failed_calls", "failed_bytes_in",
    "failed_bytes_out"};

qint64 *counterField(ToolCounter &c, int i) {
    switch (i) {
    case 0: return nullptr;   // nCalls is an int; handled by the caller
    case 1: return &c.bytesIn;
    case 2: return &c.bytesOut;
    case 3: return &c.wrapBytes;
    case 4: return &c.durationUsMin;
    case 5: return &c.durationUsMax;
    case 6: return &c.durationUsSum;
    case 7: return &c.failedCalls;
    case 8: return &c.failedBytesIn;
    case 9: return &c.failedBytesOut;
    default: return nullptr;
    }
}

// One snapshot's own figures (§ 2.3): derived from that file alone.
void addSnapshotTo(PeerUsage &u, const QString &stem,
                   const QHash<QString, ToolCounter> &tools,
                   const QHash<QString, qint64> &bytesByRoot) {
    u.savedTokens += totalSavedOf(tools);
    for (auto it = bytesByRoot.cbegin(); it != bytesByRoot.cend(); ++it)
        u.savedTokensByProject[it.key()] += it.value() / kCharsPerToken;
    for (const ToolCounter &c : tools) {
        u.calls += c.nCalls;
        u.failedCalls += c.failedCalls;
    }
    ++u.sessions;
    u.stems.append(stem);
}

// § 2.6 — read one file as untrusted input. Empty `why` on success.
bool readSnapshotFile(const QString &path, QHash<QString, ToolCounter> *tools,
                      QHash<QString, qint64> *bytesByRoot, QString *why) {
    const QFileInfo fi(path);
    if (fi.isSymLink())  { *why = QStringLiteral("a symlink"); return false; }
    if (!fi.isFile())    { *why = QStringLiteral("not a regular file"); return false; }
    if (fi.size() > kMaxSnapshotBytes) {
        *why = QStringLiteral("over %1 KiB").arg(kMaxSnapshotBytes / 1024);
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { *why = QStringLiteral("unreadable"); return false; }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        *why = QStringLiteral("not a JSON object");
        return false;
    }
    return snapshotFromJson(doc.object(), tools, bytesByRoot, why);
}

// Non-blocking exclusive lock on `path`, never creating it. The fd, or -1 when
// the lock is held elsewhere or the file is gone.
int tryLock(const QString &path) {
    const QByteArray p = QFile::encodeName(path);
    const int fd = ::open(p.constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) { ::close(fd); return -1; }
    return fd;
}

}  // namespace

QString peerSnapshotDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/ants-terminal/mcpd-usage");
}

QJsonObject snapshotToJson(const QHash<QString, ToolCounter> &tools,
                           const QHash<QString, qint64> &savedBytesByProject,
                           qint64 pid, qint64 startedMs, qint64 updatedMs) {
    QJsonObject t;
    for (auto it = tools.cbegin(); it != tools.cend(); ++it) {
        ToolCounter c = it.value();
        QJsonObject o;
        o[QLatin1String(kCounterKeys[0])] = c.nCalls;
        for (int i = 1; i < 10; ++i)
            o[QLatin1String(kCounterKeys[i])] = double(*counterField(c, i));
        t[it.key()] = o;
    }
    QJsonObject roots;
    for (auto it = savedBytesByProject.cbegin(); it != savedBytesByProject.cend(); ++it)
        roots[it.key()] = double(it.value());
    QJsonObject out;
    out[QStringLiteral("format")]                 = kSnapshotFormat;
    out[QStringLiteral("pid")]                    = double(pid);
    out[QStringLiteral("started_unix_ms")]        = double(startedMs);
    out[QStringLiteral("updated_unix_ms")]        = double(updatedMs);
    out[QStringLiteral("tools")]                  = t;
    out[QStringLiteral("saved_bytes_by_project")] = roots;
    return out;
}

bool snapshotFromJson(const QJsonObject &o, QHash<QString, ToolCounter> *tools,
                      QHash<QString, qint64> *savedBytesByProject, QString *why) {
    tools->clear();
    savedBytesByProject->clear();
    if (o.value(QStringLiteral("format")).toInt(-1) != kSnapshotFormat) {
        *why = QStringLiteral("format is not %1").arg(kSnapshotFormat);
        return false;
    }
    const QJsonObject t = o.value(QStringLiteral("tools")).toObject();
    const QJsonObject roots = o.value(QStringLiteral("saved_bytes_by_project")).toObject();
    if (t.size() > kMaxPeerTools) {
        *why = QStringLiteral("more than %1 tools").arg(kMaxPeerTools);
        return false;
    }
    if (roots.size() > kMaxPeerProjects) {
        *why = QStringLiteral("more than %1 projects").arg(kMaxPeerProjects);
        return false;
    }
    for (auto it = t.constBegin(); it != t.constEnd(); ++it) {
        const QJsonObject c = it.value().toObject();
        ToolCounter tc;
        for (int i = 0; i < 10; ++i) {
            const qint64 v = c.value(QLatin1String(kCounterKeys[i])).toInteger();
            if (v < 0) {
                *why = QStringLiteral("negative %1 for %2")
                           .arg(QLatin1String(kCounterKeys[i]), it.key());
                return false;
            }
            if (i == 0) tc.nCalls = int(v);
            else        *counterField(tc, i) = v;
        }
        tools->insert(it.key(), tc);
    }
    for (auto it = roots.constBegin(); it != roots.constEnd(); ++it) {
        const qint64 v = it.value().toInteger();
        if (v < 0) {
            *why = QStringLiteral("negative bytes for %1").arg(it.key());
            return false;
        }
        savedBytesByProject->insert(it.key(), v);
    }
    why->clear();
    return true;
}

void addCounters(QHash<QString, ToolCounter> &into,
                 const QHash<QString, ToolCounter> &from) {
    for (auto it = from.cbegin(); it != from.cend(); ++it) {
        ToolCounter &a = into[it.key()];
        const ToolCounter &b = it.value();
        if (b.nCalls > 0) {
            a.durationUsMin = a.nCalls > 0 ? std::min(a.durationUsMin, b.durationUsMin)
                                           : b.durationUsMin;
            a.durationUsMax = std::max(a.durationUsMax, b.durationUsMax);
        }
        a.nCalls         += b.nCalls;
        a.bytesIn        += b.bytesIn;
        a.bytesOut       += b.bytesOut;
        a.wrapBytes      += b.wrapBytes;
        a.durationUsSum  += b.durationUsSum;
        a.failedCalls    += b.failedCalls;
        a.failedBytesIn  += b.failedBytesIn;
        a.failedBytesOut += b.failedBytesOut;
    }
}

PeerUsage readPeerSnapshots(const QString &dir, bool claimDead,
                            PeerUsage *dead, QList<int> *heldLocks) {
    static const QRegularExpression rxJson(QStringLiteral("^([0-9]+-[0-9]+)\\.json$"));
    static const QRegularExpression rxLock(QStringLiteral("^([0-9]+-[0-9]+)\\.lock$"));
    PeerUsage all;
    const QDir d(dir);
    const QStringList names = d.entryList(QDir::Files | QDir::System | QDir::Hidden,
                                          QDir::Name);
    const QSet<QString> present(names.cbegin(), names.cend());
    const QString claimedSuffix =
        QStringLiteral(".claimed-%1").arg(QCoreApplication::applicationPid());

    for (const QString &name : names) {
        // A lone lock (§ 2.5): a writer between its lock and its first write,
        // or one that died there. Only an old one whose lock is free is removed.
        if (const auto lm = rxLock.match(name); lm.hasMatch()) {
            if (!claimDead || present.contains(lm.captured(1) + QStringLiteral(".json")))
                continue;
            const QString path = d.filePath(name);
            if (QFileInfo(path).lastModified().secsTo(QDateTime::currentDateTime())
                    <= kLoneLockAgeSecs)
                continue;
            if (const int fd = tryLock(path); fd >= 0) {
                QFile::remove(path);
                ::close(fd);
            }
            continue;
        }
        const auto jm = rxJson.match(name);
        if (!jm.hasMatch()) continue;
        const QString stem = jm.captured(1);
        const QString path = d.filePath(name);

        QHash<QString, ToolCounter> tools;
        QHash<QString, qint64> bytes;
        QString why;
        if (!readSnapshotFile(path, &tools, &bytes, &why)) {
            all.skipped.append(name + QStringLiteral(": ") + why);
            continue;
        }
        addSnapshotTo(all, stem, tools, bytes);
        if (!claimDead || !dead || !heldLocks) continue;

        const QString lockPath = d.filePath(stem + QStringLiteral(".lock"));
        if (QFileInfo::exists(lockPath)) {
            const int fd = tryLock(lockPath);
            if (fd < 0) continue;                      // live, or claimed elsewhere
            // INV-10 — re-read under the lock: another reader may have
            // released (and unlinked) this stem since the first read.
            if (!readSnapshotFile(path, &tools, &bytes, &why)) { ::close(fd); continue; }
            heldLocks->append(fd);
            addSnapshotTo(*dead, stem, tools, bytes);
        } else {
            // No lock file: dead. Claimed by an atomic rename, so only the
            // reader whose rename wins folds it.
            const QString claimed = d.filePath(stem + claimedSuffix);
            if (!QFile::rename(path, claimed)) continue;
            if (!readSnapshotFile(claimed, &tools, &bytes, &why)) {
                QFile::rename(claimed, path);          // put it back as found
                continue;
            }
            addSnapshotTo(*dead, stem, tools, bytes);
        }
    }
    return all;
}

void releaseClaimed(const QString &dir, const PeerUsage &dead,
                    const QList<int> &heldLocks) {
    const QDir d(dir);
    const QString claimedSuffix =
        QStringLiteral(".claimed-%1").arg(QCoreApplication::applicationPid());
    for (const QString &stem : dead.stems) {
        QFile::remove(d.filePath(stem + QStringLiteral(".json")));
        QFile::remove(d.filePath(stem + claimedSuffix));
        QFile::remove(d.filePath(stem + QStringLiteral(".lock")));
    }
    for (const int fd : heldLocks) ::close(fd);
}

bool foldPeerUsage(const PeerUsage &dead, QJsonObject &monthly, qint64 &lifetime,
                   QJsonObject &byProject, const QString &month,
                   const QString &nowIso) {
    bool changed = false;
    if (dead.savedTokens > 0) {
        monthly = foldMonthlyBucket(monthly, month, dead.savedTokens, /*keepMonths=*/24);
        lifetime += dead.savedTokens;
        changed = true;
    }
    bool folded = false;
    for (auto it = dead.savedTokensByProject.cbegin();
         it != dead.savedTokensByProject.cend(); ++it) {
        if (it.value() <= 0) continue;
        byProject = foldProjectBucket(byProject, it.key(), it.value(), month, nowIso,
                                      /*keepMonths=*/24);
        folded = true;
    }
    if (folded) {
        byProject = pruneProjectBuckets(byProject, /*keepProjects=*/64);
        changed = true;
    }
    return changed;
}

}  // namespace TokenUsageEngine
