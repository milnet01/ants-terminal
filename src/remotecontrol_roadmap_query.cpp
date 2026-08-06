// ANTS-3833 TU 3/12 — Roadmap read ops.
#include "remotecontrol.h"
#include "remotecontrol_internal.h"
#include "mcpspill.h"        // ANTS-2094 — read_spill
#include "mainwindow.h"
#include "mcpprojection.h"
#include "paginationengine.h"
#include "roadmapfoldin.h"
#include <QFile>
#include <QFileInfo>

using namespace rcdetail;  // ANTS-3833

// ANTS-3793 § 2.2 rules 1 and 2, discharged once for both readers above. The
// decision itself lives in the free storeFor() so INV-1's two unmigrated cases
// have a driver that needs no RemoteControl; this member owns only the caching.
//
// Absence is NOT remembered: a store that does not exist yet costs one stat per
// call to re-check, and remembering it would serve markdown for the rest of the
// session to a project migrated in between. An OPEN connection is remembered —
// that is the term § 4's p95 budget is built on.
RoadmapStore *RemoteControl::roadmapStoreOrNull(RoadmapSource::ReadError *why,
                                                QString *error) const {
    if (m_roadmapStore)
        return m_roadmapStore.get();
    RoadmapSource::ReadError openWhy = RoadmapSource::ReadError::None;
    QString openErr;
    m_roadmapStore = RoadmapSource::storeFor(RoadmapStore::defaultPath(),
                                             &openWhy, &openErr);
    if (openWhy != RoadmapSource::ReadError::None) {
        if (why)
            *why = openWhy;
        if (error)
            *error = openErr;
        return nullptr;
    }
    return m_roadmapStore.get();
}

// ANTS-3793 § 2.2 — the dispatch on its own. See remotecontrol.h for the one
// site that needs it and why it cannot ask by reading.
bool RemoteControl::roadmapStoreServes(const QString &projectRoot,
                                       const QString &markdown,
                                       RoadmapSource::ReadError *why,
                                       QString *error) const {
    if (why)
        *why = RoadmapSource::ReadError::None;
    if (error)
        error->clear();
    if (projectRoot.isEmpty())
        return false;
    // Same guard as roadmapBullets(): a dropped `why` must not silently
    // downgrade a refusal into "not migrated, parse the markdown".
    RoadmapSource::ReadError localWhy = RoadmapSource::ReadError::None;
    RoadmapStore *store = roadmapStoreOrNull(&localWhy, error);
    const bool served =
        store && RoadmapSource::migratedProject(*store, projectRoot, markdown,
                                                error, &localWhy).has_value();
    if (why)
        *why = localWhy;
    return served;
}

// ANTS-3809 § 2.1 — the write-side dispatch. See remotecontrol.h for the
// contract; it is deliberately roadmapStoreOrNull() plus migratedProject() and
// nothing more, so it cannot disagree with roadmapBullets() about whether a
// project is migrated.
std::optional<RemoteControl::RoadmapWriteTarget>
RemoteControl::roadmapWriteTarget(const QString &projectRoot,
                                  const QString &markdown,
                                  RoadmapSource::ReadError *why,
                                  QString *error) const {
    if (why)
        *why = RoadmapSource::ReadError::None;
    if (error)
        error->clear();
    // Same rule as roadmapBullets(): a caller with no project root cannot ask
    // the marker anything, and passing "" would turn every such call into a
    // refusal rather than the markdown path it belongs on.
    if (projectRoot.isEmpty())
        return std::nullopt;

    // The local rather than `why` directly: a caller that dropped `why` would
    // otherwise turn a refusal into a fall-through to the splice path — the
    // silent fallback INV-1 forbids, arriving through an omitted out-param.
    RoadmapSource::ReadError localWhy = RoadmapSource::ReadError::None;
    RoadmapStore *store = roadmapStoreOrNull(&localWhy, error);
    if (localWhy != RoadmapSource::ReadError::None) {
        if (why)
            *why = localWhy;
        return std::nullopt;
    }
    if (!store)
        return std::nullopt;

    const auto projectId = RoadmapSource::migratedProject(
        *store, projectRoot, markdown, error, &localWhy);
    if (why)
        *why = localWhy;
    if (!projectId)
        return std::nullopt;
    return RoadmapWriteTarget{store, *projectId};
}

// ANTS-3793 § 2.1 — the ReadError → refusal-envelope mapping, once for the
// whole verb layer. Returns true when it filled `out` with a refusal, so a
// read site reads `if (rcRoadmapSourceRefused(...)) return QJsonDocument(out);`.
//
// The codes are docs/standards/mcp-error-codes.md's, and the two failure kinds
// stay distinct because they send the user to different files: StoreFailed is
// the STORE not opening, SourceUnrecognised is the store being fine and the
// ROADMAP.md being absent, empty or mangled.
bool rcdetail::rcRoadmapSourceRefused(QJsonObject &out,
                                   RoadmapSource::ReadError why,
                                   const QString &err) {
    switch (why) {
    case RoadmapSource::ReadError::None:
        return false;
    case RoadmapSource::ReadError::TooLarge:
        out["ok"] = false;
        out["error"] = err.isEmpty()
            ? QStringLiteral("roadmap store project exceeds the read ceiling")
            : err;
        out["code"] = QStringLiteral("too_large");
        return true;
    case RoadmapSource::ReadError::StoreFailed:
        out["ok"] = false;
        out["error"] = err.isEmpty()
            ? QStringLiteral("could not read the roadmap store")
            : err;
        out["code"] = QStringLiteral("read_failed");
        return true;
    case RoadmapSource::ReadError::SourceUnrecognised:
        out["ok"] = false;
        out["error"] = err.isEmpty()
            ? QStringLiteral("migrated project, but its roadmap text is "
                             "unrecognisable")
            : err;
        out["code"] = QStringLiteral("unrecognised_format");
        // ANTS-1463 — every unrecognised_format envelope carries both.
        out["expected_format"] = kUnrecognisedFormatExpected();
        out["hint"] = kUnrecognisedFormatHint();
        return true;
    }
    return false;
}

// ─── ANTS-3809 — the store-backed write path, shared by all eight ops ──────

// § 7's code table, once for the whole verb layer. Mirrors
// rcRoadmapSourceRefused() above and reads the same way at a call site:
// `if (rcRoadmapWriteRefused(out, r, err, outcome)) return QJsonDocument(out);`
//
// All five values carry genuinely different remedies — fill in the missing
// Layman lines, fix the store's contents, fix the store file, re-run the
// render — which is why they are five codes and not one.
bool rcdetail::rcRoadmapWriteRefused(QJsonObject &out, RoadmapWrite::Result r,
                                  const QString &err,
                                  const RoadmapRender::Outcome &outcome) {
    const auto fail = [&out, &err](const char *code, const char *fallback) {
        out[QStringLiteral("ok")] = false;
        out[QStringLiteral("error")] = err.isEmpty() ? QString::fromLatin1(fallback) : err;
        out[QStringLiteral("code")] = QString::fromLatin1(code);
        return true;
    };
    switch (r) {
    case RoadmapWrite::Result::Ok:
        return false;
    case RoadmapWrite::Result::GateUnmet:
        // The gate is per PROJECT, so the caller's own item may be blameless;
        // naming the offenders is the only way the refusal is actionable.
        out[QStringLiteral("gate_failures")] =
            QJsonArray::fromStringList(outcome.gateFailures);
        return fail("render_gate_unmet",
                    "the roadmap render refuses this project: an open item "
                    "carries no Layman: line");
    case RoadmapWrite::Result::RenderFailed:
        return fail("render_failed", "the roadmap render failed");
    case RoadmapWrite::Result::StoreFailed:
        return fail("store_failed", "the roadmap store write failed");
    case RoadmapWrite::Result::PublishFailed:
        // § 2.1 step 7: the store IS committed and stays so. The envelope says
        // which files landed precisely so a caller can tell a total failure
        // from ANTS-3758 § 2.7's partial commit.
        out[QStringLiteral("files_written")] =
            QJsonArray::fromStringList(outcome.filesWritten);
        return fail("write_failed",
                    "the roadmap store committed, but the render did not land");
    }
    return false;
}

// The five trailer keys in ONE place: the item column, where the key sits in a
// TrailerValues, and — for the two list-valued ones — the split form and the
// ItemWrite column to compare against. § 2.5 and § 2.6 both walk exactly this
// set, and two copies of it would be two answers to "which keys are trailer
// keys".
struct RlTrailerKey {
    const char *field;
    RoadmapParse::TrailerMatch RoadmapParse::TrailerValues::*match;
    // nullptr on the three scalar keys; set on lanes/evidence, whose stored
    // form is a JSON array and not the raw capture.
    QStringList RoadmapParse::TrailerValues::*list;
    QString     RoadmapStore::ItemWrite::*col;
    QStringList RoadmapStore::ItemWrite::*colList;
};
static const RlTrailerKey kRlTrailerKeys[] = {
    {"kind",   &RoadmapParse::TrailerValues::kind,   nullptr,
     &RoadmapStore::ItemWrite::kind,   nullptr},
    {"layman", &RoadmapParse::TrailerValues::layman, nullptr,
     &RoadmapStore::ItemWrite::layman, nullptr},
    {"source", &RoadmapParse::TrailerValues::source, nullptr,
     &RoadmapStore::ItemWrite::source, nullptr},
    {"lanes",  &RoadmapParse::TrailerValues::lanes,
     &RoadmapParse::TrailerValues::lanesList,  nullptr,
     &RoadmapStore::ItemWrite::lanes},
    {"evidence", &RoadmapParse::TrailerValues::evidence,
     &RoadmapParse::TrailerValues::evidenceList, nullptr,
     &RoadmapStore::ItemWrite::evidence},
};

// The stored form of a list-valued trailer column: a JSON array of strings.
// setItemField() takes a QString for every field and treats it as JSON for
// lanes/evidence — it parses, refuses unless it is an array of strings, and
// canonicalises itself (ANTS-3767), so a caller that pre-canonicalises is
// merely doing it twice and one that passes the joined prose form ("a, b") is
// refused as "not JSON".
static QString rlJsonArrayText(const QStringList &list) {
    return QString::fromUtf8(
        QJsonDocument(QJsonArray::fromStringList(list)).toJson(QJsonDocument::Compact));
}

// What `body` yields for one trailer key, in the same form the column stores.
static QString rlBodyValueFor(const RoadmapParse::TrailerValues &tv,
                              const RlTrailerKey &k) {
    if ((tv.*(k.match)).value.isEmpty())
        return QString();
    return k.list ? rlJsonArrayText(tv.*(k.list)) : (tv.*(k.match)).value;
}

// § 2.5 — refuse a trailer-column write the body arriving in the same request
// would hide. Returns true when the op must refuse, filling *error with the
// message a caller can act on.
//
// The predicate is VALUE DIFFERENCE alone, over all five keys, and `anchored`
// is deliberately not part of it. A rendered bullet is head line, then body,
// then the canonical trailer lines, and every one of the five matchers takes
// its FIRST match over that text — so a body occurrence is reached before the
// column's own line whatever either looks like. Gating on `anchored == false`
// would exempt the commonest shadowing shape there is: a stale `Kind:`
// continuation line, which begins a line and so is anchored == true.
//
// `anchored` still earns its place — in the MESSAGE, where it picks between
// the two remedies the caller is owed.
static bool rlBodyShadows(const RoadmapParse::TrailerValues &tv,
                          const QString &body, const RlTrailerKey &k,
                          const QString &supplied, QString *error) {
    const QString fromBody = rlBodyValueFor(tv, k);
    // Value difference, not presence. A migrated item's body agrees with its
    // columns by construction (ANTS-3808 § 2.3.1), so the ordinary bullet
    // carrying its own `Kind:` line does not refuse.
    if (fromBody.isEmpty() || fromBody == supplied)
        return false;
    if (!error)
        return true;

    const RoadmapParse::TrailerMatch &m = tv.*(k.match);
    // The shadowing text itself: from the capture to the end of its line.
    QString quoted;
    if (m.offset >= 0 && m.offset <= body.size()) {
        const int nl = body.indexOf(QLatin1Char('\n'), m.offset);
        const int lineStart = body.lastIndexOf(QLatin1Char('\n'), m.offset) + 1;
        quoted = body.mid(lineStart, (nl < 0 ? body.size() : nl) - lineStart).trimmed();
        if (quoted.size() > 160)
            quoted.truncate(160);
    }
    *error = QStringLiteral(
                 "the body shadows the `%1` column: it yields \"%2\" while this "
                 "request writes \"%3\", and a re-parse reaches the body first. %4 "
                 "(shadowing text: %5)")
                 .arg(QString::fromLatin1(k.field), fromBody, supplied,
                      m.anchored
                          ? QStringLiteral("Delete or correct that stale trailer "
                                           "line in the body")
                          : QStringLiteral("Reword that mention, or wrap the key "
                                           "in backticks"),
                      quoted);
    return true;
}

// § 2.6 — after an op writes `body`, re-derive from the NEW body every trailer
// column the same request did not itself supply, and write each one that
// changed.
//
// This is what makes § 1's render gate remediable: `Layman:` is a body line in
// markdown and a column in the store, so an annotate that adds the line but not
// the column would leave the gate failing forever with no op able to clear it.
//
// `supplied` names the keys the request set itself — empty for annotate,
// amend_body and a flip carrying a note; up to five for append. A supplied key
// is the caller's and is left alone: a request with layman:"X" and a body with
// no `Layman:` line must store "X", not clear the column to match the body.
//
// The OLD body is what makes clearing safe, and dropping it inverts the rule
// one op later and invisibly. Stated the short way — "clear whatever the new
// body does not yield" — an append that supplied layman:"X" with a body
// carrying no `Layman:` line has that column cleared by the very next annotate.
// Every single-op test passes; the two-op sequence is where it shows. So a
// column is cleared ONLY when the body it replaced also yielded that key.
bool rcdetail::rlDeriveTrailerColumns(RoadmapStore &store, qint64 itemPk,
                                   const RoadmapStore::ItemWrite &before,
                                   const QString &newBody,
                                   const QSet<QString> &supplied, QString *error) {
    const RoadmapParse::TrailerValues oldTv = RoadmapParse::trailerValuesIn(before.body);
    const RoadmapParse::TrailerValues newTv = RoadmapParse::trailerValuesIn(newBody);
    for (const RlTrailerKey &k : kRlTrailerKeys) {
        const QString field = QString::fromLatin1(k.field);
        if (supplied.contains(field))
            continue;
        const QString oldValue = rlBodyValueFor(oldTv, k);
        const QString newValue = rlBodyValueFor(newTv, k);
        if (oldValue.isEmpty() && newValue.isEmpty())
            continue;  // untouched — it came from a request argument or the
                       // migration, and this op has no opinion about it.

        const QString current =
            k.colList ? rlJsonArrayText(before.*(k.colList)) : before.*(k.col);
        if (!newValue.isEmpty()) {
            if (current == newValue)
                continue;  // § 2.6 writes each one that CHANGED.
            if (!store.setItemField(itemPk, field, newValue,
                                    QStringLiteral("asserted"), error))
                return false;
        } else {
            // The body carried it and the write removed it. clearItemField(),
            // never an empty setItemField(): putItem() binds an empty value as
            // SQL NULL while setItemField() binds the string it is given, and
            // ANTS-3761's INV-2 column diff reads '' and NULL as different. For
            // lanes/evidence the trap is sharper — "[]" is valid JSON and an
            // empty array canonicalises happily, so setItemField() would store
            // an empty array where NULL is meant and nothing would refuse it.
            if (current.isEmpty() || current == QLatin1String("[]"))
                continue;
            if (!store.clearItemField(itemPk, field, QStringLiteral("asserted"), error))
                return false;
        }
    }
    return true;
}

// § 2.2's body append — the whole of `annotate`, and of `flip` when it carries
// a note. Simpler on the store path than in markdown: item.body is already
// exactly the continuation span the markdown helper has to go and find, and it
// is held UN-INDENTED (parseBullets() builds it with body.append(cont.trimmed())),
// so appendBodyNote()'s indent-sniffing and end-of-run walk both drop out. The
// render's appendIndented() puts the two spaces back on the way out.
//
// The caller scrubs and caps the note first, exactly as the markdown path does.
QString rcdetail::rlAppendBodyNote(const QString &body, const QString &note) {
    QString out = body;
    while (out.endsWith(QLatin1Char('\n')) || out.endsWith(QLatin1Char(' ')))
        out.chop(1);
    if (out.isEmpty())
        return note;
    return out + QLatin1Char('\n') + note;
}

