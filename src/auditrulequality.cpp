#include "auditrulequality.h"

#include "secureio.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

RuleQualityTracker::RuleQualityTracker(const QString &projectPath)
    : m_projectPath(projectPath),
      m_path(projectPath + "/audit_rule_quality.json")
{
    load();
}

void RuleQualityTracker::recordFire(const QString &ruleId, const QString &lineText) {
    Q_UNUSED(lineText);   // ANTS-5085 — a fire is counted, not stored
    if (ruleId.isEmpty()) return;
    const QDate today = QDate::currentDate();
    m_dirty = true;
    // Today's entries are at the tail: m_fireDays is kept in day order.
    for (int i = m_fireDays.size() - 1; i >= 0 && m_fireDays[i].day == today; --i) {
        if (m_fireDays[i].ruleId == ruleId) {
            ++m_fireDays[i].count;
            return;
        }
    }
    m_fireDays.append({ruleId, today, 1});
}

void RuleQualityTracker::recordSuppression(const QString &ruleId,
                                            const QString &dedupKey,
                                            const QString &lineText,
                                            const QString &reason) {
    if (ruleId.isEmpty()) return;
    SuppressRecord r;
    r.ruleId = ruleId;
    r.dedupKey = dedupKey;
    r.lineText = lineText;
    r.reason = reason;
    r.timestamp = QDateTime::currentDateTime();
    m_suppressions.append(r);
    // ANTS-5085 — saved at run end and on close with the fires. Saving here
    // rewrote and fsynced the whole history on the GUI thread per click.
    m_dirty = true;
}

QVector<RuleQualityTracker::RuleStats> RuleQualityTracker::report() const {
    const QDateTime cutoff30d = QDateTime::currentDateTime().addDays(-30);

    QHash<QString, RuleStats> rows;

    for (const FireDay &fd : m_fireDays) {
        RuleStats &s = rows[fd.ruleId];
        s.ruleId = fd.ruleId;
        s.firesAllTime += fd.count;
        if (fd.day >= cutoff30d.date()) s.fires30d += fd.count;
        const QDateTime dayStart(fd.day, QTime(0, 0));   // day precision
        if (!s.lastFire.isValid() || dayStart > s.lastFire)
            s.lastFire = dayStart;
    }

    for (const SuppressRecord &sr : m_suppressions) {
        RuleStats &s = rows[sr.ruleId];
        s.ruleId = sr.ruleId;
        ++s.suppressionsAllTime;
        if (sr.timestamp >= cutoff30d) ++s.suppressions30d;
        if (!s.lastSuppression.isValid() || sr.timestamp > s.lastSuppression)
            s.lastSuppression = sr.timestamp;
    }

    QVector<RuleStats> out;
    out.reserve(rows.size());
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        RuleStats s = it.value();
        if (s.fires30d > 0) {
            // Cap at 100 — suppressions can exceed fires within the
            // window if the user is suppressing findings from older
            // runs. The display is more legible with a hard cap.
            int rate = (100 * s.suppressions30d) / s.fires30d;
            s.fpRate30d = std::min(rate, 100);
        }
        out.append(s);
    }

    // Sort by 30-day FP rate (highest first), then by 30-day fires
    // (highest first). Surfaces the noisiest active rules first.
    std::sort(out.begin(), out.end(),
              [](const RuleStats &a, const RuleStats &b) {
        if (a.fpRate30d != b.fpRate30d) return a.fpRate30d > b.fpRate30d;
        return a.fires30d > b.fires30d;
    });
    return out;
}

// ANTS-1111 — caller of AuditEngine::applyCorroborationShift uses
// this to know which checkIds are "noisy" (≥ fpThreshold % FP rate
// over 30 days, with at least minSamples fires in that window).
QStringList RuleQualityTracker::noisyRuleIds(int fpThreshold, int minSamples) const {
    QStringList out;
    const QVector<RuleStats> rows = report();
    for (const RuleStats &s : rows) {
        if (s.fires30d < minSamples) continue;
        if (s.fpRate30d < fpThreshold) continue;
        out << s.ruleId;
    }
    return out;
}

namespace {
// Longest common substring of two strings. O(n*m) DP — good enough for
// the small samples we feed it (≤ N suppression lines, each ≤ a few
// hundred chars).
QString longestCommonSubstring(const QString &a, const QString &b) {
    const int n = a.size();
    const int m = b.size();
    if (n == 0 || m == 0) return {};
    QVector<int> prev(m + 1, 0);
    QVector<int> curr(m + 1, 0);
    int bestLen = 0;
    int bestEnd = 0;
    for (int i = 1; i <= n; ++i) {
        for (int j = 1; j <= m; ++j) {
            if (a.at(i - 1) == b.at(j - 1)) {
                curr[j] = prev[j - 1] + 1;
                if (curr[j] > bestLen) {
                    bestLen = curr[j];
                    bestEnd = i;
                }
            } else {
                curr[j] = 0;
            }
        }
        std::swap(prev, curr);
        std::fill(curr.begin(), curr.end(), 0);
    }
    return a.mid(bestEnd - bestLen, bestLen);
}

// Check whether a candidate substring has at least one structural
// boundary character. Pure-identifier substrings (e.g. `m_status`) are
// rejected because they tend to suggest project-noun-specific filters
// rather than rule-shape filters.
bool looksStructural(const QString &s) {
    for (QChar c : s) {
        if (c.isSpace()) return true;
        if (c == '(' || c == ')' || c == '{' || c == '}') return true;
        if (c == '[' || c == ']' || c == ';' || c == ',') return true;
        if (c == '"' || c == '\'') return true;
        if (c == '<' || c == '>') return true;
    }
    return false;
}
}  // namespace

