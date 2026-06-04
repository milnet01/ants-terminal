// ANTS-1226 — ModelRecommender implementation.
#include "modelrecommender.h"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
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

// ANTS-2010 — word-boundary guard. A bare contains() matched each stem inside
// unrelated words ("plan" in "explanation", "spec" in "especially"/"respect"/
// "unspecified", "design" in "designated"), inflating the Opus plan-score on
// prose that carried no planning intent. Anchor each stem at word boundaries
// with the same idiom hasCommitIntent uses, allowing the regular -s/-ed/-ing
// inflections (true signal: "reviewed the spec", "refactoring the parser")
// while rejecting substrings of unrelated words. Lazy-built once per process;
// linear, no nested quantifiers — no ReDoS surface.
const QVector<QRegularExpression> &planKeywordPatterns() {
    static const QVector<QRegularExpression> pats = [] {
        QVector<QRegularExpression> out;
        out.reserve(kPlanKeywords.size());
        for (const QString &kw : kPlanKeywords) {
            out.append(QRegularExpression(
                QStringLiteral("(?:^|\\W)") + kw +
                    QStringLiteral("(?:s|ed|ing)?(?:\\W|$)"),
                QRegularExpression::CaseInsensitiveOption));
        }
        return out;
    }();
    return pats;
}

bool hasPlanKeyword(const QString &text) {
    for (const QRegularExpression &re : planKeywordPatterns()) {
        if (re.match(text).hasMatch()) return true;
    }
    return false;
}

