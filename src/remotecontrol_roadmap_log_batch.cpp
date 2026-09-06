// ANTS-3833 TU 6/17 — Roadmap batch and section write ops.
//
// ANTS-4620 — cut out of TU 5, which had reached INV-6's 6,000-line cap with
// zero headroom. The seam is a member boundary and the slice is contiguous, so
// the concatenation ANTS_RC_SOURCES declares still preserves every member's
// pre-split relative order — which is what INV-3 checks and what every
// two-anchor scrape window depends on. Inserted at its slice position in
// ANTS_RC_SOURCES_REL, immediately after TU 5, and never appended.
#include "remotecontrol.h"
#include "remotecontrol_internal.h"
#include "roadmapfoldin.h"
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QCollator>
#include <QHash>
#include <QSet>
#include <algorithm>

using namespace rcdetail;  // ANTS-3833

// ANTS-4383 — say WHY the id sequence jumped.
//
// A caller reported two consecutive batches of five allocating CFG-0021…0025
// then CFG-0027…0031 — one id issued to nothing. The allocation itself is
// exact (RoadmapFoldIn::allocateIds issues current+1…current+n and writes
// current+n; the batch path's `newCounter = nextId - 1` is the same
// arithmetic, and ANTS-2078 already skips the counter write under
// stable_prefix). What moves the floor is ANTS-2179/ANTS-3450 reconciliation:
// `effCounter` is max(counter, corpus high-water), and corpusHighWater scans
// ROADMAP.md + CHANGELOG.md + docs/roadmap/*.md for `\bPFX-NNNN\b` in the
// WHOLE TEXT — bodies and prose included. So a bullet whose body merely
// MENTIONS a higher id ("superseded by CFG-0026") raises the floor past it,
// and the next batch legitimately starts above.
//
// That is the safety property working: an id that appears anywhere in the
// corpus must never be reissued, because a gap is free and a collision is
// not. It is also invisible — the envelope reported `counter_advanced_to`
// and nothing about what moved it, so a contiguous-ids assumption broke with
// no way to find out why. Which is ANTS-4374's invariant on the write side:
// a number the caller did not ask for has to say where it came from.
//
// ANTS-4631 REVISED the mechanism above and kept the explanation. "Anywhere
// in the text" was too wide: a roadmap documenting its own id format writes
// id-shaped sample text, and one deliberately-absurd `ANTS-9999` in a body
// was read as an allocation, issuing 10000 next and burning ~5,370 ids. The
// scan now counts only lines that DECLARE an id — a top-level list item
// outside a fence — so a mention in prose is text again. A gap is still free
// and a collision still is not; what changed is that a sample id is no longer
// evidence of an allocation. On a MIGRATED project none of this runs at all:
// the store's id columns answer directly.
static void rlExplainCounterFloor(QJsonObject &out, qint64 counterFileValue,
                                  qint64 corpusMax) {
    out[QStringLiteral("counter_floor")]        = corpusMax;
    out[QStringLiteral("counter_file_value")]   = counterFileValue;
    out[QStringLiteral("counter_floor_reason")] = QStringLiteral(
        "ids were allocated above `.roadmap-counter` (%1) because a BULLET "
        "declaring an id as high as %2 already exists in the corpus — "
        "ROADMAP.md, CHANGELOG.md or docs/roadmap/*.md — so the counter file "
        "was behind the committed record. Only declaring lines count: an id "
        "merely mentioned in a body, or shown as an example inside a fence, "
        "is text and does not raise the floor. A gap in the sequence is free "
        "and reissuing a live id is not, so do not assume ids from "
        "consecutive calls are contiguous.")
            .arg(counterFileValue).arg(corpusMax);
}

