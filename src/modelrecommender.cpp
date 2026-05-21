// ANTS-1226 — ModelRecommender implementation.
#include "modelrecommender.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>

namespace ModelRecommender {

namespace {

const QStringList kPlanKeywords{
    QStringLiteral("spec"),
    QStringLiteral("design"),
    QStringLiteral("architecture"),
    QStringLiteral("review"),
    QStringLiteral("plan"),
    QStringLiteral("refactor"),
};

bool hasPlanKeyword(const QString &text) {
    const QString lower = text.toLower();
    for (const QString &kw : kPlanKeywords) {
        if (lower.contains(kw)) return true;
    }
    return false;
}

}  // namespace

Result score(const QString &transcriptPath)
{
    Result def;
    def.reason = QStringLiteral("default");

    QFile f(transcriptPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return def;

    // INV-7: tail-read at most 512 KB. Store the flag BEFORE close()
    // since QFile::size() returns 0 after close on most platforms.
    constexpr qint64 kMaxTailBytes = 512LL * 1024LL;
    const bool didTailSeek = (f.size() > kMaxTailBytes);
    if (didTailSeek)
        f.seek(f.size() - kMaxTailBytes);

    QStringList allLines;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (!line.isEmpty()) allLines.append(line);
    }
    f.close();

    // Discard the first line when tail-seeking — it may be truncated.
    if (didTailSeek && !allLines.isEmpty())
        allLines.removeFirst();

    // INV-8: collect the last 20 assistant turns. Walk in reverse, append,
    // then reverse once — O(n) total (avoids prepend's O(n) per call).
    QVector<QJsonObject> turns;
    turns.reserve(20);
    for (int i = allLines.size() - 1; i >= 0 && turns.size() < 20; --i) {
        const QJsonObject obj =
            QJsonDocument::fromJson(allLines[i].toUtf8()).object();
        if (obj.value(QStringLiteral("type")).toString() ==
                QStringLiteral("assistant")) {
            turns.append(obj);
        }
    }
    std::reverse(turns.begin(), turns.end());

    if (turns.isEmpty()) return def;

    // Read current model from most-recent assistant turn's message.model.
    // This field is always present in real Claude Code transcripts.
    def.currentModel =
        turns.last()
            .value(QStringLiteral("message")).toObject()
            .value(QStringLiteral("model")).toString();

    // --- extract features ---
    int fileWriteCount = 0;
    bool planKeyword   = false;
    QSet<QString> toolNames;
    qint64 totalMsgLen = 0;
    int msgCount       = 0;

    for (const QJsonObject &turn : turns) {
        const QJsonArray content =
            turn.value(QStringLiteral("message"))
                .toObject()
                .value(QStringLiteral("content"))
                .toArray();
        for (const QJsonValue &cv : content) {
            const QJsonObject c = cv.toObject();
            const QString type = c.value(QStringLiteral("type")).toString();
            if (type == QStringLiteral("tool_use")) {
                const QString name =
                    c.value(QStringLiteral("name")).toString();
                toolNames.insert(name);
                if (name == QStringLiteral("Edit") ||
                        name == QStringLiteral("Write")) {
                    ++fileWriteCount;
                }
            } else if (type == QStringLiteral("text")) {
                const QString text =
                    c.value(QStringLiteral("text")).toString();
                totalMsgLen += text.length();
                ++msgCount;
                if (hasPlanKeyword(text)) planKeyword = true;
            }
        }
    }

    const int toolDiversity = toolNames.size();
    const double avgLen =
        (msgCount > 0) ? static_cast<double>(totalMsgLen) / msgCount : 0.0;

    // --- score ---
    int sc = 0;
    QString reasons;
    if (fileWriteCount >= 4)     { sc += 2; reasons += QStringLiteral("many_writes "); }
    if (toolDiversity >= 6)      { sc += 1; reasons += QStringLiteral("tool_diversity "); }
    if (planKeyword)             { sc += 2; reasons += QStringLiteral("plan_keyword "); }
    if (avgLen >= 500.0)         { sc += 1; reasons += QStringLiteral("long_prompts "); }
    if (fileWriteCount == 0 &&
            toolDiversity <= 2)  { sc -= 2; reasons += QStringLiteral("mechanical "); }

    Result r;
    r.currentModel = def.currentModel;
    if (sc >= 3) {
        r.tier   = Tier::Opus;
        r.reason = reasons.trimmed();
    } else if (sc <= -1) {
        r.tier   = Tier::Haiku;
        r.reason = reasons.trimmed();
    } else {
        r.tier   = Tier::Sonnet;
        r.reason = reasons.trimmed();
    }
    return r;
}

QString tierName(Tier tier)
{
    switch (tier) {
    case Tier::Haiku: return QStringLiteral("haiku");
    case Tier::Opus:  return QStringLiteral("opus");
    default:          return QStringLiteral("sonnet");
    }
}

Tier tierFromModelId(const QString &modelId)
{
    if (modelId.contains(QStringLiteral("haiku"), Qt::CaseInsensitive))
        return Tier::Haiku;
    if (modelId.contains(QStringLiteral("opus"),  Qt::CaseInsensitive))
        return Tier::Opus;
    return Tier::Sonnet;
}

}  // namespace ModelRecommender