QString RuleQualityTracker::suggestTightening(const QString &ruleId,
                                               int maxSamples,
                                               int minLength) const {
    if (ruleId.isEmpty() || maxSamples < 2) return {};

    // Collect the most-recent suppressed line texts for this rule.
    QStringList samples;
    for (auto it = m_suppressions.crbegin(); it != m_suppressions.crend(); ++it) {
        if (it->ruleId != ruleId) continue;
        if (it->lineText.isEmpty()) continue;
        samples.append(it->lineText);
        if (samples.size() >= maxSamples) break;
    }
    if (samples.size() < 2) return {};  // can't LCS with one sample

    // LCS-fold across all samples. Start with the first as the running
    // intersection, then narrow against each subsequent sample.
    QString candidate = samples.first();
    for (int i = 1; i < samples.size(); ++i) {
        candidate = longestCommonSubstring(candidate, samples.at(i));
        if (candidate.size() < minLength) return {};
    }

    candidate = candidate.trimmed();
    if (candidate.size() < minLength) return {};
    if (!looksStructural(candidate)) return {};
    return candidate;
}

void RuleQualityTracker::save() const {
    if (m_projectPath.isEmpty() || !m_dirty) return;

    // Re-run prune logic on a const copy so save() can be const.
    QVector<SuppressRecord> suppOut = m_suppressions;

    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-RETENTION_DAYS);
    suppOut.erase(std::remove_if(suppOut.begin(), suppOut.end(),
        [cutoff](const SuppressRecord &r) { return r.timestamp < cutoff; }), suppOut.end());

    // Tail-clamp at MAX_RECORDS — keeps the file bounded on pathological
    // days even within the 90-day window.
    if (suppOut.size() > MAX_RECORDS)
        suppOut.remove(0, suppOut.size() - MAX_RECORDS);

    // ANTS-5085 — one entry per rule per day; days past retention are dropped.
    QJsonArray fireDaysJson;
    for (const FireDay &fd : m_fireDays) {
        if (fd.day < cutoff.date()) continue;
        QJsonObject o;
        o["rule"]  = fd.ruleId;
        o["day"]   = fd.day.toString(Qt::ISODate);
        o["count"] = fd.count;
        fireDaysJson.append(o);
    }
    QJsonArray suppJson;
    for (const SuppressRecord &r : suppOut) {
        QJsonObject o;
        o["rule"]   = r.ruleId;
        o["key"]    = r.dedupKey;
        o["line"]   = r.lineText;
        o["reason"] = r.reason;
        o["ts"]     = r.timestamp.toString(Qt::ISODate);
        suppJson.append(o);
    }
    QJsonObject root;
    root["schema_version"] = 2;   // ANTS-5085 — fire_days replaced v1's fires
    root["fire_days"]      = fireDaysJson;
    root["suppressions"]   = suppJson;

    // QSaveFile: write to a sibling temp file, rename atomically on commit().
    // Prevents torn writes from corrupting the long-lived quality history on
    // crash / kill -9 between recordFire calls.
    QSaveFile f(m_path);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning("auditrulequality: cannot open %s for write: %s",
                 m_path.toUtf8().constData(), f.errorString().toUtf8().constData());
        return;
    }
    // ANTS-5151 — suppression rows carry source lines, so the history is not
    // saved unless it can be made private.
    if (!setOwnerOnlyPerms(f)) {
        qWarning("auditrulequality: could not make %s owner-only; not saved",
                 m_path.toUtf8().constData());
        f.cancelWriting();
        return;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    // ANTS-1810 — check commit + fsync the dir: a full disk / perms failure
    // silently lost the entire 90-day quality history before this guard.
    if (!f.commit()) {
        qWarning("auditrulequality: commit failed for %s: %s",
                 m_path.toUtf8().constData(), f.errorString().toUtf8().constData());
        return;
    }
    fsyncParentDir(m_path);
    m_dirty = false;
}

void RuleQualityTracker::reload() {
    m_fireDays.clear();
    m_suppressions.clear();
    load();
}

void RuleQualityTracker::load() {
    QFile f(m_path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return;
    const QJsonObject root = doc.object();

    // ANTS-5085 — count fires into (rule, day) buckets: v2 fire_days as
    // stored, and a v1 file's per-fire records one each.
    QHash<QPair<QString, QDate>, int> counts;
    for (const auto &v : root.value("fire_days").toArray()) {
        const QJsonObject o = v.toObject();
        const QString rule = o.value("rule").toString();
        const QDate day = QDate::fromString(o.value("day").toString(), Qt::ISODate);
        const int count = o.value("count").toInt();
        if (!rule.isEmpty() && day.isValid() && count > 0)
            counts[{rule, day}] += count;
    }
    for (const auto &v : root.value("fires").toArray()) {
        const QJsonObject o = v.toObject();
        const QString rule = o.value("rule").toString();
        const QDateTime ts = QDateTime::fromString(o.value("ts").toString(), Qt::ISODate);
        if (!rule.isEmpty() && ts.isValid())
            ++counts[{rule, ts.date()}];
    }
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
        m_fireDays.append({it.key().first, it.key().second, it.value()});
    std::sort(m_fireDays.begin(), m_fireDays.end(),
              [](const FireDay &a, const FireDay &b) { return a.day < b.day; });
    for (const auto &v : root.value("suppressions").toArray()) {
        const QJsonObject o = v.toObject();
        SuppressRecord r;
        r.ruleId = o.value("rule").toString();
        r.dedupKey = o.value("key").toString();
        r.lineText = o.value("line").toString();
        r.reason = o.value("reason").toString();
        r.timestamp = QDateTime::fromString(o.value("ts").toString(), Qt::ISODate);
        if (!r.ruleId.isEmpty() && r.timestamp.isValid())
            m_suppressions.append(r);
    }
}