// § 2.2 — a located BulletRecord becomes an item_pk. Two steps, because one
// does not cover the corpus.
//
// Step 1 is the point lookup on the id the caller was shown. Step 2 exists
// because it provably misses two reachable shapes and refusing them would be a
// regression: ANTS-3793 § 2.1.1 fills rec.id from the RENDERED HEAD LINE and
// not from the id column, so an item whose column is empty takes its rec.id
// from an id-shaped headline token, and one carrying an off-grammar quarantined
// id whose prose cites another [ID] re-parses to the citation. findItem() keys
// on the column and misses both.
//
// The fallback keys on headlineFull — the stored headline unchanged — so it
// compares equal to ItemRef::headline without a truncation allowance.
//
// Fills *code with the same two refusals the markdown path's `headline` locator
// already makes.
std::optional<qint64> rcdetail::rlStoreItemPk(RoadmapStore &store, qint64 projectId,
                                           const RoadmapParse::BulletRecord &rec,
                                           QString *code, QString *error) {
    if (!rec.id.isEmpty()) {
        if (const auto pk = store.findItem(projectId, rec.id, error))
            return pk;
    }
    const auto refs = store.listItems(projectId, error);
    if (!refs) {
        if (code)
            *code = QStringLiteral("store_failed");
        return std::nullopt;
    }
    std::optional<qint64> hit;
    for (const auto &ref : *refs) {
        if (ref.headline != rec.headlineFull)
            continue;
        if (hit) {
            if (code)
                *code = QStringLiteral("bullet_ambiguous");
            if (error)
                *error = QStringLiteral("more than one bullet matches that headline");
            return std::nullopt;
        }
        hit = ref.itemPk;
    }
    if (!hit) {
        if (code)
            *code = QStringLiteral("bullet_not_found");
        if (error)
            *error = QStringLiteral("no bullet matches that locator");
    }
    return hit;
}

// § 2.3 — the counter high-water the store path allocates from, floored to the
// committed corpus.
//
// The floor is not belt-and-braces. roadmap-format.md § 3.5.1 defines
// .roadmap-counter as a derived, per-machine cache — NOT source (ANTS-3450) —
// whose true value is the highest id across the committed corpus, and both
// existing allocators already floor to corpusHighWater(). Swapping in
// idHighWater() alone would silently drop that floor, which is the mechanism
// that stops a fresh clone reissuing a live id.
//
// idHighWater() returning nullopt is NOT an error — its own comment says so,
// and it is the state of every project until its first store-side allocation.
// Treated as 0, exactly as the markdown path treats an absent counter file.
qint64 rcdetail::rlStoreIdHighWater(RoadmapStore &store, qint64 projectId,
                                 const QString &projectRoot, const QString &prefix) {
    qint64 floor = RoadmapFoldIn::corpusHighWater(projectRoot, prefix);
    QString ignored;
    if (const auto hw = store.idHighWater(projectId, prefix, &ignored))
        floor = std::max(floor, *hw);
    return floor;
}

// § 2.3 — the prefix idHighWater() is keyed on. idPrefixFor() takes the place
// of rlResolveCounterPrefix()'s markdown SNIFF step: it is the store's own
// record of the project's prefix, and it is a better answer than re-sniffing
// text. It returns nullopt when the project has no id_prefix row — an absent
// row, explicitly not an error, and a narrower condition than "no id-bearing
// item", since the row is written by raiseIdHighWater() and a project migrated
// in without an allocation ever running has ids and no row.
//
// An EXPLICIT id_prefix argument still wins, because that is what the shipped
// argument is documented to do ("overrides BOTH the prefix sniffed from
// existing IDs and the project-dir default"); on nullopt the whole markdown
// resolution applies unchanged.
QString rcdetail::rlStoreCounterPrefix(RoadmapStore &store, qint64 projectId,
                                    const QString &idPrefixArg,
                                    const QString &markdown,
                                    const QString &callerCanonical) {
    if (!idPrefixArg.isEmpty())
        return idPrefixArg;
    QString ignored;
    if (const auto pfx = store.idPrefixFor(projectId, &ignored)) {
        if (!pfx->isEmpty())
            return *pfx;
    }
    return rlResolveCounterPrefix(idPrefixArg, markdown, callerCanonical);
}

// § 2.5 and § 2.6 for the two ops that carry column arguments — `append` and
// `append_batch` — filling one item's `body`, its five trailer columns and the
// provenance for both. Shared because `append_batch` must run it inside its
// per-bullet validation loop (a body_shadowed refusal is per BULLET, § 2.5, and
// has to happen before ids are assigned or the accepted bullets' ids would not
// be contiguous — § 2.3) while `append` runs it once.
//
// The order is per KEY and not per op. A key the request SUPPLIES is the
// caller's, is written as given, and is shadow-checked against the body
// arriving in the same request. A key the request OMITS is derived from that
// body and is never shadow-checked — it came from the body, so the body cannot
// contradict it. That is also what lets a caller create an item whose `Layman:`
// line lives in the body without repeating it as an argument.
//
// Returns false with *error set on a body_shadowed refusal.
bool rcdetail::rlFillItemBody(const QJsonObject &bulletReq,
                           RoadmapStore::ItemWrite &w,
                           QStringList &scrubbedNames, QString *error) {
    // Scrubbed exactly as formatRoadmapBullet() scrubs it on the markdown path.
    QString body = bulletReq.value(QStringLiteral("body")).toString();
    rcScrubLeakedToolXml(body, scrubbedNames);
    const RoadmapParse::TrailerValues tv = RoadmapParse::trailerValuesIn(body);
    w.body = body;

    QJsonObject provenance = w.provenance;
    for (const RlTrailerKey &k : kRlTrailerKeys) {
        const QString field = QString::fromLatin1(k.field);
        QString     scalar;
        QStringList list;
        if (k.list) {
            // lanes / evidence. `evidence` gets the same per-path sanitising the
            // render applies, so the column holds what a GFM `Evidence:` line
            // could carry.
            const QJsonArray a = bulletReq.value(field).toArray();
            for (const auto &v : a) {
                if (field == QLatin1String("lanes")) {
                    list.append(v.toString());
                    continue;
                }
                QString p = rcSanitizeBulletField(v.toString(), 500);
                p.replace(QChar('\n'), QChar(' '));
                p.replace(QChar(','), QChar(' '));
                p = p.trimmed();
                if (!p.isEmpty()) list.append(p);
            }
        } else {
            // kind is enum-checked by the caller and written verbatim; source
            // and layman take the same caps the render applies.
            const QString v = bulletReq.value(field).toString();
            if (!v.isEmpty())
                scalar = (field == QLatin1String("kind"))
                             ? v
                             : rcSanitizeBulletField(
                                   v, field == QLatin1String("source") ? 200 : 1000);
        }

        const bool given = k.list ? !list.isEmpty() : !scalar.isEmpty();
        if (given) {
            const QString stored = k.list ? rlJsonArrayText(list) : scalar;
            if (rlBodyShadows(tv, body, k, stored, error))
                return false;
        } else {
            scalar = (tv.*(k.match)).value;
            if (k.list) list = tv.*(k.list);
        }
        if (k.colList) w.*(k.colList) = list;
        else           w.*(k.col)     = scalar;
        if (!(k.list ? list.isEmpty() : scalar.isEmpty()))
            provenance.insert(field, QStringLiteral("asserted"));
    }
    if (!body.isEmpty())
        provenance.insert(QStringLiteral("body"), QStringLiteral("asserted"));
    w.provenance = provenance;
    return true;
}

// ANTS-1922 — canonical bullet-cache array builder. Mirrors the
// bullet-mode pre-fill loop exactly (same fields, incl. section_slug)
// so a bundles-mode lazy-fill keeps the SHARED m_roadmapCacheBullets
// consistent for a later section_index call within the TTL. Used only
// by the bundles branch's lazy-fill; the three legacy fill sites inside
// cmdRoadmapQuery are deliberately NOT migrated to it — that would move
// their `for (const auto &b : bullets)` loops out of the function body
// and perturb the count-based roadmap_query_section_index INV-7 guard
// (a follow-on can migrate them + update that test).
//
// ANTS-3793 — takes RECORDS rather than markdown now: the backend choice is the
// caller's (RemoteControl::roadmapBullets), and this builder has no
// RemoteControl to ask. Same array, same fields.
static QJsonArray rcBuildBulletCacheArray(
        const QVector<RoadmapParse::BulletRecord> &bullets) {
    QJsonArray arr;
    for (const auto &b : bullets) {
        QJsonObject o;
        o["id"] = b.id;
        o["status"] = b.status;
        o["headline"] = b.headline;
        o["headline_oneline"] = rcHeadlineOneline(b.headline);
        rcMaybeEmitHeadlineFull(o, b);
        rcSetBodyFields(o, b.body, kRoadmapQueryBodyStoreCap);  // ANTS-3402
        o["kind"] = b.kind;
        QJsonArray lanes;
        for (const QString &l : b.lanes) lanes.append(l);
        o["lanes"] = lanes;
        rcMaybeEmitEvidence(o, b);  // ANTS-3382
        o["section_slug"] = b.sectionSlug;
        if (b.format == QLatin1String("github-task-list")) {
            o["format"] = b.format;
        }
        if (b.synthetic) o["synthetic"] = true;
        if (!b.anchor.isEmpty()) o["anchor"] = b.anchor;
        if (!b.boldId.isEmpty()) o["bold_id"] = b.boldId;
        arr.append(o);
    }
    return arr;
}

