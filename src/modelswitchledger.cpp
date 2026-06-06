// ANTS-1735 — see modelswitchledger.h.
#include "modelswitchledger.h"

#include <algorithm>     // ANTS-1891 — std::max in computeOutcome

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include "secureio.h"      // setOwnerOnlyPerms, ensurePrivateDir
#include "configbackup.h"  // ConfigWriteLock — ANTS-1989

namespace ModelSwitchLedger {

namespace {

QByteArray serialize(const Record &rec) {
    return QJsonDocument(toJson(rec)).toJson(QJsonDocument::Compact);
}

QList<QByteArray> readRawLines(const QString &path) {
    QList<QByteArray> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QByteArray all = f.readAll();
    for (const QByteArray &ln : all.split('\n'))
        if (!ln.trimmed().isEmpty()) out.append(ln);
    return out;
}

bool isPendingLine(const QByteArray &ln) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(ln, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    return doc.object().value(QStringLiteral("outcome")).toObject()
        .value(QStringLiteral("pending")).toBool(false);
}

bool writeLinesAtomic(const QString &path, const QList<QByteArray> &lines) {
    const QFileInfo fi(path);
    // ANTS-1988 — create the cache dir private (0700) with no world-readable
    // mkpath-then-chmod window. The ledger's dir listing would otherwise leak
    // its name + mtimes (switch cadence) to other local users.
    if (!ensurePrivateDir(fi.absolutePath())) return false;
    QSaveFile sf(path);
    if (!sf.open(QIODevice::WriteOnly)) return false;
    for (const QByteArray &ln : lines) {
        if (sf.write(ln) != ln.size() || sf.write("\n", 1) != 1) {
            sf.cancelWriting();
            return false;
        }
    }
    // Tighten perms on the temp file before the atomic rename so the final
    // ledger is never briefly group/world-readable (no create-then-chmod window).
    setOwnerOnlyPerms(sf);
    return sf.commit();
}

}  // namespace

QJsonObject toJson(const Record &r) {
    QJsonObject oc;
    oc[QStringLiteral("turns_on_to_tier")]              = r.outcome.turnsOnToTier;
    oc[QStringLiteral("user_override_within_5_turns")]  = r.outcome.userOverrideWithin5;
    oc[QStringLiteral("correction_signal_within_5_turns")] = r.outcome.correctionSignalWithin5;
    oc[QStringLiteral("under_route_signal_within_5_turns")] = r.outcome.underRouteSignalWithin5;
    // ANTS-1891 — new positive signal; snake_case matches the existing keys,
    // omits the _within_N_turns suffix since this is a quiet-window measurement.
    oc[QStringLiteral("session_cleanly_ended_on_new_tier")] = r.outcome.sessionCleanlyEndedOnNewTier;
    // ANTS-1957 — narrow direction-aware regret signal (see Outcome struct).
    oc[QStringLiteral("override_undid_downgrade")]      = r.outcome.overrideUndidDowngrade;
    oc[QStringLiteral("pending")]                       = r.outcome.pending;

    QJsonObject o;
    o[QStringLiteral("ts")]           = r.ts;
    o[QStringLiteral("session_id")]   = r.sessionId;
    o[QStringLiteral("project")]      = r.project;
    o[QStringLiteral("from_tier")]    = r.fromTier;
    o[QStringLiteral("to_tier")]      = r.toTier;
    o[QStringLiteral("score_reason")] = r.scoreReason;
    o[QStringLiteral("trigger")]      = r.trigger;
    o[QStringLiteral("epoch")]        = r.epoch;   // ANTS-1941
    o[QStringLiteral("outcome")]      = oc;
    return o;
}

Record fromJson(const QJsonObject &o) {
    Record r;
    r.ts          = o.value(QStringLiteral("ts")).toString();
    r.sessionId   = o.value(QStringLiteral("session_id")).toString();
    r.project     = o.value(QStringLiteral("project")).toString();
    r.fromTier    = o.value(QStringLiteral("from_tier")).toString();
    r.toTier      = o.value(QStringLiteral("to_tier")).toString();
    r.scoreReason = o.value(QStringLiteral("score_reason")).toString();
    r.trigger     = o.value(QStringLiteral("trigger")).toString();
    // ANTS-1941 — missing key defaults to 0 (pre-epoch record). Explicit
    // .toInt(0) mirrors the ANTS-1891 .toBool(false) idiom below.
    r.epoch       = o.value(QStringLiteral("epoch")).toInt(0);
    const QJsonObject oc = o.value(QStringLiteral("outcome")).toObject();
    r.outcome.turnsOnToTier           = oc.value(QStringLiteral("turns_on_to_tier")).toInt();
    r.outcome.userOverrideWithin5     = oc.value(QStringLiteral("user_override_within_5_turns")).toBool();
    r.outcome.correctionSignalWithin5 = oc.value(QStringLiteral("correction_signal_within_5_turns")).toBool();
    r.outcome.underRouteSignalWithin5 = oc.value(QStringLiteral("under_route_signal_within_5_turns")).toBool();
    // ANTS-1891 — INV-10: missing field defaults to false (pre-1891 records
    // never observed the signal). `toBool(false)` is explicit for clarity.
    r.outcome.sessionCleanlyEndedOnNewTier =
        oc.value(QStringLiteral("session_cleanly_ended_on_new_tier")).toBool(false);
    // ANTS-1957 — missing field defaults to false. Pre-1957 records lack it and
    // carry epoch < 2, so they are excluded from the trust signal anyway (the
    // epoch boundary, not this default, is what prevents old-semantics mixing).
    r.outcome.overrideUndidDowngrade =
        oc.value(QStringLiteral("override_undid_downgrade")).toBool(false);
    // Absent outcome → pending (a record with no measured outcome is in-flight,
    // hence pinned from eviction).
    r.outcome.pending = oc.value(QStringLiteral("pending")).toBool(true);
    return r;
}

QList<QByteArray> evictToCap(QList<QByteArray> lines, qint64 capBytes) {
    auto total = [&lines]() -> qint64 {
        qint64 t = 0;
        for (const QByteArray &l : lines) t += l.size() + 1;  // + '\n'
        return t;
    };
    while (total() > capBytes && lines.size() > 1) {
        int victim = -1;
        // Oldest non-pending line, never the newest (last) — pending records are
        // pinned, lines removed whole (no mid-line truncation). INV-10.
        for (int i = 0; i < lines.size() - 1; ++i) {
            if (!isPendingLine(lines.at(i))) { victim = i; break; }
        }
        if (victim < 0) break;   // only pinned records + newest remain
        lines.removeAt(victim);
    }
    return lines;
}

bool appendRecord(const QString &path, const Record &rec, qint64 capBytes) {
    // ANTS-1989 — serialize the read-modify-write against a concurrent Ants
    // instance. Without the lock, two processes both read the file, both append
    // their record, and the last atomic rename silently drops the other's row.
    // ConfigWriteLock polls flock(2) for up to 5 s; a timeout means a hung/stale
    // holder (the RMW itself is microseconds), so we proceed best-effort rather
    // than drop the record — a vanishingly rare race beats losing trust-signal data.
    ConfigWriteLock lock(path);
    QList<QByteArray> lines = readRawLines(path);
    lines.append(serialize(rec));
    lines = evictToCap(lines, capBytes);
    return writeLinesAtomic(path, lines);
}

QList<Record> readRecords(const QString &path) {
    QList<Record> out;
    for (const QByteArray &ln : readRawLines(path)) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(ln, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
            out.append(fromJson(doc.object()));
    }
    return out;
}

bool writeRecords(const QString &path, const QList<Record> &recs, qint64 capBytes) {
    // ANTS-1989 — same lock as appendRecord so a full rewrite (outcome backfill)
    // can't interleave with a concurrent append's read-modify-write.
    ConfigWriteLock lock(path);
    QList<QByteArray> lines;
    lines.reserve(recs.size());
    for (const Record &r : recs) lines.append(serialize(r));
    lines = evictToCap(lines, capBytes);
    return writeLinesAtomic(path, lines);
}

namespace {

// INV-11 correlation: a transcript /model event is auto-authored iff some auto
// record matches it by tier AND |Δts| ≤ authorWindowMs. Shared by both override
// detectors so they can never drift on what "the controller injected this" means.
bool isAutoAuthored(const ModelEvent &e, const QList<AutoSwitch> &autoRecords,
                    qint64 authorWindowMs) {
    for (const AutoSwitch &a : autoRecords) {
        if (a.toTier == e.tier && qAbs(e.tsMs - a.tsMs) <= authorWindowMs)
            return true;
    }
    return false;
}

}  // namespace

bool detectUserOverride(const QList<ModelEvent> &windowModelEvents,
                        const QList<AutoSwitch> &autoRecords,
                        qint64 authorWindowMs) {
    for (const ModelEvent &e : windowModelEvents) {
        if (!isAutoAuthored(e, autoRecords, authorWindowMs))
            return true;   // a /model the controller didn't inject
    }
    return false;
}

bool detectCorrectiveOverride(const QList<ModelEvent> &windowModelEvents,
                              const QList<AutoSwitch> &autoRecords,
                              const QString &toTier,
                              qint64 authorWindowMs) {
    const int toRank = tierRank(toTier);
    for (const ModelEvent &e : windowModelEvents) {
        if (isAutoAuthored(e, autoRecords, authorWindowMs)) continue;
        if (tierRank(e.tier) > toRank) return true;   // moved back up → undo
    }
    return false;
}

UnderRoute detectUnderRoute(const QString &toTier,
                            const QStringList &subsequentRecommendedTiers) {
    if (subsequentRecommendedTiers.isEmpty()) return UnderRoute::Pending;
    const int base = tierRank(toTier);
    const int n = qMin(subsequentRecommendedTiers.size(), kOutcomeWindowTurns);
    for (int i = 0; i < n; ++i) {
        if (tierRank(subsequentRecommendedTiers.at(i)) > base) return UnderRoute::Yes;
    }
    return UnderRoute::No;
}

bool detectCorrection(const QString &firstUserTurnText) {
    // Linear alternation, no nested quantifiers → no catastrophic backtracking.
    static const QRegularExpression re(
        QStringLiteral("\\b(no|wrong|that's not|undo|revert|try again)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(firstUserTurnText).hasMatch();
}

QString aliasFromModelId(const QString &modelId) {
    if (modelId.isEmpty()) return {};
    if (modelId.contains(QStringLiteral("haiku"), Qt::CaseInsensitive))
        return QStringLiteral("haiku");
    if (modelId.contains(QStringLiteral("opus"),  Qt::CaseInsensitive))
        return QStringLiteral("opus");
    return QStringLiteral("sonnet");
}

namespace {

// Detect "/model X" prefix in a user turn's joined text content. Returns the
// alias ("haiku"/"sonnet"/"opus") or empty if not a model command.
QString parseUserModelCommand(const QString &userText) {
    static const QRegularExpression re(
        QStringLiteral("^\\s*/model\\s+(haiku|sonnet|opus)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(userText);
    return m.hasMatch() ? m.captured(1).toLower() : QString();
}

}  // namespace

OutcomeFillResult computeOutcome(
    const QString &fromTier, const QString &toTier,
    qint64 switchTsMs,
    const QList<TranscriptTurn> &postSwitchTurns,
    const QList<AutoSwitch> &subsequentAutoSwitches,
    const QStringList &subsequentRecommendedTiers,
    const Outcome &inputBefore,
    qint64 nowMs)   // ANTS-1891 — clock seam for the quiet-window settlement
{
    OutcomeFillResult res;
    res.outcome = inputBefore;
    res.outcome.pending = true;   // start pending; clear once settled

    const bool isDowngrade = (tierRank(fromTier) > tierRank(toTier))
                          && tierRank(fromTier) >= 0
                          && tierRank(toTier)   >= 0;

    // Slice turns to strictly post-switch (tsMs > switchTsMs). Pre-switch
    // turns may slip in when the caller passes a generous window — guard here.
    QList<TranscriptTurn> turns;
    turns.reserve(postSwitchTurns.size());
    for (const TranscriptTurn &t : postSwitchTurns)
        if (t.tsMs > switchTsMs) turns.append(t);

    // turns_on_to_tier: count contiguous assistant turns whose modelId aliases
    // to `toTier`. Stop at the first divergent assistant turn.
    int onTier = 0;
    int firstDivergentIdx = -1;
    for (int i = 0; i < turns.size(); ++i) {
        if (!turns[i].isAssistant) continue;
        if (aliasFromModelId(turns[i].modelId) == toTier) {
            ++onTier;
        } else {
            firstDivergentIdx = i;
            break;
        }
    }
    res.outcome.turnsOnToTier = onTier;

    // user_override scan — first kOutcomeWindowTurns user turns post-switch
    // whose text starts with "/model X". The INV-11 correlation removes any
    // /model X that an auto record authored within kAuthorWindowMs.
    QList<ModelEvent> userModelEvents;
    {
        int userTurnsSeen = 0;
        for (const TranscriptTurn &t : turns) {
            if (!t.isUser) continue;
            if (userTurnsSeen >= kOutcomeWindowTurns) break;
            ++userTurnsSeen;
            const QString cmd = parseUserModelCommand(t.userText);
            if (!cmd.isEmpty()) {
                ModelEvent ev;
                ev.tier = cmd;
                ev.tsMs = t.tsMs;
                userModelEvents.append(ev);
            }
        }
    }
    res.outcome.userOverrideWithin5 =
        detectUserOverride(userModelEvents, subsequentAutoSwitches);
    // ANTS-1957 — narrow, direction-aware signal for regret. Only meaningful on
    // a downgrade (an "upward" override undoes it); harmlessly false otherwise.
    res.outcome.overrideUndidDowngrade = isDowngrade
        && detectCorrectiveOverride(userModelEvents, subsequentAutoSwitches, toTier);

    // correction signal — first post-switch user turn ONLY, downgrades ONLY.
    if (isDowngrade) {
        for (const TranscriptTurn &t : turns) {
            if (t.isUser) {
                res.outcome.correctionSignalWithin5 =
                    detectCorrection(t.userText);
                break;
            }
        }
    }

    // under_route signal — downgrades only.
    if (isDowngrade) {
        const UnderRoute ur = detectUnderRoute(toTier, subsequentRecommendedTiers);
        res.outcome.underRouteSignalWithin5 = (ur == UnderRoute::Yes);
    }

    // ANTS-1891 — separate scan for `lastAssistantTurnTsMs`. Do NOT fold into
    // the `onTier` loop above: that loop early-breaks at the first divergent
    // assistant, so it tracks the last *on-tier* turn, not the last assistant
    // turn overall. The quiet-window settlement clause below needs the
    // absolute last assistant turn.
    qint64 lastAssistantTurnTsMs = 0;
    for (const TranscriptTurn &t : turns) {
        if (t.isAssistant && t.tsMs > lastAssistantTurnTsMs)
            lastAssistantTurnTsMs = t.tsMs;
    }

    // Settlement: dwell ended (subsequent auto-switch exists) OR ≥ kOutcomeWindowTurns
    // post-switch assistant turns observed OR turns_on_to_tier already capped by
    // a divergent assistant turn (a user-driven /model ended the dwell).
    // ANTS-1891 — additionally: quiet-window settlement for end-of-task 0-turn
    // downgrades. Guarded by `switchTsMs > 0` so a malformed-ts record (whose
    // `ts` failed parseIso8601Ms → 0) can't spuriously settle.
    const bool dwellEndedByNextAuto  = !subsequentAutoSwitches.isEmpty();
    const bool fullWindowObserved    = (onTier >= kOutcomeWindowTurns);
    const bool dwellEndedByDivergence = (firstDivergentIdx >= 0);
    const bool dwellEndedByQuietWindow =
        isDowngrade && switchTsMs > 0
        && (nowMs - std::max(switchTsMs, lastAssistantTurnTsMs)
            >= kCleanEndQuietMs);
    if (dwellEndedByNextAuto || fullWindowObserved
        || dwellEndedByDivergence || dwellEndedByQuietWindow)
        res.outcome.pending = false;

    // ANTS-1891 — sessionCleanlyEndedOnNewTier positive predicate. Settled
    // downgrade with no negative signal AND (turnsOnToTier > 0 OR the
    // quiet-window settled it). Upgrades never set the field (mirrors the
    // INV-12 downgrade-only guard).
    if (isDowngrade && !res.outcome.pending) {
        const bool noNegativeSignal =
            !res.outcome.userOverrideWithin5
         && !res.outcome.correctionSignalWithin5
         && !res.outcome.underRouteSignalWithin5;
        const bool hasPositiveEvidence =
            res.outcome.turnsOnToTier > 0 || dwellEndedByQuietWindow;
        res.outcome.sessionCleanlyEndedOnNewTier =
            noNegativeSignal && hasPositiveEvidence;
    }

    // changed flag — caller flushes only when something differs from input.
    res.changed = (res.outcome.pending != inputBefore.pending)
               || (res.outcome.turnsOnToTier != inputBefore.turnsOnToTier)
               || (res.outcome.userOverrideWithin5 != inputBefore.userOverrideWithin5)
               || (res.outcome.overrideUndidDowngrade != inputBefore.overrideUndidDowngrade)
               || (res.outcome.correctionSignalWithin5 != inputBefore.correctionSignalWithin5)
               || (res.outcome.underRouteSignalWithin5 != inputBefore.underRouteSignalWithin5)
               || (res.outcome.sessionCleanlyEndedOnNewTier
                       != inputBefore.sessionCleanlyEndedOnNewTier);   // ANTS-1891
    return res;
}

QJsonObject statsEnvelope(const QList<Record> &recs, const StatsConfig &cfg) {
    int downgrades = 0, upgrades = 0;
    int opusAvoided = 0, opusRoutedIn = 0;
    int regretCount = 0, underRouteCount = 0, pendingCount = 0;
    int measuredDowngrades = 0;   // non-pending downgrades — regret denominator
    int inconclusiveCount = 0;    // ANTS-1891 — 0-turn, 0-signal settled downgrades
    int cleanEndCount     = 0;    // ANTS-1891 — sessionCleanlyEndedOnNewTier=true
    int toHaiku = 0, toSonnet = 0, toOpus = 0;
    // ANTS-1934 — split upgrades/downgrades by trigger so a reader can answer
    // "is the AUTONOMOUS switcher upgrading?" without conflating manual
    // chip-click / user-/model switches. NOTE: today every appended record
    // carries trigger=="auto" (the chip-click + Undo paths send /model but do
    // NOT append a ledger record — see claudestatuswidgets.cpp), so the
    // `manual` bucket is structurally 0 and `auto` mirrors the flat totals.
    // The split is still load-bearing: it makes that guarantee explicit on the
    // wire (a caller no longer has to *know* the ledger is auto-only), and it
    // future-proofs the envelope if manual switches ever start being recorded.
    int autoUp = 0, autoDown = 0, manualUp = 0, manualDown = 0;

    const QString haiku = QStringLiteral("haiku");
    const QString opus  = QStringLiteral("opus");
    const QString autoTrigger = QStringLiteral("auto");

    for (const Record &r : recs) {
        const int rf = tierRank(r.fromTier);
        const int rt = tierRank(r.toTier);

        if (r.toTier == haiku)                       ++toHaiku;
        else if (r.toTier == QStringLiteral("sonnet")) ++toSonnet;
        else if (r.toTier == opus)                   ++toOpus;

        if (r.outcome.pending) ++pendingCount;

        const bool isDowngrade = (rf >= 0 && rt >= 0 && rt < rf);
        const bool isUpgrade   = (rf >= 0 && rt >= 0 && rt > rf);
        if (isDowngrade) ++downgrades;
        if (isUpgrade)   ++upgrades;

        // ANTS-1934 — same delta, partitioned by who initiated the switch.
        const bool isAuto = (r.trigger == autoTrigger);
        if (isDowngrade) { if (isAuto) ++autoDown; else ++manualDown; }
        if (isUpgrade)   { if (isAuto) ++autoUp;   else ++manualUp; }

        // Turns on the cheaper tier after leaving Opus are Opus turns avoided;
        // turns on Opus after upgrading to it are Opus turns routed in.
        if (r.fromTier == opus && r.toTier != opus)
            opusAvoided += r.outcome.turnsOnToTier;
        if (r.toTier == opus && r.fromTier != opus)
            opusRoutedIn += r.outcome.turnsOnToTier;

        // ANTS-1891 — per § 2.2 bucketing on settled downgrades:
        //   * negative signal = user-override OR correction OR under-route
        //   * positive evidence = turnsOnToTier > 0 OR sessionCleanlyEndedOnNewTier
        //   * measured = negative OR positive; else inconclusive
        //   * regret = negative (now folds under-route in — INV-1)
        //   * under_route_count gates on its single bool (still standalone)
        //   * clean_end_count gates on the new positive signal
        if (isDowngrade && !r.outcome.pending) {
            // ANTS-1957 — regret counts a CORRECTIVE (upward, undo) override, not
            // any manual /model. A lateral / further-down / same-tier switch is
            // not evidence the downgrade was wrong, so it must not inflate regret.
            // (Clean-end credit below stays gated on the BROAD userOverrideWithin5
            // — conservative on the crediting side: don't claim a clean downgrade
            // if the user touched /model at all.)
            const bool hasNegativeSignal =
                r.outcome.overrideUndidDowngrade
             || r.outcome.correctionSignalWithin5
             || r.outcome.underRouteSignalWithin5;
            const bool hasPositiveEvidence =
                r.outcome.turnsOnToTier > 0
             || r.outcome.sessionCleanlyEndedOnNewTier;

            if (r.outcome.underRouteSignalWithin5)
                ++underRouteCount;
            if (r.outcome.sessionCleanlyEndedOnNewTier)
                ++cleanEndCount;

            if (hasNegativeSignal || hasPositiveEvidence) {
                ++measuredDowngrades;
                if (hasNegativeSignal)
                    ++regretCount;       // INV-1 — under-route now folded in
            } else {
                ++inconclusiveCount;     // INV-2 — 0-turn + 0-signals
            }
        }
    }

    const double regretRate = measuredDowngrades > 0
        ? (100.0 * regretCount / measuredDowngrades) : 0.0;
    const double weightedAvoided =                       // INV-5 — ½ per clean end
        static_cast<double>(opusAvoided) + 0.5 * cleanEndCount;

    QJsonObject byTier;
    byTier[QStringLiteral("haiku")]  = toHaiku;
    byTier[QStringLiteral("sonnet")] = toSonnet;
    byTier[QStringLiteral("opus")]   = toOpus;

    // ANTS-1934 — by_trigger:{auto,manual:{upgrades,downgrades}}.
    auto triggerObj = [](int up, int down) {
        QJsonObject o;
        o[QStringLiteral("upgrades")]   = up;
        o[QStringLiteral("downgrades")] = down;
        return o;
    };
    QJsonObject byTrigger;
    byTrigger[QStringLiteral("auto")]   = triggerObj(autoUp, autoDown);
    byTrigger[QStringLiteral("manual")] = triggerObj(manualUp, manualDown);

    // ANTS-1891 — headline format adds the floor phrase (improvement E) and
    // withholds the ratio until measuredDowngrades ≥ kHeadlineFloorMeasured.
    // ANTS-1909 — adds the `dwell=Ns` parenthetical (always) and surfaces
    // near-miss dominant-blocker context on the no-switches + below-floor
    // paths so "no firings yet" reads as "evaluating but blocked", not
    // "feature did nothing". The enrichment is bounded by the optional
    // near-miss fields on StatsConfig — empty values fall back to the
    // pre-1909 wording (back-compat for callers not yet plumbing them).
    const QString scopePhrase = (cfg.scope == QStringLiteral("global"))
        ? QStringLiteral("globally")
        : QStringLiteral("in this project");
    const QString configPhrase = QStringLiteral("floor=%1, dwell=%2s")
        .arg(cfg.floorTier).arg(cfg.minDwellSec);
    auto nearMissSuffix = [&cfg](bool calibrating) -> QString {
        // Only surface when we actually have near-miss telemetry.
        if (cfg.nearMissTotal24h <= 0
            || cfg.nearMissDominantBlocker.isEmpty()) {
            return QString();
        }
        const QString prefix = calibrating
            ? QStringLiteral(", ")    // appended onto an existing clause
            : QStringLiteral(" — ");  // headline ends with "no switches yet"
        return QStringLiteral("%1%2 near-miss%3 in 24 h blocked by %4")
            .arg(prefix)
            .arg(cfg.nearMissTotal24h)
            .arg(cfg.nearMissTotal24h == 1 ? QString() : QStringLiteral("es"))
            .arg(cfg.nearMissDominantBlocker);
    };

    QString headline;
    if (!cfg.switchEnabled) {
        headline = QStringLiteral("auto-switch OFF");
    } else if (recs.isEmpty()) {
        headline = QStringLiteral("auto-switch ON (%1) %2: no switches yet")
            .arg(configPhrase).arg(scopePhrase);
        headline += nearMissSuffix(/*calibrating=*/false);
    } else if (measuredDowngrades < kHeadlineFloorMeasured) {
        headline = QStringLiteral(
            "auto-switch ON (%1) %2: calibrating (%3/%4 measured)")
            .arg(configPhrase).arg(scopePhrase)
            .arg(measuredDowngrades).arg(kHeadlineFloorMeasured);
        headline += nearMissSuffix(/*calibrating=*/true);
    } else {
        headline = QStringLiteral(
            "auto-switch ON (%1) %2: avoided %3 Opus turns "
            "(+%4 clean end), %5 regretted (regret %6%)")
            .arg(configPhrase).arg(scopePhrase)
            .arg(opusAvoided).arg(cleanEndCount).arg(regretCount)
            .arg(QString::number(regretRate, 'f', 1));
    }

    QJsonObject env;
    env[QStringLiteral("ok")]                       = true;
    env[QStringLiteral("switches")]                 = recs.size();
    env[QStringLiteral("downgrades")]               = downgrades;
    env[QStringLiteral("upgrades")]                 = upgrades;
    env[QStringLiteral("opus_turns_avoided")]       = opusAvoided;
    env[QStringLiteral("opus_turns_routed_in")]     = opusRoutedIn;
    env[QStringLiteral("regret_count")]             = regretCount;
    env[QStringLiteral("regret_rate")]              = regretRate;
    env[QStringLiteral("under_route_count")]        = underRouteCount;
    env[QStringLiteral("pending_count")]            = pendingCount;
    env[QStringLiteral("by_tier")]                  = byTier;
    env[QStringLiteral("by_trigger")]               = byTrigger;   // ANTS-1934
    // ANTS-1891 — new envelope fields (additive per INV-9).
    env[QStringLiteral("inconclusive_count")]       = inconclusiveCount;
    env[QStringLiteral("clean_end_count")]          = cleanEndCount;
    env[QStringLiteral("weighted_avoided")]         = weightedAvoided;
    env[QStringLiteral("headline_floor")]           = kHeadlineFloorMeasured;
    env[QStringLiteral("measured_downgrades")]      = measuredDowngrades;
    // ANTS-1960 — callers should check this flag before treating regret_rate
    // as meaningful. The field is always present so callers don't need to
    // second-guess what "no field" means.
    env[QStringLiteral("regret_rate_calibrating")]  =
        measuredDowngrades < kHeadlineFloorMeasured;
    // ANTS-1889 — live switcher config triple + scope echo.
    env[QStringLiteral("auto_model_switch_enabled")] = cfg.switchEnabled;
    env[QStringLiteral("floor_tier")]                = cfg.floorTier;
    env[QStringLiteral("min_dwell_sec")]             = cfg.minDwellSec;
    env[QStringLiteral("scope")]                     = cfg.scope;
    // ANTS-2033 — when the switch is OFF, say WHY so a caller can tell
    // whether flipping it on is even an option. The switch is a single
    // global config key (no per-project opt-out), so the two honest
    // states are: "never_enabled" (the opt-in nudge hasn't been accepted
    // yet) and "user_disabled" (nudge shown, switch left/turned off).
    if (!cfg.switchEnabled) {
        env[QStringLiteral("auto_model_switch_off_reason")] =
            cfg.nudgeShown ? QStringLiteral("user_disabled")
                           : QStringLiteral("never_enabled");
        env[QStringLiteral("auto_model_switch_off_detail")] =
            cfg.nudgeShown
                ? QStringLiteral("auto-switch is disabled; re-enable via "
                                 "Settings \xE2\x86\x92 \"Let Ants pick the "
                                 "Claude model for me\" or set "
                                 "claude.auto_model_switch true")
                : QStringLiteral("auto-switch has never been enabled; the "
                                 "first-run opt-in has not been accepted "
                                 "\xE2\x80\x94 enable via Settings \xE2\x86\x92 "
                                 "\"Let Ants pick the Claude model for me\" or "
                                 "set claude.auto_model_switch true");
    }
    env[QStringLiteral("headline")]                  = headline;
    return env;
}

QJsonObject statsForScope(const QString &ledgerPath,
                          const QString &projectRoot,
                          const StatsConfig &cfg) {
    const bool global = (cfg.scope == QStringLiteral("global"));
    QList<Record> scoped;
    int excludedStaleCount = 0;
    int excludedPreEpochCount = 0;   // ANTS-1941
    // ANTS-1936 — filter by window_days if set (0 = all-time).
    const qint64 nowMs = (cfg.windowDays > 0)
        ? QDateTime::currentMSecsSinceEpoch() : -1;
    const qint64 windowMs = static_cast<qint64>(cfg.windowDays) * 24 * 60 * 60 * 1000;

    for (const Record &r : readRecords(ledgerPath)) {
        if (!global && r.project != projectRoot) continue;
        // ANTS-1941 — epoch filter first, so a pre-epoch record is never also
        // counted as stale (INV-4). Strict `<`: epoch >= minEpoch stays in-scope.
        if (cfg.minEpoch > 0 && r.epoch < cfg.minEpoch) {
            ++excludedPreEpochCount;
            continue;
        }
        // Apply window filter if enabled.
        if (cfg.windowDays > 0 && nowMs > 0) {
            const qint64 tsMs = parseIso8601Ms(r.ts);
            if (tsMs > 0 && nowMs - tsMs > windowMs) {
                ++excludedStaleCount;
                continue;
            }
        }
        scoped.append(r);
    }
    QJsonObject env = statsEnvelope(scoped, cfg);
    // Surface window info for callers to know records were filtered.
    if (cfg.windowDays > 0) {
        env[QStringLiteral("window_days")]        = cfg.windowDays;
        env[QStringLiteral("excluded_stale_count")] = excludedStaleCount;
    }
    // ANTS-1941 — surface epoch filter info (emitted whenever minEpoch > 0, even
    // when the count is 0 — mirrors the window_days pair above).
    if (cfg.minEpoch > 0) {
        env[QStringLiteral("min_epoch")]                = cfg.minEpoch;
        env[QStringLiteral("excluded_pre_epoch_count")] = excludedPreEpochCount;
    }
    return env;
}

// Back-compat thin wrapper — no epoch filter (minEpoch=0; all behaviour
// generations / forensic), but it DOES apply the default 30-day recency window
// (StatsConfig::windowDays=kDefaultStatsWindowDays). Not an all-time view —
// records older than 30 days are dropped (statsForScope, ANTS-1936).
QJsonObject statsForProject(const QString &ledgerPath, const QString &projectRoot) {
    StatsConfig cfg;   // defaults: project scope, windowDays = 30, minEpoch = 0
    return statsForScope(ledgerPath, projectRoot, cfg);
}

int tierRank(const QString &alias) {
    if (alias == QStringLiteral("haiku"))  return 0;
    if (alias == QStringLiteral("sonnet")) return 1;
    if (alias == QStringLiteral("opus"))   return 2;
    return -1;
}

QString defaultLedgerPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
        + QStringLiteral("/ants-terminal/model-switch-ledger.jsonl");
}

QString nowIso8601() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

qint64 parseIso8601Ms(const QString &ts) {
    const QDateTime dt = QDateTime::fromString(ts, Qt::ISODate);
    return dt.isValid() ? dt.toMSecsSinceEpoch() : 0;
}

}  // namespace ModelSwitchLedger
