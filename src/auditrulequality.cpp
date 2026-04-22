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
    if (ruleId.isEmpty()) return;
    FireRecord r;
    r.ruleId = ruleId;
    r.lineText = lineText;
    r.timestamp = QDateTime::currentDateTime();
    m_fires.append(r);
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
    save();  // Suppressions are user-initiated and rare; persist immediately.
}

QVector<RuleQualityTracker::RuleStats> RuleQualityTracker::report() const {
    const QDateTime cutoff30d = QDateTime::currentDateTime().addDays(-30);

    QHash<QString, RuleStats> rows;

    for (const FireRecord &fr : m_fires) {
        RuleStats &s = rows[fr.ruleId];
        s.ruleId = fr.ruleId;
        ++s.firesAllTime;
        if (fr.timestamp >= cutoff30d) ++s.fires30d;
        if (!s.lastFire.isValid() || fr.timestamp > s.lastFire)
            s.lastFire = fr.timestamp;
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
    if (m_projectPath.isEmpty()) return;

    // Re-run prune logic on a const copy so save() can be const.
    QVector<FireRecord> firesOut = m_fires;
    QVector<SuppressRecord> suppOut = m_suppressions;

    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-RETENTION_DAYS);
    firesOut.erase(std::remove_if(firesOut.begin(), firesOut.end(),
        [cutoff](const FireRecord &r) { return r.timestamp < cutoff; }), firesOut.end());
    suppOut.erase(std::remove_if(suppOut.begin(), suppOut.end(),
        [cutoff](const SuppressRecord &r) { return r.timestamp < cutoff; }), suppOut.end());

    // Tail-clamp at MAX_RECORDS — keeps the file bounded on pathological
    // days even within the 90-day window.
    if (firesOut.size() > MAX_RECORDS)
        firesOut.remove(0, firesOut.size() - MAX_RECORDS);
    if (suppOut.size() > MAX_RECORDS)
        suppOut.remove(0, suppOut.size() - MAX_RECORDS);

    QJsonArray firesJson;
    for (const FireRecord &r : firesOut) {
        QJsonObject o;
        o["rule"] = r.ruleId;
        o["line"] = r.lineText;
        o["ts"]   = r.timestamp.toString(Qt::ISODate);
        firesJson.append(o);
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
    root["schema_version"] = 1;
    root["fires"]          = firesJson;
    root["suppressions"]   = suppJson;

    // QSaveFile: write to a sibling temp file, rename atomically on commit().
    // Prevents torn writes from corrupting the long-lived quality history on
    // crash / kill -9 between recordFire calls.
    QSaveFile f(m_path);
    if (f.open(QIODevice::WriteOnly)) {
        setOwnerOnlyPerms(f);
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

void RuleQualityTracker::reload() {
    m_fires.clear();
    m_suppressions.clear();
    load();
}

void RuleQualityTracker::load() {
    QFile f(m_path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return;
    const QJsonObject root = doc.object();

    for (const QJsonValue &v : root.value("fires").toArray()) {
        const QJsonObject o = v.toObject();
        FireRecord r;
        r.ruleId = o.value("rule").toString();
        r.lineText = o.value("line").toString();
        r.timestamp = QDateTime::fromString(o.value("ts").toString(), Qt::ISODate);
        if (!r.ruleId.isEmpty() && r.timestamp.isValid())
            m_fires.append(r);
    }
    for (const QJsonValue &v : root.value("suppressions").toArray()) {
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