// ANTS-1922 — pure work-bundle builder over a cached bullet array.
// Static + public (see remotecontrol.h) so the feature test drives it
// directly with hand-authored fixtures; cmdRoadmapQuery's bundles
// branch calls it on m_roadmapCacheBullets. Active subset = id-bearing
// 📋/🚧 bullets (same posture as the default bullets[] predicate — INV-1
// wants real ids). Clusters by headline-token Jaccard ≥ 0.50 (≥2 shared
// tokens) via union-find, flags items a ✅ sibling may already cover
// (≥0.60) or a body gate-marker blocks, and bounds the emitted
// bundles[] at softCapBytes by dropping whole trailing bundles.
QJsonObject RemoteControl::buildRoadmapBundlesEnvelope(
        const QJsonArray &cacheBullets, int softCapBytes) {
    const QString plannedEmoji  = QString::fromUtf8("\xF0\x9F\x93\x8B"); // 📋
    const QString progressEmoji = QString::fromUtf8("\xF0\x9F\x9A\xA7"); // 🚧
    const QString doneEmoji     = QString::fromUtf8("\xE2\x9C\x85");     // ✅

    struct Item {
        QString id, status, headlineOneline, body;
        QString kind;                  // ANTS-3388 — Kind: facet, for the edge weight
        QString stem;                  // ANTS-3388 — <verb> <path> structural signature
        QStringList lanes;
        QSet<QString> tokens;          // raw headline tokens (labels + ✅-sibling check)
        QSet<QString> clusterTokens;   // ANTS-2155 — denoised, for the bundle edge
    };
    struct Shipped { QString id; QSet<QString> tokens; };
    QVector<Item> active;
    QVector<Shipped> shipped;

    // Tokenise every bullet's headline once (built for ✅ too, since the
    // sibling check needs shipped-item tokens). Tokens come from the
    // FULL headline; the emitted item carries headline_oneline.
    for (const auto &v : cacheBullets) {
        const QJsonObject o = v.toObject();
        const QString id       = o.value(QStringLiteral("id")).toString();
        const QString status   = o.value(QStringLiteral("status")).toString();
        const QString headline = o.value(QStringLiteral("headline")).toString();
        const QStringList toks = rcNormaliseHeadline(headline)
                                     .split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QSet<QString> tokset(toks.begin(), toks.end());
        if (status == doneEmoji) {
            if (!id.isEmpty()) shipped.append({id, tokset});
            continue;
        }
        if ((status == plannedEmoji || status == progressEmoji) &&
            !id.isEmpty()) {
            Item it;
            it.id = id;
            it.status = status;
            it.headlineOneline =
                o.value(QStringLiteral("headline_oneline")).toString();
            it.body = o.value(QStringLiteral("body")).toString();
            it.kind = o.value(QStringLiteral("kind")).toString();
            it.stem = rcStructuralStem(headline);   // ANTS-3388
            const QJsonArray la = o.value(QStringLiteral("lanes")).toArray();
            for (const auto &l : la) it.lanes.append(l.toString());
            it.tokens = tokset;
            active.append(it);
        }
    }

    const int activeTotal = active.size();

    // Label-only stop-word set (also used to denoise the clustering tokens
    // below) so a label never reads "the for" and a stop-word never forms
    // a bundle edge.
    static const QSet<QString> labelStops = {
        QStringLiteral("a"), QStringLiteral("an"), QStringLiteral("the"),
        QStringLiteral("of"), QStringLiteral("for"), QStringLiteral("to"),
        QStringLiteral("and"), QStringLiteral("or"), QStringLiteral("in"),
        QStringLiteral("on"), QStringLiteral("vs"), QStringLiteral("with"),
        QString::fromUtf8("\xE2\x80\x94"),   // em-dash —
    };

    // ANTS-2155 — build a DENOISED clustering-token set per item. The old
    // edge (token-Jaccard ≥ 0.50 over the FULL headline) divided by the
    // union, so long vocabulary-varied headlines that share 2-3 real topic
    // words scored well below 0.50 and never clustered (the real roadmap
    // produced all-singletons). Drop tokens that don't discriminate a
    // theme: stop-words, length ≤ 2, pure numbers ("6162"), and
    // file/path/qualified identifiers ("auditdialog.cpp"); then drop
    // corpus-ubiquitous tokens (TF-style) that would over-merge.
    const auto isNoiseToken = [](const QString &t) {
        if (t.size() <= 2) return true;
        bool allDigit = true;
        for (const QChar c : t) if (!c.isDigit()) { allDigit = false; break; }
        if (allDigit) return true;
        return t.contains(QLatin1Char('.')) || t.contains(QLatin1Char('/'));
    };
    QHash<QString, int> df;
    for (const Item &it : active)
        for (const QString &t : it.tokens)
            if (!isNoiseToken(t) && !labelStops.contains(t)) df[t] += 1;
    const int dfDrop = std::max(8, (activeTotal * 2) / 5);   // > max(8, 40%) ⟹ too common
    for (int i = 0; i < activeTotal; ++i)
        for (const QString &t : active[i].tokens)
            if (!isNoiseToken(t) && !labelStops.contains(t) && df.value(t) <= dfDrop)
                active[i].clusterTokens.insert(t);

    // Union-find over a length-insensitive edge. Bundles are the connected
    // components (transitive closure). Edge ⟺ ≥ 2 shared denoised tokens
    // AND (overlap coefficient ≥ 0.5 — |∩| / min(|A|,|B|) — OR ≥ 3 shared
    // tokens OR a shared lane). Overlap-coefficient + the absolute-count
    // escape replace the union-penalising Jaccard; the lane assist catches
    // same-lane items that share exactly 2 topic words in long headlines.
    QVector<int> parent(activeTotal);
    for (int i = 0; i < activeTotal; ++i) parent[i] = i;
    auto findRoot = [&parent](int x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    const auto sharedLane = [&active](int x, int y) -> bool {
        const QSet<QString> lx(active[x].lanes.begin(), active[x].lanes.end());
        for (const QString &l : active[y].lanes)
            if (lx.contains(l)) return true;
        return false;
    };
    const auto clusterEdge = [&active, &sharedLane](int x, int y) -> bool {
        // ANTS-3388 — structural-template assist: same Kind + a shared lane +
        // an identical <verb> <path> stem clusters template bullets (per-module
        // "Author …/spec.md") whose only shared tokens are the paths/filenames
        // the denoiser strips. Runs BEFORE the token guard because such bullets
        // often have near-empty clusterTokens. Guarded on all three facets so a
        // bare same-kind/same-lane pair never over-merges.
        if (!active[x].stem.isEmpty() && active[x].stem == active[y].stem &&
            !active[x].kind.isEmpty() && active[x].kind == active[y].kind &&
            sharedLane(x, y)) {
            return true;
        }
        const QSet<QString> &tx = active[x].clusterTokens;
        const QSet<QString> &ty = active[y].clusterTokens;
        if (tx.isEmpty() || ty.isEmpty()) return false;
        const QSet<QString> &small = (tx.size() <= ty.size()) ? tx : ty;
        const QSet<QString> &large = (tx.size() <= ty.size()) ? ty : tx;
        int inter = 0;
        for (const QString &t : small) if (large.contains(t)) ++inter;
        if (inter < 2) return false;                                  // ≥2 shared-topic floor
        if (inter >= 3) return true;                                  // strong overlap
        if (static_cast<double>(inter) / small.size() >= 0.5) return true;  // overlap coefficient
        return sharedLane(x, y);   // lane assist (same-lane 2-token pairs)
    };
    for (int i = 0; i < activeTotal; ++i) {
        for (int j = i + 1; j < activeTotal; ++j) {
            if (clusterEdge(i, j)) {
                const int ri = findRoot(i), rj = findRoot(j);
                if (ri != rj) parent[ri] = rj;
            }
        }
    }
    QHash<int, QVector<int>> groups;
    for (int i = 0; i < activeTotal; ++i) groups[findRoot(i)].append(i);

    struct Bundle {
        QString label;
        QStringList lanes;
        QString lowestId;
        QJsonArray items;
        int size = 0;
    };
    QVector<Bundle> bundles;

    for (auto git = groups.constBegin(); git != groups.constEnd(); ++git) {
        QVector<int> members = git.value();
        // Items sorted by id asc → byte-stable (union-find order is
        // otherwise unspecified). members.first() is then the lowest id.
        std::sort(members.begin(), members.end(), [&](int a, int b) {
            return rcRoadmapIdLess(active[a].id, active[b].id);
        });
        const int size = members.size();

        QJsonArray itemsArr;
        for (int mi : members) {
            const Item &m = active[mi];
            QJsonObject io;
            io["id"] = m.id;
            io["status"] = m.status;
            io["headline_oneline"] = m.headlineOneline;
            QStringList ml = m.lanes;
            std::sort(ml.begin(), ml.end());   // per-item lanes asc (verbatim)
            QJsonArray mla;
            for (const QString &l : ml) mla.append(l);
            io["lanes"] = mla;
            // Refinement 1 — possibly_resolved_by (max ✅ Jaccard ≥ 0.60).
            double best = -1.0;
            QString bestId;
            for (const Shipped &s : shipped) {
                const double j = rcHeadlineJaccard(m.tokens, s.tokens);
                if (j >= 0.60 &&
                    (j > best || (j == best && rcRoadmapIdLess(s.id, bestId)))) {
                    best = j;
                    bestId = s.id;
                }
            }
            if (!bestId.isEmpty()) {
                io["possibly_resolved_by"] = bestId;
                // Canonical dup-detector rounding (remotecontrol.cpp ~1002)
                // so the score is byte-identical + stable (INV-11).
                io["possibly_resolved_score"] =
                    static_cast<int>(best * 100.0 + 0.5);
            }
            // Refinement 2 — gate_note / blocked.
            const QString gate = rcExtractGateNote(m.body);
            if (!gate.isEmpty()) {
                io["gate_note"] = gate;
                io["blocked"] = true;
            }
            itemsArr.append(io);
        }

        // bundle_label: shared tokens (label stop-words dropped) in
        // ≥ ceil(size/2) members, sorted (freq desc, token asc), first 3.
        QHash<QString, int> tokFreq;
        for (int mi : members) {
            for (const QString &t : active[mi].tokens) tokFreq[t] += 1;
        }
        const int need = (size + 1) / 2;   // ceil(size/2)
        QStringList qualifying;
        for (auto t = tokFreq.constBegin(); t != tokFreq.constEnd(); ++t) {
            if (t.value() >= need && !labelStops.contains(t.key())) {
                qualifying.append(t.key());
            }
        }
        QString label;
        if (!qualifying.isEmpty()) {
            std::sort(qualifying.begin(), qualifying.end(),
                      [&](const QString &a, const QString &b) {
                          const int fa = tokFreq.value(a), fb = tokFreq.value(b);
                          if (fa != fb) return fa > fb;   // freq desc
                          return a < b;                    // token asc
                      });
            QStringList top;
            for (int k = 0; k < qualifying.size() && k < 3; ++k) {
                top.append(qualifying[k]);
            }
            label = top.join(QLatin1Char(' '));
        } else {
            // Fallback — most-common lane (case-folded tally, tie-break
            // ascending case-folded key); else lowest member id.
            QHash<QString, int> laneFreq;
            for (int mi : members) {
                for (const QString &l : active[mi].lanes) {
                    laneFreq[l.toLower()] += 1;
                }
            }
            if (!laneFreq.isEmpty()) {
                QString bestLane;
                int bestCnt = -1;
                for (auto l = laneFreq.constBegin(); l != laneFreq.constEnd(); ++l) {
                    if (l.value() > bestCnt ||
                        (l.value() == bestCnt && l.key() < bestLane)) {
                        bestCnt = l.value();
                        bestLane = l.key();
                    }
                }
                label = bestLane;
            } else {
                label = active[members.first()].id;
            }
        }

        // lanes union — distinct, case-sensitive verbatim, sorted asc.
        QSet<QString> laneUnion;
        for (int mi : members) {
            for (const QString &l : active[mi].lanes) laneUnion.insert(l);
        }
        QStringList laneList(laneUnion.begin(), laneUnion.end());
        std::sort(laneList.begin(), laneList.end());

        Bundle b;
        b.label = label;
        b.lanes = laneList;
        b.lowestId = active[members.first()].id;
        b.items = itemsArr;
        b.size = size;
        bundles.append(b);
    }

    // Sort bundles: size desc, tie-break lowest member id asc (this
    // already places 1-item bundles last).
    std::sort(bundles.begin(), bundles.end(),
              [](const Bundle &a, const Bundle &b) {
                  if (a.size != b.size) return a.size > b.size;
                  return rcRoadmapIdLess(a.lowestId, b.lowestId);
              });

    // Whole-bundle measure loop: stop before the first bundle that would
    // cross the soft cap; emitted bundles stay intact (INV-12, never
    // item-split). Always keep ≥1 bundle so a non-empty active set never
    // returns an empty bundles[] purely from the cap.
    QJsonArray bundlesArr;
    bool truncated = false;
    int runningBytes = 0;
    for (const Bundle &b : bundles) {
        QJsonObject bo;
        bo["bundle_label"] = b.label;
        QJsonArray la;
        for (const QString &l : b.lanes) la.append(l);
        bo["lanes"] = la;
        bo["size"] = b.size;
        bo["items"] = b.items;
        const int sz =
            QJsonDocument(bo).toJson(QJsonDocument::Compact).size();
        if (!bundlesArr.isEmpty() && runningBytes + sz > softCapBytes) {
            truncated = true;
            break;
        }
        bundlesArr.append(bo);
        runningBytes += sz;
    }

    QJsonObject out;
    out["ok"] = true;
    out["mode"] = QStringLiteral("bundles");
    out["active_total"] = activeTotal;
    out["bundle_count"] = bundlesArr.size();
    // ANTS-2155 — under truncation `bundle_count` is the EMITTED count;
    // total_bundle_count is the full pre-cap total so a caller can tell N
    // bundles were hidden (the old envelope made "173 active / 93 bundles"
    // read as if clustering had happened when it hadn't).
    out["total_bundle_count"] = bundles.size();
    out["bundles_omitted"] = static_cast<int>(bundles.size()) - bundlesArr.size();
    out["truncated"] = truncated;
    // ANTS-3388 — distinguish "grouped into all-singletons" (clustering ran
    // but nothing merged) from "found real clusters". True ⟺ no bundle in the
    // full pre-cap set reached size ≥ 2, so a caller never mistakes N size:1
    // bundles for a successful grouping. Measured over `bundles` (not the
    // cap-truncated `bundlesArr`), and vacuously true for an empty active set.
    bool anyCluster = false;
    for (const Bundle &b : bundles)
        if (b.size >= 2) { anyCluster = true; break; }
    out["no_clusters_found"] = !anyCluster;
    out["bundles"] = bundlesArr;
    return out;
}

// ANTS-1117 v1: roadmap-query — parse the active tab's ROADMAP.md
// (cached on mtime; INV-10 rate-limit) into a structured bullet
// stream for Claude. Returns the unified `{ok, error, code}` shape
// when no roadmap is loaded for the active tab.
//
// ANTS-1247: accepts optional `req.status` filter
// ("all"/"active"/"shipped", case-insensitive). The cache continues
// to hold the FULL unfiltered array; filtering happens at response
// build time over the cached entries (sub-ms walk).
QJsonDocument RemoteControl::cmdRoadmapQuery(const QJsonObject &req) {  // ANTS-1247-INV-1
    QJsonObject out;

    // ANTS-1247-INV-4: case-insensitive status parse; canonicalise
    // to lowercase. Anchor: filter parse.
    // ANTS-3698 — `filter` is accepted as an alias for `status`. The envelope
    // has always echoed the applied lifecycle as `filter`, so a caller who
    // reads a response and hand-writes the next call sends `filter:"active"` —
    // which was an unrecognised arg, silently dropped, and answered with the
    // FULL set under an echo (`filter:"all"`) that reads as confirmation a
    // filter was applied. Degrading on a near-miss is bad enough anywhere;
    // on this verb's own advertised lean-planning call it hands a planning
    // session a list of already-shipped ids. `status` still wins when both
    // are present.
    QString statusArg = req.value(QStringLiteral("status")).toString();
    if (statusArg.isEmpty())
        statusArg = req.value(QStringLiteral("filter")).toString();
    QString filter = statusArg.toLower();
    if (filter.isEmpty()) filter = QStringLiteral("all");

    // ANTS-1247-INV-5: unknown status → bad_status, cache untouched.
    // ANTS-3400 — accept roadmap_log's lifecycle vocabulary
    // (planned / in-progress / considered) as first-class status
    // filters, not only the active/shipped/all aggregates. Two sessions
    // (MAME Curator, Album Builder) passed the write-side enum to this
    // read verb and hit bad_status with no accepted-set hint. The
    // granular names map to a single emoji in the bullets predicate
    // (planned→📋, in-progress→🚧, considered→💭); "active" stays the
    // 📋∪🚧 aggregate. On refusal we now echo the accepted set so the
    // caller self-corrects without guessing.
    // ANTS-1247-INV-11: <verbatim> echo capped at 64 bytes; bytes
    // < 0x20 replaced with '?' to prevent ANSI/control passthrough.
    static const QStringList kAcceptedStatusFilters = {
        QStringLiteral("all"),         QStringLiteral("active"),
        QStringLiteral("shipped"),     QStringLiteral("planned"),
        QStringLiteral("in-progress"), QStringLiteral("considered") };
    if (!kAcceptedStatusFilters.contains(filter)) {
        QString verbatim = statusArg;   // ANTS-3698 — status or its alias
        if (verbatim.size() > 64) verbatim.truncate(64);
        for (int i = 0; i < verbatim.size(); ++i) {
            if (verbatim.at(i).unicode() < 0x20) verbatim[i] = QChar('?');
        }
        out["ok"] = false;
        out["error"] = QStringLiteral("unknown status filter: %1").arg(verbatim);
        out["code"] = QStringLiteral("bad_status");
        out["accepted"] = QJsonArray::fromStringList(kAcceptedStatusFilters);
        return QJsonDocument(out);
    }

    // ANTS-3400 — section_index mode tallies only active/shipped (there
    // is no per-lifecycle SectionCounts field). Collapse the granular
    // lifecycle names to their aggregate for section discovery so the
    // fine-grained filter still applies on the bullets/section= path
    // while section_index stays coarse: planned/in-progress → active;
    // considered has no section tally, so it lists every section (a
    // caller after 💭 items uses the bullets path, which does filter).
    QString sectionFilter = filter;
    if (sectionFilter == QLatin1String("planned") ||
        sectionFilter == QLatin1String("in-progress"))
        sectionFilter = QStringLiteral("active");
    else if (sectionFilter == QLatin1String("considered"))
        sectionFilter = QStringLiteral("all");

    // ANTS-1287-INV-1: optional `section` slug. Empty/missing → full-file
    // path (existing behaviour, INV-6).
    const QString section = req.value(QStringLiteral("section")).toString();

    // ANTS-3391 — optional `query` case-insensitive keyword filter. A
    // list-path filter: it narrows the status/section-filtered bullets to
    // those whose headline (or headline_full) OR body contains the needle
    // (substring, case-folded), composing with status= and section=. It is
    // NOT a targeted selector, so it refuses to combine with id / ids /
    // mode:section_index / mode:bundles (guards below) rather than silently
    // no-op'ing the arg. Needle capped at 200 chars + C0 control bytes
    // scrubbed, matching the bad_status / id echo hygiene. The match runs
    // pre-pagination while the `body` field is still present
    // (rcStripBodyFields runs post-slice), so it works even when
    // include_body is false — but the body it sees is the same 2000-char-
    // capped list text, so a keyword living only in a longer body's tail
    // won't match (documented in the schema).
    QString queryArg = req.value(QStringLiteral("query")).toString().trimmed();
    if (queryArg.size() > 200) queryArg.truncate(200);
    for (int i = 0; i < queryArg.size(); ++i) {
        if (queryArg.at(i).unicode() < 0x20) queryArg[i] = QChar('?');
    }
    auto applyQueryFilter = [&queryArg](QJsonArray &arr) {
        if (queryArg.isEmpty()) return;  // no filter → keep all, skip iteration
        QJsonArray kept;
        for (const auto &v : std::as_const(arr)) {
            if (mcp::bulletMatchesQuery(v.toObject(), queryArg))
                kept.append(v);
        }
        arr = kept;
    };

    // ANTS-1856 — optional `id` single-item selector. When set, the
    // query returns just the bullet(s) whose id equals this value,
    // bypassing the status filter + pagination so a caller after one
    // item (e.g. "show me ANTS-1853") gets it in a single small call
    // instead of paging the whole roadmap. Match is case-sensitive
    // exact (ids are canonically the upper-case [PROJ-NNNN] token);
    // a case-only mismatch surfaces bad_case + canonical_id, mirroring
    // the section= contract (ANTS-1524). 64-byte + control-char hygiene
    // matches bad_status / bad_section so a stray id can't smuggle
    // ANSI/control bytes into the echoed envelope.
    QString idArg = req.value(QStringLiteral("id")).toString();
    if (idArg.size() > 64) idArg.truncate(64);
    for (int i = 0; i < idArg.size(); ++i) {
        if (idArg.at(i).unicode() < 0x20) idArg[i] = QChar('?');
    }

    // ANTS-3402 — opt-in higher body cap for a TARGETED id/ids fetch. The
    // list default stays kRoadmapQueryBodyCap (2000); a caller after a
    // single large epic body raises it up to kRoadmapQueryBodyStoreCap
    // (16 KiB). Clamped to [2000, 16384]; ignored on list/section/
    // section_index paths (they always emit at the 2000 cap).
    int idBodyCap = kRoadmapQueryBodyCap;
    if (req.contains(QStringLiteral("max_body_bytes"))) {
        int req_cap = req.value(QStringLiteral("max_body_bytes")).toInt(
            kRoadmapQueryBodyCap);
        if (req_cap < kRoadmapQueryBodyCap)      req_cap = kRoadmapQueryBodyCap;
        if (req_cap > kRoadmapQueryBodyStoreCap) req_cap = kRoadmapQueryBodyStoreCap;
        idBodyCap = req_cap;
    }

    // ANTS-1726 — optional `ids` plural-selector. Same intent as `id`
    // (one-call fetch by stable ID) for callers that know N related
    // bullet ids (e.g. continuing a bundle: ANTS-1719..1724). Per-
    // element hygiene mirrors the singular branch: truncate to 64
    // bytes, replace C0 control bytes with `?`. Duplicates de-duped
    // here (first occurrence wins) so the document-order match loop
    // in the ids branch below doesn't have to. Empty array (zero
    // elements) is treated as absent so it falls through to the
    // normal list path. 100-element cap so a malformed caller can't
    // blow the cache scan budget.
    //
    // ANTS-3541 — malformed-`ids` hardening. Previously ONLY an array
    // was honoured: a caller who passed `ids` as a comma-joined string
    // ("ANTS-3537,ANTS-3538") fell silently through to the full
    // unfiltered list (N unrelated bullets) with no signal the arg was
    // mistyped. Now: (a) a string is COERCED — split on comma/
    // whitespace into the array form (saves the retry round-trip); and
    // (b) a present `ids` that is neither array nor string, or a
    // NON-EMPTY ids that yields zero valid ids after hygiene, REFUSES
    // bad_args rather than returning a large wrong result. An empty
    // array / empty string stays the documented "absent" fall-through.
    QStringList idsArg;
    QStringList idsArgInputOrder;  // INV-9 — first-occurrence ordering.
    {
        const QJsonValue idsVal = req.value(QStringLiteral("ids"));
        QJsonArray arr;
        bool idsPresentNonEmpty = false;
        if (idsVal.isArray()) {
            arr = idsVal.toArray();
            idsPresentNonEmpty = !arr.isEmpty();
        } else if (idsVal.isString()) {
            const QString raw = idsVal.toString().trimmed();
            if (!raw.isEmpty()) {
                const QStringList toks = raw.split(
                    QRegularExpression(QStringLiteral("[,\\s]+")),
                    Qt::SkipEmptyParts);
                for (const QString &t : toks) arr.append(t);
                idsPresentNonEmpty = !arr.isEmpty();
            }
        } else if (!idsVal.isUndefined() && !idsVal.isNull()) {
            // Present but neither array nor string (number/bool/object):
            // fail loud instead of silently dumping the full list.
            out["ok"] = false;
            out["error"] = QStringLiteral(
                "ids must be a JSON array of id strings or a "
                "comma-separated string; got a non-string scalar");
            out["code"] = QStringLiteral("bad_args");
            return QJsonDocument(out);
        }

        if (arr.size() > 100) {
            out["ok"] = false;
            out["error"] = QStringLiteral(
                "ids array must contain at most 100 elements; "
                "got %1").arg(arr.size());
            out["code"] = QStringLiteral("bad_args");
            return QJsonDocument(out);
        }
        for (const auto &v : arr) {
            if (!v.isString()) continue;  // skip non-string elements
            QString s = v.toString();
            if (s.size() > 64) s.truncate(64);
            for (int i = 0; i < s.size(); ++i) {
                if (s.at(i).unicode() < 0x20) s[i] = QChar('?');
            }
            if (s.isEmpty()) continue;
            if (!idsArg.contains(s)) {
                idsArg.append(s);
                idsArgInputOrder.append(s);
            }
        }
        // ANTS-3541 — present-but-yielded-nothing: a NON-EMPTY ids
        // (array or coerced string) whose every element was non-string /
        // empty / control-only refuses rather than falling through to
        // the full list. Empty array / empty string keeps the documented
        // absent behaviour (idsPresentNonEmpty stays false above).
        if (idsPresentNonEmpty && idsArg.isEmpty()) {
            out["ok"] = false;
            out["error"] = QStringLiteral(
                "ids contained no valid id after hygiene; pass an array "
                "of id strings (e.g. [\"ANTS-1719\"])");
            out["code"] = QStringLiteral("bad_args");
            return QJsonDocument(out);
        }
    }

    // ANTS-1436-INV-8 — optional `offset` + `limit` args. Forwarded
    // verbatim from the dispatch lambda (NOT type-gated there) so
    // we can emit bad_args on non-numeric. isUndefined() is the
    // "not passed" gate; isDouble() the "passed and well-formed"
    // gate. Negative values rejected at this layer too.
    const QJsonValue offsetVal = req.value(QStringLiteral("offset"));
    const QJsonValue limitVal  = req.value(QStringLiteral("limit"));
    const bool callerPassedOffset = !offsetVal.isUndefined();
    const bool callerPassedLimit  = !limitVal.isUndefined();
    int offsetArg = 0;
    int limitArg  = -1;  // sentinel: -1 = auto-pick
    if (callerPassedOffset) {
        if (!offsetVal.isDouble()) {
            out["ok"] = false;
            out["error"] = QStringLiteral(
                "offset must be a non-negative integer");
            out["code"] = QStringLiteral("bad_args");
            return QJsonDocument(out);
        }
        offsetArg = offsetVal.toInt();
        if (offsetArg < 0) {
            out["ok"] = false;
            out["error"] = QStringLiteral(
                "offset must be a non-negative integer");
            out["code"] = QStringLiteral("bad_args");
            return QJsonDocument(out);
        }
    }
    if (callerPassedLimit) {
        if (!limitVal.isDouble()) {
            out["ok"] = false;
            out["error"] = QStringLiteral(
                "limit must be a positive integer (1..500)");
            out["code"] = QStringLiteral("bad_args");
            return QJsonDocument(out);
        }
        limitArg = limitVal.toInt();
        if (limitArg < 1) {
            out["ok"] = false;
            out["error"] = QStringLiteral(
                "limit must be a positive integer (1..500)");
            out["code"] = QStringLiteral("bad_args");
            return QJsonDocument(out);
        }
    }

    // ANTS-1437-INV-1/2: optional `mode` arg. Default "bullets" (back-
    // compat). "section_index" returns a compact section index instead
    // of bullets. Unknown mode → bad_mode with the same 64-byte +
    // control-char hygiene as bad_status / bad_section.
    const bool hasModeArg = req.contains(QStringLiteral("mode"));
    QString mode = req.value(QStringLiteral("mode")).toString().toLower();
    if (mode.isEmpty()) mode = QStringLiteral("bullets");
    // ANTS-3617 — one list, used both to validate and to tell the caller
    // what was allowed. Previously the four modes were spelled out as a
    // chain of `!=` and the refusal named only the rejected value, so a
    // caller who guessed wrong had to go read the schema to find the
    // right spelling. The sibling changelog_query already ships an
    // `accepted` array on its bad_mode (and roadmap_query's own
    // bad_status does too) — this closes the odd one out.
    static const QStringList kModes = { QStringLiteral("bullets"),
                                        QStringLiteral("section_index"),
                                        QStringLiteral("headline_only"),
                                        QStringLiteral("bundles") };  // ANTS-1922
    if (!kModes.contains(mode)) {
        QString verbatim = req.value(QStringLiteral("mode")).toString();
        if (verbatim.size() > 64) verbatim.truncate(64);
        for (int i = 0; i < verbatim.size(); ++i) {
            if (verbatim.at(i).unicode() < 0x20) verbatim[i] = QChar('?');
        }
        out["ok"] = false;
        out["error"] = QStringLiteral("unknown mode: %1").arg(verbatim);
        out["code"] = QStringLiteral("bad_mode");
        out["accepted"] = QJsonArray::fromStringList(kModes);
        return QJsonDocument(out);
    }
    // ANTS-1437-INV-3: section_index + section is conceptually
    // exclusive — section_index IS the section-discovery surface.
    if (mode == QLatin1String("section_index") && !section.isEmpty()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "section_index mode does not accept section= filter");
        out["code"] = QStringLiteral("bad_mode_combo");
        return QJsonDocument(out);
    }
    // ANTS-1729 — section_index now ACCEPTS offset/limit (the future
    // spec the ANTS-1436-INV-6 refusal left room for). On a many-section
    // roadmap the active-filtered index can still run to tens of
    // sections (~10 KB), busting section_index's "compact discovery
    // surface" budget, so the same PaginationEngine slice + auto-truncate
    // the bullets path uses is applied to the sections[] array below.
    // offset/limit are validated for type/range above regardless of
    // mode, so no extra gating is needed here.
    // ANTS-1856 — `id` is a single-bullet selector that scans the whole
    // roadmap. section_index is the section-discovery surface and
    // section= is a sub-slice; neither composes with a global id lookup,
    // so reject loudly (vs silently ignoring one) — same stance as the
    // section_index combos above.
    if (!idArg.isEmpty() && mode == QLatin1String("section_index")) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "id selector does not combine with mode:section_index");
        out["code"] = QStringLiteral("bad_mode_combo");
        return QJsonDocument(out);
    }
    if (!idArg.isEmpty() && !section.isEmpty()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "id selector searches the whole roadmap; do not combine "
            "with section");
        out["code"] = QStringLiteral("bad_mode_combo");
        return QJsonDocument(out);
    }
    // ANTS-1726 — `ids` combos. Same rationale as `id`: a multi-id
    // lookup scans the whole roadmap, section_index is the section-
    // discovery surface, section= is a sub-slice — neither composes
    // with a global id-set lookup. Also reject `ids` + `id` (caller
    // should pick one selector, not mix both).
    if (!idsArg.isEmpty() && !idArg.isEmpty()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "ids selector does not combine with id; pick one");
        out["code"] = QStringLiteral("bad_mode_combo");
        return QJsonDocument(out);
    }
    if (!idsArg.isEmpty() && mode == QLatin1String("section_index")) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "ids selector does not combine with mode:section_index");
        out["code"] = QStringLiteral("bad_mode_combo");
        return QJsonDocument(out);
    }
    if (!idsArg.isEmpty() && !section.isEmpty()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "ids selector does not combine with section; ids scans "
            "the whole roadmap");
        out["code"] = QStringLiteral("bad_mode_combo");
        return QJsonDocument(out);
    }
    // ANTS-1922 — `bundles` is a whole-roadmap active rollup (same
    // rationale as section_index), so it does not compose with the
    // section= sub-slice or the id / ids single-item selectors. Three
    // guards modelled on the three section_index combos above
    // (bundles+section / bundles+id / bundles+ids). The mode-independent
    // id+section / ids+id / ids+section guards already fire above, so a
    // bundles request never reaches a bare id/ids+section without one of
    // these tripping first.
    if (mode == QLatin1String("bundles") && !section.isEmpty()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "bundles mode does not accept section= filter");
        out["code"] = QStringLiteral("bad_mode_combo");
        return QJsonDocument(out);
    }
    if (mode == QLatin1String("bundles") && !idArg.isEmpty()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "id selector does not combine with mode:bundles");
        out["code"] = QStringLiteral("bad_mode_combo");
        return QJsonDocument(out);
    }
    if (mode == QLatin1String("bundles") && !idsArg.isEmpty()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "ids selector does not combine with mode:bundles");
        out["code"] = QStringLiteral("bad_mode_combo");
        return QJsonDocument(out);
    }
    // ANTS-3391 — `query` is a list-path keyword filter; it composes with
    // status + section= but not with the targeted id/ids selectors (which
    // fetch specific bullets, bypassing the list) nor the aggregate
    // section_index / bundles modes (which emit no per-bullet text to
    // match). Refuse loudly rather than silently drop the arg — same stance
    // as the id / ids / bundles combos above.
    if (!queryArg.isEmpty()) {
        QString why;
        if (mode == QLatin1String("section_index"))
            why = QStringLiteral("query keyword filter does not combine "
                                 "with mode:section_index");
        else if (mode == QLatin1String("bundles"))
            why = QStringLiteral("query keyword filter does not combine "
                                 "with mode:bundles");
        else if (!idArg.isEmpty())
            why = QStringLiteral("query keyword filter does not combine "
                                 "with the id selector");
        else if (!idsArg.isEmpty())
            why = QStringLiteral("query keyword filter does not combine "
                                 "with the ids selector");
        if (!why.isEmpty()) {
            out["ok"] = false;
            out["error"] = why;
            out["code"] = QStringLiteral("bad_mode_combo");
            return QJsonDocument(out);
        }
    }

    // ANTS-1398-INV-1: `include_section_headers` opt-in. Default false
    // — section-rollup bullets (empty id + empty headline, status emoji
    // only) are dropped from `bullets[]` server-side so clients don't
    // have to scan for them. Pass true to retain the legacy shape for
    // any back-compat caller that wants them.
    const bool hasIncludeHeadersArg =
        req.contains(QStringLiteral("include_section_headers"));
    const bool includeSectionHeaders =
        req.value(QStringLiteral("include_section_headers")).toBool(false);
    // ANTS-1425 — `include_narrator_bullets` opt-in. Default false —
    // narrator bullets (empty id, non-empty headline; section-summary
    // prose like "Trust-model gaps in IPC sockets.") are also dropped
    // server-side. roadmap-format.md § 3.5.1 makes the stable
    // [PROJ-NNNN] ID mandatory for every actionable bullet, so the
    // empty-id signal is a sufficient non-actionable marker. Mirrors
    // the v1 design of `include_section_headers`; back-compat callers
    // who depend on narrator bullets can opt back in.
    const bool hasIncludeNarratorsArg =
        req.contains(QStringLiteral("include_narrator_bullets"));
    const bool includeNarratorBullets =
        req.value(QStringLiteral("include_narrator_bullets")).toBool(false);

    // ANTS-1517 — `include_body` opt-in. Default false. When true,
    // each bullet carries a `body` field (truncated to
    // kRoadmapQueryBodyCap chars; `body_truncated:true` set on
    // truncation). Saves the 3-5 follow-up Reads a session does to
    // pick up Kind / Lanes / Source prose from a dense bundle-
    // progress table whose headlines would otherwise blow the
    // harness budget. Cache always populates body — projection-out
    // happens at emission time via rcStripBodyFields when this flag
    // is false.
    const bool hasIncludeBodyArg =
        req.contains(QStringLiteral("include_body"));
    const bool includeBody =
        req.value(QStringLiteral("include_body")).toBool(false);

    // ANTS-1398-INV-2: rollup predicate. A bullet is a section rollup
    // iff its `id` and `headline` are both empty — the unambiguous
    // signature of `parseBullets`'s status-only summary cards.
    auto isRollupBullet = [](const QJsonValue &v) {
        const QJsonObject o = v.toObject();
        return o.value(QStringLiteral("id")).toString().isEmpty()
            && o.value(QStringLiteral("headline")).toString().isEmpty();
    };
    // ANTS-1425 — narrator predicate. Disjoint with isRollupBullet
    // (rollup has empty headline, narrator has non-empty headline);
    // both share empty id.
    auto isNarratorBullet = [](const QJsonValue &v) {
        const QJsonObject o = v.toObject();
        return o.value(QStringLiteral("id")).toString().isEmpty()
            && !o.value(QStringLiteral("headline")).toString().isEmpty();
    };
    // ANTS-1425 — single drop helper that composes both opt-ins.
    auto shouldDropUnnumbered =
        [&](const QJsonValue &v) {
            if (isRollupBullet(v)   && !includeSectionHeaders)  return true;
            if (isNarratorBullet(v) && !includeNarratorBullets) return true;
            return false;
        };

    // ANTS-1391: when caller_cwd is present, derive the ROADMAP.md path
    // under that root (matching MainWindow::refreshRoadmapButton's
    // case-variant search) instead of relying on the focused tab's
    // pre-discovered m_roadmapPath. Falls back to the focused tab when
    // absent (back-compat).
    QString path;
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    // ANTS-3793 — the project root the store's dispatch marker is keyed on
    // (ANTS-3756 INV-8). Empty when the caller named no cwd — the focused-tab
    // back-compat path below — which roadmapBullets() reads as "markdown, as
    // today" rather than as a project it failed to resolve.
    const QString callerCanonical =
        callerRaw.isEmpty() ? QString()
                            : QFileInfo(callerRaw).canonicalFilePath();
    if (!callerRaw.isEmpty()) {
        // ANTS-1459 — shared findRoadmapUnder() helper widens the
        // search to docs/, docs/private/, docs/internal/, .github/.
        path = findRoadmapUnder(callerCanonical);
    }
    if (path.isEmpty() && callerRaw.isEmpty()) {
        path = m_main->roadmapPathForRemote();
    }
    if (path.isEmpty()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "no ROADMAP.md detected for the active tab");
        out["code"] = QStringLiteral("no_roadmap_loaded");
        return QJsonDocument(out);
    }

    const QFileInfo fi(path);
    const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    // INV-10 wall-clock cap: even if mtime hasn't advanced (1-second
    // mtime resolution on some filesystems), force a refresh after
    // kRoadmapCacheTtlMs so an in-place edit within the same tick is
    // still picked up within the spec's "≤ 100 ms" budget.
    // ANTS-1247-INV-6: filter never invalidates the cache; the
    // TTL/mtime check is preserved exactly.
    // ANTS-1287-INV-5/8: section index + section-bullet cache share
    // the same freshness predicate.
    // ANTS-3793 — a store refusal must not leave the cache looking fresh. The
    // !fresh block below stamps path/mtime/now BEFORE it fills bullets, so a
    // refusal partway through would leave an empty array warm and the next call
    // inside the 100 ms TTL would serve it as "this roadmap has no bullets".
    // Invalidate, then fill the envelope.
    auto refuseRoadmapSource = [&](RoadmapSource::ReadError srcWhy,
                                   const QString &srcErr) {
        m_roadmapCachePath.clear();
        m_roadmapCacheMtimeMs = 0;
        m_roadmapCacheStampMs = 0;
        m_roadmapCacheBullets = QJsonArray();
        rcRoadmapSourceRefused(out, srcWhy, srcErr);
        return QJsonDocument(out);
    };

    const bool fresh = (m_roadmapCachePath == path) &&
                       (m_roadmapCacheMtimeMs == mtime) &&
                       (mtime != 0) &&
                       (nowMs - m_roadmapCacheStampMs <= kRoadmapCacheTtlMs);
    if (!fresh) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            out["ok"] = false;
            out["error"] = QStringLiteral(
                "could not open %1 for reading").arg(path);
            out["code"] = QStringLiteral("read_failed");
            return QJsonDocument(out);
        }
        const QString markdown = QString::fromUtf8(f.readAll());
        // ANTS-1287-INV-5: stale cache → wipe both bullet caches AND
        // the heading index. Both regenerate lazily below.
        m_roadmapIndex.clear();
        m_roadmapSectionCache.clear();
        m_roadmapSectionShape.clear();  // ANTS-1696 — lockstep with bullets cache.
        m_roadmapSectionEtags.clear();  // ANTS-1907 — lockstep with bullets cache.
        m_roadmapSectionLru.clear();   // ANTS-1346 — keep INV-2 in sync.
        m_roadmapCacheDuplicateIds = QJsonArray();   // ANTS-1646
        m_roadmapCachePath = path;
        m_roadmapCacheMtimeMs = mtime;
        m_roadmapCacheStampMs = nowMs;

        if (section.isEmpty()) {  // ANTS-1287-INV-6 — full-file path
            // ANTS-3793 — the store when this project is migrated, `markdown`
            // when it is not. One call swapped; the emission loop below is
            // untouched (roadmap_query_section_index INV-7 scrapes its shape).
            RoadmapSource::ReadError srcWhy = RoadmapSource::ReadError::None;
            QString srcErr;
            const auto bullets = roadmapBullets(callerCanonical, markdown,
                                                /*includeArchive=*/false,
                                                &srcWhy, &srcErr);
            if (srcWhy != RoadmapSource::ReadError::None)
                return refuseRoadmapSource(srcWhy, srcErr);
            QJsonArray arr;
            for (const auto &b : bullets) {
                QJsonObject o;
                o["id"] = b.id;
                o["status"] = b.status;
                o["headline"] = b.headline;
                // ANTS-1521 — single-line headline companion.
                o["headline_oneline"] = rcHeadlineOneline(b.headline);
                rcMaybeEmitHeadlineFull(o, b);  // ANTS-2075
                // ANTS-1517 — body (truncated). Always cached; the
                // strip pass at emission removes when include_body
                // is false. ANTS-3425 — store at the larger cap so an
                // id/ids max_body_bytes fetch can emit past 2000
                // (list/section emission still re-caps to 2000).
                rcSetBodyFields(o, b.body, kRoadmapQueryBodyStoreCap);
                o["kind"] = b.kind;
                QJsonArray lanes;
                for (const QString &l : b.lanes) lanes.append(l);
                o["lanes"] = lanes;
                rcMaybeEmitEvidence(o, b);  // ANTS-3382
                // ANTS-1442 — section_slug must be on every cache
                // population path. Without it the section_index
                // tally walks objects keyed to "" and rolls up zero.
                o["section_slug"] = b.sectionSlug;
                // ANTS-1428 — adapter-mode metadata. Native parses
                // leave these at default (empty/false), so callers
                // that ignore them see no change.
                if (b.format == QLatin1String("github-task-list")) {
                    o["format"] = b.format;
                }
                if (b.synthetic) o["synthetic"] = true;
                if (!b.anchor.isEmpty()) o["anchor"] = b.anchor;
                // ANTS-1438 — bold_id field, emitted only when set
                // (additive; absent on native ants-v1 bullets and on
                // GFM bullets with no bold prefix).
                if (!b.boldId.isEmpty()) o["bold_id"] = b.boldId;
                arr.append(o);
            }
            m_roadmapCacheBullets = arr;
            // ANTS-1646 — recompute duplicate-ID descriptors against
            // the freshly-populated cache. Stays empty on a clean
            // roadmap; emission gate suppresses the field then.
            m_roadmapCacheDuplicateIds =
                rcComputeDuplicateIds(m_roadmapCacheBullets);
        } else {
            // ANTS-1287-INV-9 — section mode does not pre-fill the
            // full bullets cache; that path is taken lazily on the
            // next no-section call. Build the index once.
            m_roadmapCacheBullets = QJsonArray();
            m_roadmapIndex = RoadmapIndex::buildIndex(markdown);
        }
    }

    // ANTS-1437 — section_index branch. Returns a compact section
    // index instead of bullets. Both caches (bullets + index) are
    // populated lazily here if cold so the count tally has every
    // bullet to look at.
    if (mode == QLatin1String("section_index")) {
        // Lazy-fill m_roadmapCacheBullets if cold (cache HIT may have
        // come from an earlier section-mode call, INV-9 mirror).
        if (m_roadmapCacheBullets.isEmpty() &&
            (m_roadmapCachePath == path) &&
            (m_roadmapCacheMtimeMs == mtime)) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QString markdown = QString::fromUtf8(f.readAll());
                // ANTS-3793 — same swap as the full-file pre-fill above.
                RoadmapSource::ReadError srcWhy =
                    RoadmapSource::ReadError::None;
                QString srcErr;
                const auto bullets = roadmapBullets(callerCanonical, markdown,
                                                    /*includeArchive=*/false,
                                                    &srcWhy, &srcErr);
                if (srcWhy != RoadmapSource::ReadError::None)
                    return refuseRoadmapSource(srcWhy, srcErr);
                QJsonArray arr;
                for (const auto &b : bullets) {
                    QJsonObject o;
                    o["id"] = b.id;
                    o["status"] = b.status;
                    o["headline"] = b.headline;
                    // ANTS-1521 — single-line headline companion.
                    o["headline_oneline"] = rcHeadlineOneline(b.headline);
                    rcMaybeEmitHeadlineFull(o, b);  // ANTS-2075
                    // ANTS-1517 — body (truncated). ANTS-3425 — store at
                    // the larger cap so an id/ids max_body_bytes fetch sees
                    // it (list/section emission still re-caps to 2000).
                    rcSetBodyFields(o, b.body, kRoadmapQueryBodyStoreCap);
                    o["kind"] = b.kind;
                    o["section_slug"] = b.sectionSlug;
                    QJsonArray lanes;
                    for (const QString &l : b.lanes) lanes.append(l);
                    o["lanes"] = lanes;
                    rcMaybeEmitEvidence(o, b);  // ANTS-3382
                    if (b.format == QLatin1String("github-task-list")) {
                        o["format"] = b.format;
                    }
                    if (b.synthetic) o["synthetic"] = true;
                    if (!b.anchor.isEmpty()) o["anchor"] = b.anchor;
                    if (!b.boldId.isEmpty()) o["bold_id"] = b.boldId;
                    arr.append(o);
                }
                m_roadmapCacheBullets = arr;
                // ANTS-1646 — recompute on the section_index lazy-fill
                // path so callers entering through section_index mode
                // still see duplicate_ids when collisions exist.
                m_roadmapCacheDuplicateIds =
                    rcComputeDuplicateIds(m_roadmapCacheBullets);
            }
        }
        // Lazy-fill the index — same shape as the section-mode branch.
        if (m_roadmapIndex.isEmpty()) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                out["ok"] = false;
                out["error"] = QStringLiteral(
                    "could not open %1 for reading").arg(path);
                out["code"] = QStringLiteral("read_failed");
                return QJsonDocument(out);
            }
            const QString markdown = QString::fromUtf8(f.readAll());
            m_roadmapIndex = RoadmapIndex::buildIndex(markdown);
        }

        // ANTS-1437-INV-8 — `unrecognised_format` gate applies before
        // emission, mirroring the bullet-mode gate below. ANTS-1462
        // adds a header-inventory fallback when buildIndex finds
        // sections; the truly-opaque "no bullets + no headings" case
        // still refuses with the typed error. ANTS-1463 adds the
        // shared hint + expected_format envelope fields.
        if (m_roadmapCacheBullets.isEmpty() &&
            fi.size() > kRoadmapMinParseableSize) {
            // section_index mode populated m_roadmapIndex upstream
            // (line ~1410); reuse it for the fallback.
            if (!m_roadmapIndex.isEmpty()) {
                return QJsonDocument(buildHeaderInventoryEnvelope(
                    m_roadmapIndex, path, fi.size()));
            }
            QJsonObject env;
            env["ok"]    = false;
            env["code"]  = QStringLiteral("unrecognised_format");
            env["error"] = QStringLiteral(
                "roadmap_query: \"%1\" parsed zero bullets from %2 "
                "bytes — format not recognised")
                    .arg(path).arg(fi.size());
            env["path"]            = path;
            env["bytes"]           = fi.size();
            env["hint"]            = kUnrecognisedFormatHint();
            env["expected_format"] = kUnrecognisedFormatExpected();
            return QJsonDocument(env);
        }

        // Tally counts per slug. parseBullets sets sectionSlug on every
        // record (RoadmapParse::parseBullets). Empty headline + empty id is
        // a rollup bullet (ANTS-1398-INV-2); INV-6 excludes those.
        const QString plannedEmoji  = QString::fromUtf8("\xF0\x9F\x93\x8B");
        const QString progressEmoji = QString::fromUtf8("\xF0\x9F\x9A\xA7");
        const QString doneEmoji     = QString::fromUtf8("\xE2\x9C\x85");
        QHash<QString, RoadmapIndex::SectionCounts> direct;
        for (const auto &v : std::as_const(m_roadmapCacheBullets)) {
            const QJsonObject o = v.toObject();
            const QString id    = o.value(QStringLiteral("id")).toString();
            const QString hl    = o.value(QStringLiteral("headline")).toString();
            if (id.isEmpty() && hl.isEmpty()) continue;  // INV-6 rollup
            const QString slug  = o.value(QStringLiteral("section_slug")).toString();
            const QString s     = o.value(QStringLiteral("status")).toString();
            const bool hasId    = !id.isEmpty();
            RoadmapIndex::SectionCounts &t = direct[slug];
            if (s == plannedEmoji || s == progressEmoji) {
                t.active++;
                if (hasId) t.activeWithId++;
            }
            if (s == doneEmoji) {
                t.shipped++;
                if (hasId) t.shippedWithId++;
            }
            t.total++;
            if (hasId) t.totalWithId++;
        }

        // ANTS-1442 — INV-10. Roll up child-section tallies into
        // their parents so level-2 sections show non-zero totals
        // when bullets live under their level-3 children.
        const auto rolled = RoadmapIndex::rollupCounts(
            m_roadmapIndex, direct);

        // INV-4 — emit EVERY indexed section, including empties, under
        // the default status:all. ANTS-1848 amends ANTS-1437 INV-7: a
        // status:active / status:shipped query drops sections whose
        // matching *_id_only tally is 0 (the lean planning call), so the
        // kept set matches the default bullets[] predicate. See spec.
        // ANTS-1622 — emit the ID-only parallel counts alongside the
        // emoji-only ones so callers see whether a section's bullets
        // would survive the default `bullets[]` ID-filter predicate.
        // The disagreement was the root of the cross-session bug: a
        // legacy GFM-task-list section reports `active_count: 11`
        // here but `roadmap_query(section=…, status="active")`
        // returns `count: 0` because none of the 11 bullets carry a
        // [PROJ-NNNN] token. Surfacing `active_count_id_only: 0`
        // alongside makes that visible without a second call.
        // ANTS-1907 — `include_section_etags:true` opt-in adds a
        // `section_etag` field to each emitted section. The etags are
        // computed lazily by slicing each kept section's bytes through
        // the same SHA-256 path the section= branch uses; cached by
        // slug so a follow-up section= call doesn't recompute. Default
        // false keeps the envelope shape unchanged for back-compat
        // callers (and skips the per-section slicing cost when the
        // caller doesn't need the etag yet).
        const bool includeSectionEtags =
            req.value(QStringLiteral("include_section_etags")).toBool(false);
        QString sectionEtagsMarkdown;   // lazy-loaded once per call
        bool sectionEtagsMarkdownLoaded = false;
        // ANTS-2052 — legacy-roadmap fallback for the lean status filter.
        // On a fully id-less roadmap every section's *_id_only tally is 0,
        // so the ANTS-1848 predicate below would drop EVERY section and the
        // flagship session_orient bundle reads as "no active work" even
        // though the emoji counts are non-zero (RetroArch / Music Production
        // cross-session reports, 2026-06-10). Worse, a section dropped by the
        // filter never reaches the per-section legacy_format check further
        // down, so the caller got sections:[] with no hint at all. Detect the
        // exact dead-end — the id-only predicate keeps nothing AND the raw
        // emoji count for the filter is > 0 — and fall back to the raw
        // active/shipped predicate, flagging the envelope so the caller knows
        // the surfaced counts are emoji-based, not id-based. raw totals come
        // from `direct` (un-rolled) so they aren't double-counted by rollup.
        int rawActiveTotal = 0, rawShippedTotal = 0;
        for (auto it = direct.constBegin(); it != direct.constEnd(); ++it) {
            rawActiveTotal  += it.value().active;
            rawShippedTotal += it.value().shipped;
        }
        bool useRawPredicate = false;
        if (sectionFilter == QLatin1String("active") ||
            sectionFilter == QLatin1String("shipped")) {
            int idOnlySurvivors = 0;
            for (const auto &sec : std::as_const(m_roadmapIndex)) {
                const auto t = rolled.value(sec.slug,
                                            RoadmapIndex::SectionCounts{});
                if ((sectionFilter == QLatin1String("active")  && t.activeWithId  > 0) ||
                    (sectionFilter == QLatin1String("shipped") && t.shippedWithId > 0))
                    ++idOnlySurvivors;
            }
            const int rawTotal = (sectionFilter == QLatin1String("active"))
                                     ? rawActiveTotal : rawShippedTotal;
            useRawPredicate = (idOnlySurvivors == 0 && rawTotal > 0);
        }
        QJsonArray sections;
        QJsonArray legacyFormatSections;
        for (const auto &sec : std::as_const(m_roadmapIndex)) {
            const auto t = rolled.value(sec.slug,
                                        RoadmapIndex::SectionCounts{});
            // ANTS-1848 — status filter drops zero-id-count sections.
            // ANTS-2052 — under the legacy fallback, drop by the raw emoji
            // count instead so an id-less roadmap still lists its sections.
            const bool dropActive  = (sectionFilter == QLatin1String("active")) &&
                (useRawPredicate ? t.active  == 0 : t.activeWithId  == 0);
            const bool dropShipped = (sectionFilter == QLatin1String("shipped")) &&
                (useRawPredicate ? t.shipped == 0 : t.shippedWithId == 0);
            if (dropActive || dropShipped) {
                continue;
            }
            QJsonObject obj;
            obj["slug"]                 = sec.slug;
            obj["headline"]             = sec.headingText;
            obj["level"]                = sec.level;
            obj["active_count"]         = t.active;
            obj["shipped_count"]        = t.shipped;
            obj["total_count"]          = t.total;
            obj["active_count_id_only"]  = t.activeWithId;
            obj["shipped_count_id_only"] = t.shippedWithId;
            obj["total_count_id_only"]   = t.totalWithId;
            // ANTS-1907 — per-section etag emission (opt-in). Honours
            // the same per-slug cache as section= so the etag stays
            // stable across mode swaps.
            if (includeSectionEtags) {
                QString etag = m_roadmapSectionEtags.value(sec.slug);
                if (etag.isEmpty()) {
                    if (!sectionEtagsMarkdownLoaded) {
                        QFile f(path);
                        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                            sectionEtagsMarkdown =
                                QString::fromUtf8(f.readAll());
                        }
                        sectionEtagsMarkdownLoaded = true;
                    }
                    if (!sectionEtagsMarkdown.isEmpty()) {
                        const QString slice = RoadmapIndex::sliceSection(
                            sectionEtagsMarkdown, sec);
                        etag = rcComputeSectionEtag(slice);
                        m_roadmapSectionEtags.insert(sec.slug, etag);
                    }
                }
                if (!etag.isEmpty()) obj["section_etag"] = etag;
            }
            // Self-only (un-rolled) check: if this section directly
            // owns bullets, all of which lack IDs, it is legacy-format.
            // Surface the slug at the top level (ANTS-1622) AND mark
            // the per-section object (ANTS-1714b) so a caller spotting
            // the discrepancy doesn't have to grep the slug against
            // the top-level array. Absent on well-tagged sections so
            // the response shape is unchanged for the common case.
            const auto self = direct.value(sec.slug,
                                           RoadmapIndex::SectionCounts{});
            if (self.total > 0 && self.totalWithId == 0) {
                obj["legacy_format"] = true;
                legacyFormatSections.append(sec.slug);
            }
            sections.append(obj);
        }

        out["ok"] = true;
        out["mode"] = mode;          // explicit in section_index path
        out["path"] = path;
        out["filter"] = filter;      // status filter echo (ANTS-1848: now shapes emission)
        // ANTS-1729 — paginate / auto-truncate the section index with the
        // same PaginationEngine the bullets path uses. Auto-pick (limit
        // omitted → -1) measure-cuts the slice under the soft cap so a
        // many-section roadmap stops busting the compact-index budget;
        // explicit offset/limit let a caller page. legacy_format_sections
        // stays the full-roadmap hint (a small slug list, not per-page).
        const auto secPage =
            PaginationEngine::pageBullets(sections, offsetArg, limitArg);
        out["sections"] = secPage.slice;
        if (PaginationEngine::shouldEmitPaginationFields(
                callerPassedOffset, callerPassedLimit, secPage.truncated)) {
            out["offset"]    = secPage.offset;
            out["limit"]     = secPage.limit;
            out["total"]     = secPage.total;
            out["truncated"] = secPage.truncated;
            if (secPage.truncated) out["next_offset"] = secPage.nextOffset;
        }
        // ANTS-1622 — top-level legacy-format hint. Only emitted
        // when at least one section's direct bullets all lack
        // [PROJ-NNNN] ids — staying absent on well-tagged roadmaps
        // keeps the response shape unchanged for the common case.
        if (!legacyFormatSections.isEmpty()) {
            out["legacy_format_sections"] = legacyFormatSections;
            out["legacy_format_hint"] = QStringLiteral(
                "%1 section(s) carry bullets that lack [PROJ-NNNN] "
                "ids — the default bullets[] filter will return "
                "count:0 for those. Re-issue any bullets-mode query "
                "against those slugs with include_narrator_bullets:true "
                "to retrieve their content.")
                    .arg(legacyFormatSections.size());
        }
        // ANTS-2052 — when the lean id-only predicate would have hidden ALL
        // active/shipped work on an id-less roadmap, we fell back to the raw
        // emoji predicate above. Flag it at the top level and expose the true
        // raw count so a fresh session_orient bundle no longer reads as an
        // empty queue. raw_*_count is emoji-based; the per-section legacy hint
        // already steers a bullets-mode follow-up to include_narrator_bullets.
        if (useRawPredicate) {
            out["legacy_format"]     = true;
            out["raw_active_count"]  = rawActiveTotal;
            out["raw_shipped_count"] = rawShippedTotal;
        }
        // ANTS-1646 — surface duplicate-ID collisions detected during
        // cache fill. Stays absent on a clean roadmap so the envelope
        // shape is unchanged for the common path.
        if (!m_roadmapCacheDuplicateIds.isEmpty()) {
            out["duplicate_ids"] = m_roadmapCacheDuplicateIds;
        }
        return QJsonDocument(out);
    }

    // ANTS-1922 — bundles branch. Groups the active subset (📋/🚧) into
    // thematic work-bundles by headline-token similarity, flagging items
    // a shipped sibling may already cover (possibly_resolved_by) or a
    // body gate-marker blocks (gate_note). Active-only; a passed `status`
    // is ignored (INV-7). Early return like section_index; reads the warm
    // cache with no re-parse (INV-8). Lazy-fills via rcBuildBulletCacheArray
    // when a prior section-mode call left the bullet cache empty on a hit
    // (mirror of the section_index lazy-fill; the helper keeps section_slug
    // on the shared cache for a follow-up section_index call within the TTL).
    if (mode == QLatin1String("bundles")) {
        if (m_roadmapCacheBullets.isEmpty() &&
            (m_roadmapCachePath == path) &&
            (m_roadmapCacheMtimeMs == mtime)) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QString markdown = QString::fromUtf8(f.readAll());
                // ANTS-3793 — store-or-markdown, then the same builder.
                RoadmapSource::ReadError srcWhy =
                    RoadmapSource::ReadError::None;
                QString srcErr;
                const auto bullets = roadmapBullets(callerCanonical, markdown,
                                                    /*includeArchive=*/false,
                                                    &srcWhy, &srcErr);
                if (srcWhy != RoadmapSource::ReadError::None)
                    return refuseRoadmapSource(srcWhy, srcErr);
                m_roadmapCacheBullets = rcBuildBulletCacheArray(bullets);
                m_roadmapCacheDuplicateIds =
                    rcComputeDuplicateIds(m_roadmapCacheBullets);
            }
        }
        const int cap = (m_bundleSoftCapOverride > 0)
                            ? m_bundleSoftCapOverride
                            : PaginationEngine::kSoftCapBytes;
        return QJsonDocument(
            buildRoadmapBundlesEnvelope(m_roadmapCacheBullets, cap));
    }

    // ANTS-1287 — section branch.
    if (!section.isEmpty()) {
        // INV-9: ensure we have an index even on a cache HIT taken
        // earlier in section-less mode (and vice versa).
        if (m_roadmapIndex.isEmpty()) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                out["ok"] = false;
                out["error"] = QStringLiteral(
                    "could not open %1 for reading").arg(path);
                out["code"] = QStringLiteral("read_failed");
                return QJsonDocument(out);
            }
            const QString markdown = QString::fromUtf8(f.readAll());
            m_roadmapIndex = RoadmapIndex::buildIndex(markdown);
        }
        const auto *sec = RoadmapIndex::findBySlug(m_roadmapIndex, section);
        if (!sec) {
            // ANTS-1287-INV-10 — bad_section, hygiene parity with INV-11.
            QString verbatim = section;
            if (verbatim.size() > 64) verbatim.truncate(64);
            for (int i = 0; i < verbatim.size(); ++i) {
                if (verbatim.at(i).unicode() < 0x20) verbatim[i] = QChar('?');
            }
            // ANTS-1524 — distinguish off-case slug from genuinely
            // unknown slug. Slugs are canonically lowercase; an
            // LLM caller that passed "Performance" instead of
            // "performance" gets a loud `bad_case` refusal with
            // the canonical form surfaced, instead of the silent
            // "doesn't exist" reading that bad_section conveys.
            const QString sectionCi = section.toLower();
            for (const auto &s : std::as_const(m_roadmapIndex)) {
                if (s.slug.toLower() == sectionCi && s.slug != section) {
                    out["ok"]             = false;
                    out["error"]          = QStringLiteral(
                        "section slug case mismatch: \"%1\" — did you "
                        "mean \"%2\"?").arg(verbatim, s.slug);
                    out["code"]           = QStringLiteral("bad_case");
                    out["canonical_slug"] = s.slug;
                    return QJsonDocument(out);
                }
            }
            out["ok"] = false;
            out["error"] = QStringLiteral("unknown section: %1").arg(verbatim);
            out["code"] = QStringLiteral("bad_section");
            return QJsonDocument(out);
        }

        QJsonArray sectionBullets;
        QString sectionEtag;   // ANTS-1907 — emitted on the response.
        if (m_roadmapSectionCache.contains(sec->slug)) {
            sectionBullets = m_roadmapSectionCache.value(sec->slug);
            // ANTS-1907 — cache hit path: etag was computed on insert.
            sectionEtag = m_roadmapSectionEtags.value(sec->slug);
            // ANTS-1346 — bump to MRU front.
            m_roadmapSectionLru.removeOne(sec->slug);
            m_roadmapSectionLru.prepend(sec->slug);
        } else {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                out["ok"] = false;
                out["error"] = QStringLiteral(
                    "could not open %1 for reading").arg(path);
                out["code"] = QStringLiteral("read_failed");
                return QJsonDocument(out);
            }
            const QString markdown = QString::fromUtf8(f.readAll());
            const QString slice = RoadmapIndex::sliceSection(markdown, *sec);
            // ANTS-1907 — compute the per-section etag from the sliced
            // bytes once, cache by slug. Skips dispatch-layer's whole-
            // file etag; the per-section value is invariant under edits
            // to OTHER sections, which is the whole point.
            sectionEtag = rcComputeSectionEtag(slice);
            m_roadmapSectionEtags.insert(sec->slug, sectionEtag);
            // ANTS-3793 — the ONE site where "swap the call" is not the whole
            // change. The markdown path slices this section's TEXT and parses
            // the slice, which a store cannot do: it holds records, not lines.
            // So the store path takes the project's whole record list and
            // filters by sectionSlug. INV-2 makes the two agree field-for-field
            // on content, and § 2.1.1 fills sectionSlug from the same stateful
            // slugger the index used, so the filter selects the same bullets
            // the slice would have. `fromStore` is what tells them apart — a
            // store-backed project must not be handed a text slice, and an
            // unmigrated one must keep slicing exactly as it does today.
            // roadmapStoreServes() answers the dispatch WITHOUT materialising
            // records, which matters here: INV-9 keeps section mode off the
            // whole-file parse, and asking the question by reading everything
            // would spend exactly what that invariant saves.
            RoadmapSource::ReadError srcWhy = RoadmapSource::ReadError::None;
            QString srcErr;
            QVector<RoadmapDialog::BulletRecord> bullets;
            const bool fromStore =
                roadmapStoreServes(callerCanonical, markdown, &srcWhy, &srcErr);
            if (srcWhy != RoadmapSource::ReadError::None)
                return refuseRoadmapSource(srcWhy, srcErr);
            if (fromStore) {
                const auto whole = roadmapBullets(callerCanonical, markdown,
                                                  /*includeArchive=*/false,
                                                  &srcWhy, &srcErr);
                if (srcWhy != RoadmapSource::ReadError::None)
                    return refuseRoadmapSource(srcWhy, srcErr);
                for (const auto &b : whole)
                    if (b.sectionSlug == sec->slug) bullets.append(b);
            } else {
                bullets = RoadmapDialog::parseBullets(slice);
            }
            // ANTS-2225 — pass-heading fallback. parseBullets engages its
            // `#### Pass N.M` reader only when it sees >= 2 pass headings AND
            // >= 2 status markers across its INPUT (RoadmapParse::detectRoadmapFormat). A
            // single-section slice may carry just one pass heading, so
            // slice-local detection fails and the synthesised bullet is
            // dropped — the section reads "prose"/empty even though
            // session_orient active_bullets, an id-query, and mode:section_index
            // all surface it from the whole-file parse. When the slice parsed
            // empty, fall back to the authoritative whole-file parse filtered
            // to this slug: the global parse has the pass-heading context, and
            // parseBullets' sectionSlug is the same global slug the index uses
            // (the section_index tally at L3646 relies on that). INV-9's
            // "section mode doesn't pre-fill the full cache" is preserved for
            // the common (non-empty-slice) path; the whole-file parse runs only
            // on this otherwise-empty branch. A non-pass-heading doc yields no
            // bullet for the slug here, so the section stays empty as before.
            // ANTS-3793 — markdown-path only. The store path already filtered
            // the whole-project record list by slug, so this recovery would
            // re-run it against a text parse the store path never made; and
            // § 5 scopes the store backend to ants-v1, which is not the
            // pass-headings dialect this fallback exists for.
            if (!fromStore && bullets.empty()) {
                const auto whole = RoadmapDialog::parseBullets(markdown);
                QVector<RoadmapDialog::BulletRecord> filtered;
                for (const auto &b : whole)
                    if (b.sectionSlug == sec->slug) filtered.append(b);
                if (!filtered.empty()) bullets = std::move(filtered);
            }
            // ANTS-1696 — classify the slice ONLY when parseBullets
            // found nothing. The hint exists to disambiguate a
            // "section is a table" empty from a truly empty section;
            // computing it for bullet-rich slices wastes cycles and
            // would just emit shape:"prose" for the inline rationale
            // text most bullets carry above them. Cache by slug so
            // subsequent section= hits don't re-classify. Runs after the
            // ANTS-2225 fallback so a recovered pass-heading bullet
            // suppresses the (now-wrong) prose hint.
            if (bullets.empty()) {
                m_roadmapSectionShape.insert(sec->slug,
                                             rcSectionShape(slice));
            }
            for (const auto &b : bullets) {
                QJsonObject o;
                o["id"] = b.id;
                o["status"] = b.status;
                o["headline"] = b.headline;
                // ANTS-1521 — single-line headline companion.
                o["headline_oneline"] = rcHeadlineOneline(b.headline);
                rcMaybeEmitHeadlineFull(o, b);  // ANTS-2075
                // ANTS-1517 — body (truncated).
                rcSetBodyFields(o, b.body);
                o["kind"] = b.kind;
                QJsonArray lanes;
                for (const QString &l : b.lanes) lanes.append(l);
                o["lanes"] = lanes;
                // ANTS-1287-INV-7 — overwrite sectionSlug so all
                // bullets in section-mode carry the requested slug,
                // regardless of slice-local uniqueSlug state.
                o["section_slug"] = sec->slug;
                // ANTS-1428 metadata — parity with the full-file
                // path's emission. Predates ANTS-1438 but only
                // surfaced now that I'm adding bold_id here too;
                // pre-1438 section-mode was missing adapter fields.
                if (b.format == QLatin1String("github-task-list")) {
                    o["format"] = b.format;
                }
                if (b.synthetic) o["synthetic"] = true;
                if (!b.anchor.isEmpty()) o["anchor"] = b.anchor;
                // ANTS-1438 — bold_id (matches full-file emission).
                if (!b.boldId.isEmpty()) o["bold_id"] = b.boldId;
                sectionBullets.append(o);
            }
            m_roadmapSectionCache.insert(sec->slug, sectionBullets);
            // ANTS-1346 — push slug to MRU front and evict tail if
            // the cap is exceeded. removeOne is a no-op on first
            // insert; harmless on duplicate-key re-insert path.
            m_roadmapSectionLru.removeOne(sec->slug);
            m_roadmapSectionLru.prepend(sec->slug);
            while (m_roadmapSectionLru.size() > kRoadmapSectionCacheCap) {
                const QString evicted = m_roadmapSectionLru.takeLast();
                m_roadmapSectionCache.remove(evicted);
                m_roadmapSectionShape.remove(evicted);  // ANTS-1696 — lockstep.
                m_roadmapSectionEtags.remove(evicted);  // ANTS-1907 — lockstep.
            }
        }

        // ANTS-1907 — section_etag_match short-circuit. When the caller
        // hands back the etag we issued on a prior section= response, and
        // it still matches the current per-section hash, return an
        // unchanged envelope that costs ~50 bytes (vs the full bullets[]
        // body that often runs >5 KB on a dense section). Composes with
        // the dispatch-layer file etag (the file etag flips on any edit;
        // the section etag is invariant under edits to OTHER sections,
        // which is the whole point — see ROADMAP entry's "cross-section
        // loops" rationale). Section-only contract: a caller asking
        // section=foo and getting unchanged knows ONLY about foo.
        const QString sectionEtagMatch =
            req.value(QStringLiteral("section_etag_match")).toString();
        if (!sectionEtagMatch.isEmpty() && sectionEtagMatch == sectionEtag) {
            QJsonObject hit;
            hit["ok"]           = true;
            hit["unchanged"]    = true;
            hit["section"]      = sec->slug;
            hit["section_etag"] = sectionEtag;
            hit["path"]         = path;
            return QJsonDocument(hit);
        }

        // ANTS-1287-INV-8: status filter applies post-section.
        QJsonArray filtered;
        if (filter == QLatin1String("all")) {
            filtered = sectionBullets;
        } else {
            const QString plannedEmoji    = QString::fromUtf8("\xF0\x9F\x93\x8B");
            const QString progressEmoji   = QString::fromUtf8("\xF0\x9F\x9A\xA7");
            const QString doneEmoji       = QString::fromUtf8("\xE2\x9C\x85");
            const QString consideredEmoji = QString::fromUtf8("\xF0\x9F\x92\xAD"); // 💭 (ANTS-3400)
            for (const auto &v : std::as_const(sectionBullets)) {
                const QString s = v.toObject().value(QStringLiteral("status")).toString();
                const bool keep =
                    (filter == QLatin1String("active")      && (s == plannedEmoji || s == progressEmoji)) ||
                    (filter == QLatin1String("shipped")     && (s == doneEmoji)) ||
                    (filter == QLatin1String("planned")     && (s == plannedEmoji)) ||
                    (filter == QLatin1String("in-progress") && (s == progressEmoji)) ||
                    (filter == QLatin1String("considered")  && (s == consideredEmoji));
                if (keep) filtered.append(v);
            }
        }
        // ANTS-1398-INV-3b + ANTS-1425 — section-mode emission drops
        // both rollup and narrator bullets post-status filter unless
        // the caller opts each class back in via the matching flag.
        // ANTS-1538 — capture pre-prune count so we can surface a
        // `warning` when the default ID-filter silently dropped every
        // actionable bullet (legacy GFM-task-list / older-spec roadmaps
        // whose authors used narrator prose instead of `[PROJ-NNNN]`
        // tokens).
        const int preIdPruneCountSec = filtered.size();
        {
            QJsonArray pruned;
            for (const auto &v : std::as_const(filtered)) {
                if (!shouldDropUnnumbered(v)) pruned.append(v);
            }
            filtered = pruned;
        }
        // ANTS-3391 — keyword filter composes after status + section, before
        // pagination (so count / next_offset reflect the narrowed set).
        applyQueryFilter(filtered);
        // ANTS-1436-INV-11 — pagination via PaginationEngine helper.
        // One call site per emission branch (section + full-file).
        // ANTS-3543 — auto-downshift a would-be-truncated fat list to its
        // headline-only projection so a scanning caller keeps every id/
        // headline rather than losing the tail. Gated off when the caller
        // already asked for lean rows (headline_only) or explicitly wants
        // bodies (include_body); the section_index sections[] branch is
        // excluded (section descriptors have no lean form — § 5).
        const bool wantDownshift =
            (mode != QLatin1String("headline_only")) && !includeBody;
        // ANTS-3577 — restore ANTS-1881 INV-6: in already-lean
        // (headline_only) mode project the FULL filtered set to its 4-key
        // shape BEFORE pagination, so the soft-cap measure counts lean bytes
        // and more rows fit per page. The prior code projected page.slice
        // AFTER the fat measure, dropping more rows than the lean shape needs.
        // filtered's only post-pageBullets use is .isEmpty() (below), which an
        // in-place projection leaves unchanged.
        if (mode == QLatin1String("headline_only"))
            rcProjectHeadlineOnly(filtered);
        auto page = PaginationEngine::pageBullets(
            filtered, offsetArg, limitArg,
            wantDownshift ? PaginationEngine::RowProjector(&rcProjectHeadlineOnly)
                          : PaginationEngine::RowProjector{});
        const bool emitPagination =
            PaginationEngine::shouldEmitPaginationFields(
                callerPassedOffset, callerPassedLimit, page.truncated);
        // ANTS-1517 — strip body fields when include_body is false (a no-op
        // when the headline_only projection above already dropped bodies).
        if (!includeBody) rcStripBodyFields(page.slice);
        out["ok"] = true;
        out["bullets"] = page.slice;
        out["path"] = path;
        out["count"] = page.slice.size();
        out["filter"] = filter;
        if (!queryArg.isEmpty()) out["query"] = queryArg;  // ANTS-3391
        out["section"] = sec->slug;
        // ANTS-1907 — always echo the section's etag on a section=
        // response so the caller can hand it back as section_etag_match
        // on the next call and short-circuit when only OTHER sections
        // have changed (file-level etag would invalidate every read).
        out["section_etag"] = sectionEtag;
        // ANTS-1696 — surface the section_shape hint when parseBullets
        // (NOT the status filter) returned zero entries AND the slice
        // has non-bullet content. Lets a caller distinguish a "table
        // / prose section, use Read" empty from a "genuinely empty"
        // empty. Gated on sectionBullets.isEmpty() per INV-4: a section
        // whose bullets the status filter happened to drop is NOT
        // shape-hinted (the parsed set wasn't actually empty). Cache-
        // hit path: m_roadmapSectionShape was populated alongside
        // m_roadmapSectionCache, so the hint survives a second hit.
        if (sectionBullets.isEmpty() &&
            m_roadmapSectionShape.contains(sec->slug)) {
            const QJsonObject shape =
                m_roadmapSectionShape.value(sec->slug);
            const QString s =
                shape.value(QStringLiteral("shape")).toString();
            if (s != QLatin1String("empty")) {
                out["section_shape"]    = s;
                out["non_bullet_lines"] =
                    shape.value(QStringLiteral("non_bullet_lines"))
                        .toInt();
            }
        }
        // ANTS-1538 — if every bullet got pruned by the default
        // ID-filter, name the opt-ins so the caller can re-issue
        // with the correct flag instead of misreading the empty
        // result as "section is genuinely empty".
        if (preIdPruneCountSec > 0 && filtered.isEmpty() &&
            !includeNarratorBullets && !includeSectionHeaders) {
            out["warning"] = QStringLiteral(
                "default ID-filter dropped all %1 bullet(s) in this "
                "section (every entry was either a rollup-summary or "
                "narrator-prose line with no [PROJ-NNNN] id). "
                "Re-issue with include_narrator_bullets:true and/or "
                "include_section_headers:true to see them.")
                    .arg(preIdPruneCountSec);
        }
        if (emitPagination) {
            out["offset"]    = page.offset;
            out["limit"]     = page.limit;
            out["total"]     = page.total;
            out["truncated"] = page.truncated;
            if (page.truncated) out["next_offset"] = page.nextOffset;
        }
        // ANTS-3543 — emit only when a downshift fired (truthy-only, like
        // truncated). Rows are then headline-shaped and the tail is complete.
        if (page.downshifted) out["downshifted"] = true;
        // ANTS-1398-INV-5: echo the opt-in only when the caller set it.
        if (hasIncludeHeadersArg) {
            out["include_section_headers"] = includeSectionHeaders;
        }
        // ANTS-1425 — same echo-only-when-set discipline.
        if (hasIncludeNarratorsArg) {
            out["include_narrator_bullets"] = includeNarratorBullets;
        }
        // ANTS-1517 — same echo-only-when-set discipline.
        if (hasIncludeBodyArg) {
            out["include_body"] = includeBody;
        }
        // ANTS-1437 — mode echo only when caller set the arg
        // (default-back-compat envelope shape per INV-1).
        if (hasModeArg) out["mode"] = mode;
        // ANTS-1646 — section-mode emission picks up the cached
        // duplicate descriptors only when m_roadmapCacheBullets has
        // been populated by a prior full-file / section_index call.
        // Pure section-only first hits stay quiet (cache may be cold).
        // ANTS-1882 — filter to entries whose occurrences[] include
        // a bullet inside THIS section, so the field is section-
        // bound (preserves per-section ETag invariance: an edit to
        // section B that introduces / removes a duplicate not
        // involving foo does NOT shift foo's response payload).
        if (!m_roadmapCacheDuplicateIds.isEmpty()) {
            const QJsonArray scoped = rcFilterDuplicateIdsForSection(
                m_roadmapCacheDuplicateIds, sec->slug);
            if (!scoped.isEmpty()) {
                out["duplicate_ids"] = scoped;
            }
        }
        return QJsonDocument(out);
    }

    // Full-file path may need to fill the bullets cache lazily if
    // an earlier hit took the section path.
    if (m_roadmapCacheBullets.isEmpty() && (m_roadmapCachePath == path) &&
        (m_roadmapCacheMtimeMs == mtime)) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString markdown = QString::fromUtf8(f.readAll());
            // ANTS-3793 — same swap as the other two fill sites.
            RoadmapSource::ReadError srcWhy = RoadmapSource::ReadError::None;
            QString srcErr;
            const auto bullets = roadmapBullets(callerCanonical, markdown,
                                                /*includeArchive=*/false,
                                                &srcWhy, &srcErr);
            if (srcWhy != RoadmapSource::ReadError::None)
                return refuseRoadmapSource(srcWhy, srcErr);
            QJsonArray arr;
            for (const auto &b : bullets) {
                QJsonObject o;
                o["id"] = b.id;
                o["status"] = b.status;
                o["headline"] = b.headline;
                // ANTS-1521 — single-line headline companion.
                o["headline_oneline"] = rcHeadlineOneline(b.headline);
                rcMaybeEmitHeadlineFull(o, b);  // ANTS-2075
                // ANTS-1517 — body (truncated). ANTS-3425 — store at the
                // larger cap so an id/ids max_body_bytes fetch sees it
                // (list/section emission still re-caps to 2000).
                rcSetBodyFields(o, b.body, kRoadmapQueryBodyStoreCap);
                o["kind"] = b.kind;
                QJsonArray lanes;
                for (const QString &l : b.lanes) lanes.append(l);
                o["lanes"] = lanes;
                // ANTS-1442 — section_slug must be on every cache
                // population path.
                o["section_slug"] = b.sectionSlug;
                // ANTS-1428 — adapter-mode metadata (lazy-fill path).
                if (b.format == QLatin1String("github-task-list")) {
                    o["format"] = b.format;
                }
                if (b.synthetic) o["synthetic"] = true;
                if (!b.anchor.isEmpty()) o["anchor"] = b.anchor;
                // ANTS-1438 — bold_id field, emitted only when set
                // (additive; absent on native ants-v1 bullets and on
                // GFM bullets with no bold prefix).
                if (!b.boldId.isEmpty()) o["bold_id"] = b.boldId;
                arr.append(o);
            }
            m_roadmapCacheBullets = arr;
            // ANTS-1646 — recompute on the full-file lazy-fill path
            // taken after a section-mode cache HIT.
            m_roadmapCacheDuplicateIds =
                rcComputeDuplicateIds(m_roadmapCacheBullets);
        }
    }

    // ANTS-1429 — unrecognised_format gate. After cache fill /
    // lazy fill, if the parsed bullet count is zero AND the file
    // is larger than the conservative stub threshold, return a
    // typed error envelope rather than the legitimate-but-
    // misleading {ok:true, bullets:[], count:0} shape. Single
    // gate site covers cache-hit, cache-miss, and lazy-fill
    // paths because all three populate m_roadmapCacheBullets
    // from the same parseBullets call.
    //
    // ANTS-1462 — header-inventory fallback: when the bullet
    // parser yielded zero entries but the file still has ##/###
    // headings, return them as a section inventory instead of
    // refusing. Bullets-mode doesn't call buildIndex upstream,
    // so the fallback path does a lazy call (refusal-path only;
    // cost is bounded by the file size).
    //
    // ANTS-1463 — refusal envelope carries shared hint +
    // expected_format[] fields so callers can act on what the
    // parser was expecting without reading the source.
    if (m_roadmapCacheBullets.isEmpty() &&
        fi.size() > kRoadmapMinParseableSize) {
        // Bullets-mode fallback (ANTS-1462): re-read the file for
        // a lazy buildIndex. This is the refusal path — cost of
        // one extra read is bounded and acceptable. Cache m_roadmapIndex
        // for any subsequent section_index query against the same file.
        if (m_roadmapIndex.isEmpty()) {
            QFile fb(path);
            if (fb.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QString md = QString::fromUtf8(fb.readAll());
                m_roadmapIndex = RoadmapIndex::buildIndex(md);
            }
        }
        if (!m_roadmapIndex.isEmpty()) {
            return QJsonDocument(buildHeaderInventoryEnvelope(
                m_roadmapIndex, path, fi.size()));
        }
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = QStringLiteral("unrecognised_format");
        env["error"] = QStringLiteral(
            "roadmap_query: \"%1\" parsed zero bullets from %2 "
            "bytes — format not recognised")
                .arg(path).arg(fi.size());
        env["path"]            = path;
        env["bytes"]           = fi.size();
        env["hint"]            = kUnrecognisedFormatHint();
        env["expected_format"] = kUnrecognisedFormatExpected();
        return QJsonDocument(env);
    }

    // ANTS-1856 — single-item fetch. Runs on the full bullets cache
    // (section= is rejected upstream when id is set, so the cache is
    // the whole-file array here), bypassing the status filter and
    // pagination: an explicit id request wants THAT item regardless of
    // lifecycle, and the result is at most a handful of bullets. Body
    // is included by default for an id fetch — the intent is "give me
    // the whole item" — and stripped only on an explicit
    // include_body:false. A case-only mismatch returns bad_case with
    // the canonical id so a wrong-case caller gets the exact spelling
    // (mirrors section= ANTS-1524) rather than a bare found:false.
    if (!idArg.isEmpty()) {
        QJsonArray matches;
        for (const auto &v : std::as_const(m_roadmapCacheBullets)) {
            if (v.toObject().value(QStringLiteral("id")).toString() == idArg)
                matches.append(v);
        }
        if (matches.isEmpty()) {
            QString canonical;
            for (const auto &v : std::as_const(m_roadmapCacheBullets)) {
                const QString bid =
                    v.toObject().value(QStringLiteral("id")).toString();
                if (!bid.isEmpty() &&
                    bid.compare(idArg, Qt::CaseInsensitive) == 0) {
                    canonical = bid;
                    break;
                }
            }
            if (!canonical.isEmpty()) {
                out["ok"] = false;
                out["error"] = QStringLiteral(
                    "id case mismatch: \"%1\" — did you mean \"%2\"?")
                        .arg(idArg, canonical);
                out["code"] = QStringLiteral("bad_case");
                out["canonical_id"] = canonical;
                return QJsonDocument(out);
            }
            // ANTS-3387 — an id-token-SHAPED locator that fails the
            // canonical PROJ-NNNN gate (e.g. a digit-leading prefix like
            // "3D_E-0022") is never adopted as a bullet id, so a bare
            // found:false reads as "the item vanished". Name the real
            // cause: bad_id_format + the canonical-form hint. Placed after
            // the case-mismatch check so a conforming wrong-case id still
            // gets the friendlier bad_case.
            if (rcIsNonconformingIdToken(idArg)) {
                out["ok"]    = false;
                out["code"]  = QStringLiteral("bad_id_format");
                out["error"] = QStringLiteral(
                    "id \"%1\" is id-shaped but not a canonical "
                    "[PROJ-NNNN] token (roadmap-format.md § 3.5.1 requires "
                    "a letter-leading prefix); a bullet authored with such "
                    "a token is addressable only by its headline.")
                        .arg(idArg);
                out["id"]   = idArg;
                out["hint"] = QStringLiteral(
                    "Canonical ids are letter-led "
                    "(<LETTER><alnum/_/->*-<N>, e.g. ANTS-1234). A "
                    "digit-leading prefix never parses as a project id — "
                    "re-target this bullet by `headline`.");
                return QJsonDocument(out);
            }
        }
        if (hasIncludeBodyArg && !includeBody) rcStripBodyFields(matches);
        else rcCapBodyFields(matches, idBodyCap);  // ANTS-3402
        // ANTS-1881 — id-branch projection (second emission surface;
        // INV-5 anchors the dual-surface requirement). Applied after
        // body-strip so the projected object inherits the same key
        // set on every call regardless of include_body.
        if (mode == QLatin1String("headline_only")) {
            rcProjectHeadlineOnly(matches);
        }
        out["ok"]      = true;
        out["bullets"] = matches;
        out["path"]    = path;
        out["count"]   = matches.size();
        out["id"]      = idArg;
        out["found"]   = !matches.isEmpty();
        if (hasModeArg) out["mode"] = mode;
        if (hasIncludeBodyArg) out["include_body"] = includeBody;
        // ANTS-1646 — surface duplicate-id descriptors (cache is the
        // full-file array, so they are current) when the same id was
        // seen on more than one bullet — exactly the case an id fetch
        // most needs to flag (matches.size() > 1).
        if (!m_roadmapCacheDuplicateIds.isEmpty()) {
            out["duplicate_ids"] = m_roadmapCacheDuplicateIds;
        }
        return QJsonDocument(out);
    }

    // ANTS-1726 — multi-item fetch by id-set. Same shape as the
    // singular `id` branch: scans m_roadmapCacheBullets ONCE in
    // document order, keeping bullets whose id is in the requested
    // set. Result preserves the roadmap's document order (NOT the
    // input order). Body included by default; explicit
    // include_body:false strips. The envelope carries matched_ids[]
    // and missing_ids[] so a caller can spot stale/typoed ids
    // without diffing the input array against bullets[].id.
    if (!idsArg.isEmpty()) {
        // ANTS-3387 — refuse the whole batch if any requested id is
        // id-token SHAPED but non-canonical (see rcIsNonconformingIdToken):
        // such a token can never resolve, so letting it fall silently into
        // missing_ids is exactly the "vanished item" trap this guards
        // against. Name it with bad_id_format instead.
        QJsonArray badFormatIds;
        for (const QString &id : std::as_const(idsArgInputOrder)) {
            if (rcIsNonconformingIdToken(id)) badFormatIds.append(id);
        }
        if (!badFormatIds.isEmpty()) {
            out["ok"]             = false;
            out["code"]           = QStringLiteral("bad_id_format");
            out["error"]          = QStringLiteral(
                "ids[] contains %1 id-shaped token(s) that are not "
                "canonical [PROJ-NNNN] ids (roadmap-format.md § 3.5.1 "
                "requires a letter-leading prefix).")
                    .arg(badFormatIds.size());
            out["bad_format_ids"] = badFormatIds;
            out["hint"]           = QStringLiteral(
                "Canonical ids are letter-led (e.g. ANTS-1234); a "
                "digit-leading prefix never parses as a project id. "
                "Re-target those bullets by `headline`.");
            return QJsonDocument(out);
        }
        const QSet<QString> wanted(idsArg.cbegin(), idsArg.cend());
        QSet<QString> seen;
        QJsonArray matches;
        for (const auto &v : std::as_const(m_roadmapCacheBullets)) {
            const QString bid =
                v.toObject().value(QStringLiteral("id")).toString();
            if (wanted.contains(bid)) {
                matches.append(v);
                seen.insert(bid);
            }
        }
        if (hasIncludeBodyArg && !includeBody) rcStripBodyFields(matches);
        else rcCapBodyFields(matches, idBodyCap);  // ANTS-3402
        if (mode == QLatin1String("headline_only")) {
            rcProjectHeadlineOnly(matches);
        }
        // INV-3 — accounting arrays. Preserve INPUT order so a caller
        // diffing against the request gets a positional read.
        QJsonArray matchedIds;
        QJsonArray missingIds;
        QJsonArray idsEcho;
        for (const QString &id : std::as_const(idsArgInputOrder)) {
            idsEcho.append(id);
            if (seen.contains(id)) matchedIds.append(id);
            else                    missingIds.append(id);
        }
        out["ok"]          = true;
        out["bullets"]     = matches;
        out["path"]        = path;
        out["count"]       = matches.size();
        out["ids"]         = idsEcho;
        out["matched_ids"] = matchedIds;
        out["missing_ids"] = missingIds;
        out["found"]       = !matches.isEmpty();
        if (hasModeArg) out["mode"] = mode;
        if (hasIncludeBodyArg) out["include_body"] = includeBody;
        if (!m_roadmapCacheDuplicateIds.isEmpty()) {
            out["duplicate_ids"] = m_roadmapCacheDuplicateIds;
        }
        return QJsonDocument(out);
    }

    // ANTS-1247-INV-2/3: filter the cached array post-cache.
    // "active" → 📋+🚧 (planned + in-progress);
    // "shipped" → ✅ only; "all" → pass-through. Anchor: filter switch.
    QJsonArray filtered;
    if (filter == QLatin1String("all")) {
        filtered = m_roadmapCacheBullets;
    } else {
        const QString plannedEmoji    = QString::fromUtf8("\xF0\x9F\x93\x8B"); // 📋
        const QString progressEmoji   = QString::fromUtf8("\xF0\x9F\x9A\xA7"); // 🚧
        const QString doneEmoji       = QString::fromUtf8("\xE2\x9C\x85");     // ✅
        const QString consideredEmoji = QString::fromUtf8("\xF0\x9F\x92\xAD"); // 💭 (ANTS-3400)
        for (const auto &v : std::as_const(m_roadmapCacheBullets)) {
            const QString s = v.toObject().value(QStringLiteral("status")).toString();
            // ANTS-3408 — mirror the section= branch's granular arms.
            // ANTS-3400 added planned/in-progress/considered to the
            // section path only; this full-file (no `section` arg) path
            // kept just active/shipped, so a granular filter without a
            // section fell through the else and returned 0 with no error
            // (Contact_List feedback 2026-07-01). Root cause was path
            // divergence, not the ants-v1/GFM roadmap format.
            const bool keep =
                (filter == QLatin1String("active")      && (s == plannedEmoji || s == progressEmoji)) ||
                (filter == QLatin1String("shipped")     && (s == doneEmoji)) ||
                (filter == QLatin1String("planned")     && (s == plannedEmoji)) ||
                (filter == QLatin1String("in-progress") && (s == progressEmoji)) ||
                (filter == QLatin1String("considered")  && (s == consideredEmoji));
            if (keep) filtered.append(v);
        }
    }
    // ANTS-1398-INV-3a + ANTS-1425 — full-file emission drops both
    // rollup and narrator bullets post-status filter unless the caller
    // opts each class back in via the matching flag.
    // ANTS-1538 — capture pre-prune count for the warning gate below.
    const int preIdPruneCountFull = filtered.size();
    {
        QJsonArray pruned;
        for (const auto &v : std::as_const(filtered)) {
            if (!shouldDropUnnumbered(v)) pruned.append(v);
        }
        filtered = pruned;
    }
    // ANTS-3560 — count AFTER the ID-prune but BEFORE the keyword filter,
    // so the warning gate below can tell "the ID-mandate dropped every
    // bullet" (id-bearing count 0) from "the query matched nothing"
    // (id-bearing count > 0). Without this split a query that matched no
    // bullet fell through the ANTS-1538 gate and wrongly claimed every
    // entry lacked a [PROJ-NNNN] id (Rolodex feedback 2026-07-16).
    const int postIdPruneCountFull = filtered.size();
    // ANTS-3391 — keyword filter composes after status, before pagination
    // (so count / next_offset reflect the narrowed set).
    applyQueryFilter(filtered);

    // ANTS-1436-INV-11 — pagination via PaginationEngine helper.
    // Second of two call sites (the other is in the section-mode
    // branch above). Stateless; auto-truncate fires when caller
    // omitted limit AND filtered exceeds the soft cap.
    // ANTS-3543 — same auto-downshift gate as the section branch.
    const bool wantDownshift =
        (mode != QLatin1String("headline_only")) && !includeBody;
    // ANTS-3577 — restore ANTS-1881 INV-6: project the FULL filtered set to
    // its lean 4-key shape BEFORE pagination in headline_only mode, so the
    // soft-cap measure counts lean bytes (more rows per page). The prior code
    // measured the fat row then projected page.slice, dropping more than
    // needed. filtered's only later use is .isEmpty() (below), unaffected by an
    // in-place projection.
    if (mode == QLatin1String("headline_only"))
        rcProjectHeadlineOnly(filtered);
    auto page = PaginationEngine::pageBullets(
        filtered, offsetArg, limitArg,
        wantDownshift ? PaginationEngine::RowProjector(&rcProjectHeadlineOnly)
                      : PaginationEngine::RowProjector{});
    const bool emitPagination =
        PaginationEngine::shouldEmitPaginationFields(
            callerPassedOffset, callerPassedLimit, page.truncated);

    // ANTS-1517 — strip body fields when include_body is false (a no-op when
    // the headline_only projection above already dropped bodies).
    // ANTS-3402 — else re-truncate to the 2000 list cap: this path emits
    // from m_roadmapCacheBullets, whose bodies are now stored up to
    // kRoadmapQueryBodyStoreCap; the larger body is reserved for the
    // opt-in id/ids fetch, so a list stays at the 2000 cap.
    if (!includeBody) rcStripBodyFields(page.slice);
    else              rcCapBodyFields(page.slice, kRoadmapQueryBodyCap);

    out["ok"] = true;
    out["bullets"] = page.slice;
    out["path"] = path;
    // ANTS-1247-INV-10: count is post-filter post-pagination size.
    out["count"] = page.slice.size();
    // ANTS-1247-INV-7: filter echo (canonicalised lowercase).
    out["filter"] = filter;
    if (!queryArg.isEmpty()) out["query"] = queryArg;  // ANTS-3391
    if (emitPagination) {
        out["offset"]    = page.offset;
        out["limit"]     = page.limit;
        out["total"]     = page.total;
        out["truncated"] = page.truncated;
        if (page.truncated) out["next_offset"] = page.nextOffset;
    }
    // ANTS-3543 — emit only when a downshift fired (truthy-only, like
    // truncated). Rows are then headline-shaped and the tail is complete.
    if (page.downshifted) out["downshifted"] = true;
    // ANTS-1538 — when the default ID-filter silently dropped every
    // actionable bullet, surface a warning naming the opt-ins. Common
    // on legacy GFM-task-list or older-spec roadmaps whose authors
    // didn't tag bullets with [PROJ-NNNN] tokens; without this hint
    // the caller reads {ok:true, bullets:[], count:0} as "no work"
    // when in reality every bullet was pruned by the ID-mandate.
    if (filtered.isEmpty()) {
        // ANTS-3583 — does the WHOLE file have any [PROJ-NNNN]-tagged bullet?
        // preIdPruneCountFull is captured AFTER the status filter, so a
        // status filter that drops the status-less narrator bullets of a
        // prose/milestone roadmap zeroes the ANTS-1538 gate below: the caller
        // then gets a bare count:0 that reads as "no work left" when the truth
        // is "this tool can't parse this roadmap format" (perch feedback
        // 2026-07-18 — status:'planned' had no warning while status:'all' did).
        // Whether the roadmap has ZERO id-bearing bullets is a property of the
        // file, not of the filter, so surface it on EVERY filter with a
        // machine-detectable parseable_bullets:0. Early-exit keeps it O(1) on
        // a normal roadmap (first id-bearing bullet ends the scan), and it only
        // runs on the already-empty path.
        bool fileHasIdBearingBullet = false;
        for (const auto &v : std::as_const(m_roadmapCacheBullets)) {
            if (!shouldDropUnnumbered(v)) { fileHasIdBearingBullet = true; break; }
        }
        if (!fileHasIdBearingBullet &&
            !includeNarratorBullets && !includeSectionHeaders) {
            out["parseable_bullets"] = 0;
            out["warning"] = QStringLiteral(
                "this roadmap has no [PROJ-NNNN]-tagged bullets that "
                "roadmap_query can parse (it is prose/milestone-style). "
                "count:0 means \"format not recognised\", NOT \"no outstanding "
                "work\" — identical on every status filter. Re-issue with "
                "include_narrator_bullets:true to see the raw bullets.");
        } else if (!queryArg.isEmpty() && postIdPruneCountFull > 0) {
            // ANTS-3560 — the keyword query, not the ID-mandate, emptied
            // the set: N id-bearing bullets were searched and none matched.
            // The old ANTS-1538 text misdirected toward
            // include_narrator_bullets when the real fix is to fix the
            // query; a space/comma-separated multi-id query is the common
            // trigger (Rolodex), so hint at ids:[...] / one-id-at-a-time.
            out["warning"] = QStringLiteral(
                "no bullet matched query \"%1\" (%2 id-bearing bullet(s) "
                "searched). If you passed multiple space/comma-separated "
                "ids, use ids:[...] or query one id at a time.")
                    .arg(queryArg).arg(postIdPruneCountFull);
        } else if (preIdPruneCountFull > 0 &&
                   !includeNarratorBullets && !includeSectionHeaders) {
            // ANTS-1538 — the default ID-filter genuinely dropped every
            // actionable bullet (no query narrowed it, or the file has no
            // id-bearing bullets at all). Surface the opt-ins.
            out["warning"] = QStringLiteral(
                "default ID-filter dropped all %1 bullet(s) (every entry "
                "was either a rollup-summary or narrator-prose line with "
                "no [PROJ-NNNN] id). Re-issue with "
                "include_narrator_bullets:true and/or "
                "include_section_headers:true to see them.")
                    .arg(preIdPruneCountFull);
        }
    }
    // ANTS-1428 — envelope-level format echo. The adapter parses
    // the whole file in one shape; if any bullet was tagged GFM,
    // surface the format so callers know they're in adapter mode
    // and the (kind/lanes/layman) fields are degraded.
    for (const auto &v : std::as_const(m_roadmapCacheBullets)) {
        if (v.toObject().value(QStringLiteral("format")).toString() ==
            QLatin1String("github-task-list")) {
            out["format"] = QStringLiteral("github-task-list");
            break;
        }
    }
    // ANTS-1398-INV-5: echo the opt-in only when the caller set it
    // so the default-false case stays trim on the wire.
    if (hasIncludeHeadersArg) {
        out["include_section_headers"] = includeSectionHeaders;
    }
    // ANTS-1425 — same echo-only-when-set discipline.
    if (hasIncludeNarratorsArg) {
        out["include_narrator_bullets"] = includeNarratorBullets;
    }
    // ANTS-1517 — same echo-only-when-set discipline.
    if (hasIncludeBodyArg) {
        out["include_body"] = includeBody;
    }
    // ANTS-1437 — mode echo only when caller set the arg
    // (default-back-compat envelope shape per INV-1).
    if (hasModeArg) out["mode"] = mode;
    // ANTS-1646 — full-file emission always sees the just-computed
    // duplicate descriptors (cache fill happened above). Emit when
    // non-empty so a clean roadmap envelope stays back-compat.
    if (!m_roadmapCacheDuplicateIds.isEmpty()) {
        out["duplicate_ids"] = m_roadmapCacheDuplicateIds;
    }
    return QJsonDocument(out);
}