// ANTS-1916 — extract the tier arg from a `/model <tier>` slash command.
// Claude Code records these as `{type:"system", subtype:"local_command"}`
// events whose top-level `content` string is e.g.
//   <command-name>/model</command-name>
//   <command-message>model</command-message>
//   <command-args>haiku</command-args>
// Returns "haiku" / "sonnet" / "opus" (lower-cased, trimmed) or an empty
// string when the event is not a /model command or carries no arg (the
// user opened the picker without choosing). The model field on assistant
// turns only updates on the NEXT assistant turn, so a /model command that
// is more recent than the last assistant turn is the true current model —
// without this the chip + auto-switcher both lag a full turn behind and
// the switcher re-fires the same tier every dwell window (the thrash bug).
QString modelTierFromLocalCommand(const QJsonObject &ev) {
    if (ev.value(QStringLiteral("type")).toString() != QStringLiteral("system"))
        return {};
    if (ev.value(QStringLiteral("subtype")).toString()
            != QStringLiteral("local_command"))
        return {};
    const QString content = ev.value(QStringLiteral("content")).toString();
    if (!content.contains(QStringLiteral("<command-name>/model</command-name>")))
        return {};
    static const QRegularExpression kArgRe(
        QStringLiteral("<command-args>(.*?)</command-args>"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = kArgRe.match(content);
    if (!m.hasMatch()) return {};
    return m.captured(1).trimmed().toLower();
}

// ANTS-1890 — narrow stem regex over the latest user prompt. Per-keyword
// alternations handle the actual English morphology of each verb (a flat
// `(?:ed|ing|s)?` would miss `committed` / `pushes` / `rebased`):
//
//   commit  → commit | commits | committed | committing   (double-t)
//   push    → push   | pushes  | pushed    | pushing      (-es plural)
//   stage   → stage  | stages  | staged    | staging      (final-e elision)
//   bump    → bump   | bumps   | bumped    | bumping      (regular)
//   rebase  → rebase | rebases | rebased   | rebasing     (final-e elision)
//
// Each compiled as `(?:^|\W)/?<alt>(?:\W|$)`: word-boundary stem, optional
// `/`-prefix for `/commit` slash-command form. Lazy-built once per process.
// Linear; no nested quantifiers — no ReDoS surface (sibling regex idiom
// matches `thinkingLevelFromLatestUserTurn` further down this TU).
const QStringList kCommitIntentAlternations{
    QStringLiteral("commit(?:s|ted|ting)?"),
    QStringLiteral("push(?:es|ed|ing)?"),
    QStringLiteral("stage(?:s|d|ing)?"),
    QStringLiteral("bump(?:s|ed|ing)?"),
    QStringLiteral("rebase(?:s|d|ing)?"),
};

const QVector<QRegularExpression> &commitIntentPatterns() {
    static const QVector<QRegularExpression> pats = [] {
        QVector<QRegularExpression> out;
        out.reserve(kCommitIntentAlternations.size());
        for (const QString &alt : kCommitIntentAlternations) {
            out.append(QRegularExpression(
                QStringLiteral("(?:^|\\W)/?") + alt +
                QStringLiteral("(?:\\W|$)")));
        }
        return out;
    }();
    return pats;
}

}  // namespace

bool hasCommitIntent(const QString &latestUserText) {
    if (latestUserText.isEmpty()) return false;
    for (const QRegularExpression &re : commitIntentPatterns()) {
        if (re.match(latestUserText).hasMatch()) return true;
    }
    return false;
}

double weightForTurnIndex(int idx, int total) {
    if (total <= 1) return 1.0;
    return 1.0 + 2.0 * (static_cast<double>(idx) / (total - 1));
}

Tier tierForScore(int sc, bool mechanical, Tier homeTier)
{
    // Tier is enum class {Haiku=0, Sonnet=1, Opus=2}. Step relative to home in
    // int space, clamp to [Haiku, Opus], re-cast. See ANTS-1974 §Band design.
    const int home = static_cast<int>(homeTier);
    const auto clampTier = [](int v) -> Tier {
        if (v < static_cast<int>(Tier::Haiku)) v = static_cast<int>(Tier::Haiku);
        if (v > static_cast<int>(Tier::Opus))  v = static_cast<int>(Tier::Opus);
        return static_cast<Tier>(v);
    };
    if (sc >= 2)  return clampTier(home + 1);          // up
    if (sc <= -2) return clampTier(home - (mechanical ? 2 : 1));  // strong / mild down
    return homeTier;                                   // neutral
}

Result score(const QString &transcriptPath, Tier homeTier)
{
    Result def;
    def.reason = QStringLiteral("default");
    def.tier   = homeTier;   // ANTS-1974 — absent transcript ⇒ home, not hardcoded Sonnet.

    // ANTS-1927 — memoize the score by (mtime, size). The controller ticks
    // score() every ~2 s per focused tab (and several times per refresh:
    // chip, gate, outcome-fill), yet the transcript only changes when Claude
    // writes a turn. An unchanged file returns the cached Result without
    // re-tail-reading + re-parsing up to 512 KB. A transcript append always
    // grows `size`, so size+mtime keying can never serve a stale parse.
    // GUI-thread only (the controller's QTimer + the read-only stats verb on
    // the same thread) — the static cache is deliberately unsynchronised.
    // Bounded: cleared past kCacheCap distinct paths so a long multi-tab
    // session can't grow it without limit (RAM budget — one entry per live
    // transcript path, a handful at most in practice).
    struct CacheEntry { qint64 mtimeMs = 0; qint64 size = -1; Result result; };
    static QHash<QString, CacheEntry> cache;
    constexpr int kCacheCap = 16;

    const QFileInfo fi(transcriptPath);
    const bool fileExists = fi.exists();
    const qint64 mtimeMs  = fi.lastModified().toMSecsSinceEpoch();
    const qint64 fsize    = fi.size();
    // ANTS-1974 — the home tier shifts the band mapping, so the same transcript
    // scored under two home tiers yields different tiers. Fold homeTier into the
    // cache key so a home change is never served a stale tier.
    const QString cacheKey = transcriptPath + QLatin1Char('\x1f')
                           + QString::number(static_cast<int>(homeTier));
    if (fileExists) {
        const auto it = cache.constFind(cacheKey);
        if (it != cache.constEnd() && it->mtimeMs == mtimeMs && it->size == fsize)
            return it->result;
    }
    // Single store-point for every computed (post-open) return below. Skipped
    // on the open-failure path (def returned directly) so a not-yet-existing
    // transcript is re-probed each tick rather than pinning an empty result.
    const auto memoize = [&](const Result &r) -> Result {
        if (fileExists) {
            if (cache.size() >= kCacheCap) cache.clear();
            cache.insert(cacheKey, {mtimeMs, fsize, r});
        }
        return r;
    };

    QFile f(transcriptPath);
    // ANTS-1756 — open without QIODevice::Text. The tail-read seeks by
    // byte offset (f.size() - kMaxTailBytes); Text-mode CRLF→LF
    // translation would desync read accounting against that byte seek.
    // Siblings (claudebgtasks, claudetasklist) open plain ReadOnly.
    if (!f.open(QIODevice::ReadOnly))
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
    // ANTS-1890 (INV-5): same single pass also collects the latest
    // `{type:"user"}` line's joined text (skipping tool_result-only user
    // lines), feeding the commit-intent hard override. No extra
    // QFile::open / tail-read.
    QVector<QJsonObject> turns;
    turns.reserve(20);
    QString latestUserText;
    bool latestUserFound = false;
    // ANTS-1916 — capture a `/model <tier>` command that is MORE RECENT than
    // the newest assistant turn. Walking in reverse, the first assistant turn
    // hit is the newest; any /model command seen before it (higher index) is
    // newer still, and is the true current model. Guard with isEmpty() so we
    // keep only the most-recent command, and stop honouring it once we pass
    // the newest assistant turn.
    QString pendingModelTier;
    bool sawAssistantTurn = false;
    for (int i = allLines.size() - 1; i >= 0 && turns.size() < 20; --i) {
        const QJsonObject obj =
            QJsonDocument::fromJson(allLines[i].toUtf8()).object();
        const QString type = obj.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("assistant")) {
            turns.append(obj);
            sawAssistantTurn = true;
        } else if (!sawAssistantTurn && pendingModelTier.isEmpty()) {
            const QString tier = modelTierFromLocalCommand(obj);
            if (!tier.isEmpty()) pendingModelTier = tier;
        }
        if (!latestUserFound && type == QStringLiteral("user")) {
            // Joined text-content blocks; skip user lines with no text
            // (e.g. tool_result envelopes — those carry no human prompt).
            const QJsonValue content =
                obj.value(QStringLiteral("message")).toObject()
                   .value(QStringLiteral("content"));
            QStringList parts;
            if (content.isArray()) {
                for (const QJsonValue &cv : content.toArray()) {
                    const QJsonObject c = cv.toObject();
                    if (c.value(QStringLiteral("type")).toString() ==
                            QStringLiteral("text"))
                        parts.append(c.value(QStringLiteral("text")).toString());
                }
            } else if (content.isString()) {
                parts.append(content.toString());
            }
            const QString joined = parts.join(QLatin1Char(' '));
            if (!joined.isEmpty()) {
                latestUserText = joined.toLower();
                latestUserFound = true;
            }
        }
    }
    std::reverse(turns.begin(), turns.end());

    // Capture currentModel from the most-recent assistant turn BEFORE the
    // override branch — the override returns currentModel verbatim and
    // empty is acceptable on a fresh-session commit-intent.
    if (!turns.isEmpty()) {
        def.currentModel =
            turns.last()
                .value(QStringLiteral("message")).toObject()
                .value(QStringLiteral("model")).toString();
        // ANTS-1944 — record the assistant turn's timestamp so the auto-switch
        // gate can tell a stale read from a fresh one. Inline ISO-8601 parse
        // (byte-identical to ModelSwitchLedger::parseIso8601Ms) keeps this a
        // leaf TU — 0 on a missing/unparseable timestamp.
        const QDateTime ts = QDateTime::fromString(
            turns.last().value(QStringLiteral("timestamp")).toString(),
            Qt::ISODate);
        def.currentModelTsMs = ts.isValid() ? ts.toMSecsSinceEpoch() : 0;
    }
    // ANTS-1916 — a /model command newer than the last assistant turn is the
    // real current model. tierFromModelId() substring-matches on the tier
    // name, so the raw "haiku"/"sonnet"/"opus" arg flows through unchanged.
    // Fixes the stale model-state chip AND the auto-switch thrash (the
    // switcher's gate.current no longer lags a turn behind the actuator).
    if (!pendingModelTier.isEmpty()) {
        def.currentModel = pendingModelTier;
        def.currentModelFromCommand = true;   // ANTS-1944 — authoritative
    }

    // INV-2 — commit-intent hard override. Fires AFTER the walk (so
    // currentModel is captured) and BEFORE the empty-window short-circuit
    // (so a fresh-session "commit and push" routes to Haiku). The Haiku
    // target is then subject to ModelAutoSwitch::clampToFloor in the gate.
    if (hasCommitIntent(latestUserText)) {
        Result r;
        r.tier         = Tier::Haiku;
        r.reason       = QStringLiteral("commit_intent");
        r.currentModel = def.currentModel;
        r.currentModelFromCommand = def.currentModelFromCommand;  // ANTS-1944
        r.currentModelTsMs        = def.currentModelTsMs;
        return memoize(r);
    }

    if (turns.isEmpty()) return memoize(def);

    // --- extract features ---
    // ANTS-1890 (INV-4): count-based features (fileWriteCount, avgLen) are
    // recency-weighted via weightForTurnIndex(idx, total) so a tail of
    // recent activity is not drowned by 19 old turns. toolDiversity (set
    // cardinality) and planKeyword (window-wide OR) stay UNWEIGHTED —
    // weighting is meaningless for them. fileWriteCount also stays as an
    // unweighted int so the `fileWriteCount == 0` mechanical-feature
    // predicate keeps its v1 semantics.
    int fileWriteCount      = 0;        // unweighted — mechanical predicate
    double weightedWrites   = 0.0;      // weighted sum, threshold >= 8
    double weightedMsgLen   = 0.0;
    double weightedMsgCount = 0.0;
    bool planKeyword        = false;
    QSet<QString> toolNames;

    const int totalTurns = turns.size();
    for (int t = 0; t < totalTurns; ++t) {
        const double w = weightForTurnIndex(t, totalTurns);
        const QJsonArray content =
            turns[t].value(QStringLiteral("message"))
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
                    weightedWrites += w;
                }
            } else if (type == QStringLiteral("text")) {
                const QString text =
                    c.value(QStringLiteral("text")).toString();
                weightedMsgLen   += w * text.length();
                weightedMsgCount += w;
                if (hasPlanKeyword(text)) planKeyword = true;
            }
        }
    }

    const int toolDiversity = toolNames.size();
    // Weighted-average length: ratio of weighted-total to weighted-count
    // (both scale with the same weights, so the >= 500 threshold is
    // unchanged from v1).
    const double avgLen = (weightedMsgCount > 0.0)
        ? (weightedMsgLen / weightedMsgCount) : 0.0;

    // --- score ---
    // Threshold raises from v1's `>= 4` (equal weights summing to 20) to
    // `>= 8` (v2 weights sum to 40 across a full window — 2× the v1 mass).
    // Calibration is 1:1 only under a uniform distribution; recent
    // clustering trips earlier, old clustering trips later, by design.
    int sc = 0;
    QString reasons;
    // ANTS-1940 — mechanical predicate hoisted to a named bool so the
    // task-shape signal can ride out on the Result (the gate uses it as a
    // safe-downgrade-window hint). Same condition as the score contribution.
    const bool mechanical = (fileWriteCount == 0 && toolDiversity <= 2);
    if (weightedWrites >= 8.0)   { sc += 2; reasons += QStringLiteral("many_writes "); }
    if (toolDiversity >= 6)      { sc += 1; reasons += QStringLiteral("tool_diversity "); }
    if (planKeyword)             { sc += 2; reasons += QStringLiteral("plan_keyword "); }
    if (avgLen >= 500.0)         { sc += 1; reasons += QStringLiteral("long_prompts "); }
    if (mechanical)              { sc -= 2; reasons += QStringLiteral("mechanical "); }

    Result r;
    r.currentModel = def.currentModel;
    r.isMechanical = mechanical;
    r.currentModelFromCommand = def.currentModelFromCommand;  // ANTS-1944
    r.currentModelTsMs        = def.currentModelTsMs;
    // ANTS-1930 — rebalanced thresholds for symmetric upgrades/downgrades
    // (>= 2 up, <= -2 down). ANTS-1974 — the up/neutral/down decision is now
    // RELATIVE to the user's home tier via tierForScore(); home==Sonnet
    // reproduces the pre-1974 Opus/Sonnet/Haiku mapping exactly.
    r.tier   = tierForScore(sc, mechanical, homeTier);
    r.reason = reasons.trimmed();
    return memoize(r);
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