// ANTS-1690 — roadmap_log op:"flip_batch". Flip N bullets to one
// to_status in a single read + single QSaveFile commit, so a bundle
// close (cold-eyes / indie-review / release sweep) doesn't pay N
// round-trips or race the file watcher across N writes. Each locator
// is {id|anchor|headline|line_range} + optional per-locator `note` and
// `no_anchor` opt-out. Partial success: an unresolvable locator lands
// in skipped[] and the rest still apply. Reuses the same walk/apply
// helpers as the single-flip path (cmdRoadmapLogFlip).
//
// Index-stability: status flips + anchor injection are in-place (no
// line-count change); only appendBodyNote shifts lines. So targets are
// resolved once (one walk) and applied in DESCENDING firstLine order —
// a note inserted below a target only shifts already-applied (lower)
// targets, leaving every not-yet-applied target's captured line valid.
QJsonDocument RemoteControl::cmdRoadmapLogFlipBatch(const QJsonObject &req) {
    auto rlErr = [](const QString &code, const QString &message) {
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = code;
        env["error"] = message;
        return QJsonDocument(env);
    };

    // ANTS-4470 — annotate_batch shares this handler, exactly as op:"annotate"
    // shares cmdRoadmapLogFlip: it is this path with the status write omitted.
    // The parameter shape needed no invention — flip_batch's locators[] has
    // carried an optional per-locator `note` since ANTS-1690, so annotate_batch
    // is that locator set with no `to_status`, which is also precisely how
    // annotate relates to flip.
    const bool annotateMode =
        req.value(QStringLiteral("op")).toString() ==
            QStringLiteral("annotate_batch");
    // Used for every refusal and envelope below, so a caller is never told
    // about an op it did not call.
    const QString opName = annotateMode ? QStringLiteral("annotate_batch")
                                        : QStringLiteral("flip_batch");

    // 1. caller_cwd + to_status (required under flip_batch; refused under
    //    annotate_batch, which leaves every located item's status untouched).
    const QString callerRaw = req.value(QStringLiteral("caller_cwd")).toString();
    if (callerRaw.isEmpty())
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: caller_cwd is required"));
    const QString toStatus = req.value(QStringLiteral("to_status")).toString();
    QString targetEmoji;
    // ANTS-3809 § 2.2 — the same choice as a lifecycle WORD, which is what
    // setItemField(itemPk, "status", …) takes. Resolved here beside the emoji
    // rather than mapped back from it later, so the two cannot disagree.
    QString targetStatusWord;
    if (annotateMode) {
        // Mirrors op:"annotate"'s bad_op_combo rather than ignoring the field:
        // a caller who passes to_status here means to change status, and
        // silently dropping it would leave them believing a flip had happened.
        if (!toStatus.isEmpty())
            return rlErr(QStringLiteral("bad_op_combo"),
                QStringLiteral("roadmap_log: to_status is not accepted under "
                               "op:\"annotate_batch\" — annotate leaves status "
                               "unchanged; use op:\"flip_batch\" with a "
                               "per-locator `note` to change status and "
                               "annotate in one call"));
    }
    else if (toStatus == QStringLiteral("planned")     ||
             toStatus == QStringLiteral("📋")) { targetEmoji = QStringLiteral("📋"); targetStatusWord = QStringLiteral("planned"); }
    else if (toStatus == QStringLiteral("in-progress") ||
             toStatus == QStringLiteral("🚧")) { targetEmoji = QStringLiteral("🚧"); targetStatusWord = QStringLiteral("in-progress"); }
    else if (toStatus == QStringLiteral("shipped")     ||
             toStatus == QStringLiteral("✅")) { targetEmoji = QStringLiteral("✅"); targetStatusWord = QStringLiteral("shipped"); }
    else if (toStatus == QStringLiteral("considered")  ||
             toStatus == QStringLiteral("💭")) { targetEmoji = QStringLiteral("💭"); targetStatusWord = QStringLiteral("considered"); }
    else if (toStatus.isEmpty())
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: to_status is required under "
                           "op:\"flip_batch\""));
    else
        return rlErr(QStringLiteral("bad_status"),
            QStringLiteral("roadmap_log: unknown to_status \"%1\" — expected "
                           "planned / in-progress / shipped / considered")
                .arg(toStatus));

    // 2. locators array (required, non-empty).
    if (!req.value(QStringLiteral("locators")).isArray())
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: op:\"%1\" needs a `locators` "
                           "array of {id|anchor|headline|line_range} objects")
                .arg(opName));
    const QJsonArray locators = req.value(QStringLiteral("locators")).toArray();
    if (locators.isEmpty())
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: `locators` is empty"));

    // 3. resolve caller_cwd → ROADMAP.md.
    const QString callerCanonical = QFileInfo(callerRaw).canonicalFilePath();
    if (callerCanonical.isEmpty())
        return rlErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: caller_cwd \"%1\" does not "
                           "canonicalise to an existing directory")
                .arg(callerRaw));
    const QString roadmapPath = findRoadmapUnder(callerCanonical);
    if (roadmapPath.isEmpty())
        return rlErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: no ROADMAP.md under \"%1\"")
                .arg(callerCanonical));

    // 4. read once.
    QFile rf(roadmapPath);
    if (!rf.open(QIODevice::ReadOnly | QIODevice::Text))
        return rlErr(QStringLiteral("roadmap_read_failed"),
            QStringLiteral("roadmap_log: could not read \"%1\"").arg(roadmapPath));
    const QString markdown = QString::fromUtf8(rf.readAll());
    rf.close();
    const qint64 markdownBytes = markdown.toUtf8().size();
    QStringList lines = markdown.split(QChar('\n'));

    // 5. format detect (GFM first, then ants-v1 — mirrors single-flip).
    // ANTS-2048 — pass-headings check FIRST, unconditionally, mirroring the
    // single-flip path (cmdRoadmapLogFlip). A `#### Pass N.M` heading
    // roadmap routinely carries stray `- [ ]` sub-tasks under its passes;
    // those make walkGfmBullets non-empty, so the OLD code (which gated the
    // pass-headings refusal behind `!isGfm`) let isGfm win and the batch
    // silently walked the GFM path → per-locator bullet_not_found instead
    // of the precise format_mismatch the caller needs. parseBullets
    // classifies the whole doc, so the strong 2+2 pass-headings signal
    // beats a lone checkbox (see detectRoadmapFormat).
    // ANTS-2126 — route to the pass-headings flip_batch writer (replaces
    // the ANTS-2031 format_mismatch refusal). The pre-gate validation
    // (to_status canonical + non-empty locators) matches what it needs.
    // ANTS-4470 — the pass writer takes the mode too, so annotate_batch reaches
    // a pass-headings roadmap rather than silently flipping every located pass
    // to a status the caller never named.
    if (rcBulletsArePassHeadings(rlParse(markdown, callerCanonical)))
        return cmdRoadmapLogPassFlipBatch(req, roadmapPath, markdown,
                                          annotateMode);
    const RoadmapParse::IdFormat batchFlipIdFormat = rlDecl(callerCanonical);
    const bool isGfm = !walkGfmBullets(lines, batchFlipIdFormat).isEmpty();
    // ANTS-3565 — a mixed GFM+ants-v1 roadmap (GFM-majority with appended
    // `- 📋 [ID]` emoji bullets) must resolve locators against EITHER set, so
    // walk ants-v1 whenever the file is big enough — not only when there are
    // zero GFM bullets. `hasV1walk` gates that walk; `isV1` stays the pure
    // ants-v1 signal (no GFM bullets) used for the top-level `format` label
    // and the unrecognised-format refusal.
    const bool hasV1walk = markdownBytes > kRoadmapMinParseableSize;
    bool isV1 = false;
    if (!isGfm && hasV1walk)
        isV1 = !walkAntsV1Bullets(lines).isEmpty();
    if (!isGfm && !isV1) {
        return rlErr(QStringLiteral("unrecognised_format"),
            QStringLiteral("roadmap_log: \"%1\" parsed zero bullets (neither "
                           "GFM-task-list nor ants-v1) — cannot flip_batch")
                .arg(roadmapPath));
    }

    // ANTS-3809 § 2.1 — resolve the write target once, HERE, because § 2.4's
    // per-locator `line_range` refusal below has to know whether this project is
    // store-served. Resolving is not the dispatch: the store-versus-markdown
    // choice happens after phase 1, so every FORMAT refusal — the `anchor`
    // bad_op_combo in the ants-v1 branch — still runs ahead of it, which is what
    // § 2.4 requires.
    RoadmapSource::ReadError srcWhy = RoadmapSource::ReadError::None;
    QString srcErr;
    // ANTS-3863 — fromMemory: the batch walks `markdown` to locate every
    // bullet, so it is already read and needed on both backends.
    auto seamText = RoadmapSource::RoadmapText::fromMemory(markdown);
    const auto writeTarget =
        roadmapWriteTarget(callerCanonical, seamText, &srcWhy, &srcErr);
    {
        QJsonObject refusal;
        if (rcRoadmapSourceRefused(refusal, srcWhy, srcErr))
            return QJsonDocument(refusal);
    }

    auto headlineHash = [](const QString &h) {
        return rcFnv1a64(rcNormaliseHeadline(h));
    };

    // 6. Phase 1 — resolve every locator against ONE walk. A target
    //    carries everything needed to apply later: line indices, note,
    //    and (GFM) whether anchor injection is wanted.
    struct Target {
        int     firstLine     = -1;
        int     headlineLine  = -1;  // GFM note/anchor target; == firstLine for v1
        QString fromStatus;
        QString id;                  // boldId (GFM) or [PREFIX-NNNN] id (v1)
        QString headline;            // ANTS-3809 § 2.2 — the store path's
                                     // step-2 locate key, and the post_bullets
                                     // echo's headline
        QString anchor;              // existing anchor, if any (GFM)
        QString note;
        QStringList noteScrubbed;
        bool    wantAnchor    = false;  // GFM: inject because no id & no anchor
        QString anchorToInject;         // filled in phase 1.5
        int     locatorIndex  = -1;
        bool    isV1Bullet    = false;  // ANTS-3565 — resolved via ants-v1
    };
    QVector<Target> targets;
    QJsonArray skipped;
    QSet<int> claimedFirstLines;  // dedup: a bullet flips at most once

    // ANTS-3565 — walk ants-v1 whenever the file is big enough (not only for a
    // pure-v1 file) so a GFM-majority roadmap's appended emoji bullets are a
    // per-locator fallback below.
    const QVector<GfmBullet>    gbs = isGfm    ? walkGfmBullets(lines, batchFlipIdFormat)
                                               : QVector<GfmBullet>();
    const QVector<AntsV1Bullet> vbs = hasV1walk ? walkAntsV1Bullets(lines)
                                                : QVector<AntsV1Bullet>();

    auto skip = [&](int idx, const QString &code, const QString &err) {
        QJsonObject s;
        s["locator_index"] = idx;
        s["code"]          = code;
        s["error"]         = err;
        skipped.append(s);
    };

    for (int li = 0; li < locators.size(); ++li) {
        const QJsonObject loc = locators.at(li).toObject();
        const QString locId       = loc.value(QStringLiteral("id")).toString();
        const QString locAnchor   = loc.value(QStringLiteral("anchor")).toString();
        const QString locHeadline = loc.value(QStringLiteral("headline")).toString();
        const QJsonArray locRange = loc.value(QStringLiteral("line_range")).toArray();
        const bool noAnchor       = loc.value(QStringLiteral("no_anchor")).toBool(false);
        QString note              = loc.value(QStringLiteral("note")).toString();
        // ANTS-1995 — reject an oversize note before the backtracking scrub.
        if (note.size() > kRcMaxNoteChars) {
            skip(li, QStringLiteral("too_large"),
                 QStringLiteral("note exceeds %1-char cap (got %2)")
                     .arg(kRcMaxNoteChars).arg(note.size()));
            continue;
        }
        QStringList noteScrubbed;
        if (!note.isEmpty()) rcScrubLeakedToolXml(note, noteScrubbed);
        // ANTS-4470 — an annotate with no note writes nothing, which op:
        // "annotate" refuses outright (missing_field). Here it is refused PER
        // LOCATOR into skipped[], because that is this op's failure model: one
        // noteless locator must not cost the batch its other closures. When
        // every locator is noteless the all-failed path below still fires, so
        // the whole-call refusal is preserved for the whole-call mistake.
        if (annotateMode && note.isEmpty()) {
            skip(li, QStringLiteral("missing_field"),
                 QStringLiteral("op:\"annotate_batch\" requires a non-empty "
                                "`note` on every locator — this one has none, "
                                "and an annotate without a note writes nothing"));
            continue;
        }
        // ANTS-4549 — per LOCATOR, so one bad note does not cost the batch the
        // other closures; the same guard op:"flip"/"annotate" runs above.
        {
            QString shadowErr;
            if (rlNoteDeclaresTrailer(note, &shadowErr)) {
                skip(li, QStringLiteral("body_shadowed"), shadowErr);
                continue;
            }
        }
        // ANTS-4532 — after the guard above, never before it.
        note = rlWrapNote(note);

        const bool hasRange = (locRange.size() == 2);
        if (locId.isEmpty() && locAnchor.isEmpty() &&
            locHeadline.isEmpty() && !hasRange) {
            skip(li, QStringLiteral("missing_field"),
                 QStringLiteral("locator needs one of id / anchor / "
                                "headline / line_range"));
            continue;
        }

        // ANTS-3809 § 2.4 — `line_range` cannot be served by the store.
        // ANTS-3793 § 2.1.1 fills firstLine with 0 on the store path, so the
        // predicate `firstLine + 1 >= a` is true for EVERY bullet whenever the
        // range starts at line 1 — a locator that silently flips the whole
        // roadmap, strictly worse than one that matches nothing. Refused per
        // locator, into the skipped[] this op already returns, so a mixed batch
        // still applies its other locators.
        //
        // The three emptiness guards are what keep § 2.4's OTHER ordering rule
        // intact without duplicating this check in both format branches: a range
        // is the EFFECTIVE locator only when id / anchor / headline are all
        // absent (both branches put it last in precedence), so a locator
        // carrying an `anchor` never reaches here and still gets the ants-v1
        // branch's bad_op_combo — the format refusal § 2.4 requires ahead of the
        // store dispatch.
        if (writeTarget && hasRange && locId.isEmpty() &&
            locAnchor.isEmpty() && locHeadline.isEmpty()) {
            skip(li, QStringLiteral("locator_unsupported"),
                 QStringLiteral("line_range cannot be served by this project's "
                                "roadmap store — locate by id or headline"));
            continue;
        }

        // Collect candidate firstLines for this locator (precedence
        // id > anchor > headline > line_range; range may match many).
        QVector<int> candFirstLines;
        bool isRange = false;
        bool locViaV1 = !isGfm;  // ANTS-3565 — this locator resolved via ants-v1
        if (isGfm) {
            if (!locId.isEmpty()) {
                for (const auto &b : gbs)
                    if (b.boldId == locId) candFirstLines.append(b.firstLine);
            } else if (!locAnchor.isEmpty()) {
                for (const auto &b : gbs)
                    if (b.anchor == locAnchor) candFirstLines.append(b.firstLine);
            } else if (!locHeadline.isEmpty()) {
                const quint64 need = headlineHash(locHeadline);
                for (const auto &b : gbs)
                    if (headlineHash(b.headline) == need)
                        candFirstLines.append(b.firstLine);
            } else {
                isRange = true;
                const int a = locRange.at(0).toInt(), z = locRange.at(1).toInt();
                for (const auto &b : gbs)
                    if (b.firstLine + 1 >= a && b.firstLine + 1 <= z)
                        candFirstLines.append(b.firstLine);
            }
            // ANTS-3565 — mixed-format fallback: no GFM candidate for an
            // id/headline locator → try the appended ants-v1 emoji bullets
            // (anchor is GFM-only). ANTS-3570 extends this to a line_range
            // locator: an emoji bullet appended into a GFM-majority roadmap
            // sits on a line no GFM row occupies, so a range that matched zero
            // GFM rows must still walk the ants-v1 set for a line in-range.
            if (candFirstLines.isEmpty() && locAnchor.isEmpty()) {
                if (!locId.isEmpty()) {
                    for (const auto &b : vbs)
                        if (b.id == locId) candFirstLines.append(b.firstLine);
                } else if (!locHeadline.isEmpty()) {
                    const quint64 need = headlineHash(locHeadline);
                    for (const auto &b : vbs)
                        if (headlineHash(b.headline) == need)
                            candFirstLines.append(b.firstLine);
                } else if (isRange) {
                    const int a = locRange.at(0).toInt(),
                              z = locRange.at(1).toInt();
                    for (const auto &b : vbs)
                        if (b.firstLine + 1 >= a && b.firstLine + 1 <= z)
                            candFirstLines.append(b.firstLine);
                }
                if (!candFirstLines.isEmpty()) locViaV1 = true;
            }
        } else {  // ants-v1
            if (!locAnchor.isEmpty()) {
                // ANTS-3809 § 7 — `line_range` dropped from the advice. It is
                // refused outright on a migrated project (§ 2.4: the store
                // path zeroes firstLine, so a range starting at line 1 would
                // match EVERY bullet), and a migrated project is the only kind
                // the store path serves — so this hint sent a caller from one
                // refusal straight into another.
                skip(li, QStringLiteral("bad_op_combo"),
                     QStringLiteral("anchor locator unsupported on ants-v1 "
                                    "— use id or headline"));
                continue;
            }
            if (!locId.isEmpty()) {
                for (const auto &b : vbs)
                    if (b.id == locId) candFirstLines.append(b.firstLine);
            } else if (!locHeadline.isEmpty()) {
                const quint64 need = headlineHash(locHeadline);
                for (const auto &b : vbs)
                    if (headlineHash(b.headline) == need)
                        candFirstLines.append(b.firstLine);
            } else {
                isRange = true;
                const int a = locRange.at(0).toInt(), z = locRange.at(1).toInt();
                for (const auto &b : vbs)
                    if (b.firstLine + 1 >= a && b.firstLine + 1 <= z)
                        candFirstLines.append(b.firstLine);
            }
        }

        if (candFirstLines.isEmpty()) {
            skip(li, QStringLiteral("bullet_not_found"),
                 QStringLiteral("locator matched zero bullets"));
            continue;
        }
        if (!isRange && candFirstLines.size() > 1) {
            skip(li, QStringLiteral("bullet_ambiguous"),
                 QStringLiteral("locator matched %1 bullets — narrow with id")
                     .arg(candFirstLines.size()));
            continue;
        }

        // Materialise a Target per matched bullet (dedup on firstLine).
        for (const int fl : candFirstLines) {
            if (claimedFirstLines.contains(fl)) continue;
            Target t;
            t.firstLine    = fl;
            t.note         = note;
            t.noteScrubbed = noteScrubbed;
            t.locatorIndex = li;
            t.isV1Bullet   = locViaV1;
            if (!locViaV1) {
                const auto it = std::find_if(gbs.begin(), gbs.end(),
                    [fl](const GfmBullet &b){ return b.firstLine == fl; });
                if (it->insideFenced) {
                    skip(li, QStringLiteral("anchor_unsafe_context"),
                         QStringLiteral("bullet at line %1 is inside a fenced "
                                        "code block — refusing").arg(fl + 1)
                             + rcFenceOpenerHint(it->fenceOpenLine));
                    continue;
                }
                t.headlineLine = it->headlineLine;
                t.fromStatus   = it->status;
                t.id           = it->boldId;
                t.headline     = it->headline;
                t.anchor       = it->anchor;
                t.wantAnchor   = !noAnchor && it->boldId.isEmpty() &&
                                 it->anchor.isEmpty();
            } else {
                const auto it = std::find_if(vbs.begin(), vbs.end(),
                    [fl](const AntsV1Bullet &b){ return b.firstLine == fl; });
                if (it->insideFenced) {
                    skip(li, QStringLiteral("anchor_unsafe_context"),
                         QStringLiteral("bullet at line %1 is inside a fenced "
                                        "code block — refusing").arg(fl + 1)
                             + rcFenceOpenerHint(it->fenceOpenLine));
                    continue;
                }
                t.headlineLine = fl;
                t.fromStatus   = it->status;
                t.id           = it->id;
                t.headline     = it->headline;
            }
            claimedFirstLines.insert(fl);
            targets.append(t);
        }
    }

    if (targets.isEmpty()) {
        QJsonObject out;
        // ANTS-4109 — nothing resolved is a refusal, not a partial success.
        // Partial success is the documented shape when SOME locators apply;
        // with none applied there is no rest to still apply, and ok:true read
        // as "flipped three bullets" to a caller that did not also check
        // flipped_count — so a bundle close reported items shipped that were
        // still planned. skipped[] keeps carrying the per-locator detail.
        const bool allSkipped = !skipped.isEmpty();
        out["ok"]            = !allSkipped;
        if (allSkipped) {
            // The shared code when every locator failed the same way, else
            // the generic locate failure; skipped[] has the per-locator truth.
            QString code = skipped.first().toObject()
                               .value(QStringLiteral("code")).toString();
            for (const QJsonValue &v : std::as_const(skipped)) {
                if (v.toObject().value(QStringLiteral("code")).toString()
                        != code) {
                    code = QStringLiteral("bullet_not_found");
                    break;
                }
            }
            out["code"]  = code;
            out["error"] = QStringLiteral("roadmap_log op:\"%1\": all "
                "%2 locator(s) failed to resolve — nothing was %3")
                    .arg(opName).arg(skipped.size())
                    .arg(annotateMode ? QStringLiteral("annotated")
                                      : QStringLiteral("flipped"));
        }
        out["op"]            = opName;
        out["format"]        = isGfm ? QStringLiteral("gfm")
                                     : QStringLiteral("ants-v1");
        out["file"]          = QStringLiteral("ROADMAP.md");
        out["flipped"]       = QJsonArray();
        out["flipped_count"] = 0;
        out["skipped"]       = skipped;
        out["skipped_count"] = skipped.size();
        return QJsonDocument(out);
    }

    // ANTS-3809 § 2.2 — the store path: N × flip in ONE transaction. It sits
    // after phase 1 because § 2.4 requires every per-locator FORMAT refusal to
    // run ahead of the store-versus-markdown dispatch — the `anchor`
    // bad_op_combo, and the `line_range` locator_unsupported beside it.
    // Locating is shared with the markdown path for the same reason
    // create_section shares its validation: on a migrated project the file is
    // the render's own output, so the same walk finds the same bullet.
    //
    // Placed ahead of 6.5 because anchor injection is a GFM concern and the
    // store path never wants one (ants-v1 has no caret anchors).
    if (writeTarget) {
        RoadmapStore &store = *writeTarget->store;
        const qint64 projectId = writeTarget->projectId;
        const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();

        // Everything one target needs, resolved BEFORE the transaction opens.
        struct StoreTarget {
            const Target *t = nullptr;
            qint64  itemPk = -1;
            RoadmapStore::ItemWrite before;
            QString newBody;
            bool    noteAlreadyPresent = false;
        };
        QVector<StoreTarget> resolved;
        for (const Target &t : targets) {
            // § 2.2's two-step locate. AntsV1Bullet::headline is the post-strip
            // headline with its `**` wrappers removed, which is the form
            // ItemRef::headline holds, so step 2 compares equal without a
            // truncation allowance.
            RoadmapParse::BulletRecord rec;
            rec.id           = t.id;
            rec.headlineFull = t.headline;
            QString pkCode, pkErr;
            const auto itemPk =
                rlStoreItemPk(store, projectId, rec, &pkCode, &pkErr);
            if (!itemPk) {
                // A locator the store cannot resolve lands in skipped[] like any
                // other, so a mixed batch still applies the rest. Only a store
                // failure — which is about the backend and not about one
                // locator — refuses the whole call.
                if (pkCode == QLatin1String("store_failed"))
                    return rlErr(pkCode,
                        QStringLiteral("roadmap_log: %1").arg(pkErr));
                skip(t.locatorIndex, pkCode, pkErr);
                continue;
            }
            QString readErr;
            const auto before = store.readItem(*itemPk, &readErr);
            if (!before)
                return rlErr(QStringLiteral("store_failed"), readErr);

            StoreTarget st;
            st.t      = &t;
            st.itemPk = *itemPk;
            st.before = *before;
            // Idempotent re-annotate, mirroring appendBodyNote()'s
            // noteAlreadyPresent: the markdown path does not append a note the
            // bullet already carries, and a caller re-running a batch must not
            // get a second copy for having migrated.
            st.noteAlreadyPresent =
                !t.note.isEmpty() && before->body.contains(t.note);
            st.newBody = (t.note.isEmpty() || st.noteAlreadyPresent)
                             ? before->body
                             : rlAppendBodyNote(before->body, t.note);
            resolved.append(st);
        }

        if (resolved.isEmpty()) {
            // Every locator was skipped at the store. Same no-write envelope the
            // markdown path returns when phase 1 resolved nothing — no render
            // ran, so no files_written / items_rendered.
            QJsonObject out;
            out["ok"]            = true;
            out["op"]            = QStringLiteral("flip_batch");
            out["format"]        = QStringLiteral("ants-v1");
            out["file"]          = QStringLiteral("ROADMAP.md");
            out["flipped"]       = QJsonArray();
            out["flipped_count"] = 0;
            out["skipped"]       = skipped;
            out["skipped_count"] = skipped.size();
            return QJsonDocument(out);
        }

        // `flipped` is returned in firstLine-ascending order on the markdown
        // path; sorting here rather than at the envelope keeps the write order
        // deterministic too.
        std::sort(resolved.begin(), resolved.end(),
            [](const StoreTarget &a, const StoreTarget &b) {
                return a.t->firstLine < b.t->firstLine;
            });

        // ANTS-3822 § 2.5 — ONE stamp for the whole batch, not one per item: the
        // items share a revision timestamp, while `seq` stays scoped per
        // (item_pk, changed_at) so each item still numbers from its own base.
        HistoryContext hist;
        hist.changedAt = rlHistoryStamp();
        QString deriveCode;               // ANTS-4577 — see the flip site

        const auto mutate = [&](QString *err) -> bool {
            for (const StoreTarget &st : resolved) {
                // ANTS-4470 — `wrote` mirrors the single-item annotate path:
                // it is what decides the `last_modified` stamp, and an
                // annotate whose note is already present writes nothing at
                // all. An item nothing touched must not read as modified today.
                bool wrote = false;
                if (!annotateMode) {
                    if (!store.setItemField(st.itemPk, QStringLiteral("status"),
                                            targetStatusWord,
                                            QStringLiteral("asserted"), err))
                        return false;
                    hist.record(st.itemPk, QStringLiteral("status"),
                                st.before.status, targetStatusWord);
                    // ANTS-4501 § 2.2 — per item, from that item's OWN prior
                    // status: a batch flipping ten to shipped may hold one that
                    // was already shipped, and that one's date must not move.
                    if (!rlStampShipped(store, st.itemPk, st.before.status,
                                        targetStatusWord, err))
                        return false;
                    wrote = true;
                }
                if (st.newBody == st.before.body) {
                    if (wrote && !rlStampModified(store, st.itemPk, err))
                        return false;
                    continue;
                }
                if (!store.setItemField(st.itemPk, QStringLiteral("body"),
                                        st.newBody, QStringLiteral("asserted"), err))
                    return false;
                hist.record(st.itemPk, QStringLiteral("body"),
                            st.before.body, st.newBody);
                // § 2.6 — a body write re-derives every trailer column the
                // request did not supply, which for flip_batch is all five.
                if (!rlDeriveTrailerColumns(store, st.itemPk, st.before,
                                            st.newBody, {}, &hist, err,
                                            nullptr, &deriveCode))
                    return false;
                if (!rlStampModified(store, st.itemPk, err))
                    return false;
            }
            // Flushed once for the whole batch, outside the loop: § 2.3 asks the
            // cap once per OP, so a per-item flush would let item 1's rows land
            // and item 2's be refused — a batch half recorded.
            return rlFlushHistory(store, hist, err);
        };

        RoadmapRender::Outcome outcome;
        QString writeErr;
        const auto r = RoadmapWrite::commitAndRender(
            store, projectId, rcProjectRootFor(callerCanonical), roadmapPath, dryRun,
            mutate, &outcome, &writeErr);
        QJsonObject env;
        if (rcRoadmapWriteRefused(env, r, writeErr, outcome)) {
            if (!deriveCode.isEmpty())        // ANTS-4577
                env[QStringLiteral("code")] = deriveCode;
            return QJsonDocument(env);
        }
        rlAttachHistoryNote(env, store, hist);   // ANTS-3822 § 2.3.1

        const bool echoHeadline = rcReturnHeadlineOnly(req);
        QJsonArray flipped, postBullets;
        for (const StoreTarget &st : resolved) {
            const Target &t = *st.t;
            QJsonObject o;
            // ANTS-4466 — from the STORE, not from `t`, which is the parsed
            // FILE. Same defect and same reasoning as the single-item flip /
            // annotate envelope: on this path the file is the render's output,
            // so the two normally agree, and where they do not the file is the
            // stale one. Under annotate the item keeps its own status, so
            // to_status is that status rather than a batch-wide target.
            const QString storeFromEmoji = rcStatusEmoji(st.before.status);
            o["from_status"] = storeFromEmoji;
            o["to_status"]   = annotateMode ? storeFromEmoji : targetEmoji;
            if (t.fromStatus != storeFromEmoji)
                o["file_status"] = t.fromStatus;
            o["format"]      = QStringLiteral("ants-v1");
            if (!t.id.isEmpty()) o["id"] = t.id;
            // No `line` / `note_line`: a store has no lines (ANTS-3793 INV-2's
            // declared field difference) and the render decides placement.
            if (!t.note.isEmpty()) {
                o["note_appended"] = !st.noteAlreadyPresent;
                if (st.noteAlreadyPresent) o["note_already_present"] = true;
            }
            if (!t.noteScrubbed.isEmpty()) {
                QJsonArray dropped;
                for (const QString &n : t.noteScrubbed) dropped.append(n);
                o["note_scrubbed_params"] = dropped;
            }
            flipped.append(o);
            if (echoHeadline)
                postBullets.append(rcCompactBullet(
                    t.id,
                    rcStatusWord(o.value(QStringLiteral("to_status")).toString()),
                    t.headline));
        }

        env["ok"]        = true;
        env["op"]        = opName;
        env["format"]    = QStringLiteral("ants-v1");
        env["file"]      = QStringLiteral("ROADMAP.md");
        // ANTS-4470 — no batch-wide target status under annotate: each item
        // keeps its own, and a single value here could only be wrong. The
        // per-item `to_status` in `flipped[]` carries the truth.
        if (!annotateMode) env["to_status"] = targetEmoji;
        env["write_path"] = QStringLiteral("render");   // ANTS-4464
        env["flipped"]   = flipped;
        // `would_flip_count` under dry_run, mirroring the markdown preview
        // envelope so a caller's branch on it does not change with the backend.
        if (dryRun) {
            env["dry_run"]          = true;
            env["would_flip_count"] = flipped.size();
        } else {
            env["flipped_count"]    = flipped.size();
        }
        env["skipped"]        = skipped;
        env["skipped_count"]  = skipped.size();
        rcRoadmapWriteFields(env, outcome, dryRun);   // ANTS-4463
        // No `counter`: anchor injection is a GFM concern and ants-v1 never
        // injects one, so nothing here consumes the counter.
        if (echoHeadline) env["post_bullets"] = postBullets;
        return QJsonDocument(env);
    }

    // 6.5 Anchor assignment (GFM injections) — deterministic ascending
    //     firstLine order so counter consumption is stable.
    qint64 newCounter = -1, counterStart = -1;
    QString counterPath;
    {
        QVector<Target*> injectTargets;
        for (Target &t : targets) if (t.wantAnchor) injectTargets.append(&t);
        if (!injectTargets.isEmpty()) {
            std::sort(injectTargets.begin(), injectTargets.end(),
                [](Target *a, Target *b){ return a->firstLine < b->firstLine; });
            QString prefix = req.value(QStringLiteral("prefix_hint")).toString();
            if (prefix.isEmpty()) {
                prefix = QFileInfo(callerCanonical).fileName().left(4).toUpper();
                if (prefix.isEmpty()) prefix = QStringLiteral("ROOT");
            } else {
                static const QRegularExpression rxPrefix(
                    QStringLiteral("^[A-Z][A-Z0-9_-]{0,15}$"));
                if (!rxPrefix.match(prefix).hasMatch())
                    return rlErr(QStringLiteral("bad_op_combo"),
                        QStringLiteral("roadmap_log: prefix_hint \"%1\" does "
                                       "not match ^[A-Z][A-Z0-9_-]{0,15}$")
                            .arg(prefix));
            }
            // ANTS-3350 — beside the RESOLVED roadmap, as op:"append" does.
            counterPath = QFileInfo(roadmapPath).absolutePath() +
                          QLatin1Char('/') +
                          QStringLiteral(".roadmap-counter");
            qint64 counter = 0;
            if (QFile::exists(counterPath)) {
                QFile cf(counterPath);
                if (!cf.open(QIODevice::ReadOnly | QIODevice::Text))
                    return rlErr(QStringLiteral("counter_read_failed"),
                        QStringLiteral("roadmap_log: could not read "
                                       ".roadmap-counter"));
                const QByteArray raw = cf.readAll().trimmed();
                if (!raw.isEmpty()) {
                    bool ok = false;
                    counter = QString::fromUtf8(raw).toLongLong(&ok);
                    if (!ok)
                        return rlErr(QStringLiteral("counter_read_failed"),
                            QStringLiteral("roadmap_log: .roadmap-counter is "
                                           "not a number"));
                }
            }
            counterStart = counter;
            for (Target *t : injectTargets) {
                ++counter;
                t->anchorToInject = prefix.toLower() + QLatin1Char('-') +
                    QStringLiteral("%1").arg(counter, 4, 10, QLatin1Char('0'));
            }
            newCounter = counter;
        }
    }

    // 7. Phase 2 — apply DESCENDING firstLine so note inserts only shift
    //    already-applied (lower) targets. Build result in input order.
    QVector<Target> applyOrder = targets;
    std::sort(applyOrder.begin(), applyOrder.end(),
        [](const Target &a, const Target &b){ return a.firstLine > b.firstLine; });
    QHash<int, QJsonObject> resultByFirstLine;
    QHash<int, QString> headlineByFirstLine;  // ANTS-2089 — post_bullets echo
    for (const Target &t : applyOrder) {
        // Locate the live bullet at t.firstLine and apply.
        QString hlText;  // ANTS-2089 — for the post_bullets compact echo
        // ANTS-3565 — apply per the bullet's OWN format, not the file's
        // dominant one: a mixed roadmap flips its GFM and emoji bullets in the
        // same batch.
        if (!t.isV1Bullet) {
            const QVector<GfmBullet> live = walkGfmBullets(lines, rlDecl(callerCanonical));   // ANTS-3771
            const auto it = std::find_if(live.begin(), live.end(),
                [&t](const GfmBullet &b){ return b.firstLine == t.firstLine; });
            if (it == live.end()) continue;  // should not happen
            hlText = it->headline;
            // ANTS-4470 — the walk still runs under annotate (it supplies
            // hlText and proves the bullet is still there); only the status
            // surgery is skipped. Anchor injection goes with it: it is part of
            // the flip, and op:"annotate" injects none either.
            if (!annotateMode)
                applyGfmFlip(lines, *it, targetEmoji, t.anchorToInject);
        } else {
            const QVector<AntsV1Bullet> live = walkAntsV1Bullets(lines);
            const auto it = std::find_if(live.begin(), live.end(),
                [&t](const AntsV1Bullet &b){ return b.firstLine == t.firstLine; });
            if (it == live.end()) continue;
            hlText = it->headline;
            if (!annotateMode)                       // ANTS-4470
                applyAntsV1Flip(lines, *it, targetEmoji);
        }
        headlineByFirstLine.insert(t.firstLine, hlText);
        int noteLine = -1;
        bool noteAlreadyPresent = false;
        if (!t.note.isEmpty())
            noteLine = appendBodyNote(lines, t.headlineLine, t.note,
                                      &noteAlreadyPresent);

        QJsonObject r;
        r["line"]        = t.firstLine + 1;
        r["from_status"] = t.fromStatus;
        // ANTS-4470 — under annotate the item keeps the status it had.
        r["to_status"]   = annotateMode ? t.fromStatus : targetEmoji;
        // ANTS-3565 — tag emoji bullets resolved via the mixed-format fallback
        // so the caller can tell them apart from the file's dominant GFM rows.
        if (t.isV1Bullet) r["format"] = QStringLiteral("ants-v1");
        if (!t.id.isEmpty())     r["id"]     = t.id;
        if (!t.anchor.isEmpty()) r["anchor"] = t.anchor;
        if (!t.anchorToInject.isEmpty()) {
            r["anchor_injected"] = true;
            r["anchor"]          = t.anchorToInject;
        }
        if (!t.note.isEmpty()) {
            r["note_appended"] = !noteAlreadyPresent;
            r["note_line"]     = noteLine + 1;
            if (noteAlreadyPresent) r["note_already_present"] = true;
        }
        if (!t.noteScrubbed.isEmpty()) {
            QJsonArray dropped;
            for (const QString &n : t.noteScrubbed) dropped.append(n);
            r["note_scrubbed_params"] = dropped;
        }
        resultByFirstLine.insert(t.firstLine, r);
    }

    // 8. write once.
    const QString updated = lines.join(QChar('\n'));

    // ANTS-2136 — dry_run preview: every locator has resolved or landed in
    // `skipped`, the surgery is computed in-memory; return the would-be
    // flipped/skipped sets + bytes WITHOUT writing ROADMAP.md or bumping
    // .roadmap-counter. `would_flip_count` parallels append_batch's
    // would_apply_count.
    if (req.value(QStringLiteral("dry_run")).toBool()) {
        QList<int> previewOrder = claimedFirstLines.values();
        std::sort(previewOrder.begin(), previewOrder.end());
        QJsonArray previewFlipped;
        for (const int fl : previewOrder)
            if (resultByFirstLine.contains(fl))
                previewFlipped.append(resultByFirstLine.value(fl));
        QJsonObject out;
        out["ok"]               = true;
        out["op"]               = opName;
        out["dry_run"]          = true;
        out["format"]           = isGfm ? QStringLiteral("gfm")
                                         : QStringLiteral("ants-v1");
        out["file"]             = QStringLiteral("ROADMAP.md");
        if (!annotateMode) out["to_status"] = targetEmoji;   // ANTS-4470
        out["write_path"]       = QStringLiteral("patch");   // ANTS-4464
        out["flipped"]          = previewFlipped;
        out["would_flip_count"] = previewFlipped.size();
        out["skipped"]          = skipped;
        out["skipped_count"]    = skipped.size();
        out["bytes"]            = static_cast<qint64>(updated.toUtf8().size());
        if (newCounter >= 0 && newCounter != counterStart)
            out["counter"] = newCounter;
        return QJsonDocument(out);
    }

    const qint64 sizeBefore = QFileInfo(roadmapPath).size();   // ANTS-3702
    QSaveFile rw(roadmapPath);
    if (!rw.open(QIODevice::WriteOnly | QIODevice::Text))
        return rlErr(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("roadmap_log: could not open \"%1\" for writing")
                .arg(roadmapPath));
    const QByteArray utf8 = updated.toUtf8();
    if (rw.write(utf8) != utf8.size() || !rw.commit())
        return rlErr(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("roadmap_log: atomic write of \"%1\" failed")
                .arg(roadmapPath));

    // 9. counter once, only if an anchor was injected.
    if (newCounter >= 0 && newCounter != counterStart) {
        QSaveFile cw(counterPath);
        if (!cw.open(QIODevice::WriteOnly | QIODevice::Text))
            return rlErr(QStringLiteral("counter_write_failed"),
                QStringLiteral("roadmap_log: could not open .roadmap-counter "
                               "for writing"));
        const QByteArray cv =
            (QString::number(newCounter) + QChar('\n')).toUtf8();
        if (cw.write(cv) != cv.size() || !cw.commit())
            return rlErr(QStringLiteral("counter_write_failed"),
                QStringLiteral("roadmap_log: atomic write of .roadmap-counter "
                               "failed"));
    }

    // 10. envelope — `flipped` in firstLine-ascending order.
    QList<int> orderedFirstLines = claimedFirstLines.values();
    std::sort(orderedFirstLines.begin(), orderedFirstLines.end());
    QJsonArray flipped;
    // ANTS-2089 — confirm-after compact echo, built in the same
    // firstLine-ascending order as `flipped`.
    const bool echoHeadline = rcReturnHeadlineOnly(req);
    QJsonArray postBullets;
    for (const int fl : orderedFirstLines) {
        if (!resultByFirstLine.contains(fl)) continue;
        const QJsonObject r = resultByFirstLine.value(fl);
        flipped.append(r);
        if (echoHeadline) {
            postBullets.append(rcCompactBullet(
                r.value(QStringLiteral("id")).toString(),
                rcStatusWord(r.value(QStringLiteral("to_status")).toString()),
                headlineByFirstLine.value(fl)));
        }
    }

    QJsonObject out;
    out["ok"]            = true;
    out["op"]            = opName;
    out["format"]        = isGfm ? QStringLiteral("gfm")
                                 : QStringLiteral("ants-v1");
    out["file"]          = QStringLiteral("ROADMAP.md");
    if (!annotateMode) out["to_status"] = targetEmoji;   // ANTS-4470
    out["write_path"]    = QStringLiteral("patch");      // ANTS-4464
    out["flipped"]       = flipped;
    out["flipped_count"] = flipped.size();
    out["skipped"]       = skipped;
    out["skipped_count"] = skipped.size();
    rcSetWriteBytes(out, sizeBefore, static_cast<qint64>(utf8.size()));
    if (newCounter >= 0 && newCounter != counterStart)
        out["counter"] = newCounter;
    if (echoHeadline) out["post_bullets"] = postBullets;
    return QJsonDocument(out);
}