// ANTS-1424 — roadmap_log: append a new bullet to ROADMAP.md +
// bump .roadmap-counter atomically. Required-contract gated at the
// dispatcher (ANTS-1404). All paths derived from caller_cwd so the
// write stays anchored to the caller's project root. See
// docs/specs/ANTS-1424.md.
//
// ANTS-1428 — adapter mode adds `op:"flip"` for GFM-format
// roadmaps. Default `op:"append"` preserves the ANTS-1424 path
// byte-for-byte. See docs/specs/ANTS-1428.md § Tier 2.
QJsonDocument RemoteControl::cmdRoadmapLog(const QJsonObject &req) {
    // ANTS-1566 — caller_cwd-related refusals carry an `example`
    // field so IPC-direct callers (the MCP dispatcher's Required
    // gate catches the empty case upstream for tools/call requests)
    // self-correct in one round-trip without round-tripping through
    // the schema description.
    auto rlErr = [](const QString &code, const QString &message) {
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = code;
        env["error"] = message;
        if (code == QStringLiteral("missing_field") ||
            code == QStringLiteral("no_roadmap")) {
            QJsonObject ex;
            ex[QStringLiteral("op")]         = QStringLiteral("append");
            ex[QStringLiteral("caller_cwd")] = QStringLiteral("<your $PWD>");
            ex[QStringLiteral("section")]    = QStringLiteral("<roadmap H2/H3 slug>");
            ex[QStringLiteral("status")]     = QStringLiteral("planned");
            ex[QStringLiteral("headline")]   = QStringLiteral("<one-line bullet headline>");
            ex[QStringLiteral("kind")]       = QStringLiteral("implement");
            ex[QStringLiteral("source")]     = QStringLiteral("<origin tag>");
            env[QStringLiteral("example")] = ex;
        }
        return QJsonDocument(env);
    };

    // ANTS-1428 — `op` dispatch. Default "append" preserves
    // ANTS-1424 behaviour byte-for-byte. "flip" routes to the
    // adapter-mode locator + surgery path below. Flip is m_main-
    // independent (operates only on caller_cwd + filesystem) so the
    // m_main guard only fires on the append path.
    const QString op =
        req.value(QStringLiteral("op")).toString(
            QStringLiteral("append"));
    // ANTS-1717 — op:"annotate" shares the flip handler's locator +
    // file-surgery machinery; it appends a body note and leaves the
    // status untouched (the emoji-swap is skipped inside the handler).
    if (op == QStringLiteral("flip") ||
        op == QStringLiteral("annotate")) {
        return cmdRoadmapLogFlip(req);
    }
    // ANTS-3406 — op:"amend_body": patch a bullet's body prose in place
    // (exact single-line old_text→new_text). Standalone handler,
    // m_main-independent (caller_cwd + filesystem only).
    if (op == QStringLiteral("amend_body")) {
        return cmdRoadmapLogAmendBody(req);
    }
    // ANTS-1690 — batch flip: N bullets, one read + one commit.
    if (op == QStringLiteral("flip_batch")) {
        return cmdRoadmapLogFlipBatch(req);
    }
    // ANTS-1878 — create_section. m_main-independent (no counter
    // touch, no MainWindow needed); dispatch BEFORE the m_main guard.
    if (op == QStringLiteral("create_section")) {
        return cmdRoadmapLogCreateSection(req);
    }
    // ANTS-1879 — append_batch. m_main-independent like the
    // single-bullet append's *ForTest seam (counter + markdown only).
    if (op == QStringLiteral("append_batch")) {
        return cmdRoadmapLogAppendBatch(req);
    }
    // ANTS-1691 — bundle_row. Appends a row to a Markdown table under a
    // named section (the "## 📊 Bundle progress" cross-session table).
    // m_main-independent (caller_cwd + filesystem only); no counter.
    if (op == QStringLiteral("bundle_row")) {
        return cmdRoadmapLogBundleRow(req);
    }
    if (op != QStringLiteral("append")) {
        return rlErr(QStringLiteral("bad_op_combo"),
            QStringLiteral("roadmap_log: unknown op \"%1\" — expected "
                           "\"append\" (default), \"append_batch\", "
                           "\"flip\", \"flip_batch\", \"annotate\", "
                           "\"amend_body\", \"bundle_row\", or "
                           "\"create_section\"").arg(op));
    }

    if (!m_main) {
        return rlErr(QStringLiteral("no_main"),
                     QStringLiteral("roadmap_log: no main window"));
    }

    return cmdRoadmapLogAppend(req);
}