// ANTS-1888 — Inline-directive parser for thinking budgets. Tail-reads the
// transcript like score(), but walks for the most recent `{type:"user"}` line
// rather than aggregating assistant turns. Pure regex match against the
// joined user text — Claude Code's directive set is inline keywords + the
// canonical /ultrathink, /think, /nothink slash forms (mainwindow's Thinking
// Level submenu). Linear regex; no ReDoS surface (fixed alternations, no
// nested quantifiers).
ThinkingLevel thinkingLevelFromLatestUserTurn(const QString &transcriptPath)
{
    QFile f(transcriptPath);
    if (!f.open(QIODevice::ReadOnly)) return ThinkingLevel::Unknown;

    constexpr qint64 kMaxTailBytes = 512LL * 1024LL;
    const bool didTailSeek = (f.size() > kMaxTailBytes);
    if (didTailSeek) f.seek(f.size() - kMaxTailBytes);

    QStringList allLines;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (!line.isEmpty()) allLines.append(line);
    }
    f.close();

    // The first line after a tail seek may be a truncated JSON fragment.
    if (didTailSeek && !allLines.isEmpty()) allLines.removeFirst();

    // Walk in reverse for the most recent {type:"user"} line.
    QString userText;
    for (int i = allLines.size() - 1; i >= 0; --i) {
        const QJsonObject obj =
            QJsonDocument::fromJson(allLines[i].toUtf8()).object();
        if (obj.value(QStringLiteral("type")).toString() !=
                QStringLiteral("user")) continue;
        const QJsonValue content =
            obj.value(QStringLiteral("message")).toObject()
               .value(QStringLiteral("content"));
        QStringList parts;
        if (content.isArray()) {
            for (const QJsonValue &cv : content.toArray()) {
                const QJsonObject c = cv.toObject();
                if (c.value(QStringLiteral("type")).toString() ==
                        QStringLiteral("text"))
                    parts.append(c.value(QStringLiteral("text")).toString());
            }
        } else if (content.isString()) {
            // Some session writers emit a bare-string content; tolerate both.
            parts.append(content.toString());
        }
        userText = parts.join(QLatin1Char(' ')).toLower();
        break;
    }

    if (userText.isEmpty()) return ThinkingLevel::Unknown;

    // Order matters — longest match wins so "think hard" / "ultrathink" beat
    // the substring "think". Word boundaries prevent matches inside identifiers
    // like "rethink" or "thinkpad". Slash-prefixed variants pair with the
    // mainwindow.cpp Thinking-Level submenu (`/ultrathink` / `/think` /
    // `/nothink`); `/nothink` resolves explicitly to Standard rather than
    // Unknown so the chip shows the user's last *active* choice.
    static const QRegularExpression kNoThink(
        QStringLiteral("(?:^|\\W)/?nothink(?:\\W|$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kUltra(
        QStringLiteral("(?:^|\\W)/?ultrathink(?:\\W|$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kThinkHard(
        QStringLiteral("(?:^|\\W)/?think[ -]hard(?:er)?(?:\\W|$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kThink(
        QStringLiteral("(?:^|\\W)/?think(?:\\W|$)"),
        QRegularExpression::CaseInsensitiveOption);

    if (kUltra.match(userText).hasMatch())     return ThinkingLevel::Ultrathink;
    if (kThinkHard.match(userText).hasMatch()) return ThinkingLevel::ThinkHard;
    if (kNoThink.match(userText).hasMatch())   return ThinkingLevel::Standard;
    if (kThink.match(userText).hasMatch())     return ThinkingLevel::Think;
    return ThinkingLevel::Standard;
}

QString thinkingLevelLabel(ThinkingLevel level)
{
    switch (level) {
    case ThinkingLevel::Ultrathink: return QStringLiteral("ultrathink");
    case ThinkingLevel::ThinkHard:  return QStringLiteral("think hard");
    case ThinkingLevel::Think:      return QStringLiteral("think");
    case ThinkingLevel::Standard:   return QStringLiteral("standard");
    case ThinkingLevel::Unknown:    return QString();
    }
    return QString();
}

}  // namespace ModelRecommender