// ANTS-1879 INV-10 — shared bullet-formatting helper. Extracted from
// cmdRoadmapLogAppend's :3293-3344 block so cmdRoadmapLogAppendBatch
// can format each bullet through the same code path. `bulletReq` is
// the per-bullet object (single-bullet path passes the whole `req`).
QString RemoteControl::formatRoadmapBullet(
    const QJsonObject &bulletReq,
    const QString    &idStr,
    const QString    &statusEmoji,
    QStringList      &scrubbedNames,
    int              *unnamedRemovals)
{
    const QString headline =
        bulletReq.value(QStringLiteral("headline")).toString();
    const QString kind =
        bulletReq.value(QStringLiteral("kind")).toString();
    const QString source =
        bulletReq.value(QStringLiteral("source")).toString();

    QString bullet;
    bullet += QStringLiteral("- ") + statusEmoji + QChar(' ') +
              QChar('[') + idStr + QChar(']') + QChar(' ') +
              QStringLiteral("**") +
              rcSanitizeBulletField(headline, 500) +
              QStringLiteral("**\n");

    // ANTS-1551 — defensive scrub of leaked tool-call XML.
    QString body = bulletReq.value(QStringLiteral("body")).toString();
    rcScrubLeakedToolXml(body, scrubbedNames, unnamedRemovals);
    if (!body.isEmpty()) {
        const QStringList lines = body.split(QChar('\n'));
        for (const QString &ln : lines)
            // ANTS-3417 — right-strip so an empty/space-only body line doesn't
            // emit the bare "  " hang indent as trailing whitespace.
            bullet += rcRightStrip(QStringLiteral("  ") + ln) + QChar('\n');
    }
    const QString layman =
        bulletReq.value(QStringLiteral("layman")).toString();
    if (!layman.isEmpty()) {
        bullet += QStringLiteral("  **Layman:** ") +
                  rcSanitizeBulletField(layman, 1000) +
                  QChar('\n');
    }
    bullet += QStringLiteral("  Kind: ") + kind + QStringLiteral(".\n");
    const QJsonArray lanesArr =
        bulletReq.value(QStringLiteral("lanes")).toArray();
    if (!lanesArr.isEmpty()) {
        QStringList laneStrs;
        for (const auto &v : lanesArr) laneStrs.append(v.toString());
        bullet += QStringLiteral("  Lanes: ") +
                  laneStrs.join(QStringLiteral(", ")) +
                  QStringLiteral(".\n");
    }
    // ANTS-3382 — optional Evidence: file paths for image/log-driven
    // bullets. Rendered WITHOUT a trailing period (evidence paths contain
    // dots; a sentence period would read as part of the last path).
    const QJsonArray evidenceArr =
        bulletReq.value(QStringLiteral("evidence")).toArray();
    if (!evidenceArr.isEmpty()) {
        QStringList evStrs;
        for (const auto &v : evidenceArr) {
            // Strip embedded newlines/commas so one path can't break the
            // single-line `Evidence:` field shape.
            QString p = rcSanitizeBulletField(v.toString(), 500);
            p.replace(QChar('\n'), QChar(' '));
            p.replace(QChar(','), QChar(' '));
            p = p.trimmed();
            if (!p.isEmpty()) evStrs.append(p);
        }
        if (!evStrs.isEmpty())
            bullet += QStringLiteral("  Evidence: ") +
                      evStrs.join(QStringLiteral(", ")) +
                      QChar('\n');
    }
    bullet += QStringLiteral("  Source: ") +
              rcSanitizeBulletField(source, 200) +
              QStringLiteral(".\n\n");
    return bullet;
}

// ANTS-1878 — roadmap_log op:"create_section". Splice a new
// ## / ### heading at after_section.lineEnd. No counter touch
// (creating a section does not allocate an id). See docs/specs/ANTS-1878.md.
QJsonDocument RemoteControl::cmdRoadmapLogCreateSection(const QJsonObject &req) {
    auto rlErr = [](const QString &code, const QString &message) {
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = code;
        env["error"] = message;
        return QJsonDocument(env);
    };

    // 1. Required fields.
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    const QString afterSection =
        req.value(QStringLiteral("after_section")).toString();
    const QString title =
        req.value(QStringLiteral("title")).toString();
    // `level` is a JSON number; absence vs explicit 0 both map to 0 here,
    // so distinguish "absent" via contains().
    const bool hasLevel = req.contains(QStringLiteral("level"));
    const int level    = hasLevel
        ? req.value(QStringLiteral("level")).toInt() : 0;

    if (callerRaw.isEmpty())
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: caller_cwd is required"));
    if (afterSection.isEmpty())
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: after_section is required"));
    if (!hasLevel)
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: level is required"));
    if (title.isEmpty())
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: title is required"));

    // INV-3 — level enum.
    if (level != 2 && level != 3)
        return rlErr(QStringLiteral("bad_level"),
            QStringLiteral("roadmap_log: create_section level must be 2 "
                           "or 3 (got %1)").arg(level));

    // 2. Resolve ROADMAP.md.
    const QString callerCanonical =
        QFileInfo(callerRaw).canonicalFilePath();
    if (callerCanonical.isEmpty())
        return rlErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: caller_cwd \"%1\" does not "
                           "canonicalise to an existing directory")
                .arg(callerRaw));
    const QString roadmapPath = findRoadmapUnder(callerCanonical);
    if (roadmapPath.isEmpty())
        return rlErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: no ROADMAP.md under \"%1\"")
                .arg(callerCanonical));

    // 3. Read markdown.
    QFile rf(roadmapPath);
    if (!rf.open(QIODevice::ReadOnly | QIODevice::Text))
        return rlErr(QStringLiteral("roadmap_read_failed"),
            QStringLiteral("roadmap_log: could not read \"%1\"")
                .arg(roadmapPath));
    const QString markdown = QString::fromUtf8(rf.readAll());
    rf.close();

    // INV-9 — unrecognised_format gate (parity with append path).
    const auto preflightBullets = rlParse(markdown, callerCanonical);
    const qint64 markdownBytes = markdown.toUtf8().size();
    if (preflightBullets.isEmpty() &&
        markdownBytes > kRoadmapMinParseableSize) {
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = QStringLiteral("unrecognised_format");
        env["error"] = QStringLiteral(
            "roadmap_log: \"%1\" parsed zero bullets from %2 "
            "bytes — format not recognised; cannot safely splice")
                .arg(roadmapPath).arg(markdownBytes);
        env["path"]            = roadmapPath;
        env["bytes"]           = markdownBytes;
        env["hint"]            = kUnrecognisedFormatHint();
        env["expected_format"] = kUnrecognisedFormatExpected();
        return QJsonDocument(env);
    }
    // ANTS-2031 — heading-format roadmaps have no create_section writer.
    if (rcBulletsArePassHeadings(preflightBullets)) {
        return rcPassHeadingsWriteRefusal(
            roadmapPath, QStringLiteral("create_section"));
    }

    // 4. Locate after_section.
    const auto index = RoadmapIndex::buildIndex(markdown);
    const RoadmapIndex::Section *sec =
        RoadmapIndex::findBySlug(index, afterSection);
    if (!sec) {
        // bad_case parity (canonical_slug echoed if a case-only match exists).
        const QString needCi = afterSection.toLower();
        for (const auto &s : index) {
            if (s.slug.toLower() == needCi && s.slug != afterSection) {
                QJsonObject env;
                env["ok"]             = false;
                env["code"]           = QStringLiteral("bad_case");
                env["error"]          = QStringLiteral(
                    "roadmap_log: after_section slug case mismatch: "
                    "\"%1\" — did you mean \"%2\"?")
                        .arg(afterSection, s.slug);
                env["canonical_slug"] = s.slug;
                return QJsonDocument(env);
            }
        }
        return rlErr(QStringLiteral("bad_section"),
            QStringLiteral("roadmap_log: unknown after_section slug \"%1\"")
                .arg(afterSection));
    }

    // INV-5 — slug computation via the shared helper.
    const QString newSlug = RoadmapIndex::slugifyHeading(title);
    if (newSlug.isEmpty())
        return rlErr(QStringLiteral("bad_title"),
            QStringLiteral("roadmap_log: title \"%1\" produced an "
                           "empty slug").arg(title));

    // INV-6 — slug_collision.
    for (const auto &s : index) {
        if (s.slug == newSlug) {
            QJsonObject env;
            env["ok"]             = false;
            env["code"]           = QStringLiteral("slug_collision");
            env["error"]          = QStringLiteral(
                "roadmap_log: computed slug \"%1\" already exists in "
                "ROADMAP.md (heading: \"%2\") — pick a different title")
                .arg(newSlug, s.headingText);
            env["computed_slug"]  = newSlug;
            return QJsonDocument(env);
        }
    }

    // 5. Build inserted block.
    const QString hashes = QString(level, QLatin1Char('#'));
    QStringList toInsert;
    toInsert << (hashes + QChar(' ') + title);
    toInsert << QString();   // blank after heading

    const QString introBody =
        req.value(QStringLiteral("intro_body")).toString();
    // ANTS-3809 § 2.2 — hoisted out of the block below so the store path can
    // hand setSectionIntro() the SAME wrapped text the splice would have
    // inserted, rather than re-deriving it and drifting.
    QStringList introWrapped;
    if (!introBody.isEmpty()) {
        // Single newline = paragraph break; double newlines collapse.
        // Then hard-wrap each paragraph at 80 cols on word boundaries.
        QStringList paragraphs;
        {
            QString current;
            const QStringList rawLines = introBody.split(QChar('\n'));
            for (const QString &ln : rawLines) {
                const QString trimmed = ln.trimmed();
                if (trimmed.isEmpty()) {
                    if (!current.isEmpty()) {
                        paragraphs.append(current);
                        current.clear();
                    }
                } else {
                    if (!current.isEmpty()) {
                        paragraphs.append(current);
                        current.clear();
                    }
                    current = trimmed;
                }
            }
            if (!current.isEmpty()) paragraphs.append(current);
        }
        QStringList &wrapped = introWrapped;
        constexpr int kWrapCols = 80;
        for (const QString &para : paragraphs) {
            const QStringList words = para.split(QChar(' '),
                                                 Qt::SkipEmptyParts);
            QString line;
            for (const QString &w : words) {
                const int need = line.isEmpty() ? w.size()
                                                : line.size() + 1 + w.size();
                if (line.isEmpty()) {
                    line = w;
                } else if (need <= kWrapCols) {
                    line += QChar(' ') + w;
                } else {
                    wrapped << line;
                    line = w;
                }
            }
            if (!line.isEmpty()) wrapped << line;
            wrapped << QString();   // blank between paragraphs
        }
        // Drop trailing blank from the paragraph-block.
        while (!wrapped.isEmpty() && wrapped.last().isEmpty())
            wrapped.removeLast();

        // INV-10 — stray-heading guard. ^#{1,6}\s
        static const QRegularExpression kHeadingRx(
            QStringLiteral("^#{1,6}\\s"));
        for (const QString &ln : wrapped) {
            if (kHeadingRx.match(ln).hasMatch()) {
                QJsonObject env;
                env["ok"]    = false;
                env["code"]  = QStringLiteral("bad_intro");
                env["error"] = QStringLiteral(
                    "roadmap_log: intro_body line \"%1\" matches a "
                    "Markdown heading (^#{1,6}\\s) and would silently "
                    "add a section — reword the line").arg(ln);
                return QJsonDocument(env);
            }
        }
        for (const QString &ln : wrapped) toInsert << ln;
        toInsert << QString();   // blank closing the intro block
    }

    // ANTS-3809 § 2.2 — the store path. Everything above is validation against
    // the markdown index, and it is SHARED deliberately: on a migrated project
    // the file is the render's own output, so after_section, the computed slug
    // and the collision check resolve against the same text either way. Only
    // the mutation and the write differ, which is what INV-2 is about.
    {
        RoadmapSource::ReadError why = RoadmapSource::ReadError::None;
        QString seamErr;
        // ANTS-3863 — fromMemory: `markdown` is already read and spliced below.
        auto seamText = RoadmapSource::RoadmapText::fromMemory(markdown);
        const auto target =
            roadmapWriteTarget(callerCanonical, seamText, &why, &seamErr);
        QJsonObject refusal;
        if (rcRoadmapSourceRefused(refusal, why, seamErr))
            return QJsonDocument(refusal);
        if (target) {
            RoadmapStore &store = *target->store;
            const qint64 projectId = target->projectId;
            const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();

            const auto mutate = [&](QString *err) -> bool {
                const auto afterId = store.findSection(projectId, afterSection, err);
                if (!afterId) {
                    if (err && err->isEmpty())
                        *err = QStringLiteral("section \"%1\" is not in the store")
                                   .arg(afterSection);
                    return false;
                }
                const auto afterRow = store.readSection(*afterId, err);
                if (!afterRow)
                    return false;
                const int newPos = afterRow->position + 1;

                // The renumber. `section` is deliberately NOT
                // UNIQUE (project_id, position) — the DDL comment says so
                // outright — so this has no transient-collision problem. It is
                // still required rather than optional: sectionOrderLess()'s key
                // is (position, slug), so two sections left sharing a position
                // would order by slug and not by where the caller put them.
                const auto all = store.listSections(projectId, err);
                if (!all)
                    return false;
                // listSections() and not listSectionsOrdered(): a renumber keys
                // each row by its own id and reads no order, so it would pay for
                // a sort it never looks at (ANTS-3818).
                for (const auto &s : *all) {
                    if (s.position < newPos)
                        continue;
                    if (!store.updateSection(s.sectionId, s.title, s.level,
                                             s.position + 1, s.parentId, err))
                        return false;
                }

                const auto created = store.addSection(projectId, newSlug, title,
                                                      level, newPos,
                                                      std::nullopt, err);
                if (!created)
                    return false;
                return introWrapped.isEmpty()
                       || store.setSectionIntro(
                              *created, introWrapped.join(QChar('\n')), err);
            };

            RoadmapRender::Outcome outcome;
            QString writeErr;
            const auto r = RoadmapWrite::commitAndRender(
                store, projectId, rcProjectRootFor(callerCanonical), roadmapPath, dryRun, mutate,
                &outcome, &writeErr);
            QJsonObject env;
            if (rcRoadmapWriteRefused(env, r, writeErr, outcome))
                return QJsonDocument(env);

            env[QStringLiteral("ok")]   = true;
            env[QStringLiteral("op")]   = QStringLiteral("create_section");
            env[QStringLiteral("slug")] = newSlug;
            env[QStringLiteral("file")] = QStringLiteral("ROADMAP.md");
            // No `line`: a store has no lines — ANTS-3793 INV-2's one declared
            // field difference — and the render, not this op, decides placement.
            // The render's own result is what a caller can act on, and under
            // dry_run it is the non-empty preview INV-7 requires.
            rcRoadmapWriteFields(env, outcome, dryRun);   // ANTS-4463
            return QJsonDocument(env);
        }
    }

    // 6. Splice at sec->lineEnd (0-indexed, exclusive).
    QStringList lines = markdown.split(QChar('\n'));
    const int insertAt = sec->lineEnd;
    for (int i = toInsert.size() - 1; i >= 0; --i)
        lines.insert(insertAt, toInsert.at(i));
    const QString updated = lines.join(QChar('\n'));

    // ANTS-2136 — dry_run preview: the heading + intro block are rendered
    // and the insertion point resolved; return the would-be slug, 1-based
    // heading line and inserted-byte count WITHOUT writing ROADMAP.md.
    if (req.value(QStringLiteral("dry_run")).toBool()) {
        qint64 previewBytes = 0;
        for (const QString &ln : toInsert)
            previewBytes += ln.toUtf8().size() + 1;   // + '\n'
        QJsonObject out;
        out["ok"]      = true;
        out["op"]      = QStringLiteral("create_section");
        out["dry_run"] = true;
        out["slug"]    = newSlug;
        out["file"]    = QStringLiteral("ROADMAP.md");
        out["line"]    = insertAt + 1;                // 1-based heading line
        out["bytes"]   = previewBytes;
        return QJsonDocument(out);
    }

    // 7. Atomic write.
    QSaveFile rw(roadmapPath);
    if (!rw.open(QIODevice::WriteOnly | QIODevice::Text))
        return rlErr(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("roadmap_log: could not open \"%1\" for writing")
                .arg(roadmapPath));
    const QByteArray utf8 = updated.toUtf8();
    if (rw.write(utf8) != utf8.size() || !rw.commit())
        return rlErr(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("roadmap_log: atomic write of \"%1\" failed")
                .arg(roadmapPath));

    // 8. Success envelope.
    qint64 bytesInserted = 0;
    for (const QString &ln : toInsert)
        bytesInserted += ln.toUtf8().size() + 1;   // + '\n'

    QJsonObject out;
    out["ok"]            = true;
    out["slug"]          = newSlug;
    out["file"]          = QStringLiteral("ROADMAP.md");
    out["line"]          = insertAt + 1;          // 1-based heading line
    out["bytes_written"] = bytesInserted;
    return QJsonDocument(out);
}