// ANTS-1433 — test-only failure-injection flag for the two-stage
// QSaveFile commit in cmdRoadmapLogAppend. Default false; flipped only
// via setForceCounterCommitFailForTest() from the
// mcp_roadmap_log_atomicity feature test. No IPC/MCP verb reaches it,
// so production attack surface is nil and the single always-false
// branch it adds to the rare counter-commit path is free. Follows the
// codebase's existing always-compiled *ForTest seam convention
// (setVerifyInFlightForTest / cmdVerifyChangesWithRoot) rather than an
// #ifdef build flag, because remotecontrol.cpp ships inside the shared
// ants_core_lib — a build define would leak into the main binary.
bool rcdetail::g_forceCounterCommitFail = false;

void RemoteControl::setForceCounterCommitFailForTest(bool on) {
    g_forceCounterCommitFail = on;
}

// ANTS-1433 — test-only entry point. Drives the append path against a
// synthetic caller_cwd (req must carry it) without a MainWindow,
// mirroring cmdVerifyChangesWithRoot. See
// tests/features/mcp_roadmap_log_atomicity/spec.md.
QJsonDocument RemoteControl::cmdRoadmapLogAppendForTest(
        const QJsonObject &req) {
    return cmdRoadmapLogAppend(req);
}

// ANTS-1717/1793 — test seam for the flip/annotate path. Flip is
// m_main-independent (caller_cwd + filesystem only), so the test drives
// it directly against a synthetic project root.
QJsonDocument RemoteControl::cmdRoadmapLogFlipForTest(
        const QJsonObject &req) {
    return cmdRoadmapLogFlip(req);
}