// ANTS-1691 — test seam for the bundle_row path (m_main-independent).
QJsonDocument RemoteControl::cmdRoadmapLogBundleRowForTest(
        const QJsonObject &req) {
    return cmdRoadmapLogBundleRow(req);
}

// ANTS-1691 — roadmap_log op:"bundle_row". Append a row to a Markdown
// table held under a named section (the "## 📊 Bundle progress" table
// some cross-session reporters maintain). Pipe/newline-escapes every
// cell so a `|` inside a multi-KB cell can't corrupt the column count —
// the corruption risk that made the hand-`Edit` fallback unsafe. Finds
// the table inside the section, or creates one from `header`. No counter
// touch; single read + single atomic QSaveFile commit. See
// tests/features/roadmap_log_bundle_row/spec.md.
QJsonDocument RemoteControl::cmdRoadmapLogBundleRow(const QJsonObject &req) {
    auto rlErr = [](const QString &code, const QString &message) {
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = code;
        env["error"] = message;
        return QJsonDocument(env);
    };

    // Escape one cell for safe placement inside a GFM table: a literal
    // `|` would otherwise open a new column, and a raw newline is illegal
    // in a table cell. Backslash-escape pipes; fold newlines to <br>.
    auto escapeCell = [](const QString &raw) {
        QString s = raw;
        s.replace(QStringLiteral("|"), QStringLiteral("\\|"));
        s.replace(QStringLiteral("\r\n"), QStringLiteral("<br>"));
        s.replace(QChar('\n'), QStringLiteral("<br>"));
        s.replace(QChar('\r'), QStringLiteral("<br>"));
        return s.trimmed();
    };
    // Render an escaped cell list as a GFM row: `| a | b | c |`.
    auto renderRow = [&escapeCell](const QStringList &cells) {
        QString row = QStringLiteral("|");
        for (const QString &c : cells)
            row += QChar(' ') + escapeCell(c) + QStringLiteral(" |");
        return row;
    };

    // 1. Required fields.
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    const QString section =
        req.value(QStringLiteral("section")).toString();
    if (callerRaw.isEmpty())
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: caller_cwd is required"));
    if (section.isEmpty())
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: bundle_row section is required"));
    if (!req.value(QStringLiteral("cells")).isArray())
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: bundle_row cells (array) is "
                           "required"));
    QStringList cells;
    for (const QJsonValue &v :
         req.value(QStringLiteral("cells")).toArray())
        cells << v.toString();
    if (cells.isEmpty())
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: bundle_row cells must be a "
                           "non-empty array"));

    QStringList header;
    if (req.value(QStringLiteral("header")).isArray())
        for (const QJsonValue &v :
             req.value(QStringLiteral("header")).toArray())
            header << v.toString();

    const QString position =
        req.value(QStringLiteral("position")).toString(
            QStringLiteral("end"));
    const int sortCol =
        req.value(QStringLiteral("sort_col")).toInt(0);

    // 2. Resolve ROADMAP.md (parity with create_section).
    const QString callerCanonical =
        QFileInfo(callerRaw).canonicalFilePath();
    if (callerCanonical.isEmpty())
        return rlErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: caller_cwd \"%1\" does not "
                           "canonicalise to an existing directory")
                .arg(callerRaw));
    const QString roadmapPath = findRoadmapUnder(callerCanonical);
    if (roadmapPath.isEmpty())
        return rlErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: no ROADMAP.md under \"%1\"")
                .arg(callerCanonical));

    // 3. Read markdown.
    QFile rf(roadmapPath);
    if (!rf.open(QIODevice::ReadOnly | QIODevice::Text))
        return rlErr(QStringLiteral("roadmap_read_failed"),
            QStringLiteral("roadmap_log: could not read \"%1\"")
                .arg(roadmapPath));
    const QString markdown = QString::fromUtf8(rf.readAll());
    rf.close();

    // 4. Locate section (parity with create_section's bad_case echo).
    const auto index = RoadmapIndex::buildIndex(markdown);
    const RoadmapIndex::Section *sec =
        RoadmapIndex::findBySlug(index, section);
    if (!sec) {
        const QString needCi = section.toLower();
        for (const auto &s : index) {
            if (s.slug.toLower() == needCi && s.slug != section) {
                QJsonObject env;
                env["ok"]             = false;
                env["code"]           = QStringLiteral("bad_case");
                env["error"]          = QStringLiteral(
                    "roadmap_log: section slug case mismatch: \"%1\" — "
                    "did you mean \"%2\"?").arg(section, s.slug);
                env["canonical_slug"] = s.slug;
                return QJsonDocument(env);
            }
        }
        return rlErr(QStringLiteral("bad_section"),
            QStringLiteral("roadmap_log: unknown section slug \"%1\"")
                .arg(section));
    }

    // ANTS-3809 § 2.2 — the store path, branching ABOVE the markdown table
    // scan rather than below it: everything that scan derives — the column
    // count, the data-row span, the insertion line — is markdown geometry the
    // store answers differently. ANTS-3756 INV-24 makes element.payload
    // canonical JSON when kind='table', so the new row is an array insert into
    // "rows", not a rendered markdown line.
    //
    // ANTS-3832 gave RoadmapRender::render() the table renderer this path
    // needs; before it, a rendered file got the payload's raw JSON where the
    // rows belonged.
    {
        RoadmapSource::ReadError why = RoadmapSource::ReadError::None;
        QString seamErr;
        // ANTS-3863 — fromMemory: `markdown` is already read and spliced below.
        auto seamText = RoadmapSource::RoadmapText::fromMemory(markdown);
        const auto target =
            roadmapWriteTarget(callerCanonical, seamText, &why, &seamErr);
        QJsonObject refusal;
        if (rcRoadmapSourceRefused(refusal, why, seamErr))
            return QJsonDocument(refusal);
        if (target) {
            RoadmapStore &store = *target->store;
            const qint64 projectId = target->projectId;
            const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();

            const auto sectionId = store.findSection(projectId, section, &seamErr);
            if (!sectionId)
                return rlErr(QStringLiteral("section_not_found"),
                    QStringLiteral("roadmap_log: section \"%1\" is not in the "
                                   "roadmap store").arg(section));
            const auto elements = store.listElements(*sectionId, &seamErr);
            if (!elements)
                return rlErr(QStringLiteral("store_failed"), seamErr);

            // The FIRST kind='table' element BY POSITION. Nothing in the schema
            // constrains a section to one table, and first-by-position is the
            // one choice that matches what the markdown path does — it splices
            // into the first table under the heading. Compared explicitly
            // rather than taken as listElements()' first hit, so the choice
            // does not silently depend on that reader's ordering.
            const RoadmapStore::ElementRow *tableRow = nullptr;
            int maxPos = -1;
            for (const RoadmapStore::ElementRow &e : *elements) {
                maxPos = std::max(maxPos, e.position);
                if (e.kind == QLatin1String("table") &&
                    (!tableRow || e.position < tableRow->position))
                    tableRow = &e;
            }

            // A newline is folded HERE and not in the render, unlike the pipe
            // (ANTS-3832). The render's escaping has to be invertible or
            // ANTS-3758 INV-1's round-trip fails, and `<br>` is not: a
            // re-migration reads it as the literal text. So the store never
            // holds a newline in a cell, while it does hold the author's `|`
            // and lets the render spell it `\|`.
            const auto foldCells = [](const QStringList &in) {
                QStringList out;
                out.reserve(in.size());
                for (const QString &raw : in) {
                    QString s = raw;
                    s.replace(QStringLiteral("\r\n"), QStringLiteral("<br>"));
                    s.replace(QChar('\n'), QStringLiteral("<br>"));
                    s.replace(QChar('\r'), QStringLiteral("<br>"));
                    out.append(s.trimmed());
                }
                return out;
            };
            const QStringList storeCells = foldCells(cells);

            QJsonArray headerArr, rowsArr;
            bool createdTable = false;
            if (tableRow) {
                const QJsonObject payload =
                    QJsonDocument::fromJson(
                        tableRow->payload.value_or(QString()).toUtf8()).object();
                headerArr = payload.value(QStringLiteral("header")).toArray();
                rowsArr   = payload.value(QStringLiteral("rows")).toArray();
            } else {
                if (header.isEmpty())
                    return rlErr(QStringLiteral("no_table"),
                        QStringLiteral("roadmap_log: section \"%1\" has no "
                                       "Markdown table — pass `header` (column "
                                       "names) to create one").arg(section));
                headerArr    = QJsonArray::fromStringList(foldCells(header));
                createdTable = true;
            }

            // The shipped column_mismatch refusal, unchanged in meaning: the
            // same check over a parsed shape rather than over a header row's
            // pipe count.
            const int columns = headerArr.size();
            if (cells.size() != columns)
                return rlErr(QStringLiteral("column_mismatch"),
                    QStringLiteral("roadmap_log: cells (%1) must match the "
                                   "table's column count (%2)")
                        .arg(cells.size()).arg(columns));
            if (!createdTable && !header.isEmpty() && header.size() != columns)
                return rlErr(QStringLiteral("column_mismatch"),
                    QStringLiteral("roadmap_log: header (%1) must match the "
                                   "existing table's column count (%2)")
                        .arg(header.size()).arg(columns));

            // `position` and `sort_col` decide where the row lands WITHIN
            // "rows" — an array insert. Neither touches element.position, which
            // is where the TABLE sits in the section.
            int rowIndex = rowsArr.size() + 1;   // 1-based; default is the end
            if (position == QStringLiteral("sorted") && sortCol >= 0 &&
                sortCol < columns) {
                // Same explicit locale as the markdown path, and for the same
                // reason: a default QCollator follows the system locale, which
                // under C/POSIX degrades to a codepoint compare that ignores
                // setNumericMode — filing "40" before "9".
                QCollator coll(QLocale(QLocale::English, QLocale::UnitedStates));
                coll.setNumericMode(true);
                coll.setCaseSensitivity(Qt::CaseInsensitive);
                const QString key = storeCells.value(sortCol);
                for (int i = 0; i < rowsArr.size(); ++i) {
                    const QString existing =
                        rowsArr.at(i).toArray().at(sortCol).toString();
                    if (coll.compare(key, existing) < 0) {
                        rowIndex = i + 1;
                        break;
                    }
                }
            }

            // Pipes are NOT escaped — that is a GFM spelling and the payload is
            // JSON, so the render owns it. Newlines are already folded above,
            // for the invertibility reason stated there.
            rowsArr.insert(rowIndex - 1, QJsonArray::fromStringList(storeCells));
            QJsonObject payload;
            payload.insert(QStringLiteral("header"), headerArr);
            payload.insert(QStringLiteral("rows"), rowsArr);
            const QString payloadText = QString::fromUtf8(
                QJsonDocument(payload).toJson(QJsonDocument::Compact));

            const int tablePos = tableRow ? tableRow->position : maxPos + 1;
            const auto mutate = [&](QString *err) -> bool {
                if (tableRow)
                    return store.setElementPayload(*sectionId, tablePos,
                                                   payloadText, err);
                // The create case. Pinned at max(position) + 1 — or 0 for an
                // empty section — because element carries
                // UNIQUE (section_id, position) and item rows occupy positions
                // too, so "at the end" left unpinned is a constraint violation
                // rather than a mis-placement.
                return store.addElement(*sectionId, tablePos,
                                        QStringLiteral("table"), payloadText, err);
            };

            RoadmapRender::Outcome outcome;
            QString writeErr;
            const auto r = RoadmapWrite::commitAndRender(
                store, projectId, rcProjectRootFor(callerCanonical), roadmapPath, dryRun, mutate,
                &outcome, &writeErr);
            QJsonObject env;
            if (rcRoadmapWriteRefused(env, r, writeErr, outcome))
                return QJsonDocument(env);

            env[QStringLiteral("ok")]            = true;
            env[QStringLiteral("op")]            = QStringLiteral("bundle_row");
            env[QStringLiteral("file")]          = QStringLiteral("ROADMAP.md");
            env[QStringLiteral("section")]       = section;
            env[QStringLiteral("row_index")]     = rowIndex;
            env[QStringLiteral("columns")]       = columns;
            env[QStringLiteral("created_table")] = createdTable;
            rcRoadmapWriteFields(env, outcome, dryRun);   // ANTS-4463
            return QJsonDocument(env);
        }
    }

    // 5. Within the section's line range, find an existing GFM table:
    //    a header row (starts with `|`) immediately followed by a
    //    separator row (`|---|---|`). Track the data-row span.
    QStringList lines = markdown.split(QChar('\n'));
    static const QRegularExpression kTableSep(
        QStringLiteral("^\\s*\\|?\\s*:?-{1,}:?\\s*(\\|\\s*:?-{1,}:?\\s*)*"
                       "\\|?\\s*$"));
    auto isTableRow = [](const QString &ln) {
        return ln.trimmed().startsWith(QChar('|'));
    };

    int headerLine = -1;   // 0-indexed header row
    int lastDataLine = -1; // 0-indexed last data row
    int columns = 0;
    // Count GFM columns in a row line (cells between the outer pipes).
    auto rowColumns = [](const QString &ln) {
        QString t = ln.trimmed();
        if (t.startsWith(QChar('|'))) t.remove(0, 1);
        if (t.endsWith(QChar('|'))) t.chop(1);
        // Split on UNescaped pipes only.
        int cols = 0, i = 0;
        bool any = false;
        QString cur;
        while (i < t.size()) {
            if (t[i] == QChar('\\') && i + 1 < t.size()) {
                cur += t[i]; cur += t[i + 1]; i += 2; continue;
            }
            if (t[i] == QChar('|')) { cols++; any = true; cur.clear(); }
            else cur += t[i];
            ++i;
        }
        return any ? cols + 1 : (t.isEmpty() ? 0 : 1);
    };

    for (int i = sec->lineStart + 1;
         i < sec->lineEnd && i < lines.size(); ++i) {
        if (headerLine < 0) {
            // Looking for a header line followed by a separator line.
            if (isTableRow(lines.at(i)) && i + 1 < lines.size() &&
                kTableSep.match(lines.at(i + 1)).hasMatch()) {
                headerLine   = i;
                columns      = rowColumns(lines.at(i));
                lastDataLine = i + 1;   // separator; data rows follow
            }
            continue;
        }
        // In-table: extend the data span while rows keep matching.
        if (isTableRow(lines.at(i))) lastDataLine = i;
        else break;   // first non-table line ends the table
    }

    bool createdTable = false;
    int rowIndex = 0;   // 1-based position among DATA rows after insert

    if (headerLine < 0) {
        // 6a. No table — create one from `header`, else refuse.
        if (header.isEmpty())
            return rlErr(QStringLiteral("no_table"),
                QStringLiteral("roadmap_log: section \"%1\" has no "
                               "Markdown table — pass `header` (column "
                               "names) to create one").arg(section));
        if (cells.size() != header.size())
            return rlErr(QStringLiteral("column_mismatch"),
                QStringLiteral("roadmap_log: cells (%1) must match header "
                               "columns (%2)").arg(cells.size())
                    .arg(header.size()));
        QStringList block;
        block << renderRow(header);
        QString sep = QStringLiteral("|");
        for (int c = 0; c < header.size(); ++c)
            sep += QStringLiteral(" --- |");
        block << sep;
        block << renderRow(cells);
        // Insert right after the heading + its blank line (or at the
        // heading line's end). Skip a single blank line that follows.
        int insertAt = sec->lineStart + 1;
        if (insertAt < lines.size() &&
            lines.at(insertAt).trimmed().isEmpty())
            ++insertAt;
        // Ensure a trailing blank line separates the new table.
        block << QString();
        for (int k = block.size() - 1; k >= 0; --k)
            lines.insert(insertAt, block.at(k));
        createdTable = true;
        rowIndex     = 1;
    } else {
        // 6b. Table exists — validate column count, then insert.
        if (cells.size() != columns)
            return rlErr(QStringLiteral("column_mismatch"),
                QStringLiteral("roadmap_log: cells (%1) must match the "
                               "table's column count (%2)")
                    .arg(cells.size()).arg(columns));
        if (!header.isEmpty() && header.size() != columns)
            return rlErr(QStringLiteral("column_mismatch"),
                QStringLiteral("roadmap_log: header (%1) must match the "
                               "existing table's column count (%2)")
                    .arg(header.size()).arg(columns));

        const QString newRow = renderRow(cells);
        const int firstDataLine = headerLine + 2;  // skip header+sep

        if (position == QStringLiteral("sorted") &&
            sortCol >= 0 && sortCol < columns) {
            // Numeric-aware ascending insert by the sort_col cell.
            // Pin an explicit locale: a default QCollator follows the
            // system locale, which under C/POSIX (LANG unset on CI runners)
            // silently degrades to a codepoint compare that ignores
            // setNumericMode — filing "40" before "9". An explicit locale
            // keeps ICU's numeric collation regardless of $LANG.
            QCollator coll(QLocale(QLocale::English, QLocale::UnitedStates));
            coll.setNumericMode(true);
            coll.setCaseSensitivity(Qt::CaseInsensitive);
            const QString key = escapeCell(cells.value(sortCol));
            int insertAt = lastDataLine + 1;   // default: end
            int seen = 0;
            for (int i = firstDataLine; i <= lastDataLine; ++i) {
                // Extract the sort_col cell of this existing data row.
                QString t = lines.at(i).trimmed();
                if (t.startsWith(QChar('|'))) t.remove(0, 1);
                QStringList parts;
                QString cur; int j = 0;
                while (j < t.size()) {
                    if (t[j] == QChar('\\') && j + 1 < t.size()) {
                        cur += t[j]; cur += t[j + 1]; j += 2; continue;
                    }
                    if (t[j] == QChar('|')) { parts << cur.trimmed();
                                              cur.clear(); }
                    else cur += t[j];
                    ++j;
                }
                const QString existing =
                    parts.value(sortCol).trimmed();
                if (coll.compare(key, existing) < 0) {
                    insertAt = i; break;
                }
                ++seen;
            }
            lines.insert(insertAt, newRow);
            rowIndex = seen + 1;
        } else {
            // Append after the last data row.
            lines.insert(lastDataLine + 1, newRow);
            rowIndex = (lastDataLine - firstDataLine + 1) + 1;
        }
    }

    const QString updated = lines.join(QChar('\n'));
    const QString rowText = renderRow(cells);

    // ANTS-3798 — dry_run preview, parity with create_section directly above.
    // Every guard has already run and every derived value is resolved: the
    // section located, the column count validated, the cells escaped, and the
    // insertion point chosen (including the numeric-aware `sorted` placement,
    // which is the one thing here a caller cannot predict). So the preview
    // reports the SAME row_index / columns / created_table the real write
    // would, plus the rendered `row` — and `bytes` in place of bytes_written,
    // the house convention for "this is what it would have cost".
    //
    // This op was the last roadmap_log write with no preview: ANTS-2077 added
    // one to append/append_batch and ANTS-2136 swept flip, flip_batch,
    // annotate, create_section and amend_body, but bundle_row was missed. It
    // is a poor one to miss, because a mismatched column count is refused
    // while a wrongly-sorted or wrongly-escaped cell is not — a mangled table
    // is exactly the outcome you would want to see before it lands.
    if (req.value(QStringLiteral("dry_run")).toBool()) {
        QJsonObject out;
        out["ok"]            = true;
        out["op"]            = QStringLiteral("bundle_row");
        out["dry_run"]       = true;
        out["file"]          = QStringLiteral("ROADMAP.md");
        out["section"]       = section;
        out["row_index"]     = rowIndex;
        out["columns"]       = createdTable ? header.size() : columns;
        out["created_table"] = createdTable;
        out["row"]           = rowText;
        out["bytes"]         = static_cast<int>(rowText.toUtf8().size() + 1);
        return QJsonDocument(out);
    }

    // 7. Atomic write.
    QSaveFile rw(roadmapPath);
    if (!rw.open(QIODevice::WriteOnly | QIODevice::Text))
        return rlErr(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("roadmap_log: could not open \"%1\" for writing")
                .arg(roadmapPath));
    const QByteArray utf8 = updated.toUtf8();
    if (rw.write(utf8) != utf8.size() || !rw.commit())
        return rlErr(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("roadmap_log: atomic write of \"%1\" failed")
                .arg(roadmapPath));

    // 8. Success envelope.
    QJsonObject out;
    out["ok"]            = true;
    out["file"]          = QStringLiteral("ROADMAP.md");
    out["section"]       = section;
    out["row_index"]     = rowIndex;
    out["columns"]       = createdTable ? header.size() : columns;
    out["created_table"] = createdTable;
    out["bytes_written"] = static_cast<int>(rowText.toUtf8().size() + 1);
    return QJsonDocument(out);
}

// ANTS-1879 — roadmap_log op:"append_batch". Append N bullets to one
// section in a single read + single atomic commit. Semantic parity
// with cmdRoadmapLogFlipBatch (ok:true even when all-skipped). See
// docs/specs/ANTS-1879.md.
QJsonDocument RemoteControl::cmdRoadmapLogAppendBatch(const QJsonObject &req) {
    auto rlErr = [](const QString &code, const QString &message) {
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = code;
        env["error"] = message;
        return QJsonDocument(env);
    };

    // 1. Top-level required fields.
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    const QString section =
        req.value(QStringLiteral("section")).toString();
    if (callerRaw.isEmpty())
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: caller_cwd is required"));
    if (section.isEmpty())
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: section is required"));
    if (!req.value(QStringLiteral("bullets")).isArray() ||
        req.value(QStringLiteral("bullets")).toArray().isEmpty())
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: op:\"append_batch\" needs a "
                           "non-empty `bullets` array"));
    const QJsonArray bullets =
        req.value(QStringLiteral("bullets")).toArray();

    // ANTS-2126 — pass-headings roadmaps route to the heading-format
    // batch writer here, BEFORE the counter read below (a pass roadmap
    // legitimately has no .roadmap-counter; INV-10). Mirrors the single
    // op:append early route.
    {
        const QString cc = QFileInfo(callerRaw).canonicalFilePath();
        const QString rp = cc.isEmpty() ? QString() : findRoadmapUnder(cc);
        if (!rp.isEmpty()) {
            QFile pf(rp);
            if (pf.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QString md = QString::fromUtf8(pf.readAll());
                pf.close();
                if (rcBulletsArePassHeadings(rlParse(md, cc)))   // ANTS-3771
                    return cmdRoadmapLogPassAppendBatch(req, rp, md);
            }
        }
    }

    // ANTS-2076 — batch-wide explicit counter-ID prefix override
    // (validated here; resolution precedence in rlResolveCounterPrefix).
    // ANTS-2077 — dry_run preview flag.
    const QString idPrefixArg =
        req.value(QStringLiteral("id_prefix")).toString();
    if (!idPrefixArg.isEmpty() &&
        !kIdPrefixShape.match(idPrefixArg).hasMatch()) {
        return rlErr(QStringLiteral("bad_args"),
            QStringLiteral("roadmap_log: id_prefix \"%1\" must contain a "
                           "letter and be 1-16 chars of [A-Za-z0-9_-] "
                           "(e.g. ANTS, 3D_E) — ANTS-3492").arg(idPrefixArg));
    }
    const bool dryRun =
        req.value(QStringLiteral("dry_run")).toBool();

    // ANTS-2078 — per-bullet stable-string IDs. Batch-wide id_strategy
    // mirrors single op:append (ANTS-1905): "counter" (default) bumps
    // .roadmap-counter; "stable_prefix" skips the counter entirely and
    // takes each bullet's own `stable_id` (the full ID string). Lets a
    // whole stable-ID roadmap be scaffolded in one call instead of N
    // single op:append calls.
    const QString idStrategy =
        req.value(QStringLiteral("id_strategy")).toString();
    if (!idStrategy.isEmpty() &&
        idStrategy != QStringLiteral("counter") &&
        idStrategy != QStringLiteral("stable_prefix"))
        return rlErr(QStringLiteral("bad_args"),
            QStringLiteral("roadmap_log: id_strategy must be "
                           "\"counter\" or \"stable_prefix\""));
    const bool useStablePrefix =
        idStrategy == QStringLiteral("stable_prefix");
    // ANTS-4849 — shared shape; see remotecontrol_internal.h.
    const QRegularExpression &kBatchStableIdShape = rcdetail::rlStableIdShape();

    // 2. Resolve ROADMAP.md.
    const QString callerCanonical =
        QFileInfo(callerRaw).canonicalFilePath();
    if (callerCanonical.isEmpty())
        return rlErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: caller_cwd \"%1\" does not "
                           "canonicalise to an existing directory")
                .arg(callerRaw));
    // ANTS-4882 — `projectRootDir` is the directory the roadmap was resolved
    // under, which is the root the store keys this project on. From a
    // subdirectory it is an ancestor of callerCanonical, and the store floor
    // below has to ask by it or match nothing.
    QString projectRootDir;
    const QString roadmapPath = findRoadmapUnder(callerCanonical, &projectRootDir);
    if (roadmapPath.isEmpty())
        return rlErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: no ROADMAP.md under \"%1\"")
                .arg(callerCanonical));

    // ANTS-3771 — ONCE for the batch (load() re-reads the file every call).
    const RoadmapParse::IdFormat batchIdFormat = rlDecl(callerCanonical);
    // ANTS-3350 — beside the RESOLVED roadmap, as op:"append" does. This also
    // aims the committed-corpus floor below, which is derived from this path's
    // own directory: under caller_cwd it scanned the subdirectory and found
    // nothing to floor to.
    const QString counterPath =
        QFileInfo(roadmapPath).absolutePath() + QLatin1Char('/') +
        QStringLiteral(".roadmap-counter");

    // ANTS-3809 § 2.2 — the write target, resolved BEFORE the counter read for
    // the reason op:append resolves it there: § 2.3 allocates from the store and
    // leaves .roadmap-counter alone, and the read below would otherwise refuse
    // or auto-create a file a migrated project does not allocate from.
    //
    // Resolved here and consumed at four later points rather than as one early
    // branch, because everything between — per-bullet validation, the section
    // refusals, the skipped[] granularity, the contiguous id assignment — is
    // shared with the markdown path. Only allocation and the write differ.
    std::optional<RoadmapWriteTarget> writeTarget;
    {
        // ANTS-3863 § 1 — this read had NO consumer at all: `probe` was filled,
        // handed to the seam, and fell out of scope at the closing brace, so on
        // a migrated project 3 MiB was read and discarded unexamined. The block
        // does NOT branch on the open today — a failed open simply left `probe`
        // empty and let it reach the seam — and an unopenable RoadmapText
        // yields exactly that empty prefix, so the behaviour survives without a
        // guard being added. (op:append above DOES branch; the two differ, and
        // INV-4 keeps both.)
        auto probe = RoadmapSource::RoadmapText::fromFile(roadmapPath);
        RoadmapSource::ReadError why = RoadmapSource::ReadError::None;
        QString seamErr;
        writeTarget = roadmapWriteTarget(callerCanonical, probe, &why, &seamErr);
        QJsonObject refusal;
        if (rcRoadmapSourceRefused(refusal, why, seamErr))
            return QJsonDocument(refusal);
    }

    // 3. Counter read (parity with append path). ANTS-2078 — the
    // stable_prefix strategy skips the counter machinery entirely
    // (a stable-ID project has no .roadmap-counter).
    qint64 counter = 0;
    // ANTS-4691 — set when the counter came from the in-memory seed below
    // (dry run) rather than from the file. Mirrors the op:append path.
    bool counterResolved = false;
    if (!useStablePrefix && !writeTarget) {
        QFile cf(counterPath);
        if (!cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (QFile::exists(counterPath))
                return rlErr(QStringLiteral("counter_read_failed"),
                    QStringLiteral("roadmap_log: could not read "
                                   ".roadmap-counter at \"%1\"")
                        .arg(counterPath));
            // ANTS-3450 — parity with the single-bullet append path:
            // `.roadmap-counter` is a derived, gitignored cache, so an
            // absent counter on a fresh clone recovers the high-water mark
            // from the committed corpus (ROADMAP + CHANGELOG +
            // docs/roadmap/*.md) instead of refusing. corpusHighWater
            // returns 0 for a greenfield roadmap (→ ids from <prefix>-0001)
            // and for a stable-string-id project; in the latter, a roadmap
            // that carries ids is still a genuine desync we surface.
            QFile rmf(roadmapPath);
            QString rmText;
            if (rmf.open(QIODevice::ReadOnly | QIODevice::Text))
                rmText = QString::fromUtf8(rmf.readAll());
            const qint64 seed = RoadmapFoldIn::corpusHighWater(
                QFileInfo(counterPath).absolutePath());
            if (seed == 0 && rlRoadmapHasAnyBulletId(rmText, batchIdFormat))
                return rlErr(QStringLiteral("counter_missing"),
                    QStringLiteral("roadmap_log: .roadmap-counter does not "
                                   "exist at \"%1\" and no counter-style "
                                   "ids were found to recover a high-water "
                                   "mark from — restore it with: echo "
                                   "<highest-id> > %1").arg(counterPath));
            // ANTS-4691 — a dry run must not touch disk; see the op:append
            // path for the reported case. The seed is already the value the
            // file would hold, so the preview uses it in memory.
            if (dryRun) {
                counter = seed;
                counterResolved = true;
            } else {
                QSaveFile init(counterPath);
                const QByteArray seedBytes = QByteArray::number(seed) + '\n';
                if (!init.open(QIODevice::WriteOnly | QIODevice::Text) ||
                    init.write(seedBytes) != seedBytes.size() ||
                    !init.commit())
                    return rlErr(QStringLiteral("counter_write_failed"),
                        QStringLiteral("roadmap_log: could not auto-create "
                                       ".roadmap-counter at \"%1\"")
                            .arg(counterPath));
                if (!cf.open(QIODevice::ReadOnly | QIODevice::Text))
                    return rlErr(QStringLiteral("counter_read_failed"),
                        QStringLiteral("roadmap_log: could not read the "
                                       "just-created .roadmap-counter at "
                                       "\"%1\"").arg(counterPath));
                // cf now reads "0"; fall through.
            }
        }
        if (!counterResolved) {
            const QByteArray raw = cf.readAll().trimmed();
            if (raw.isEmpty())
                return rlErr(QStringLiteral("counter_missing"),
                    QStringLiteral("roadmap_log: .roadmap-counter at "
                                   "\"%1\" is empty — initialise with: "
                                   "echo 0 > %1").arg(counterPath));
            bool ok = false;
            counter = QString::fromUtf8(raw).toLongLong(&ok);
            if (!ok)
                return rlErr(QStringLiteral("counter_read_failed"),
                    QStringLiteral("roadmap_log: .roadmap-counter is "
                                   "not a number"));
        }
    }

    // 4. Read markdown.
    QFile rf(roadmapPath);
    if (!rf.open(QIODevice::ReadOnly | QIODevice::Text))
        return rlErr(QStringLiteral("roadmap_read_failed"),
            QStringLiteral("roadmap_log: could not read \"%1\"")
                .arg(roadmapPath));
    const QString markdown = QString::fromUtf8(rf.readAll());
    rf.close();

    // INV-9 — unrecognised_format short-circuits the whole batch.
    const auto preflightBullets = rlParse(markdown, callerCanonical);
    const qint64 markdownBytes = markdown.toUtf8().size();
    if (preflightBullets.isEmpty() &&
        markdownBytes > kRoadmapMinParseableSize) {
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = QStringLiteral("unrecognised_format");
        env["error"] = QStringLiteral(
            "roadmap_log: \"%1\" parsed zero bullets from %2 "
            "bytes — format not recognised; cannot safely splice")
                .arg(roadmapPath).arg(markdownBytes);
        env["path"]            = roadmapPath;
        env["bytes"]           = markdownBytes;
        env["hint"]            = kUnrecognisedFormatHint();
        env["expected_format"] = kUnrecognisedFormatExpected();
        return QJsonDocument(env);
    }
    // ANTS-2126 — a pass-headings roadmap was already routed to
    // cmdRoadmapLogPassAppendBatch by the early gate above (before the
    // counter read), so by here the roadmap is GFM / ants-v1.

    // 5. Section lookup (parity with single-bullet path).
    const auto index = RoadmapIndex::buildIndex(markdown);
    const auto *sec = RoadmapIndex::findBySlug(index, section);
    if (!sec) {
        const QString needCi = section.toLower();
        for (const auto &s : index) {
            if (s.slug.toLower() == needCi && s.slug != section) {
                QJsonObject env;
                env["ok"]             = false;
                env["code"]           = QStringLiteral("bad_case");
                env["error"]          = QStringLiteral(
                    "roadmap_log: section slug case mismatch: \"%1\" "
                    "— did you mean \"%2\"?").arg(section, s.slug);
                env["canonical_slug"] = s.slug;
                return QJsonDocument(env);
            }
        }
        return rlErr(QStringLiteral("bad_section"),
            QStringLiteral("roadmap_log: unknown section slug \"%1\"")
                .arg(section));
    }

    // ANTS-2055 — same parent-section guard as the single-bullet path:
    // refuse before any bullet is formatted so the whole batch
    // short-circuits (parity with the bad_section / unrecognised_format
    // short-circuits above).
    //
    // ANTS-3809 — markdown-path only, as on op:append's store path. The guard
    // exists because splicing at sec.lineEnd would orphan the bullet past the
    // last child heading; the store files an item by its element row in the
    // named section, where there is no line to be on the wrong side of.
    if (!writeTarget) {
        const QStringList childSlugs = rcSectionChildSlugs(index, *sec);
        if (!childSlugs.isEmpty()) {
            return rcSectionHasSubsectionsRefusal(sec->slug, childSlugs);
        }
    }

    // ANTS-3809 § 2.2 — the store's own section handle and the end-of-section
    // position, pinned as bundle_row's create case pins it: element carries
    // UNIQUE (section_id, position) and item rows occupy positions too, so "at
    // the end" left unpinned is a constraint violation rather than a
    // mis-placement. Resolved after the markdown slug refusals above so a
    // mistyped slug still gets bad_case / bad_section, exactly as bundle_row
    // resolves both in that order.
    qint64 storeSectionId = 0;
    int    storePosition  = 0;
    if (writeTarget) {
        QString seamErr;
        const auto sid = writeTarget->store->findSection(
            writeTarget->projectId, section, &seamErr);
        if (!sid)
            return rlErr(QStringLiteral("section_not_found"),
                QStringLiteral("roadmap_log: section \"%1\" is not in the "
                               "roadmap store").arg(section));
        const auto elements = writeTarget->store->listElements(*sid, &seamErr);
        if (!elements)
            return rlErr(QStringLiteral("store_failed"), seamErr);
        int maxPos = -1;
        for (const RoadmapStore::ElementRow &e : *elements)
            maxPos = std::max(maxPos, e.position);
        storeSectionId = *sid;
        storePosition  = maxPos + 1;
    }

    // 6. Per-bullet validation. Kinds + statuses enum-checked.
    static const QSet<QString> kValidKinds = {
        QStringLiteral("implement"),    QStringLiteral("fix"),
        QStringLiteral("audit-fix"),    QStringLiteral("review-fix"),
        QStringLiteral("doc"),          QStringLiteral("doc-fix"),
        QStringLiteral("refactor"),     QStringLiteral("test"),
        QStringLiteral("chore"),        QStringLiteral("release"),
        QStringLiteral("perf"),         QStringLiteral("security"),
        QStringLiteral("feature"),      QStringLiteral("enhancement"),
        QStringLiteral("investigate"),  QStringLiteral("research"),
        QStringLiteral("accessibility"),QStringLiteral("optimize"),
        QStringLiteral("package"),      QStringLiteral("marketing"),
        QStringLiteral("ux"),
    };
    auto statusToEmoji = [](const QString &s) -> QString {
        if (s == QLatin1String("planned"))     return QString::fromUtf8("\xF0\x9F\x93\x8B"); // 📋
        if (s == QLatin1String("in-progress")) return QString::fromUtf8("\xF0\x9F\x9A\xA7"); // 🚧
        if (s == QLatin1String("shipped"))     return QString::fromUtf8("\xE2\x9C\x85");     // ✅
        if (s == QLatin1String("considered"))  return QString::fromUtf8("\xF0\x9F\x92\xAD"); // 💭
        return QString();
    };

    struct Accepted {
        int     bulletIndex;
        QString idStr;
        QString emoji;
        QJsonObject bulletReq;
        // ANTS-3809 — the store path's body + trailer columns, filled during
        // validation because a body_shadowed refusal is per bullet (§ 2.5) and
        // must land before ids are assigned, or the accepted bullets' ids would
        // not be contiguous (§ 2.3). Untouched on the markdown path.
        RoadmapStore::ItemWrite w;
    };
    QList<Accepted> accepted;
    QJsonArray skipped;
    QSet<QString> seenStableIds;   // ANTS-2078 — intra-batch dup guard
    QStringList scrubbedRollup;    // deduped across bullets, both paths
    int scrubbedUnnamedRollup = 0;  // ANTS-4572 — summed across bullets
    QStringList evNotPathRollup;    // ANTS-4527 — collected across bullets

    // ANTS-2054 / ANTS-2076 — resolve the project's counter prefix once
    // for all bullets. Precedence: explicit id_prefix > prefix sniffed
    // from existing IDs > project-dir default (no hardcoded "ANTS"
    // fallback for a fresh / id-less roadmap).
    // ANTS-3809 § 2.3 — on the store path idPrefixFor() takes the place of the
    // markdown sniff step; an explicit id_prefix argument still wins.
    // ANTS-3863 — fromMemory: `markdown` is the batch's own already-read text,
    // spliced below on the markdown path, so the provider costs nothing here.
    auto counterText = RoadmapSource::RoadmapText::fromMemory(markdown);
    const QString counterPfx = writeTarget
        ? rlStoreCounterPrefix(*writeTarget->store, writeTarget->projectId,
                               idPrefixArg, counterText, callerCanonical)
        : rlResolveCounterPrefix(idPrefixArg, markdown, callerCanonical);

    // ANTS-2179 — reconcile the (possibly lagging) .roadmap-counter against
    // the file's true max id for this prefix: a stale counter must not
    // reissue a live id. effCounter is the high-water mark the auto
    // allocation and any id_hint must clear; preflightBullets is in hand so
    // the scan is free. stable_prefix bypasses the counter entirely, so the
    // reconcile flag never fires there.
    // ANTS-3450 — also floor to ids migrated out of ROADMAP.md (CHANGELOG +
    // docs/roadmap/*.md), so the now-untracked counter can't reissue a
    // shipped id after a fresh-clone recovery.
    qint64 maxFileId =
        rlMaxExistingIdForPrefix(preflightBullets, counterPfx);
    maxFileId = std::max(maxFileId, RoadmapFoldIn::corpusHighWater(
        QFileInfo(counterPath).absolutePath(), counterPfx));
    // ANTS-4493 — and floor to the STORE's high-water, which neither term above
    // can see. A project MIGRATED but not SERVED from the store reaches this
    // branch, and the ids its migration synthesised live in the store and in no
    // file. op:"append" has carried this floor since ANTS-4493; the batch op
    // did not, so it reissued one silently with ok:true.
    //
    // Markdown path only: with a writeTarget the store's own high-water is
    // already what effCounter takes below.
    if (!writeTarget) {
        if (RoadmapStore *store = roadmapStoreOrNull(nullptr, nullptr)) {
            QString storeErr;
            if (const auto row =
                    store->readProjectByRoot(projectRootDir, &storeErr)) {
                QString hwErr;
                if (const auto hw =
                        store->idHighWater(row->projectId, counterPfx, &hwErr))
                    maxFileId = std::max(maxFileId, *hw);
            }
        }
    }
    // ANTS-3809 § 2.3 — on the store path the high-water is the store's own
    // (ANTS-4631: its two id columns, no text scan). There is no counter file
    // to lag or to self-heal, so counterReconciled cannot fire.
    const qint64 effCounter = writeTarget
        ? rlStoreIdHighWater(*writeTarget->store, writeTarget->projectId,
                             counterPfx)
        : std::max(counter, maxFileId);
    const bool counterReconciled =
        !useStablePrefix && !writeTarget && effCounter > counter;

    qint64 nextId = effCounter + 1;
    bool firstAccepted = true;

    for (int i = 0; i < bullets.size(); ++i) {
        const QJsonObject b = bullets.at(i).toObject();
        auto skip = [&](const QString &code, const QString &msg) {
            QJsonObject s;
            s["bullet_index"] = i;
            s["code"]         = code;
            s["error"]        = msg;
            skipped.append(s);
        };

        const QString headline = b.value(QStringLiteral("headline")).toString();
        const QString status   = b.value(QStringLiteral("status")).toString();
        const QString kind     = b.value(QStringLiteral("kind")).toString();
        const QString source   = b.value(QStringLiteral("source")).toString();

        if (headline.isEmpty()) {
            skip(QStringLiteral("headline_empty"),
                 QStringLiteral("headline must not be empty"));
            continue;
        }
        const QString emoji = statusToEmoji(status);
        if (emoji.isEmpty()) {
            skip(QStringLiteral("bad_status"),
                 QStringLiteral("unknown status \"%1\" — expected "
                                "planned/in-progress/shipped/considered")
                     .arg(status));
            continue;
        }
        if (!kValidKinds.contains(kind)) {
            skip(QStringLiteral("bad_kind"),
                 QStringLiteral("unknown kind \"%1\"").arg(kind));
            continue;
        }
        if (source.isEmpty()) {
            skip(QStringLiteral("missing_field"),
                 QStringLiteral("source is required"));
            continue;
        }

        // ANTS-3809 § 2.5 / § 2.6 — the store path's body + trailer columns.
        // Here, inside validation, for two reasons: a body_shadowed refusal is
        // per BULLET and lands in the same skipped[] a validation failure uses,
        // so one prose sentence in one bullet does not cost the other nine; and
        // it must precede id assignment or a bullet dropped afterwards would
        // leave a gap in the contiguous run (§ 2.3).
        RoadmapStore::ItemWrite itemW;
        if (writeTarget) {
            QStringList scrubbed;
            QString shadowErr;
            if (!rlFillItemBody(b, itemW, scrubbed, &shadowErr,
                                &scrubbedUnnamedRollup, &evNotPathRollup)) {
                skip(QStringLiteral("body_shadowed"), shadowErr);
                continue;
            }
            for (const QString &n : scrubbed)
                if (!scrubbedRollup.contains(n)) scrubbedRollup.append(n);
        }

        QString idStr;
        if (useStablePrefix) {
            // ANTS-2078 — each bullet carries its own full ID string.
            const QString sid =
                b.value(QStringLiteral("stable_id")).toString();
            if (sid.isEmpty()) {
                skip(QStringLiteral("missing_field"),
                     QStringLiteral("id_strategy=\"stable_prefix\" "
                                    "requires `stable_id` (the full ID "
                                    "string, e.g. \"Ts20-SP6\")"));
                continue;
            }
            if (!kBatchStableIdShape.match(sid).hasMatch()) {
                skip(QStringLiteral("bad_args"),
                     QStringLiteral("stable_id \"%1\" does not match the "
                                    "stable-prefix shape: it must contain a "
                                    "letter (a bare number is "
                                    "indistinguishable from a counter ID). A "
                                    "leading digit is fine — 3D_E-0682").arg(sid));
                continue;
            }
            {   // ANTS-3771 § 2.3, per bullet: a breaching id is skipped and
                QString c;   // the rest of the batch still applies.
                const QString m = rcdetail::rlDeclaredIdRefusal(sid, batchIdFormat, &c);
                if (!m.isEmpty()) { skip(c, m); continue; }
            }
            if (seenStableIds.contains(sid)) {
                skip(QStringLiteral("id_taken"),
                     QStringLiteral("stable_id \"%1\" is duplicated "
                                    "within this batch").arg(sid));
                continue;
            }
            seenStableIds.insert(sid);
            idStr = sid;
        } else {
            // INV-6 — id_hint only honoured on FIRST accepted bullet.
            if (firstAccepted && b.contains(QStringLiteral("id_hint"))) {
                const qint64 hint =
                    b.value(QStringLiteral("id_hint")).toInteger();
                if (hint <= effCounter) {
                    // ANTS-2179 — effCounter folds in the file's true max,
                    // so a hint that collides with a live id the lagging
                    // counter never knew about is refused too.
                    // ANTS-3809 — the store path has no counter file, so the
                    // parenthetical would name two numbers that mean nothing
                    // there; the number that decided the refusal is the same.
                    skip(QStringLiteral("id_taken"), writeTarget
                        ? QStringLiteral("id_hint %1 is at or below the "
                                         "roadmap store's high-water %2")
                              .arg(hint).arg(effCounter)
                        : QStringLiteral("id_hint %1 is at or below the "
                                         "highest live id %2 (counter %3, "
                                         "file max %4)").arg(hint)
                              .arg(effCounter).arg(counter).arg(maxFileId));
                    continue;
                }
                nextId = hint;
            }
            idStr = QStringLiteral("%1-%2")
                        .arg(counterPfx).arg(nextId, 4, 10,
                                             QLatin1Char('0'));
            if (nextId > 9999)
                idStr = QStringLiteral("%1-%2").arg(counterPfx).arg(nextId);
        }
        firstAccepted = false;

        Accepted a;
        a.bulletIndex = i;
        a.idStr       = idStr;
        a.emoji       = emoji;
        a.bulletReq   = b;
        a.w           = itemW;
        accepted.append(a);
        ++nextId;
    }

    // INV-4 — all-skipped → ok:true, files untouched.
    if (accepted.isEmpty()) {
        QJsonObject out;
        out["ok"]            = true;
        out["op"]            = QStringLiteral("append_batch");
        out["file"]          = QStringLiteral("ROADMAP.md");
        out["ids"]           = QJsonArray();
        out["applied_count"] = 0;
        out["skipped"]       = skipped;
        out["skipped_count"] = skipped.size();
        // ANTS-3793 INV-2's declared field difference: a store has no lines and
        // no spliced byte count.
        if (!writeTarget) {
            out["lines"]         = QJsonArray();
            out["bytes_written"] = 0;
        }
        return QJsonDocument(out);
    }

    // ANTS-3809 § 2.2 — the store path's write: N putItem()s in ONE
    // transaction, then one raiseIdHighWater() for the last id (§ 2.3). Each
    // item's body and trailer columns were filled during validation above; only
    // its identity, its filing and its position are decided here.
    if (writeTarget) {
        RoadmapStore &store = *writeTarget->store;
        const qint64 projectId = writeTarget->projectId;

        QVector<RoadmapStore::ItemWrite> writes;
        writes.reserve(accepted.size());
        int pos = storePosition;
        for (const Accepted &a : accepted) {
            RoadmapStore::ItemWrite w = a.w;
            w.projectId = projectId;
            w.id        = a.idStr;
            // roadmap-data-model.md § 7.1 — `synthesised` covers every id the
            // store allocates after cutover, and a caller's `stable_id` is the
            // same kind of thing: `parsed` would claim it matched the grammar in
            // source text, `quarantined` would file a first-class project id
            // with the junk.
            w.idOrigin  = QStringLiteral("synthesised");
            w.status    = a.bulletReq.value(QStringLiteral("status")).toString();
            w.headline  = rcSanitizeBulletField(
                a.bulletReq.value(QStringLiteral("headline")).toString(), 500);
            w.sectionId = storeSectionId;
            w.position  = pos++;
            // ANTS-3838 — same branch as the single-append path: an allocated
            // id is `store-generated` (roadmap-data-model.md § 7.7 over
            // § 4.1's `write (store-populated)` marking), a caller's
            // `stable_id` is `asserted`. `idOrigin` stays `synthesised` for
            // both — it records how the id was formed, not who supplied it.
            QJsonObject provenance = w.provenance;
            provenance.insert(QStringLiteral("id"),
                              useStablePrefix
                                  ? QStringLiteral("asserted")
                                  : QStringLiteral("store-generated"));
            provenance.insert(QStringLiteral("status"), QStringLiteral("asserted"));
            provenance.insert(QStringLiteral("headline"), QStringLiteral("asserted"));
            // ANTS-4501 § 2.2 — same insert stamps as the single-append path.
            w.created      = rlStampToday();
            w.lastModified = w.created;
            if (w.status == QLatin1String("shipped"))
                w.shipped = w.created;
            provenance.insert(QStringLiteral("created"),
                              QStringLiteral("store-generated"));
            provenance.insert(QStringLiteral("last_modified"),
                              QStringLiteral("store-generated"));
            if (!w.shipped.isEmpty())
                provenance.insert(QStringLiteral("shipped"),
                                  QStringLiteral("store-generated"));
            w.provenance = provenance;
            writes.append(w);
        }

        const auto mutate = [&](QString *err) -> bool {
            for (const RoadmapStore::ItemWrite &w : writes)
                if (!store.putItem(w, err))
                    return false;
            if (useStablePrefix)
                return true;
            // One raise for the last allocated id — the ids are contiguous, so
            // the highest is the only one the high-water has to record.
            return store.raiseIdHighWater(projectId, counterPfx, nextId - 1, err);
        };

        RoadmapRender::Outcome outcome;
        QString writeErr;
        const auto r = RoadmapWrite::commitAndRender(
            store, projectId, rcProjectRootFor(callerCanonical), roadmapPath, dryRun, mutate,
            &outcome, &writeErr);
        QJsonObject env;
        if (rcRoadmapWriteRefused(env, r, writeErr, outcome))
            return QJsonDocument(env);

        QJsonArray ids;
        for (const Accepted &a : accepted) ids.append(a.idStr);
        env[QStringLiteral("ok")]   = true;
        env[QStringLiteral("op")]   = QStringLiteral("append_batch");
        env[QStringLiteral("file")] = QStringLiteral("ROADMAP.md");
        // ANTS-4634 — `would_be_ids` on a preview, for the reason ANTS-4508
        // gives; see the singular path's note. The rename reached the markdown
        // branch below and not this one, which is the branch every migrated
        // project takes.
        env[dryRun ? QStringLiteral("would_be_ids")
                   : QStringLiteral("ids")] = ids;
        // `would_apply_count` under dry_run, mirroring the markdown preview so a
        // caller's branch on it does not change with the backend. No `lines` /
        // `bytes`.
        env[QStringLiteral("applied_count")] = dryRun ? 0 : accepted.size();
        if (dryRun) {
            env[QStringLiteral("dry_run")]           = true;
            env[QStringLiteral("would_apply_count")] = accepted.size();
        }
        env[QStringLiteral("skipped")]        = skipped;
        env[QStringLiteral("skipped_count")]  = skipped.size();
        // ANTS-4635 — the reconciliation op:append has run since ANTS-4141
        // part 2 and this path never did, which is what let the cache drift by
        // a whole batch. `nextId - 1` is the last id allocated here; the ids
        // are contiguous, so the highest is the only one the cache records.
        if (!dryRun && !useStablePrefix)
            rcRoadmapReconcileCounterCache(env, counterPath, nextId - 1);
        rcRoadmapWriteFields(env, outcome, dryRun);   // ANTS-4463

        // ANTS-2043 — the per-bullet near-duplicate advisory, kept: the file is
        // the render's own output and was parsed BEFORE the write, so a
        // just-appended bullet cannot match itself.
        QJsonArray possibleDuplicates;
        for (const Accepted &a : accepted) {
            const QJsonArray cands = rcComputePossibleDuplicates(
                preflightBullets,
                a.bulletReq.value(QStringLiteral("headline")).toString());
            if (cands.isEmpty()) continue;
            QJsonObject o;
            o["bullet_index"] = a.bulletIndex;
            o["id"]           = a.idStr;
            o["candidates"]   = cands;
            possibleDuplicates.append(o);
        }
        if (!possibleDuplicates.isEmpty())
            env[QStringLiteral("possible_duplicates")] = possibleDuplicates;
        // ANTS-4572 — fire when the scrub removed ANYTHING, not only a named
        // parameter pair. A batch is where a silent partial scrub hides best:
        // one bullet of many, and nobody re-reads the rendered file.
        if (!scrubbedRollup.isEmpty() || scrubbedUnnamedRollup > 0) {
            QJsonArray names;
            for (const QString &n : scrubbedRollup) names.append(n);
            QJsonObject warn;
            warn["code"]            = QStringLiteral("body_scrubbed_tool_xml");
            warn["message"]         = QStringLiteral(
                "Stripped leaked tool-call XML from bullet bodies; resend "
                "any named siblings as proper JSON fields if intended, and "
                "re-read the stored bodies if the count is unexpected.");
            if (!names.isEmpty()) warn["lost_parameters"] = names;
            if (scrubbedUnnamedRollup > 0)
                warn["unnamed_fragments_removed"] = scrubbedUnnamedRollup;
            rlAddWarning(env, warn);
        }
        // ANTS-4527 — rolled up across the batch, as the scrub warning is.
        if (const QJsonObject ev = rlEvidenceAdvisory(evNotPathRollup);
            !ev.isEmpty()) {
            rlAddWarning(env, ev);
        }
        if (rcReturnHeadlineOnly(req)) {
            QJsonArray postBullets;
            for (const Accepted &a : accepted)
                postBullets.append(rcCompactBullet(
                    a.idStr,
                    a.bulletReq.value(QStringLiteral("status")).toString(),
                    a.bulletReq.value(QStringLiteral("headline")).toString()));
            env[QStringLiteral("post_bullets")] = postBullets;
        }
        return QJsonDocument(env);
    }

    // 7. Format every accepted bullet via the shared helper.
    QStringList bulletBlocks;
    qint64 totalBytes = 0;
    for (const Accepted &a : accepted) {
        QStringList scrubbed;
        const QString blk = formatRoadmapBullet(
            a.bulletReq, a.idStr, a.emoji, scrubbed, &scrubbedUnnamedRollup);
        bulletBlocks.append(blk);
        totalBytes += blk.toUtf8().size();
        // Dedup scrubbed names across all bullets.
        for (const QString &n : scrubbed)
            if (!scrubbedRollup.contains(n)) scrubbedRollup.append(n);
    }

    // 8. Splice all blocks at sec->lineEnd in document order.
    QStringList docLines = markdown.split(QChar('\n'));
    const int insertAt = sec->lineEnd;
    QJsonArray emittedLines;
    int cursor = insertAt;
    QStringList toSplice;
    for (const QString &blk : bulletBlocks) {
        QString trimmedBlk = blk;
        if (trimmedBlk.endsWith(QChar('\n'))) trimmedBlk.chop(1);
        const QStringList blkLines = trimmedBlk.split(QChar('\n'));
        emittedLines.append(cursor + 1);   // 1-based line of this bullet's first emitted line
        cursor += blkLines.size();
        for (const QString &l : blkLines) toSplice.append(l);
    }
    for (int i = toSplice.size() - 1; i >= 0; --i)
        docLines.insert(insertAt, toSplice.at(i));
    const QString updated = docLines.join(QChar('\n'));

    // ANTS-2077 — dry_run preview: return the would-be ids, formatted
    // bullets and 1-based insertion lines WITHOUT writing ROADMAP.md or
    // bumping .roadmap-counter. applied_count stays 0 (nothing written);
    // would_apply_count carries the count that a real call would apply.
    if (dryRun) {
        QJsonArray ids;
        for (const Accepted &a : accepted) ids.append(a.idStr);
        QJsonArray previewBullets;
        for (const QString &blk : bulletBlocks) {
            QString b = blk;
            if (b.endsWith(QChar('\n'))) b.chop(1);
            previewBullets.append(b);
        }
        QJsonObject out;
        out["ok"]                = true;
        out["op"]                = QStringLiteral("append_batch");
        out["dry_run"]           = true;
        out["file"]              = QStringLiteral("ROADMAP.md");
        out["would_be_ids"]      = ids;   // ANTS-4508 — not `ids`: a preview
                                          // must not read as a reservation
        out["lines"]             = emittedLines;
        out["bullets"]           = previewBullets;
        out["applied_count"]     = 0;
        out["would_apply_count"] = accepted.size();
        out["skipped"]           = skipped;
        out["skipped_count"]     = skipped.size();
        out["bytes"]             = totalBytes;
        if (counterReconciled)        // ANTS-2179 — last allocated id
            out["counter_advanced_to"] = nextId - 1;
        if (counterReconciled) rlExplainCounterFloor(out, counter, maxFileId);
        return QJsonDocument(out);
    }

    // 9. Atomic write — ROADMAP first, then counter (parity with append).
    QSaveFile rw(roadmapPath);
    if (!rw.open(QIODevice::WriteOnly | QIODevice::Text))
        return rlErr(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("roadmap_log: could not open \"%1\" for writing")
                .arg(roadmapPath));
    const QByteArray utf8 = updated.toUtf8();
    if (rw.write(utf8) != utf8.size() || !rw.commit())
        return rlErr(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("roadmap_log: atomic write of \"%1\" failed")
                .arg(roadmapPath));

    auto rollbackRoadmap = [&]() {
        QSaveFile restore(roadmapPath);
        if (!restore.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        const QByteArray orig = markdown.toUtf8();
        if (restore.write(orig) == orig.size()) restore.commit();
    };

    // ANTS-2078 — stable_prefix wrote no counter to bump.
    if (!useStablePrefix) {
    const qint64 newCounter = nextId - 1;   // last allocated id
    QSaveFile cw(counterPath);
    if (!cw.open(QIODevice::WriteOnly | QIODevice::Text)) {
        rollbackRoadmap();
        return rlErr(QStringLiteral("counter_write_failed"),
            QStringLiteral("roadmap_log: could not open "
                           ".roadmap-counter for writing"));
    }
    const QByteArray cv =
        (QString::number(newCounter) + QChar('\n')).toUtf8();
    bool counterCommitted = (cw.write(cv) == cv.size());
    if (counterCommitted) {
        // Reuse the same file-scope test seam ANTS-1433 added at :2793.
        if (g_forceCounterCommitFail) {
            cw.cancelWriting();
            counterCommitted = false;
        } else {
            counterCommitted = cw.commit();
        }
    }
    if (!counterCommitted) {
        rollbackRoadmap();
        return rlErr(QStringLiteral("counter_write_failed"),
            QStringLiteral("roadmap_log: atomic write of "
                           ".roadmap-counter failed"));
    }
    }   // ANTS-2078 — end !useStablePrefix counter write

    // 10. Success envelope.
    QJsonArray ids;
    for (const Accepted &a : accepted) ids.append(a.idStr);

    // ANTS-2043 — non-blocking near-duplicate advisory, per accepted
    // bullet (checked against the pre-splice on-disk bullets, so a
    // just-appended bullet can't match itself). Only bullets with at
    // least one candidate appear; absent when the whole batch is clean.
    QJsonArray possibleDuplicates;
    for (const Accepted &a : accepted) {
        const QString hl =
            a.bulletReq.value(QStringLiteral("headline")).toString();
        const QJsonArray cands =
            rcComputePossibleDuplicates(preflightBullets, hl);
        if (cands.isEmpty()) continue;
        QJsonObject o;
        o["bullet_index"] = a.bulletIndex;
        o["id"]           = a.idStr;
        o["candidates"]   = cands;
        possibleDuplicates.append(o);
    }

    QJsonObject out;
    out["ok"]            = true;
    out["op"]            = QStringLiteral("append_batch");
    out["file"]          = QStringLiteral("ROADMAP.md");
    out["ids"]           = ids;
    out["lines"]         = emittedLines;
    out["applied_count"] = accepted.size();
    out["skipped"]       = skipped;
    out["skipped_count"] = skipped.size();
    out["bytes_written"] = totalBytes;
    if (counterReconciled) {          // ANTS-2179 — self-healed high-water
        out["counter_advanced_to"] = nextId - 1;
        rlExplainCounterFloor(out, counter, maxFileId);
    }
    if (!possibleDuplicates.isEmpty()) {
        out["possible_duplicates"] = possibleDuplicates;
    }
    if (!scrubbedRollup.isEmpty()) {
        QJsonArray names;
        for (const QString &n : scrubbedRollup) names.append(n);
        QJsonObject warn;
        warn["code"]            = QStringLiteral("body_scrubbed_tool_xml");
        warn["message"]         = QStringLiteral(
            "Stripped leaked <parameter name=\"…\"> tool-call XML "
            "from bullet bodies; resend as proper JSON fields if "
            "intended.");
        warn["lost_parameters"] = names;
        rlAddWarning(out, warn);
    }
    // ANTS-4527 — rolled up over the batch's bullets, like the scrub warning.
    {
        QStringList bad;
        for (const QJsonValue &bv : req.value(QStringLiteral("bullets")).toArray()) {
            const QJsonObject ev = rlEvidenceAdvisoryForReq(bv.toObject());
            for (const QJsonValue &e : ev.value(QStringLiteral("elements")).toArray())
                bad << e.toString();
        }
        if (const QJsonObject ev = rlEvidenceAdvisory(bad); !ev.isEmpty())
            rlAddWarning(out, ev);
    }
    // ANTS-2080 — confirm-after compact echo of every applied bullet.
    if (rcReturnHeadlineOnly(req)) {
        QJsonArray postBullets;
        for (const Accepted &a : accepted) {
            postBullets.append(rcCompactBullet(
                a.idStr,
                a.bulletReq.value(QStringLiteral("status")).toString(),
                a.bulletReq.value(QStringLiteral("headline")).toString()));
        }
        out["post_bullets"] = postBullets;
    }
    return QJsonDocument(out);
}