// ANTS-3406 — test seam for the amend_body path (m_main-independent:
// caller_cwd + filesystem only). See
// tests/features/roadmap_log_amend_body/spec.md.
QJsonDocument RemoteControl::cmdRoadmapLogAmendBodyForTest(
        const QJsonObject &req) {
    return cmdRoadmapLogAmendBody(req);
}

// ANTS-1690 — test seam for the batch-flip path (m_main-independent).
QJsonDocument RemoteControl::cmdRoadmapLogFlipBatchForTest(
        const QJsonObject &req) {
    return cmdRoadmapLogFlipBatch(req);
}

// ANTS-1878 — test seam for the create_section path (m_main-independent).
QJsonDocument RemoteControl::cmdRoadmapLogCreateSectionForTest(
        const QJsonObject &req) {
    return cmdRoadmapLogCreateSection(req);
}

// ANTS-1879 — test seam for the append_batch path (m_main-independent;
// reaches counter + markdown only, no MainWindow needed).
QJsonDocument RemoteControl::cmdRoadmapLogAppendBatchForTest(
        const QJsonObject &req) {
    return cmdRoadmapLogAppendBatch(req);
}

// ANTS-2125 / ANTS-3401 — shared malformed-[Unreleased] advisory text.
// Four emission sites (single add: dry_run + write; add_batch: dry_run +
// write) differ only in singular/plural and tense, so build the string
// once. ANTS-3401 (MAME Curator feedback): when the section deliberately
// uses feature-grouped `### ` subsections instead of flat categories, the
// canonical flat-category insert reads inconsistently — name the
// alternative insert paths so the caller can choose before committing.
QString rcdetail::changelogMalformedAdvisory(int line, bool plural,
                                          bool applied) {
    const QString subject = plural ? QStringLiteral("entries")
                                    : QStringLiteral("the entry");
    const QString tense = applied
        ? (plural ? QStringLiteral("were inserted")
                  : QStringLiteral("was inserted"))
        : QStringLiteral("would be inserted");
    return QStringLiteral(
        "changelog_log: `## [Unreleased]` interleaves non-heading prose "
        "between its `### ` category blocks (first at line %1) — %2 %3 in "
        "canonical order, but the section layout is malformed. If this "
        "section deliberately uses feature-grouped `### ` subsections "
        "(not flat Keep-a-Changelog categories), the flat-category insert "
        "may read inconsistently; consider op:add_from_roadmap into a "
        "named subsection or a hand-edit. Otherwise, consider tidying it.")
        .arg(line).arg(subject, tense);
}