// ANTS-4070 — test seams for the two section-scoped store ops
// (m_main-independent: both reach the store through roadmapWriteTarget()).
QJsonDocument RemoteControl::cmdRoadmapLogRotateMinorForTest(
        const QJsonObject &req) {
    return cmdRoadmapLogRotateMinor(req);
}

QJsonDocument RemoteControl::cmdRoadmapLogRetitleSectionForTest(
        const QJsonObject &req) {
    return cmdRoadmapLogRetitleSection(req);
}

namespace {

QJsonDocument rcSectionOpErr(const QString &code, const QString &message) {
    QJsonObject env;
    env[QStringLiteral("ok")]    = false;
    env[QStringLiteral("code")]  = code;
    env[QStringLiteral("error")] = message;
    return QJsonDocument(env);
}

}  // namespace

std::optional<RemoteControl::RoadmapWriteTarget>
RemoteControl::roadmapSectionOpTarget(const QJsonObject &req,
                                      QString *projectRoot,
                                      QString *roadmapPath,
                                      QJsonDocument *refusal) const {
    const QString callerRaw = req.value(QStringLiteral("caller_cwd")).toString();
    if (callerRaw.isEmpty()) {
        *refusal = rcSectionOpErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: caller_cwd is required"));
        return std::nullopt;
    }
    const QString callerCanonical = QFileInfo(callerRaw).canonicalFilePath();
    if (callerCanonical.isEmpty()) {
        *refusal = rcSectionOpErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: caller_cwd \"%1\" does not canonicalise "
                           "to an existing directory").arg(callerRaw));
        return std::nullopt;
    }
    // ANTS-4602 — assigned as soon as each is known, NOT on the success path
    // alone. Two callers (`repair_trailers`, `backfill_dates`) remap this
    // function's refusals and interpolate `root.isEmpty() ? roadmapPath : root`
    // into the message; assigning at the end left both empty on every refusal,
    // so the one envelope a caller has to act on named neither the project nor
    // the file, and its ternary had nothing to fall back to.
    *projectRoot = callerCanonical;

    const QString found = findRoadmapUnder(callerCanonical);
    if (found.isEmpty()) {
        *refusal = rcSectionOpErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: no ROADMAP.md under \"%1\"")
                .arg(callerCanonical));
        return std::nullopt;
    }

    *roadmapPath = found;
    // ANTS-4884 — upgrade the root now that the roadmap has resolved: `found`
    // was matched under the project root, and the render's containment check
    // and the store's project key both need that rather than the caller's cwd.
    // The early assignment above stays as ANTS-4602 requires, because it is
    // what the refusal messages above this line interpolate.
    *projectRoot = rcProjectRootFor(callerCanonical);

    QFile rf(found);
    if (!rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *refusal = rcSectionOpErr(QStringLiteral("roadmap_read_failed"),
            QStringLiteral("roadmap_log: could not read \"%1\"").arg(found));
        return std::nullopt;
    }
    const QString markdown = QString::fromUtf8(rf.readAll());
    rf.close();

    RoadmapSource::ReadError why = RoadmapSource::ReadError::None;
    QString seamErr;
    // ANTS-3863 — fromMemory: `markdown` is already read and spliced below.
    auto seamText = RoadmapSource::RoadmapText::fromMemory(markdown);
    const auto target =
        roadmapWriteTarget(callerCanonical, seamText, &why, &seamErr);
    QJsonObject seamRefusal;
    if (rcRoadmapSourceRefused(seamRefusal, why, seamErr)) {
        *refusal = QJsonDocument(seamRefusal);
        return std::nullopt;
    }
    if (!target) {
        // Not migrated. Rotation stays /bump's snip-and-create there, and
        // retitling a heading is a hand edit — growing a markdown writer for
        // either would be a second writer for one file (ANTS-3809 INV-2).
        *refusal = rcSectionOpErr(QStringLiteral("op_unsupported"),
            QStringLiteral("roadmap_log: \"%1\" is not store-migrated — this op "
                           "has no markdown path").arg(found));
        return std::nullopt;
    }

    return target;
}

// ANTS-4070 § 2.2 — rotate_minor. A caller over the store's own setters:
// select the closed minor's sections by TITLE, take their descendants, re-slug
// them the way the next import will, reassign their source_path, and let
// RoadmapWrite::commitAndRender() do the rest.
QJsonDocument RemoteControl::cmdRoadmapLogRotateMinor(const QJsonObject &req) {
    if (!req.contains(QStringLiteral("minor")))
        return rcSectionOpErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: rotate_minor requires `minor`"));

    // § 3.9's FILENAME rule, and the single place it is stated: the derived
    // path is `docs/roadmap/<captures>.md`, so validating the argument against
    // this shape is what makes the path satisfy
    // RoadmapMigrateLoad::isPlaceableSourcePath() by construction rather than
    // by a second copy of the same regex.
    static const QRegularExpression kMinorRx(
        QStringLiteral("\\A(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\z"));
    const QString minorArg = req.value(QStringLiteral("minor")).toString();
    const auto minorMatch = kMinorRx.match(minorArg);
    if (!minorMatch.hasMatch())
        return rcSectionOpErr(QStringLiteral("bad_args"),
            QStringLiteral("roadmap_log: `minor` must be <MAJOR>.<MINOR> with no "
                           "leading zeros and no patch component (got \"%1\")")
                .arg(minorArg));
    const QString major = minorMatch.captured(1);
    const QString minor = minorMatch.captured(2);

    // Derived, never passed: a caller-supplied path would move § 3.9's naming
    // rule out of the one place that states it, and a non-conforming name
    // renders a file the migration's own discovery then refuses to read.
    const QString archiveRel =
        QStringLiteral("docs/roadmap/%1.%2.md").arg(major, minor);
    const QString slugPrefix = major + QLatin1Char('-') + minor + QLatin1Char('-');

    QString root, roadmapPath;
    QJsonDocument refusal;
    const auto target =
        roadmapSectionOpTarget(req, &root, &roadmapPath, &refusal);
    if (!target) return refusal;
    RoadmapStore &store   = *target->store;
    const qint64 projectId = target->projectId;
    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();

    QString err;
    const auto allOpt = store.listSections(projectId, &err);
    if (!allOpt)
        return rcSectionOpErr(QStringLiteral("store_failed"), err);
    const QVector<RoadmapStore::SectionRow> all = *allOpt;

    // The id arrives on the row (ANTS-3817) — it is the handle
    // setSectionSlug() / setSectionSource() take. This was a findSection() per
    // section with a "vanished mid-read" path for the row disappearing between
    // the two queries; one read cannot race itself, so both are gone.
    QHash<QString, qint64> idBySlug;
    QHash<qint64, RoadmapStore::SectionRow> rowById;
    QHash<qint64, QVector<qint64>> childrenOf;
    for (const auto &s : all) {
        idBySlug.insert(s.slug, s.sectionId);
        rowById.insert(s.sectionId, s);
    }
    for (auto it = rowById.cbegin(); it != rowById.cend(); ++it)
        if (it.value().parentId)
            childrenOf[*it.value().parentId].append(it.key());

    // § 2.2's three-case title match. Case 2 excludes `.` on purpose, so the
    // three are disjoint: `[^0-9.]` rejects `## 0.5.x and 0.6.x — archived`
    // (a signpost that belongs to neither minor) and `\.[0-9]` rejects
    // `## 0.70.0`, which a bare startsWith() would claim.
    const QRegularExpression titleRx(
        QStringLiteral("\\Av?%1\\.%2(?:\\z|[^0-9.]|\\.[0-9])").arg(major, minor));

    QVector<qint64> frontier;
    for (auto it = rowById.cbegin(); it != rowById.cend(); ++it) {
        if (it.value().level != 2) continue;
        if (titleRx.match(it.value().title).hasMatch())
            frontier.append(it.key());
    }
    if (frontier.isEmpty())
        return rcSectionOpErr(QStringLiteral("section_not_found"),
            QStringLiteral("roadmap_log: no top-level section's title names "
                           "minor %1").arg(minorArg));

    // Every descendant moves with its parent (§ 3.9 rotates the heading "and
    // its sub-headings").
    QSet<qint64> candidates;
    while (!frontier.isEmpty()) {
        const qint64 id = frontier.takeLast();
        if (candidates.contains(id)) continue;
        candidates.insert(id);
        for (qint64 child : childrenOf.value(id))
            frontier.append(child);
    }

    // Skip on EQUALITY with the derived path, not on "already archived": a 0.7
    // section mistakenly sitting in 0.6.md must be reassigned, or its parent
    // renders into 0.7.md while it stays in 0.6.md.
    QVector<RoadmapStore::SectionRow> moveSet;
    QSet<qint64> moveIds;
    for (qint64 id : candidates) {
        const RoadmapStore::SectionRow &row = rowById[id];
        if (row.sourcePath && *row.sourcePath == archiveRel) continue;
        moveSet.append(row);
        moveIds.insert(id);
    }
    std::sort(moveSet.begin(), moveSet.end(), sectionOrderLess);

    // The openness guard covers the whole MOVE SET, matched sections and
    // descendants alike — a guard reading only the level == 2 matches would
    // archive an in-progress item living under a `###` child while reporting
    // success. "Open" is RoadmapRender::isOpen()'s: 📋, 🚧 AND 💭.
    if (!moveIds.isEmpty()) {
        const auto itemsOpt = store.readItems(projectId, &err);
        if (!itemsOpt)
            return rcSectionOpErr(QStringLiteral("store_failed"), err);
        QStringList openIds;
        for (const auto &it : *itemsOpt)
            if (moveIds.contains(it.sectionId) && RoadmapRender::isOpen(it.status))
                openIds << it.id;
        if (!openIds.isEmpty()) {
            openIds.sort();
            return rcSectionOpErr(QStringLiteral("minor_not_closed"),
                QStringLiteral("roadmap_log: minor %1 still holds open items: %2")
                    .arg(minorArg, openIds.join(QStringLiteral(", "))));
        }
    }

    // A rotation must not empty the live file: the render assembles content
    // only for paths that still have sections, so a file with none is never
    // written and is left on disk with its old content while the store says
    // otherwise.
    int liveRemaining = 0;
    for (auto it = rowById.cbegin(); it != rowById.cend(); ++it)
        if (!it.value().sourcePath && !moveIds.contains(it.key()))
            ++liveRemaining;
    if (!moveIds.isEmpty() && liveRemaining == 0)
        return rcSectionOpErr(QStringLiteral("bad_args"),
            QStringLiteral("roadmap_log: rotating minor %1 would leave the live "
                           "roadmap with no sections, and the render never "
                           "rewrites a file that has none").arg(minorArg));

    // § 2.2's re-slug rule, and it is a WHOLE-SET computation: uniqueSlug()
    // over the move set alone, seeded empty, in the render's document order —
    // exactly what the next import's walk of that archive will derive, because
    // the archive will hold exactly this set in exactly this order.
    QHash<qint64, QString> newSlugOf;
    QStringList movedSlugs;
    {
        QSet<QString> seen;
        for (const auto &row : moveSet) {
            const QString slug =
                slugPrefix + RoadmapIndex::uniqueSlug(seen, row.title);
            newSlugOf.insert(idBySlug.value(row.slug), slug);
            movedSlugs << slug;
        }
    }

    // A new slug landing on a section OUTSIDE the move set is refused, never
    // auto-suffixed — the suffix invented here could differ from the one a
    // re-import assigns, which is the divergence this whole design removes.
    for (auto it = newSlugOf.cbegin(); it != newSlugOf.cend(); ++it) {
        const qint64 holder = idBySlug.value(it.value(), 0);
        if (holder != 0 && !moveIds.contains(holder))
            return rcSectionOpErr(QStringLiteral("bad_args"),
                QStringLiteral("roadmap_log: rotating minor %1 would give a moved "
                               "section the slug \"%2\", which section \"%2\" "
                               "already holds").arg(minorArg, it.value()));
    }

    // The live side frees the slugs it gives up. If a REMAINING live section
    // holds a uniqueSlug()-disambiguated slug whose un-suffixed base the move
    // frees, the next import re-derives a different slug for a section this
    // operation never touched.
    {
        QSet<QString> freed;
        for (const auto &row : moveSet) freed.insert(row.slug);
        QStringList clashes;
        for (auto it = rowById.cbegin(); it != rowById.cend(); ++it) {
            const RoadmapStore::SectionRow &row = it.value();
            if (row.sourcePath || moveIds.contains(it.key())) continue;
            const QString base = RoadmapIndex::slugifyHeading(row.title);
            if (row.slug != base && freed.contains(base))
                clashes << QStringLiteral("%1 (would re-derive \"%2\")")
                               .arg(row.slug, base);
        }
        if (!clashes.isEmpty()) {
            clashes.sort();
            return rcSectionOpErr(QStringLiteral("bad_args"),
                QStringLiteral("roadmap_log: rotating minor %1 frees a slug base a "
                               "remaining live section was disambiguated out of: "
                               "%2").arg(minorArg, clashes.join(QStringLiteral(", "))));
        }
    }

    const auto mutate = [&](QString *mErr) -> bool {
        for (const auto &row : moveSet) {
            const qint64 id = idBySlug.value(row.slug);
            if (!store.setSectionSlug(id, newSlugOf.value(id), mErr))
                return false;
            if (!store.setSectionSource(id, archiveRel, mErr))
                return false;
        }
        return true;
    };

    RoadmapRender::Outcome outcome;
    QString writeErr;
    const auto r = RoadmapWrite::commitAndRender(
        store, projectId, root, roadmapPath, dryRun, mutate, &outcome, &writeErr);
    QJsonObject env;
    if (rcRoadmapWriteRefused(env, r, writeErr, outcome))
        return QJsonDocument(env);

    env[QStringLiteral("ok")]           = true;
    env[QStringLiteral("op")]           = QStringLiteral("rotate_minor");
    env[QStringLiteral("archive_path")] = archiveRel;
    // The slugs the moved sections carry AFTERWARDS — the address a caller
    // needs next, on the same reasoning retitle_section reports the new `slug`
    // rather than the old one. `sections_moved` is always sections.length: a
    // count of matched `##` sections would exclude descendants, and on the
    // idempotent re-run it would be non-zero while nothing moved.
    env[QStringLiteral("sections")]       = QJsonArray::fromStringList(movedSlugs);
    env[QStringLiteral("sections_moved")] = movedSlugs.size();
    rcRoadmapWriteFields(env, outcome, dryRun);   // ANTS-4463
    return QJsonDocument(env);
}

// ANTS-4070 § 2.3 — retitle_section. The slug IS recomputed from the new
// title: a section's slug is derived, not round-tripped, so keeping the stored
// one does not keep it stable — it makes the store disagree with what any
// re-import derives.
QJsonDocument RemoteControl::cmdRoadmapLogRetitleSection(const QJsonObject &req) {
    if (!req.contains(QStringLiteral("section")))
        return rcSectionOpErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: retitle_section requires `section`"));
    if (!req.contains(QStringLiteral("title")))
        return rcSectionOpErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: retitle_section requires `title`"));
    const QString sectionSlug = req.value(QStringLiteral("section")).toString();
    const QString title       = req.value(QStringLiteral("title")).toString();

    if (title.trimmed().isEmpty())
        return rcSectionOpErr(QStringLiteral("bad_args"),
            QStringLiteral("roadmap_log: `title` must not be empty or "
                           "whitespace-only"));
    if (title.contains(QChar('\n')))
        return rcSectionOpErr(QStringLiteral("bad_args"),
            QStringLiteral("roadmap_log: `title` is one heading line and must "
                           "not contain a newline"));
    // Emptiness is tested on the SLUG, not only on the title:
    // slugifyHeading() keeps only letters and digits, so `———` is neither
    // empty nor whitespace-only and slugifies to "" — an unaddressable
    // section, in a UNIQUE column where a second one surfaces as store_failed
    // rather than as a caller-side refusal. uniqueSlug() does not save it: an
    // empty base is returned WITHOUT being inserted into `seen`.
    const QString newSlug = RoadmapIndex::slugifyHeading(title);
    if (newSlug.isEmpty())
        return rcSectionOpErr(QStringLiteral("bad_args"),
            QStringLiteral("roadmap_log: `title` \"%1\" has no letters or digits, "
                           "so it slugifies to the empty string and the section "
                           "would be unaddressable").arg(title));

    QString root, roadmapPath;
    QJsonDocument refusal;
    const auto target =
        roadmapSectionOpTarget(req, &root, &roadmapPath, &refusal);
    if (!target) return refusal;
    RoadmapStore &store    = *target->store;
    const qint64 projectId = target->projectId;
    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();

    QString err;
    const auto sectionId = store.findSection(projectId, sectionSlug, &err);
    if (!sectionId)
        return rcSectionOpErr(QStringLiteral("section_not_found"),
            QStringLiteral("roadmap_log: section \"%1\" is not in the store")
                .arg(sectionSlug));
    const auto row = store.readSection(*sectionId, &err);
    if (!row)
        return rcSectionOpErr(QStringLiteral("store_failed"), err);

    const auto allOpt = store.listSections(projectId, &err);
    if (!allOpt)
        return rcSectionOpErr(QStringLiteral("store_failed"), err);

    if (newSlug != sectionSlug) {
        // Forward — the new heading slugifies onto a slug another section holds.
        for (const auto &s : *allOpt) {
            if (s.slug == newSlug)
                return rcSectionOpErr(QStringLiteral("bad_args"),
                    QStringLiteral("roadmap_log: \"%1\" slugifies to \"%2\", which "
                                   "section \"%2\" already holds — a collision is "
                                   "refused, never auto-suffixed")
                        .arg(title, newSlug));
        }
        // Backward — the retitled section is the un-suffixed HEAD of a family
        // another section was disambiguated out of. Retitling it frees the
        // base, so the next import hands it to the sibling, changing the slug
        // of a section this call never touched. Scoped to this section's own
        // family: a condition phrased over "any section anywhere holds a
        // disambiguated slug" would refuse every retitle on any project with
        // one repeated heading.
        if (sectionSlug == RoadmapIndex::slugifyHeading(row->title)) {
            for (const auto &s : *allOpt) {
                if (s.slug == sectionSlug) continue;
                const QString base = RoadmapIndex::slugifyHeading(s.title);
                if (s.slug != base && base == sectionSlug)
                    return rcSectionOpErr(QStringLiteral("bad_args"),
                        QStringLiteral("roadmap_log: \"%1\" is the un-suffixed head "
                                       "of a family section \"%2\" was "
                                       "disambiguated out of — retitling it frees "
                                       "\"%1\" and the next import would re-derive "
                                       "it for \"%2\"").arg(sectionSlug, s.slug));
            }
        }
    }

    const auto mutate = [&](QString *mErr) -> bool {
        // updateSection() takes the whole tuple and a partial update has no
        // meaning, so every field it does not change is passed back verbatim.
        if (!store.updateSection(*sectionId, title, row->level, row->position,
                                 row->parentId, mErr))
            return false;
        return newSlug == sectionSlug
               || store.setSectionSlug(*sectionId, newSlug, mErr);
    };

    RoadmapRender::Outcome outcome;
    QString writeErr;
    const auto r = RoadmapWrite::commitAndRender(
        store, projectId, root, roadmapPath, dryRun, mutate, &outcome, &writeErr);
    QJsonObject env;
    if (rcRoadmapWriteRefused(env, r, writeErr, outcome))
        return QJsonDocument(env);

    env[QStringLiteral("ok")]   = true;
    env[QStringLiteral("op")]   = QStringLiteral("retitle_section");
    env[QStringLiteral("slug")] = newSlug;
    // So a caller holding the old address learns it moved, rather than
    // discovering it at the next call that fails.
    env[QStringLiteral("previous_slug")] = sectionSlug;
    env[QStringLiteral("title")]         = title;
    rcRoadmapWriteFields(env, outcome, dryRun);   // ANTS-4463
    return QJsonDocument(env);
}

// ANTS-1248: workspace_search — structured ripgrep wrapper for MCP +
// IPC. Replaces `Bash grep -rn 'pattern' src/` with a server-clamped
// {matches[], truncated, elapsed_ms} envelope. ~6-15 K tokens saved
// per typical "investigate a bug" session.
//
// Process model: QProcess::start("rg", argv) — argv-only, no shell
// interpolation (INV-3). Hard wall-clock budget enforced via
// waitForFinished(kWorkspaceSearchHardKillMs) then SIGTERM, then
// waitForFinished(kWorkspaceSearchKillGraceMs) then SIGKILL (INV-5).
// Stderr capped at 4 KiB and surfaced only in the ok:false branch
// to avoid path enumeration on the ok:true path (INV-8).
namespace rcdetail {
// Forward decl — definition in the second anonymous namespace below
// (it lives next to the rest of the git_state helpers). Both
// unnamed-namespace blocks in this TU share linkage.
QString resolveRootCanonical(MainWindow *main);
// ANTS-1391 — read-verb overload (see top-of-file forward decl).
QString resolveRootCanonical(MainWindow *main, const QJsonObject &req);
// ANTS-1565: default budget raised from 2 s (ANTS-1248) to 5 s — the
// pre-rg setup (gitignore parse, glob expansion, ANTS-1501 dedup
// grouping) is a fixed-cost floor that left the original 2 s ceiling
// tight on > 2 k-file projects. Callers can override via the new
// `timeout_sec` arg (INV-2), clamped to [kWorkspaceSearchMinBudgetMs,
// kWorkspaceSearchMaxBudgetMs].
constexpr int kWorkspaceSearchHardKillMs   = 5000;  // ANTS-1248/1565-INV-1
constexpr int kWorkspaceSearchMinBudgetMs  = 1000;  // ANTS-1565-INV-2 floor
constexpr int kWorkspaceSearchMaxBudgetMs  = 30000; // ANTS-1565-INV-2/5 cap
constexpr int kWorkspaceSearchKillGraceMs  =  200;  // ANTS-1248-INV-5
constexpr int kWorkspaceSearchMaxResultsCap = 500;  // ANTS-1248-INV-4
constexpr int kWorkspaceSearchMaxColumns    = 500;
// ANTS-3732 — rg worker threads. Was 1 from ANTS-1248, filed there under
// "DoS mitigation"; measurement says that was the wrong lever. `--threads 1`
// caps no per-line work (that is --max-columns) and costs the SAME total CPU
// (0.84 cpu-s at 1 thread vs 0.82 at 12, measured) — what it actually removes
// is rg's parallel directory walker, so every file open is serialised. On this
// project's spinning disk (/mnt/Games, ROTA=1) seek latency then dominates:
// a cold-cache scan of 31.5k files took 24.6 s single-threaded vs 0.09 s
// multi-threaded, and even the ordinary 1.6k-file scan overran the 5 s budget
// whenever a concurrent build had churned the page cache. The wall budget
// (kWorkspaceSearchHardKillMs) is the real resource cap; threads are not.
// 4 recovers ~94% of the speedup and matches the project-wide parallelism
// cap used for ctest (see CLAUDE.md) so a heavy desktop session can't thrash.
constexpr int kWorkspaceSearchThreads       =   4;  // ANTS-3732
constexpr int kWorkspaceSearchStderrCapBytes = 4096; // ANTS-1248-INV-8
constexpr int kWorkspaceSearchGlobBytesCap   =  256; // ANTS-1248-INV-9
// ANTS-3548 — default-ON per-match line-length clip. An absent
// `max_match_bytes` clips to this many UTF-8 bytes/line (token-saver
// default-ON per the "savers default ON, keep an off switch" rule); an
// explicit `<= 0` opts out. Within the [50, 10000] effective clip range.
constexpr int kDefaultMaxMatchBytes         =  512;  // ANTS-3548

QJsonObject wsErr(const char *code, const QString &message) {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = message;
    o["code"]  = QString::fromLatin1(code);
    return o;
}

}  // namespace rcdetail

// ANTS-2181 — a regex:true alternation that contains very short (<=3 char)
// bare (un-anchored, plain-word) terms substring-matches inside longer words
// (e.g. "tan" inside "constant"), flooding the result set. Collect those
// terms so cmdWorkspaceSearch can advise anchoring them with \b. Heuristic
// and non-load-bearing: a false positive is just an ignorable nudge, a false
// negative simply omits it. Only top-level `|`-split pieces stripped of
// wrapping group syntax that are purely [A-Za-z]{1,3} qualify — any piece
// carrying an anchor (\b, ^, $) or other metacharacter is exempt (that's the
// user already being specific). Caps at 5 distinct terms.
QStringList rcdetail::rcShortBareAltTerms(const QString &pattern) {
    if (!pattern.contains(QChar('|'))) return {};
    static const QRegularExpression bareShort(
        QStringLiteral("^[A-Za-z]{1,3}$"));
    QStringList terms;
    const QStringList pieces = pattern.split(QChar('|'));
    for (QString p : pieces) {
        while (p.startsWith(QStringLiteral("(?:"))) p.remove(0, 3);
        while (p.startsWith(QChar('('))) p.remove(0, 1);
        while (p.endsWith(QChar(')'))) p.chop(1);
        p = p.trimmed();
        if (bareShort.match(p).hasMatch() && !terms.contains(p)) {
            terms.append(p);
            if (terms.size() >= 5) break;
        }
    }
    return terms;
}

// ANTS-3466 — high-precision "you probably meant regex:true" detector for a
// regex:false pattern that returned zero matches. Deliberately narrow: only
// the metacharacters that signal a deliberately-constructed regex and are
// rare/meaningless as a literal search — alternation `|`, wildcard `.*`/`.+`,
// a `[...]` character class, and `\d \w \s \b` escape-classes. A lone `.`,
// `(`, `+` or `?` is EXCLUDED (ubiquitous in literal code searches like
// `cfg.get(` — flagging them would cry wolf and dilute the hint).
bool rcdetail::rcLooksLikeRegexButLiteral(const QString &pattern) {
    if (pattern.contains(QChar('|'))) return true;              // alternation
    if (pattern.contains(QStringLiteral(".*")) ||
        pattern.contains(QStringLiteral(".+"))) return true;    // wildcard
    if (pattern.contains(QChar('[')) && pattern.contains(QChar(']')))
        return true;                                            // char class
    static const QRegularExpression escClass(QStringLiteral("\\\\[dwsbDWSB]"));
    return escClass.match(pattern).hasMatch();                  // \d \w \s \b …
}

// ANTS-4419's sibling, ANTS-4420 — a pattern carrying HTML entities where the
// caller meant the literal characters. Searching an HTML file for heading tags
// as `&lt;h[123][^&gt;]*&gt;` returns a clean zero-match reply and reads as a
// confident "this file has no headings"; the literal `<h[123][ >]` found 11 in
// the same file. The slip is easiest to make precisely when searching HTML/XML,
// because the surrounding conversation is full of escaped markup.
//
// Narrow on purpose: a NAMED or numeric entity, i.e. `&` + name + `;`. A bare
// `&` is ubiquitous in real code (`a && b`, `&ref`) and carries no `;`
// terminator, so it cannot reach this. Fires only on an already-empty result,
// so a legitimate search for the literal text `&amp;` that genuinely finds
// nothing gets an advisory phrased as a question rather than an assertion.
bool rcdetail::rcContainsHtmlEntity(const QString &pattern) {
    static const QRegularExpression ent(
        QStringLiteral("&(lt|gt|amp|quot|apos|nbsp|#[0-9]+|#x[0-9A-Fa-f]+);"));
    return ent.match(pattern).hasMatch();
}
