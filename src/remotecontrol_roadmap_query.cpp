// ANTS-3833 TU 3/19 — Roadmap read ops.
#include "remotecontrol.h"
#include "roadmapparse.h"   // ANTS-4989
#include <QRegularExpression>
#include "remotecontrol_internal.h"
#include "guithread.h"
#include "projectsettings.h"   // ANTS-3771 — the declared id format
#include "mcpspill.h"        // ANTS-2094 — read_spill
#include "mcpprojection.h"
#include "paginationengine.h"
#include "roadmapfoldin.h"
#include "roadmapclock.h"   // ANTS-4501 § 2.2 — the report reads "today" through the seam
#include "roadmapwrite.h"   // ANTS-4462 — measureDrift, the read-side staleness check
#include "passheadingwrite.h"   // ANTS-2126 — the pass-headings writer (moved from TU 2)
#include <QDateTime>
#include <QFile>
#include <QSaveFile>   // ANTS-4635 — the shared .roadmap-counter cache refresh
#include <QFileInfo>
#include <algorithm>   // ANTS-4636 — std::max over the two high-water sources

using namespace rcdetail;  // ANTS-3833

// ANTS-4932 § 2.2 — the roadmap_log helpers below were the tail of TU 2. They
// read no window, so they moved here when TU 2 went GUI-side; the
// concatenated order of the RemoteControl text is unchanged.

// ANTS-1932 — to_status synonym resolver. Maps the natural English words
// a caller reaches for first (done/wip/todo/maybe …) onto the canonical
// roadmap_log status. Extracted (ANTS-2126) so the GFM flip path and the
// pass-headings flip path share one map instead of duplicating it.
QString rcdetail::rlCanonicalToStatus(const QString &toStatus) {
    const QString lo = toStatus.toLower();
    if (lo == QLatin1String("done")     || lo == QLatin1String("complete") ||
        lo == QLatin1String("completed"))
        return QStringLiteral("shipped");
    if (lo == QLatin1String("wip")      || lo == QLatin1String("in_progress"))
        return QStringLiteral("in-progress");
    if (lo == QLatin1String("todo")     || lo == QLatin1String("open"))
        return QStringLiteral("planned");
    if (lo == QLatin1String("maybe")    || lo == QLatin1String("idea"))
        return QStringLiteral("considered");
    return toStatus;
}

// ============================ ANTS-2126 ============================
// Pass-headings (`#### Pass N.M`) write handlers. The GFM/ants-v1 write
// paths route here (instead of returning the ANTS-2031 format_mismatch
// refusal) when the target roadmap is pass-headings. File-static free
// functions: each is pure given (req, roadmapPath, markdown) — invoked
// from inside the member handlers at the format-detection gate, so they
// need no test seam of their own (the existing *ForTest seams reach them
// through the gate). The splice/flip primitives live in
// passheadingwrite.{h,cpp}. See docs/specs/ANTS-2126.md.

// Atomic ROADMAP.md write (QSaveFile). No counter side-effect (INV-10).
static bool rcAtomicWriteRoadmap(const QString &path, const QString &content) {
    QSaveFile rw(path);
    if (!rw.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    const QByteArray utf8 = content.toUtf8();
    return rw.write(utf8) == utf8.size() && rw.commit();
}

// Validate + render one pass bullet (shared by single + batch append).
struct PassAppendItem {
    bool    ok = false;
    QString code;     // refusal code iff !ok
    QString error;    // message iff !ok
    QString block;    // rendered `#### Pass …` block iff ok
    QString synthId;  // reader-synthesised id iff ok
};
// ANTS-4354 — `fallbackPass` is the CALL-level `pass`, used when a bullet
// carries none. On the single-append path the bullet object IS the request,
// so nothing changed there. On `append_batch` the bullet is one `bullets[]`
// item with no `pass` slot, and this function was handed it alone — so every
// bullet refused bad_args "pass is required", and supplying the top-level
// `pass` did not help because nothing read it. The refusal was byte-identical
// either way, which is the shape that reads as "I passed it wrong" and
// invites retries that cannot work. So append_batch could write NONE on a
// pass-headings roadmap, not merely all-under-one-heading as the schema
// suggested.
//
// Both halves of the reporter's first option ship: a per-bullet `pass`
// (mirroring how `stable_id` is already per-bullet under
// id_strategy:"stable_prefix"), AND the call-level one as the fallback, so a
// batch of N passes can name N designators or share one.
static PassAppendItem rcRenderPassBullet(const QJsonObject &b,
                                         const QString &fallbackPass = {}) {
    PassAppendItem it;
    const QString status   = b.value(QStringLiteral("status")).toString();
    const QString headline = b.value(QStringLiteral("headline")).toString();
    const QString perBullet = b.value(QStringLiteral("pass")).toString();
    const QString pass = perBullet.isEmpty() ? fallbackPass : perBullet;
    QString body           = b.value(QStringLiteral("body")).toString();
    // INV-3 — status / pass / headline are required; bad_args is the
    // documented missing/ill-shaped-arg code (ANTS-2128 keeps the GFM
    // append path's undocumented missing_field out of this new site).
    if (status.isEmpty()) {
        it.code = QStringLiteral("bad_args");
        it.error = QStringLiteral("status is required"); return it;
    }
    const QString keyword = PassHeadingWrite::passStatusKeyword(status);
    if (keyword.isEmpty()) {
        it.code = QStringLiteral("bad_status");
        it.error = QStringLiteral("unknown status \"%1\" — expected "
            "planned / in-progress / shipped / considered / dropped").arg(status);
        return it;
    }
    if (pass.isEmpty()) {
        it.code = QStringLiteral("bad_args");
        it.error = QStringLiteral("pass is required on a pass-headings "
            "roadmap (e.g. \"43.5\" or \"43.5.B\") — set it per bullet, or "
            "once at the call to share one designator across the batch");
        return it;
    }
    if (!PassHeadingWrite::isValidPassDesignator(pass)) {
        it.code = QStringLiteral("bad_args");
        it.error = QStringLiteral("pass \"%1\" is malformed — expected "
            "^\\d+\\.\\d+(?:\\.[A-Za-z][A-Za-z0-9]*)?$").arg(pass);
        return it;
    }
    if (headline.trimmed().isEmpty()) {
        it.code = QStringLiteral("bad_args");
        it.error = QStringLiteral("headline is required"); return it;
    }
    QStringList scrubbed;
    rcScrubLeakedToolXml(body, scrubbed);
    it.block   = PassHeadingWrite::formatPassBlock(pass, headline, keyword, body);
    it.synthId = PassHeadingWrite::passIdFromDesignator(pass);
    it.ok = true;
    return it;
}

// Locate `section` in a pass-headings roadmap; emit the GFM-parity
// bad_case (case-sensitive slug) / bad_section refusal when absent.
// Returns nullptr + fills *refusal on miss; the section pointer on hit.
static const RoadmapIndex::Section *rcPassFindSection(
        const QVector<RoadmapIndex::Section> &index,
        const QString &section, QJsonDocument *refusal) {
    const auto *sec = RoadmapIndex::findBySlug(index, section);
    if (sec) return sec;
    // Sanitise the echoed slug (≤ 64 B + control-char filter), same
    // hygiene as the GFM cmdRoadmapLogAppend bad_section path — never
    // reflect arbitrary caller bytes through the response.
    QString verbatim = section;
    if (verbatim.size() > 64) verbatim.truncate(64);
    for (int i = 0; i < verbatim.size(); ++i) {
        if (verbatim.at(i).unicode() < 0x20) verbatim[i] = QChar('?');
    }
    const QString sectionCi = section.toLower();
    for (const auto &s : index) {
        if (s.slug.toLower() == sectionCi && s.slug != section) {
            QJsonObject e;
            e["ok"]             = false;
            e["code"]           = QStringLiteral("bad_case");
            e["error"]          = QStringLiteral("roadmap_log: section "
                "slug case mismatch: \"%1\" — did you mean \"%2\"?")
                    .arg(verbatim, s.slug);
            e["canonical_slug"] = s.slug;
            e["format"]         = QStringLiteral("pass-headings");
            *refusal = QJsonDocument(e);
            return nullptr;
        }
    }
    QJsonObject e;
    e["ok"]     = false;
    e["code"]   = QStringLiteral("bad_section");
    e["error"]  = QStringLiteral("roadmap_log: unknown section slug "
        "\"%1\"").arg(verbatim);
    e["format"] = QStringLiteral("pass-headings");
    *refusal = QJsonDocument(e);
    return nullptr;
}

// ANTS-4117 — does this roadmap separate its pass blocks with a `---` rule?
// RetroDB's ~200 passes each end in one, so a block appended without it does
// not close and the next append reads as part of it; that session abandoned
// the verb and hand-edited three passes. Detect rather than assume: a `---`
// whose next non-blank line is a `#### Pass` heading is a block separator and
// nothing else, so a file that does not use them (Ants' own fixtures, and the
// shape ANTS-2126 § 2.2 renders) sees no change at all.
static bool rcPassBlocksUseSeparator(const QString &markdown) {
    const QStringList lines = markdown.split(QChar('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).trimmed() != QStringLiteral("---")) continue;
        for (int j = i + 1; j < lines.size(); ++j) {
            const QString t = lines.at(j).trimmed();
            if (t.isEmpty()) continue;
            if (t.startsWith(QStringLiteral("#### Pass "))) return true;
            break;  // this `---` is some other rule — keep scanning
        }
    }
    return false;
}

// Splice `block` (already rendered, no surrounding blanks) at the end of
// the section body, managing one leading/trailing blank line for layout.
// Returns the updated body; *headingIdx0 ← 0-based line of the first
// rendered `####` heading.
static QString rcSplicePassBlock(const QString &markdown,
                                 const RoadmapIndex::Section &sec,
                                 const QString &block, int *headingIdx0) {
    QStringList lines = markdown.split(QChar('\n'));
    const int insertAt = sec.lineEnd;  // 0-indexed, exclusive
    QStringList toInsert;
    bool leadingBlank = false;
    if (insertAt > 0 && insertAt <= lines.size() &&
        !lines.value(insertAt - 1).trimmed().isEmpty()) {
        toInsert << QString();
        leadingBlank = true;
    }
    toInsert += block.split(QChar('\n'));
    if (insertAt < lines.size() &&
        !lines.value(insertAt).trimmed().isEmpty()) {
        toInsert << QString();
    }
    for (int k = toInsert.size() - 1; k >= 0; --k)
        lines.insert(insertAt, toInsert.at(k));
    if (headingIdx0) *headingIdx0 = insertAt + (leadingBlank ? 1 : 0);
    return lines.join(QChar('\n'));
}

// ANTS-4357 — the pass-headings block has no slot for kind / source / lanes /
// layman (ANTS-2126 § 2.2), so those fields are correctly DROPPED on this
// dialect. What was wrong is that the drop was silent: four supplied fields
// went nowhere with ok:true and nothing in the envelope, so a silent drop and
// a faithful write were indistinguishable and the only way to learn was to
// re-read the file. That matters because roadmap-format.md § 3.5 makes `Kind:`
// and `Layman:` REQUIRED parts of a bullet — an author conforming to the
// standard supplies them, the writer drops them, and both the author and every
// later reader believe the roadmap conforms.
//
// Echo, do not refuse: dropping them IS correct here, and refusing would make
// a batch across mixed projects unwritable.
QJsonArray rcPassIgnoredFields(const QJsonObject &req) {
    QJsonArray out;
    for (const char *k : {"kind", "source", "lanes", "layman", "evidence"}) {
        const QJsonValue v = req.value(QLatin1String(k));
        const bool present = !v.isUndefined() && !v.isNull() &&
                             !(v.isString() && v.toString().isEmpty()) &&
                             !(v.isArray() && v.toArray().isEmpty());
        if (present) out.append(QLatin1String(k));
    }
    return out;
}

QJsonDocument rcdetail::cmdRoadmapLogPassAppend(
        const QJsonObject &req, const QString &roadmapPath,
        const QString &markdown) {
    auto err = [](const QString &code, const QString &message) {
        QJsonObject e;
        e["ok"]     = false;
        e["code"]   = code;
        e["error"]  = QStringLiteral("roadmap_log op:\"append\": %1").arg(message);
        e["format"] = QStringLiteral("pass-headings");
        return QJsonDocument(e);
    };
    const QString section = req.value(QStringLiteral("section")).toString();
    const PassAppendItem it = rcRenderPassBullet(req);
    if (!it.ok) return err(it.code, it.error);

    const auto index = RoadmapIndex::buildIndex(markdown);
    QJsonDocument refusal;
    const auto *sec = rcPassFindSection(index, section, &refusal);
    if (!sec) return refusal;

    // ANTS-4117 — close the block the way this file closes its others.
    QString block = it.block;
    if (rcPassBlocksUseSeparator(markdown))
        block += QStringLiteral("\n\n---");

    int headingIdx0 = 0;
    const QString updated =
        rcSplicePassBlock(markdown, *sec, block, &headingIdx0);

    if (req.value(QStringLiteral("dry_run")).toBool()) {
        QJsonObject out;
        out["ok"]      = true;
        out["dry_run"] = true;
        out["id"]      = it.synthId;
        // ANTS-4116 — echo the roadmap file actually resolved, not a canonical
        // display name. RetroDB's roadmap is lowercase `roadmap.md`, so
        // echoing "ROADMAP.md" named a file that does not exist in that repo:
        // on a case-sensitive filesystem that reads as "the verb is about to
        // create a second, wrong roadmap", and it cost them the verb.
        out["file"]    = QFileInfo(roadmapPath).fileName();
        out["line"]    = headingIdx0 + 1;
        out["bullet"]  = block;
        out["bytes"]   = static_cast<qint64>(block.toUtf8().size());
        out["format"]  = QStringLiteral("pass-headings");
        const QJsonArray ignored = rcPassIgnoredFields(req);
        if (!ignored.isEmpty()) out["ignored_fields"] = ignored;  // ANTS-4357
        return QJsonDocument(out);
    }
    if (!rcAtomicWriteRoadmap(roadmapPath, updated))
        return err(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("atomic write of \"%1\" failed").arg(roadmapPath));

    QJsonObject out;
    out["ok"]            = true;
    out["id"]            = it.synthId;
    out["file"]          = QFileInfo(roadmapPath).fileName();   // ANTS-4116
    out["line"]          = headingIdx0 + 1;
    out["format"]        = QStringLiteral("pass-headings");
    // ANTS-4117 — echo what was actually rendered, not only its size. The
    // pass-headings shape is fixed by ANTS-2126 § 2.2 (canonical Status
    // keyword, body verbatim, no kind/source/lanes/layman slot) and differs
    // from some projects' house style; a caller who did not dry_run first had
    // no way to see that until they re-read the file.
    out["bullet"]        = block;
    out["bytes_written"] = static_cast<qint64>(block.toUtf8().size());
    const QJsonArray ignored = rcPassIgnoredFields(req);
    if (!ignored.isEmpty()) out["ignored_fields"] = ignored;  // ANTS-4357
    return QJsonDocument(out);
}

QJsonDocument rcdetail::cmdRoadmapLogPassAppendBatch(
        const QJsonObject &req, const QString &roadmapPath,
        const QString &markdown) {
    const QString section = req.value(QStringLiteral("section")).toString();
    const QJsonArray bullets = req.value(QStringLiteral("bullets")).toArray();

    const auto index = RoadmapIndex::buildIndex(markdown);
    QJsonDocument refusal;
    const auto *sec = rcPassFindSection(index, section, &refusal);
    if (!sec) return refusal;

    QJsonArray applied, skipped;
    QStringList renderedBlocks;
    const bool useSeparator = rcPassBlocksUseSeparator(markdown);  // ANTS-4117
    for (int i = 0; i < bullets.size(); ++i) {
        const PassAppendItem it = rcRenderPassBullet(
            bullets.at(i).toObject(),
            req.value(QStringLiteral("pass")).toString());   // ANTS-4354
        if (!it.ok) {
            QJsonObject s;
            s["bullet_index"] = i;
            s["code"]         = it.code;
            s["error"]        = it.error;
            skipped.append(s);
            continue;
        }
        const QString block = useSeparator
            ? it.block + QStringLiteral("\n\n---") : it.block;
        renderedBlocks << block;
        QJsonObject a;
        a["bullet_index"] = i;
        a["id"]           = it.synthId;
        a["bullet"]       = block;   // ANTS-4117
        applied.append(a);
    }

    auto envelope = [&](qint64 bytesWritten) {
        QJsonObject out;
        out["ok"]            = true;
        out["op"]            = QStringLiteral("append_batch");
        out["format"]        = QStringLiteral("pass-headings");
        out["file"]          = QFileInfo(roadmapPath).fileName();   // ANTS-4116
        out["applied"]       = applied;
        out["applied_count"] = applied.size();
        out["skipped"]       = skipped;
        out["skipped_count"] = skipped.size();
        if (bytesWritten >= 0) out["bytes_written"] = bytesWritten;
        return QJsonDocument(out);
    };

    // INV-14 — an all-invalid batch leaves the file untouched.
    if (renderedBlocks.isEmpty()) return envelope(-1);

    const QString combined = renderedBlocks.join(QStringLiteral("\n\n"));
    int headingIdx0 = 0;
    const QString updated =
        rcSplicePassBlock(markdown, *sec, combined, &headingIdx0);

    if (req.value(QStringLiteral("dry_run")).toBool()) {
        QJsonObject out = envelope(-1).object();
        out["dry_run"] = true;
        return QJsonDocument(out);
    }
    if (!rcAtomicWriteRoadmap(roadmapPath, updated)) {
        QJsonObject e;
        e["ok"]     = false;
        e["code"]   = QStringLiteral("roadmap_write_failed");
        e["error"]  = QStringLiteral("roadmap_log op:\"append_batch\": "
            "atomic write of \"%1\" failed").arg(roadmapPath);
        e["format"] = QStringLiteral("pass-headings");
        return QJsonDocument(e);
    }
    return envelope(static_cast<qint64>(combined.toUtf8().size()));
}

// Serves op:"flip" AND op:"annotate" (the member gate routes both here,
// reading op from req — mirrors cmdRoadmapLogFlip).
QJsonDocument rcdetail::cmdRoadmapLogPassFlip(
        const QJsonObject &req, const QString &roadmapPath,
        const QString &markdown) {
    const bool annotateMode =
        req.value(QStringLiteral("op")).toString() ==
            QStringLiteral("annotate");
    auto err = [&](const QString &code, const QString &message) {
        QJsonObject e;
        e["ok"]     = false;
        e["code"]   = code;
        e["error"]  = QStringLiteral("roadmap_log op:\"%1\": %2")
            .arg(annotateMode ? QStringLiteral("annotate")
                              : QStringLiteral("flip"), message);
        e["format"] = QStringLiteral("pass-headings");
        return QJsonDocument(e);
    };
    const QString locId = req.value(QStringLiteral("id")).toString();
    const QString locHeadline =
        req.value(QStringLiteral("headline")).toString();
    // INV-3 — a locator is required; a pass is addressed by its
    // synthesised `PASS-N-M` id or its heading tail.
    if (locId.isEmpty() && locHeadline.isEmpty())
        return err(QStringLiteral("bad_args"),
            QStringLiteral("needs a locator — `id` (PASS-N-M) or `headline`"));

    PassHeadingWrite::WriteResult r;
    QString toKeyword;
    if (annotateMode) {
        QString note = req.value(QStringLiteral("note")).toString();
        QStringList scrubbed;
        rcScrubLeakedToolXml(note, scrubbed);
        // INV-8 — empty note → bad_args (deliberately the documented code,
        // diverging from the GFM annotate guard's missing_field; ANTS-2128).
        if (note.isEmpty())
            return err(QStringLiteral("bad_args"),
                QStringLiteral("a non-empty `note` is required"));
        r = PassHeadingWrite::annotatePass(markdown, locId, locHeadline, note);
    } else {
        const QString toStatus =
            req.value(QStringLiteral("to_status")).toString();
        if (toStatus.isEmpty())
            return err(QStringLiteral("bad_args"),
                QStringLiteral("to_status is required"));
        toKeyword = PassHeadingWrite::passStatusKeyword(
            rlCanonicalToStatus(toStatus));
        if (toKeyword.isEmpty())
            return err(QStringLiteral("bad_status"),
                QStringLiteral("unknown to_status \"%1\"").arg(toStatus));
        r = PassHeadingWrite::flipPassStatus(markdown, locId, locHeadline,
                                             toKeyword);
    }
    if (!r.ok)
        return err(r.code, QStringLiteral("no pass matched the locator"));

    // ANTS-2136 — dry_run preview: the locator resolved and the would-be
    // markdown is computed; return the preview (located id, target line,
    // would-be bytes) WITHOUT writing ROADMAP.md. A dry_run that returns
    // ok:true proves the locator resolves on a pass-headings roadmap —
    // the exact verification gap RetroDB flagged for flip/annotate.
    if (req.value(QStringLiteral("dry_run")).toBool()) {
        QJsonObject out;
        out["ok"]      = true;
        out["op"]      = annotateMode ? QStringLiteral("annotate")
                                       : QStringLiteral("flip");
        out["dry_run"] = true;
        out["id"]      = r.matchedId;
        out["file"]    = QFileInfo(roadmapPath).fileName();   // ANTS-4116
        out["line"]    = r.headingLine + 1;
        out["format"]  = QStringLiteral("pass-headings");
        out["bytes"]   = static_cast<qint64>(r.markdown.toUtf8().size());
        if (annotateMode) {
            out["note_appended"] = true;
            out["note_line"]     = r.changedLine + 1;
        }
        return QJsonDocument(out);
    }

    const qint64 sizeBefore = QFileInfo(roadmapPath).size();   // ANTS-3702
    if (!rcAtomicWriteRoadmap(roadmapPath, r.markdown))
        return err(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("atomic write of \"%1\" failed").arg(roadmapPath));

    QJsonObject out;
    out["ok"]            = true;
    out["op"]            = annotateMode ? QStringLiteral("annotate")
                                        : QStringLiteral("flip");
    out["id"]            = r.matchedId;
    out["file"]          = QFileInfo(roadmapPath).fileName();   // ANTS-4116
    out["line"]          = r.headingLine + 1;
    out["format"]        = QStringLiteral("pass-headings");
    rcSetWriteBytes(out, sizeBefore,
                    static_cast<qint64>(r.markdown.toUtf8().size()));
    if (annotateMode) {
        out["note_appended"] = true;
        out["note_line"]     = r.changedLine + 1;
    } else {
        out["to_status"] = toKeyword;
    }
    return QJsonDocument(out);
}

QJsonDocument rcdetail::cmdRoadmapLogPassFlipBatch(
        const QJsonObject &req, const QString &roadmapPath,
        const QString &markdown, bool annotateMode) {
    const QString canon =
        rlCanonicalToStatus(req.value(QStringLiteral("to_status")).toString());
    // ANTS-4470 — empty under annotate_batch, and unused there: the gate
    // refuses a to_status on that op, so there is no status to resolve.
    const QString keyword = PassHeadingWrite::passStatusKeyword(canon);
    const QString opName = annotateMode ? QStringLiteral("annotate_batch")
                                        : QStringLiteral("flip_batch");
    // to_status is validated canonical at the gate, so keyword is non-empty.
    const QJsonArray locators = req.value(QStringLiteral("locators")).toArray();

    QString md = markdown;
    QJsonArray flipped, skipped;
    for (int i = 0; i < locators.size(); ++i) {
        const QJsonObject loc = locators.at(i).toObject();
        const QString locId = loc.value(QStringLiteral("id")).toString();
        const QString locHeadline =
            loc.value(QStringLiteral("headline")).toString();
        if (locId.isEmpty() && locHeadline.isEmpty()) {
            QJsonObject s;
            s["locator_index"] = i;
            s["code"]          = QStringLiteral("missing_field");
            s["error"]         = QStringLiteral("locator needs one of "
                "id / headline");
            skipped.append(s);
            continue;
        }
        QString note = loc.value(QStringLiteral("note")).toString();
        // ANTS-4470 — an annotate with no note writes nothing; refused per
        // locator, as on the GFM/ants-v1 batch path.
        if (annotateMode && note.isEmpty()) {
            QJsonObject s;
            s["locator_index"] = i;
            s["code"]          = QStringLiteral("missing_field");
            s["error"]         = QStringLiteral("op:\"annotate_batch\" requires "
                "a non-empty `note` on every locator");
            skipped.append(s);
            continue;
        }
        // ANTS-4470 — under annotate the status surgery is skipped entirely.
        // The locator must still RESOLVE, so the note has somewhere to land and
        // an unmatched locator is still reported; annotatePass is what resolves
        // it in that case.
        QString matchedId;
        if (!annotateMode) {
            PassHeadingWrite::WriteResult r =
                PassHeadingWrite::flipPassStatus(md, locId, locHeadline, keyword);
            if (!r.ok) {
                QJsonObject s;
                s["locator_index"] = i;
                s["code"]          = r.code;
                s["error"]         = QStringLiteral("locator matched no pass");
                skipped.append(s);
                continue;
            }
            md = r.markdown;
            matchedId = r.matchedId;
        }
        if (!note.isEmpty()) {
            QStringList sc;
            rcScrubLeakedToolXml(note, sc);
            PassHeadingWrite::WriteResult an =
                PassHeadingWrite::annotatePass(md, locId, locHeadline, note);
            if (annotateMode && !an.ok) {
                QJsonObject s;
                s["locator_index"] = i;
                s["code"]          = an.code;
                s["error"]         = QStringLiteral("locator matched no pass");
                skipped.append(s);
                continue;
            }
            if (an.ok) {
                md = an.markdown;
                if (matchedId.isEmpty()) matchedId = an.matchedId;
            }
        }
        QJsonObject f;
        f["locator_index"] = i;
        f["id"]            = matchedId;
        flipped.append(f);
    }

    // ANTS-3702 — `before` < 0 means nothing was written (INV-14).
    auto envelope = [&](qint64 before, qint64 after) {
        QJsonObject out;
        out["ok"]            = true;
        out["op"]            = opName;                       // ANTS-4470
        out["format"]        = QStringLiteral("pass-headings");
        out["file"]          = QFileInfo(roadmapPath).fileName();   // ANTS-4116
        out["flipped"]       = flipped;
        out["flipped_count"] = flipped.size();
        out["skipped"]       = skipped;
        out["skipped_count"] = skipped.size();
        if (before >= 0) rcSetWriteBytes(out, before, after);
        return QJsonDocument(out);
    };

    // INV-14 — nothing applied → file untouched.
    if (flipped.isEmpty()) return envelope(-1, 0);
    const qint64 sizeBefore = QFileInfo(roadmapPath).size();   // ANTS-3702
    if (!rcAtomicWriteRoadmap(roadmapPath, md)) {
        QJsonObject e;
        e["ok"]     = false;
        e["code"]   = QStringLiteral("roadmap_write_failed");
        e["error"]  = QStringLiteral("roadmap_log op:\"%1\": "
            "atomic write of \"%2\" failed").arg(opName).arg(roadmapPath);
        e["format"] = QStringLiteral("pass-headings");
        return QJsonDocument(e);
    }
    return envelope(sizeBefore, static_cast<qint64>(md.toUtf8().size()));
}
// ========================== end ANTS-2126 ==========================

// ANTS-1922 — id-ordering for bundles mode. Compares the integer
// suffix of an ANTS-NNNN id ascending (so ANTS-999 precedes ANTS-1000,
// which a plain string sort inverts); falls back to a lexicographic
// compare on the full id for any non-conforming id or an equal suffix.
// One rule, three call-sites (items[] sort, bundle size-tie-break,
// bundle_label lowest-id fallback) so the envelope is byte-stable.
bool rcdetail::rcRoadmapIdLess(const QString &a, const QString &b) {
    auto suffix = [](const QString &id, bool *ok) -> qlonglong {
        const int dash = id.lastIndexOf(QLatin1Char('-'));
        if (dash < 0 || dash + 1 >= id.size()) { *ok = false; return 0; }
        // ANTS-4500 § 4.4 — step past the synthesis namespace's `S` so a
        // synthesised id orders by its own number. Without this it falls to the
        // lexicographic branch below, which puts `-S10000` before `-S9999`.
        int start = dash + 1;
        if (id.at(start) == QLatin1Char('S') && start + 1 < id.size())
            ++start;
        return id.mid(start).toLongLong(ok);
    };
    bool okA = false, okB = false;
    const qlonglong na = suffix(a, &okA);
    const qlonglong nb = suffix(b, &okB);
    if (okA && okB) {
        if (na != nb) return na < nb;
        return a < b;   // equal numeric suffix → lexicographic tie-break
    }
    return a < b;       // non-conforming id → lexicographic
}

// ANTS-1922 — scan a (≤2000-char cached) bullet body for the FIRST
// line carrying a directional gate/blocker marker; return it trimmed
// then capped to ≤160 chars (empty = no marker). A "line" is the text
// between `\n` separators. Match is a case-folded substring test per
// line; the `until` rule additionally requires `lands` or `ships` on
// the same line, in any order. `blocks ` is deliberately NOT a marker
// (an item that blocks others is itself actionable, and the bare verb
// false-fires on prose like "blocks the cursor"). Only `blocked by` and
// the `until`+(`lands`|`ships`) rule are INV-locked (INV-5); the rest
// are best-effort. The 2000-char cap is a hard character cut, so a
// marker past it — or on the line straddling it — is missed (acceptable
// for v1: gate notes sit near the bullet head by convention).
QString rcdetail::rcExtractGateNote(const QString &body) {
    static const QStringList markers = {
        QStringLiteral("blocked by"), QStringLiteral("gated"),
        QStringLiteral("depends on"), QStringLiteral("waiting on"),
        QStringLiteral("parked"),     QStringLiteral("superseded"),
        // ANTS-3389 — the imperative prose form ("Wait for P10 to close"),
        // which no -ing/participle marker above catches.
        QStringLiteral("wait for "),
    };
    const QStringList lines = body.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString lower = line.toLower();
        bool hit = false;
        for (const QString &m : markers) {
            if (lower.contains(m)) { hit = true; break; }
        }
        if (!hit && lower.contains(QStringLiteral("until ")) &&
            (lower.contains(QStringLiteral("lands")) ||
             lower.contains(QStringLiteral("ships")))) {
            hit = true;
        }
        if (hit) {
            QString note = line.trimmed();
            if (note.size() > 160) note.truncate(160);
            return note;
        }
    }
    return QString();
}

// ANTS-3793 § 2.1 — the owner wrapper. See remotecontrol.h for the contract;
// the two rules discharged here are § 2.2's first and second, in that order.
QVector<RoadmapParse::BulletRecord>
RemoteControl::roadmapBullets(const QString &projectRoot,
                              RoadmapSource::RoadmapText &text,
                              bool includeArchive,
                              RoadmapSource::ReadError *why,
                              QString *error) const {
    if (why)
        *why = RoadmapSource::ReadError::None;
    if (error)
        error->clear();

    // A caller with no project root cannot ask the marker anything —
    // migratedProject() refuses a root it cannot canonicalise, and passing ""
    // would turn every focused-tab call into a refusal.
    // ANTS-4884 — callers hand this their caller_cwd, which is at or below the
    // root the store keys on. Resolve before asking, or a subdirectory caller
    // reads as an unmigrated project of its own. idFormatFor() below reads
    // .ants/project.json, which lives at the root, and missed the same way.
    const QString root = rcProjectRootFor(projectRoot);
    if (!root.isEmpty()) {
        // The local rather than `why` directly: a caller that passed nullptr
        // would otherwise turn rule 2's refusal into a fall-through to
        // markdown — the silent fallback INV-1 forbids, arriving through an
        // omitted out-param.
        RoadmapSource::ReadError openWhy = RoadmapSource::ReadError::None;
        RoadmapStore *store = roadmapStoreOrNull(&openWhy, error);
        if (openWhy != RoadmapSource::ReadError::None) {
            if (why)
                *why = openWhy;
            return {};
        }
        if (store) {
            RoadmapSource::ReadError seamWhy = RoadmapSource::ReadError::None;
            auto records = RoadmapSource::bulletsFor(
                *store, root, text, includeArchive,
                &seamWhy, error,
                ProjectSettings::idFormatFor(root));   // ANTS-3771
            if (why)
                *why = seamWhy;
            if (records)
                return *records;
            // nullopt with a reason is a refusal and never a fallback (INV-1);
            // nullopt with None means "not migrated", which falls through.
            if (seamWhy != RoadmapSource::ReadError::None)
                return {};
        }
    }
    // ANTS-3863 — the line that makes the laziness pay. It is reached only when
    // the project is NOT migrated, which is the one case where the whole file
    // was always going to be needed.
    // ANTS-3771 — the same declaration the seam above was given, so the two
    // outcomes of this function cannot resolve one bullet to two ids (INV-13).
    return RoadmapParse::parseBullets(
        text.full(), ProjectSettings::idFormatFor(projectRoot));
}

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
    m_roadmapStore = RoadmapSource::storeFor(
        RoadmapStore::defaultPath(), &openWhy, &openErr,
        // ANTS-3822 § 2.3.2 — production takes the default; only a test lowers it.
        m_roadmapHistoryCap < 0 ? RoadmapStore::kDefaultHistoryCapBytes
                                : m_roadmapHistoryCap);
    if (openWhy != RoadmapSource::ReadError::None) {
        if (why)
            *why = openWhy;
        if (error)
            *error = openErr;
        return nullptr;
    }
    return m_roadmapStore.get();
}

// ANTS-3822 § 2.3.2 — here rather than inline in the header, because discarding
// the cached store needs RoadmapStore complete.
void RemoteControl::setRoadmapHistoryCapForTest(qint64 bytes) {
    m_roadmapHistoryCap = bytes;
    m_roadmapStore.reset();
}

// ANTS-3793 § 2.2 — the dispatch on its own. See remotecontrol.h for the one
// site that needs it and why it cannot ask by reading.
bool RemoteControl::roadmapStoreServes(const QString &projectRoot,
                                       RoadmapSource::RoadmapText &text,
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
    // ANTS-4884 — as roadmapBullets() and roadmapWriteTarget(): resolve the
    // caller's cwd to the root the store keys on before asking.
    const bool served =
        store && RoadmapSource::migratedProject(*store,
                                                rcProjectRootFor(projectRoot),
                                                text, error,
                                                &localWhy).has_value();
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
                                  RoadmapSource::RoadmapText &text,
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

    // ANTS-4884 — as roadmapBullets(): the caller's cwd is at or below the root
    // the store keys on, and a subdirectory miss here silently splices markdown
    // that the next store render discards.
    const auto projectId = RoadmapSource::migratedProject(
        *store, rcProjectRootFor(projectRoot), text, error, &localWhy);
    if (why)
        *why = localWhy;
    if (!projectId)
        return std::nullopt;
    // ANTS-4953 — refused by NAME. Before this a worktree outside the project
    // was stopped only by the render's containment check, whose message sent
    // the caller to fix a path, and a worktree INSIDE the project (Claude
    // Code's own isolation puts them there) was not stopped at all.
    if (const QString main = rcWorktreeMainCheckout(projectRoot); !main.isEmpty()) {
        if (why)
            *why = RoadmapSource::ReadError::WorktreeWrite;
        if (error)
            *error = QStringLiteral(
                "roadmap_log: caller_cwd is a git worktree. This project's roadmap "
                "lives in the roadmap store, keyed to the main checkout at %1, and "
                "writing here would render it into the worktree's own ROADMAP.md. "
                "Pass caller_cwd as %1 instead; nothing was written.").arg(main);
        return std::nullopt;
    }
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
    case RoadmapSource::ReadError::WorktreeWrite:   // ANTS-4953
        out["ok"] = false;
        out["error"] = err.isEmpty()
            ? QStringLiteral("roadmap writes are refused from a git worktree; "
                             "write from the main checkout")
            : err;
        out["code"] = QStringLiteral("worktree_write");
        return true;
    }
    return false;
}

// ─── ANTS-3809 — the store-backed write path, shared by all eight ops ──────

// § 7's code table, once for the whole verb layer. Mirrors
// rcRoadmapSourceRefused() above and reads the same way at a call site:
// `if (rcRoadmapWriteRefused(out, r, err, outcome)) return QJsonDocument(out);`
//
// Every failing value carries a genuinely different remedy — fill in the
// missing Layman lines, import what the store is missing (ANTS-4141), fix the
// store's contents, fix the store file, re-run the render — which is why they
// are five codes and not one.
void rcdetail::rcRoadmapWriteFields(QJsonObject &out,
                                    const RoadmapRender::Outcome &outcome,
                                    bool dryRun) {
    // ANTS-4463 — see remotecontrol_internal.h for why the tense matters.
    if (dryRun) {
        out[QStringLiteral("dry_run")]    = true;
        out[QStringLiteral("would_write")] =
            QJsonArray::fromStringList(outcome.filesWritten);
        // ANTS-5283 — would_write names only files whose bytes differ
        // (ANTS-5016), so this is the answer to "is a render owed?". Stated
        // outright because would_discard_external_edits:false was read as it.
        out[QStringLiteral("would_change")] = !outcome.filesWritten.isEmpty();
    } else {
        out[QStringLiteral("files_written")] =
            QJsonArray::fromStringList(outcome.filesWritten);
    }
    // ANTS-5016 — files the render left alone because their bytes already
    // matched. A comparison result, not a write claim, so one name serves a
    // real run and a dry run.
    if (!outcome.filesUnchanged.isEmpty())
        out[QStringLiteral("files_unchanged")] =
            QJsonArray::fromStringList(outcome.filesUnchanged);
    out[QStringLiteral("items_rendered")] = outcome.itemsRendered;

    // ANTS-4462 / ANTS-4465 — what the publish overwrote that the store never
    // held. Absent when nothing measured it, because false would claim the file
    // was clean when in fact nobody looked (the ANTS-4463 lesson, in the other
    // direction: there a present field asserted an action that never happened).
    //
    // Tense follows the same rule as the two fields above. The count rides only
    // on the true arm: on a healthy project it is always zero, and a zero
    // emitted on every write is a field nobody reads.
    if (outcome.externalEditsChecked) {
        // ANTS-4729 — the BOOLEAN asks whether the publish overwrote content
        // the file held, so it is computed from the file's own classified
        // lines. externalEditLines is two-directional: it also counts lines the
        // RENDER holds and the file lacks, and those are additions, which can
        // never be something the publish discarded.
        //
        // The reported case is the format-marker header on a project whose file
        // was rendered before that marker existed: two render-only lines, a
        // true flag, and every content counter zero. This is documented as the
        // one place a silently discarded hand-edit surfaces, so a session is
        // told to stop and investigate whenever it fires — and an investigation
        // that routinely finds the renderer's own output is how the flag stops
        // being read.
        //
        // The TOTAL is deliberately unchanged. ANTS-4462 is explicit that
        // deciding which differences are cosmetic is not this check's
        // judgement, so `discarded_edit_lines` still counts every differing
        // line in both directions; only the claim made ABOUT it is narrowed.
        const bool any = (outcome.externalRestyledLines
                          + outcome.externalRepunctuatedLines
                          + outcome.externalRestructuredLines
                          + outcome.externalTextLines) > 0;
        out[dryRun ? QStringLiteral("would_discard_external_edits")
                   : QStringLiteral("discarded_external_edits")] = any;
        if (any) {
            out[dryRun ? QStringLiteral("would_discard_edit_lines")
                       : QStringLiteral("discarded_edit_lines")] =
                outcome.externalEditLines;
            // ANTS-4957 — the flag above is also true on a stale render nobody
            // edited (a `git checkout` of ROADMAP.md, say), where the publish
            // loses nothing and IS the recovery. One key names the worst thing
            // at stake, so a caller branches once instead of reading a zero
            // and the absence of another key.
            out[dryRun ? QStringLiteral("would_discard_reason")
                       : QStringLiteral("discard_reason")] =
                outcome.externalTextLines > 0         ? QStringLiteral("text_lost")
              : outcome.externalRestructuredLines > 0 ? QStringLiteral("structure")
              : outcome.externalRepunctuatedLines > 0 ? QStringLiteral("punctuation")
                                                      : QStringLiteral("restyle_only");
            // ANTS-4615 — the breakdown. One number could not be acted on: 84
            // drifted lines were 24 bullets restyled into the canonical id form
            // and ONE sentence that no longer existed anywhere. Rides on the
            // same true arm as the total, for the same reason — a breakdown
            // present on every write is a breakdown nobody reads.
            out[dryRun ? QStringLiteral("would_discard_restyled_lines")
                       : QStringLiteral("discarded_restyled_lines")] =
                outcome.externalRestyledLines;
            // ANTS-4695 — a punctuation-only change to the author's own prose
            // is neither of the two above. The Layman: parse drops one
            // trailing period by design, so a project whose Layman lines were
            // hand-authored with periods sees every one of them change on the
            // first render -- and contentKey() cannot see punctuation at all,
            // so all of them scored as `restyled` while `discarded_text_lines`
            // read 0. That is the field a caller checks before allowing the
            // overwrite, so the one number that made the render look safe was
            // the one hiding the change.
            if (outcome.externalRepunctuatedLines > 0) {
                out[dryRun ? QStringLiteral("would_discard_repunctuated_lines")
                           : QStringLiteral("discarded_repunctuated_lines")] =
                    outcome.externalRepunctuatedLines;
            }
            // ANTS-4965 — whitespace-only changes: a nested list or an aligned
            // table flattened. It used to score as restyling, beside benign
            // dialect rewrites, so the preview read as safe.
            if (outcome.externalRestructuredLines > 0) {
                out[dryRun ? QStringLiteral("would_discard_structure_lines")
                           : QStringLiteral("discarded_structure_lines")] =
                    outcome.externalRestructuredLines;
            }
            out[dryRun ? QStringLiteral("would_discard_text_lines")
                       : QStringLiteral("discarded_text_lines")] =
                outcome.externalTextLines;
            // ANTS-4839 — whose text is it? A project keeping a frozen branch
            // sees a large `text_lost` figure on an ordinary one-item write,
            // because the file there is an older publication of this same
            // store and every bullet has moved on since. The fields were
            // honest and unreadable: a caller who does not know that commits a
            // large unrelated diff into whatever change they were making.
            //
            // The claim is FREE and exact, not a heuristic. ANTS-4141's guard
            // refuses the write outright when the file holds a bullet the
            // store has never imported, and it runs before the dry-run return
            // as well — so reaching this line at all proves every id in the
            // file is one the store holds.
            //
            // What it deliberately does NOT claim is that nothing was lost. A
            // hand-edited BODY belongs to a known bullet, so it lands here too,
            // and saying "safe" would be the ANTS-4522 failure in a new place:
            // a reassurance that reads as coverage. It narrows the population
            // and points at the two fields that answer the rest.
            if (outcome.externalTextLines > 0) {
                out[dryRun ? QStringLiteral("would_discard_hint")
                           : QStringLiteral("discard_hint")] = QStringLiteral(
                    "Every bullet in the file is one the store holds — a bullet "
                    "it had never imported would have refused this write "
                    "outright (render_would_drop). So these lines are an OLDER "
                    "PUBLICATION of this same store, which is the ordinary "
                    "case on a branch whose roadmap is behind. It does NOT "
                    "follow that nothing was lost: a hand-edited body belongs "
                    "to a known bullet and lands here too. Read "
                    "`discarded_text` for the lines themselves, and "
                    "`discarded_backup_paths` for where the overwritten file "
                    "was kept.");
            }
            if (!outcome.externalLostText.isEmpty()) {
                // The text itself, not just its size. A count alone still
                // leaves the caller grepping for a sentence they have to
                // remember writing, which is how ANTS-4596 was found.
                out[dryRun ? QStringLiteral("would_discard_text")
                           : QStringLiteral("discarded_text")] =
                    QJsonArray::fromStringList(outcome.externalLostText);
                if (outcome.externalLostTextTruncated) {
                    out[dryRun ? QStringLiteral("would_discard_text_truncated")
                               : QStringLiteral("discarded_text_truncated")] = true;
                }
            }
            // ANTS-4947 — where the overwritten file was kept. The echo above
            // is capped at twenty lines and is the caller's only copy of prose
            // the render reproduces nowhere; a report of destroyed text that
            // leaves nothing to restore it from is what made this loss silent
            // in practice even though a field announced it.
            //
            // No dry-run twin: a preview overwrites nothing, so there is no
            // backup to name and a future-tense key would promise a file that
            // is never written. Absent also when the copy failed — the list
            // says what WAS kept, never what was at stake, which
            // `discarded_text_lines` already answers.
            if (!outcome.externalLostBackups.isEmpty()) {
                out[QStringLiteral("discarded_backup_paths")] =
                    QJsonArray::fromStringList(outcome.externalLostBackups);
            }
        }
    }
}

// ANTS-4635 — see remotecontrol_internal.h for why this is shared and why it
// is best-effort. Every early return leaves the envelope untouched: the two
// counter fields are reported when the cache MOVES, so their absence says it
// did not, and nothing here can fail an append that already landed.
void rcdetail::rcRoadmapReconcileCounterCache(QJsonObject &env,
                                              const QString &counterPath,
                                              qint64 allocated) {
    if (allocated <= 0 || !QFile::exists(counterPath))
        return;
    qint64 cached = 0;
    QFile cr(counterPath);
    if (cr.open(QIODevice::ReadOnly)) {
        cached = QString::fromUtf8(cr.readAll().trimmed()).toLongLong();
        cr.close();
    }
    if (allocated <= cached)
        return;
    QSaveFile cw(counterPath);
    if (!cw.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    const QByteArray cv = (QString::number(allocated) + QChar('\n')).toUtf8();
    if (cw.write(cv) == cv.size() && cw.commit()) {
        env[QStringLiteral("counter_advanced_to")]   = allocated;  // ANTS-2179
        env[QStringLiteral("counter_advanced_past")] = cached;     // ANTS-4493
        // ANTS-4969 — on the store path the STORE allocated the id and this
        // file is a cache kept in step for readers that predict the next id.
        // The two fields above read as the allocation source; this says the
        // file only mirrors it. The markdown path, where the counter IS the
        // source, never reaches this helper and never carries the marker.
        env[QStringLiteral("counter_mirrored")]      = true;
    }
}

bool rcdetail::rcRoadmapWriteRefused(QJsonObject &out, RoadmapWrite::Result r,
                                  const QString &err,
                                  const RoadmapRender::Outcome &outcome,
                                  const QStringList &previewOwnIds) {
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
        // ANTS-4628 — the gate judges the items this write touched, so every
        // named id is one the caller is already editing. Naming them is still
        // what makes the refusal actionable; what changed is that the list can
        // no longer contain an item the caller has never heard of.
        // ANTS-4593 — a dry run runs `mutate` inside the transaction and rolls
        // it back (ANTS-4548), so the caller's candidate row IS in the store
        // when the gate evaluates, and its id was reported verbatim beside
        // genuine ones. That id exists nowhere afterwards: a caller greps for
        // it, finds nothing, and cannot tell a bad input from a diverged
        // store. The two need different actions — fix the call, versus go and
        // repair the roadmap — so they are reported under different keys.
        //
        // `previewOwnIds` is empty on every real write, where the row is real
        // and belongs in gate_failures, so that shape is untouched.
        {
        QStringList theirs;
        QStringList ours;
        for (const QString &id : outcome.gateFailures) {
            if (previewOwnIds.contains(id)) ours.append(id);
            else                            theirs.append(id);
        }
        out[QStringLiteral("gate_failures")] =
            QJsonArray::fromStringList(theirs);
        if (!previewOwnIds.isEmpty()) {
            out[QStringLiteral("request_gate_failures")] =
                QJsonArray::fromStringList(ours);
        }
        // When the ONLY offender is the row this call is proposing, the
        // refusal is about the arguments, not about the roadmap. Say so
        // instead of pointing at an id that will never exist.
        if (!ours.isEmpty() && theirs.isEmpty()) {
            out[QStringLiteral("ok")]    = false;
            out[QStringLiteral("code")]  = QStringLiteral("render_gate_unmet");
            out[QStringLiteral("error")] = QStringLiteral(
                "the roadmap render refuses this write: the item this call "
                "would append carries no Layman: line. Add a one-sentence "
                "`layman` summary and re-send. The id in "
                "`request_gate_failures` is this preview's own candidate and "
                "is rolled back — do not look for it in the roadmap.");
            return true;
        }
        // Reached only when RoadmapWrite left `err` empty — it normally sets a
        // fuller message naming the ids and the remedy. Kept in step with it.
        return fail("render_gate_unmet",
                    "the roadmap render refuses this write: an open item it "
                    "touches carries no Layman: line");
        }
    case RoadmapWrite::Result::WouldDrop:
        // ANTS-4141. No array beside it: unlike the gate, the offenders are
        // named in `error` itself, because the remedy is a migration the caller
        // runs once and not a per-id edit, and the set is ~200 wide in the case
        // this was written for.
        return fail("render_would_drop",
                    "the roadmap render would delete bullets the store has "
                    "never imported");
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
//
// ANTS-4576 — and what an UN-declared column becomes, which is the DDL's answer
// rather than this file's. `layman` is nullable; `lanes` and `evidence` are
// NOT NULL DEFAULT '[]'; `kind` and `source` are NOT NULL with no default. A
// single rule ("clear it") reached the engine on four of the five and came back
// as a raw `NOT NULL constraint failed: item.kind`.
enum class RlUndeclare {
    ClearToNull,   // nullable: absence is a value it can hold
    EmptyList,     // NOT NULL DEFAULT '[]': the empty list IS its absent state
    Keep,          // NOT NULL, no default: it HAS no absent state
};
struct RlTrailerKey {
    const char *field;
    RoadmapParse::TrailerMatch RoadmapParse::TrailerValues::*match;
    // nullptr on the three scalar keys; set on lanes/evidence, whose stored
    // form is a JSON array and not the raw capture.
    QStringList RoadmapParse::TrailerValues::*list;
    QString     RoadmapStore::ItemWrite::*col;
    QStringList RoadmapStore::ItemWrite::*colList;
    RlUndeclare undeclare;
};
static const RlTrailerKey kRlTrailerKeys[] = {
    {"kind",   &RoadmapParse::TrailerValues::kind,   nullptr,
     &RoadmapStore::ItemWrite::kind,   nullptr, RlUndeclare::Keep},
    {"layman", &RoadmapParse::TrailerValues::layman, nullptr,
     &RoadmapStore::ItemWrite::layman, nullptr, RlUndeclare::ClearToNull},
    {"source", &RoadmapParse::TrailerValues::source, nullptr,
     &RoadmapStore::ItemWrite::source, nullptr, RlUndeclare::Keep},
    {"lanes",  &RoadmapParse::TrailerValues::lanes,
     &RoadmapParse::TrailerValues::lanesList,  nullptr,
     &RoadmapStore::ItemWrite::lanes, RlUndeclare::EmptyList},
    {"evidence", &RoadmapParse::TrailerValues::evidence,
     &RoadmapParse::TrailerValues::evidenceList, nullptr,
     &RoadmapStore::ItemWrite::evidence, RlUndeclare::EmptyList},
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
                          // ANTS-4424 asked for "put the backticks IMMEDIATELY
                          // around the key", because the guard was three
                          // fixed-length lookbehinds and a key nested inside a
                          // LONGER span was still read as a trailer — hit
                          // in-session by a body writing `` `query:'Source:'` ``.
                          //
                          // ANTS-4504 shipped the repair that note called for:
                          // trailerValuesIn() masks every inline code span
                          // before matching, so a key ANYWHERE inside one
                          // declares nothing and cannot be the shadowing match.
                          // The advice is plain backticks again, and telling a
                          // caller to move backticks they already have would
                          // now send them nowhere.
                          : QStringLiteral("Reword that mention, or wrap the "
                                           "key in backticks — a key inside a "
                                           "code span is not read as a "
                                           "trailer"),
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
                                   const QSet<QString> &supplied,
                                   HistoryContext *hist, QString *error,
                                   QStringList *kept, QString *code) {
    const RoadmapParse::TrailerValues oldTv = RoadmapParse::trailerValuesIn(before.body);
    const RoadmapParse::TrailerValues newTv = RoadmapParse::trailerValuesIn(newBody);
    for (const RlTrailerKey &k : kRlTrailerKeys) {
        const QString field = QString::fromLatin1(k.field);
        if (supplied.contains(field))
            continue;
        const QString oldValue = rlBodyValueFor(oldTv, k);
        QString newValue = rlBodyValueFor(newTv, k);   // ANTS-4576 canonicalises `kind`
        if (oldValue.isEmpty() && newValue.isEmpty())
            continue;  // untouched — it came from a request argument or the
                       // migration, and this op has no opinion about it.

        const QString current =
            k.colList ? rlJsonArrayText(before.*(k.colList)) : before.*(k.col);
        if (!newValue.isEmpty()) {
            // ANTS-4576 — `kind` is a CLOSED vocabulary the column CHECKs, and
            // the capture arrives raw: `Kind: Fix.` and the mapped alias
            // `Kind: bug.` are both recognised by the parser and both refused
            // by the engine. Canonicalise with the migration's own mapper
            // (roadmap-data-model.md § 7.4, applied and not restated), and say
            // so in words when nothing recognises it — the alternative is the
            // engine's `CHECK constraint failed: kind IN (...)`, which quotes
            // 21 values and not the one the caller wrote.
            if (k.undeclare == RlUndeclare::Keep && field == QLatin1String("kind")) {
                const QString folded = newValue.trimmed().toLower();
                if (RoadmapParse::canonicalKinds().contains(folded)) {
                    newValue = folded;
                } else if (const QString mapped = RoadmapParse::mappedKind(folded);
                           !mapped.isEmpty()) {
                    newValue = mapped;
                } else {
                    if (error) {
                        *error = QStringLiteral(
                                     "the body declares `Kind: %1`, which is not one of "
                                     "the accepted kinds (implement, fix, audit-fix, "
                                     "review-fix, doc, doc-fix, refactor, test, chore, "
                                     "release, perf, security, feature, enhancement, "
                                     "investigate, research, accessibility, optimize, "
                                     "package, marketing, ux). Nothing was written. Use "
                                     "one of those, or — if the line is prose rather "
                                     "than a declaration — wrap `Kind:` in backticks or "
                                     "reword it so it does not begin the line.")
                                     .arg(newValue);
                    }
                    // ANTS-4577 — the only refusal in this function that is the
                    // caller's rather than the engine's. mcp-error-codes.md § 1
                    // already owns this condition under `bad_kind` ("a `kind`
                    // enum doesn't match the recognised set"), so the code is
                    // picked from the taxonomy rather than minted: the value is
                    // the same value, the remedy is the same remedy, and the
                    // only difference is that it arrived in the body instead of
                    // an argument. Every other `return false` below is a store
                    // write that failed, which IS `store_failed`.
                    if (code)
                        *code = QStringLiteral("bad_kind");
                    return false;
                }
                if (current == newValue)
                    continue;
            }
            if (current == newValue)
                continue;  // § 2.6 writes each one that CHANGED.
            if (!store.setItemField(itemPk, field, newValue,
                                    QStringLiteral("asserted"), error))
                return false;
            // ANTS-3822 — recorded AFTER the write succeeds, so a refused write
            // leaves no revision claiming it happened. `current` may be empty
            // where the column held nothing; normalise that to a null QString so
            // the row stores SQL NULL rather than '' (§ 2.1).
            if (hist)
                hist->record(itemPk, field,
                             current.isEmpty() ? QString() : current, newValue);
        } else {
            // The body carried it and the write removed it. ANTS-4576 — what
            // that MEANS is the column's own storage contract, and reading it
            // as "clear" on all five was a raw `NOT NULL constraint failed`
            // on four of them.
            if (k.undeclare == RlUndeclare::Keep) {
                // NOT NULL with no default: the column has no absent state, so
                // a body that stopped declaring the key cannot mean the item
                // has none. Keeping loses nothing — the render emits a trailer
                // line from the column exactly when the body does NOT declare
                // that key at a line start, so the deleted line comes back
                // canonically and the file still says what the column says.
                if (kept && !current.isEmpty())
                    kept->append(field);
                continue;
            }
            if (current.isEmpty() || current == QLatin1String("[]"))
                continue;
            if (k.undeclare == RlUndeclare::EmptyList) {
                // NOT NULL DEFAULT '[]'. The empty array is what "no lanes"
                // is stored as, so this is a real clear and not a fudge — the
                // one thing it must not be is NULL, which the schema refuses.
                const QString empty = QStringLiteral("[]");
                if (!store.setItemField(itemPk, field, empty,
                                        QStringLiteral("asserted"), error))
                    return false;
                if (hist)
                    hist->record(itemPk, field, current, empty);
                continue;
            }
            // Nullable. clearItemField(), never an empty setItemField():
            // putItem() binds an empty value as SQL NULL while setItemField()
            // binds the string it is given, and ANTS-3761's INV-2 column diff
            // reads '' and NULL as different.
            if (!store.clearItemField(itemPk, field, QStringLiteral("asserted"), error))
                return false;
            // A clear IS a change, and its new value is absence — a null
            // QString, so the row stores SQL NULL (§ 2.1). Recording it as ""
            // would claim the column now holds an empty string, which is a
            // different stored value.
            if (hist)
                hist->record(itemPk, field, current, QString());
        }
    }
    return true;
}

std::optional<QString> rcdetail::rlRedundantTrailerRunStripped(
        const RoadmapStore::ItemWrite &w, bool *conflict) {
    if (conflict)
        *conflict = false;
    QString stripped = RoadmapParse::stripTrailingTrailerLines(w.body);
    if (stripped == w.body)
        return std::nullopt;

    const RoadmapParse::TrailerValues oldTv = RoadmapParse::trailerValuesIn(w.body);
    const RoadmapParse::TrailerValues newTv = RoadmapParse::trailerValuesIn(stripped);
    for (const RlTrailerKey &k : kRlTrailerKeys) {
        QString oldValue = rlBodyValueFor(oldTv, k);
        const QString newValue = rlBodyValueFor(newTv, k);
        if (oldValue == newValue)
            continue;  // the run did not carry this key
        // A value left in the stripped prose would take over from the run on the
        // next re-parse, so the strip would change the key rather than drop a
        // copy of the column.
        bool same = newValue.isEmpty();
        // The capture arrives raw; the column holds the canonical kind. Mapped
        // exactly as rlDeriveTrailerColumns() maps it.
        if (same && QLatin1String(k.field) == QLatin1String("kind")) {
            const QString folded = oldValue.trimmed().toLower();
            if (RoadmapParse::canonicalKinds().contains(folded))
                oldValue = folded;
            else if (const QString mapped = RoadmapParse::mappedKind(folded);
                     !mapped.isEmpty())
                oldValue = mapped;
        }
        const QString current =
            k.colList ? rlJsonArrayText(w.*(k.colList)) : w.*(k.col);
        if (!same || oldValue != current) {
            if (conflict)
                *conflict = true;
            return std::nullopt;
        }
    }
    return stripped;
}

void rcdetail::rlAttachHistoryNote(QJsonObject &env, const RoadmapStore &store,
                                   const HistoryContext &hist) {
    if (hist.skippedRows <= 0)
        return;
    env[QStringLiteral("history_note")] =
        QStringLiteral("history cap reached (%1 bytes); %2 history row(s) not recorded")
            .arg(store.historyCapBytes())
            .arg(hist.skippedRows);
}

QString rcdetail::rlHistoryStamp() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

// ANTS-3822 § 2.3 — the all-or-nothing flush. Header carries the contract.
bool rcdetail::rlFlushHistory(RoadmapStore &store, HistoryContext &hist,
                              QString *error) {
    if (hist.pending.isEmpty())
        return true;

    // Asked ONCE for the whole op, which is what stops a revision landing with
    // some of its fields. Over the cap: nothing is written, the count is
    // reported, and the op still succeeds.
    if (store.historyWouldExceedCap(hist.pendingBytes())) {
        hist.skippedRows += int(hist.pending.size());
        hist.pending.clear();
        return true;
    }

    // seq is scoped per (item_pk, changed_at), so each item numbers from its own
    // stored maximum — never one counter across the op, which on a batch would
    // give the second item's rows the first item's numbering.
    QHash<qint64, int> nextSeq;
    for (const HistoryContext::Row &r : std::as_const(hist.pending)) {
        if (!nextSeq.contains(r.itemPk)) {
            QString seqErr;
            const auto stored = store.maxHistorySeq(r.itemPk, hist.changedAt, &seqErr);
            if (!seqErr.isEmpty()) {
                if (error) *error = seqErr;
                return false;
            }
            nextSeq.insert(r.itemPk, stored.value_or(-1) + 1);
        }
        int &seq = nextSeq[r.itemPk];
        if (!store.appendHistory(r.itemPk, hist.changedAt, seq, r.field,
                                 r.oldValue, r.newValue, error)) {
            // Not the cap — the batch was measured against it above and fit. So
            // this is a real failure and it aborts the op, per § 2.3's second
            // branch. Swallowing it is the silently-dropped revision INV-14
            // forbids, reported as a successful write.
            return false;
        }
        ++seq;
    }
    hist.pending.clear();
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

rcdetail::LocateOutcome rcdetail::rlLocateTarget(RoadmapStore &store, qint64 projectId,
                                                 const QString &locId,
                                                 const QString &locHeadline,
                                                 const QStringList &fileIds,
                                                 const QStringList &fileHeadlines) {
    LocateOutcome out;
    QString err;
    const quint64 need = rcFnv1a64(rcNormaliseHeadline(locHeadline));
    if (!locId.isEmpty()) {
        const auto pk = store.findItem(projectId, locId, &err);
        if (!pk && !err.isEmpty()) {
            out.code  = QStringLiteral("store_failed");
            out.error = err;
            return out;
        }
        if (pk) out.itemPk = *pk;
    } else if (!locHeadline.isEmpty()) {
        const auto refs = store.listItems(projectId, &err);
        if (!refs) {
            out.code  = QStringLiteral("store_failed");
            out.error = err;
            return out;
        }
        int hits = 0;
        for (const auto &ref : *refs) {
            if (rcFnv1a64(rcNormaliseHeadline(ref.headline)) != need)
                continue;
            ++hits;
            out.itemPk = ref.itemPk;
        }
        if (hits > 1) {
            out.itemPk = 0;
            out.code   = QStringLiteral("bullet_ambiguous");
            out.error  = QStringLiteral(
                "roadmap_log: that headline matches %1 items in this project's "
                "store — narrow with `id`").arg(hits);
            return out;
        }
    }
    if (out.itemPk) {
        const auto item = store.readItem(out.itemPk, &err);
        if (!item) {
            out.itemPk = 0;
            out.code   = QStringLiteral("store_failed");
            out.error  = err;
            return out;
        }
        out.id       = item->id;
        out.headline = item->headline;
        out.status   = item->status;
        return out;
    }
    // § 4.4 — the file is ahead of the store, which the render's divergence
    // guard would refuse anyway; say so instead of reporting absence.
    const QString locator = locId.isEmpty() ? locHeadline : locId;
    if (!locId.isEmpty()) {
        out.inFileOnly = fileIds.contains(locId);
    } else {
        for (const QString &h : fileHeadlines)
            if (rcFnv1a64(rcNormaliseHeadline(h)) == need) out.inFileOnly = true;
    }
    out.code  = QStringLiteral("bullet_not_found");
    out.error = out.inFileOnly
        ? QStringLiteral("roadmap_log: \"%1\" is in ROADMAP.md but not in this "
                         "project's store. The store is the source of truth for "
                         "this project, so the write would drop it. Run "
                         "roadmap_migrate to import it, then retry.").arg(locator)
        : QStringLiteral("roadmap_log: no item in this project's roadmap store "
                         "matches \"%1\"").arg(locator);
    return out;
}

// § 2.3 — the high-water the store path allocates from. Two store columns,
// no text scan: the next id is the highest this project has, plus one.
//
// ANTS-4631 — this used to floor to RoadmapFoldIn::corpusHighWater(), a
// `\bPFX-NNNN\b` scan of ROADMAP.md, CHANGELOG.md and every archive. That
// floor was written when the markdown file WAS the record (ANTS-3450), and it
// was never retired when the store became one: a migrated project's file is a
// rendered OUTPUT of these very rows, so the scan re-read the store's own
// answer through prose that cannot distinguish an allocated id from a
// documented example. Measured 2026-08-24: one deliberately-absurd `ANTS-9999`
// inside a body was read as the high water and the next append issued 10000,
// burning ~5,370 ids. A column cannot make that mistake.
//
// Both columns, because neither alone is complete. idHighWater() is the
// allocator's own counter and remembers an id whose item was later deleted —
// but migration never writes that row, only an id-allocating append does, so a
// migrated-but-never-appended project reports nullopt while holding thousands
// of ids. maxAllocatedId() reads those ids straight off the items. nullopt from
// either is NOT an error; it is the ordinary state of a project that has not
// reached that half yet.
//
// The un-migrated markdown path still floors to the corpus, and must: with no
// project row there is no column to ask.
//
// ANTS-5087 — the two-column rule itself now lives on the store, because
// RoadmapFoldIn::allocateIds needs the same floor and cannot see this file.
// This stays as the name its callers already use.
qint64 rcdetail::rlStoreIdHighWater(RoadmapStore &store, qint64 projectId,
                                 const QString &prefix) {
    QString ignored;
    return store.allocationFloor(projectId, prefix, &ignored);
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
                                    RoadmapSource::RoadmapText &text,
                                    const QString &callerCanonical,
                                    bool *guessed) {
    if (guessed) *guessed = false;
    if (!idPrefixArg.isEmpty())
        return idPrefixArg;
    // ANTS-3771 § 2.3 — new step 2, ABOVE the store row. The store row records
    // what THIS STORE has allocated; the declaration records what the PROJECT
    // has decided, and where they disagree the project is right and the row is
    // a stale artefact of a migration. An explicit id_prefix argument still
    // wins (INV-7) — it is documented to override everything.
    const QString declared =
        ProjectSettings::idFormatFor(callerCanonical).prefix;
    if (!declared.isEmpty())
        return declared;
    QString ignored;
    if (const auto pfx = store.idPrefixFor(projectId, &ignored)) {
        if (!pfx->isEmpty())
            return *pfx;
    }
    // ANTS-3863 — the ONE full() below the seam, and it is deliberately here:
    // sniffing the prefix out of the corpus needs every byte, and a migrated
    // project normally has a stored prefix and never reaches this line.
    return rlResolveCounterPrefix(idPrefixArg, text.full(), callerCanonical,
                                  guessed);
}

// ANTS-3771 — see remotecontrol_internal.h for what each of these three owes.
QJsonDocument rcdetail::rlWrappedBlockErr() {
    QJsonObject o;
    o[QStringLiteral("ok")]    = false;
    o[QStringLiteral("code")]  = QStringLiteral("body_match_wrapped_block");
    o[QStringLiteral("error")] = QStringLiteral(
        "roadmap_log: `old_text` spans a hard-wrapped break whose lines are "
        "indented differently \u2014 an aligned or nested block, which "
        "re-flowing would collapse. Amend one line of it, or rewrite the "
        "block so it does not depend on column alignment");
    o[QStringLiteral("hint")]  = QStringLiteral(
        "amend one line of the block, or rewrite it so it does not depend on "
        "column alignment");
    return QJsonDocument(o);
}

RoadmapParse::IdFormat rcdetail::rlDecl(const QString &root) {
    return ProjectSettings::idFormatFor(root);
}

QVector<RoadmapParse::BulletRecord> rcdetail::rlParse(const QString &markdown,
                                                      const QString &root) {
    return RoadmapParse::parseBullets(markdown, rlDecl(root));
}

// § 2.3 — see the declaration for the two tests, the two codes, and why this is
// gated on the project having declared.
QString rcdetail::rlDeclaredIdRefusal(const QString &writtenId,
                                      const RoadmapParse::IdFormat &fmt,
                                      QString *code) {
    const auto refuse = [&](const QString &c, const QString &msg) {
        if (code) *code = c;
        return msg;
    };
    if (code) code->clear();
    if (!fmt.isDeclared() || writtenId.isEmpty())
        return QString();

    // Row 1. The universal grammar, spelled from RoadmapParse::idTokenPattern()
    // rather than restated, OR the declared pattern matching the WHOLE token.
    // The declared arm never narrows: it can only accept more.
    static const QRegularExpression kUniversal(
        QStringLiteral("\\A(?:") + RoadmapParse::idTokenPattern() +
        QStringLiteral(")\\z"));
    bool accepted = kUniversal.match(writtenId).hasMatch();
    if (!accepted && !fmt.pattern.isEmpty()) {
        const QRegularExpression declared(
            QLatin1String(RoadmapParse::kIdFormatMatchLimit) +
            QStringLiteral("\\A(?:") + fmt.pattern + QStringLiteral(")\\z"));
        accepted = declared.isValid() && declared.match(writtenId).hasMatch();
    }
    if (!accepted) {
        return refuse(QStringLiteral("bad_id_format"),
            QStringLiteral("id \"%1\" is accepted neither by "
                           "roadmap-format.md § 3.5.1's grammar (a "
                           "letter-bearing prefix, then `-`, then digits) nor "
                           "by this project's declared `id_format.pattern` — "
                           "see .ants/project.json").arg(writtenId));
    }

    // Row 2. The prefix, when one is declared. `pattern` takes no part here:
    // it is authored against a bold lead-in, and running it over a bracket id
    // would reject a conforming ANTS-0042 (§ 2.3).
    if (!fmt.prefix.isEmpty()) {
        const int cut = writtenId.lastIndexOf(QLatin1Char('-'));
        const QString pfx = cut > 0 ? writtenId.left(cut) : writtenId;
        if (pfx != fmt.prefix) {
            return refuse(QStringLiteral("id_format_mismatch"),
                QStringLiteral("id \"%1\" has the prefix \"%2\", but this "
                               "project declares `id_format.prefix` = \"%3\" "
                               "in .ants/project.json").arg(writtenId, pfx, fmt.prefix));
        }
    }
    return QString();
}

// ANTS-4549 — caller prose may declare a trailer key only in the shape the
// RENDERER writes one: the label first on its line. Anywhere else the key sits
// in running prose, and reading it as metadata is the defect. Returns true when
// the op must refuse, filling *error with the message.
//
// ANTS-4576 — the text is a `note` (annotate / flip / flip_batch) or an
// amend_body `new_text`, and `argName` says which. Both are prose the caller
// wrote about the item, both land in the body, and § 2.6 re-parses what lands:
// one rule, named in the message for the argument it fired on.
//
// The note is appended to the bullet's body, and § 2.6 then re-derives every
// trailer column from that new body — so a bare `Kind:` mid-sentence is read as
// a declaration and the words after it are written to the column. Reported
// against AI_Prompts/AIPR-0033, where it produced an invalid kind and the
// store's CHECK constraint refused the whole write. That refusal was luck: a
// following word inside the 21 accepted kinds would have SUCCEEDED and silently
// re-kinded the item, and the other four keys have no CHECK at all.
//
// Why POSITION and not the value comparison rlBodyShadows() uses: a note has no
// column argument to compare against, and blanket-refusing every declaration
// would break § 2.6's own reason for existing — an annotate whose note carries
// a `Layman:` line is the only way to fill that column on a migrated item, and
// the render gates on it. The two shapes are distinguishable without asking the
// caller: a deliberate declaration is written as its own line, exactly as the
// render emits it, and prose that happens to name a key is not. Leading
// whitespace is allowed, so a caller indenting their line is not caught out.
//
// Two of the five patterns (`Layman:`, `Evidence:`) are anchored already and
// cannot match mid-line at all, so this never fires on them — that is the
// parser's rule, kept rather than second-guessed: a guard stricter than the
// parser refuses notes that were never at risk.
//
// ANTS-4504's code-span masking is what makes the remedy cheap: a key inside
// backticks declares nothing, so wrapping it is a one-character fix.
// ANTS-4532 — see remotecontrol_internal.h for why this is single-line only
// and why it must run after the trailer guard.
QString rcdetail::rlWrapNote(const QString &note) {
    if (note.contains(QLatin1Char('\n'))) return note;
    // ~70 columns, which is what the corpus around it uses.
    constexpr int kCols = 70;
    const QStringList words = note.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList lines;
    QString cur;
    for (const QString &w : words) {
        if (!cur.isEmpty() && cur.size() + 1 + w.size() > kCols) {
            lines << cur;
            cur.clear();
        }
        if (!cur.isEmpty()) cur += QLatin1Char(' ');
        cur += w;   // a word longer than the column budget takes its own line
    }
    if (!cur.isEmpty()) lines << cur;
    return lines.join(QLatin1Char('\n'));
}

bool rcdetail::rlNoteDeclaresTrailer(const QString &note, QString *error,
                                     const char *argName) {
    if (note.isEmpty())
        return false;
    for (const RlTrailerKey &k : kRlTrailerKeys) {
        const RoadmapParse::TrailerValues tv = RoadmapParse::trailerValuesIn(note);
        const QString fromNote = rlBodyValueFor(tv, k);
        if (fromNote.isEmpty())
            continue;
        const RoadmapParse::TrailerMatch &m = tv.*(k.match);
        if (m.offset < 0 || m.offset > note.size())
            continue;
        // Everything on the line before the VALUE must be the label itself.
        // Computed off the capture rather than off TrailerMatch::anchored,
        // which is false for an indented un-anchored key (`  Kind: fix.`) —
        // the shape a caller writing a declaration by hand actually produces.
        const QString field = QString::fromLatin1(k.field);
        const QString label = field.left(1).toUpper() + field.mid(1);
        const int lineStart = note.lastIndexOf(QLatin1Char('\n'), m.offset) + 1;
        const QString before = note.mid(lineStart, m.offset - lineStart);
        const QRegularExpression labelOnly(
            QStringLiteral("^\\s*(?:\\*\\*)?%1:(?:\\*\\*)?\\s*$").arg(label),
            QRegularExpression::CaseInsensitiveOption);
        if (labelOnly.match(before).hasMatch())
            continue;   // a declaration in the render's own shape — allowed
        if (!error)
            return true;
        QString quoted = note.mid(lineStart,
                                  (note.indexOf(QLatin1Char('\n'), m.offset) < 0
                                       ? note.size()
                                       : note.indexOf(QLatin1Char('\n'), m.offset))
                                      - lineStart).trimmed();
        if (quoted.size() > 160)
            quoted.truncate(160);
        *error = QStringLiteral(
                     "`%5` names the `%1` trailer key mid-line, so a re-parse "
                     "of the resulting body would read \"%2\" as that column's "
                     "value. `%5` is prose: wrap the key in backticks (`%3:`), "
                     "which declares nothing, or reword the mention. To DECLARE "
                     "the key deliberately, put `%3:` first on its own line, as "
                     "the render writes it. (offending text: %4)")
                     .arg(field, fromNote, label, quoted,
                          QString::fromLatin1(argName));
        return true;
    }
    return false;
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
void rcdetail::rlAddWarning(QJsonObject &env, const QJsonObject &warn) {
    QJsonArray arr = env.value(QStringLiteral("warnings")).toArray();
    arr.append(warn);
    env[QStringLiteral("warnings")] = arr;
}

QJsonObject rcdetail::rlEvidenceAdvisory(const QStringList &notPathShaped) {
    if (notPathShaped.isEmpty()) return {};
    QJsonArray vals;
    for (const QString &e : notPathShaped) vals.append(e);
    QJsonObject warn;
    warn[QStringLiteral("code")] = QStringLiteral("evidence_not_path_shaped");
    warn[QStringLiteral("message")] = QStringLiteral(
        "roadmap-format 3.5 defines every Evidence: element as a path, and the "
        "field is split on commas — so prose written here is stored as several "
        "fragments rather than as the sentence you wrote. Move the explanation "
        "into the body and leave Evidence: for paths.");
    warn[QStringLiteral("elements")] = vals;
    return warn;
}

QJsonObject rcdetail::rlReviewKindAdvisory(const QString &kind,
                                           const QString &source) {
    // ANTS-4989 — the DECISION is RoadmapParse's (it is a roadmap-convention
    // rule, and lives where tests can reach it); this builds the envelope.
    const RoadmapParse::ReviewKindMismatch m =
        RoadmapParse::reviewKindMismatch(kind, source);
    if (m.matchedPrefix.isEmpty()) return {};

    QJsonObject warn;
    warn[QStringLiteral("code")] = QStringLiteral("kind_ignores_review_provenance");
    warn[QStringLiteral("message")] = QStringLiteral(
        "roadmap-format.md v1.2 3.5.3: a fix arising from a review takes "
        "`review-fix` (or `audit-fix`), not a plain `fix` — those kinds carry a "
        "different follow-through, citing the finding source. This item's "
        "Source: names a review origin. Written, not refused: the kind may be "
        "deliberate, and provenance is recorded either way.");
    warn[QStringLiteral("source_prefix")]  = m.matchedPrefix;
    warn[QStringLiteral("suggested_kind")] = m.suggestedKind;
    return warn;
}

QJsonObject rcdetail::rlEvidenceAdvisoryForReq(const QJsonObject &bulletReq) {
    QStringList bad;
    const QJsonArray a = bulletReq.value(QStringLiteral("evidence")).toArray();
    for (const auto &v : a) {
        // Sanitised as the render sanitises it, so the advisory judges the
        // element that will actually be stored — the comma fold in particular
        // turns one prose sentence into one ugly element rather than several.
        QString p = rcSanitizeBulletField(v.toString(), 500);
        p.replace(QChar('\n'), QChar(' '));
        p.replace(QChar(','), QChar(' '));
        p = p.trimmed();
        if (!p.isEmpty() && !rlEvidenceLooksLikePath(p)) bad << p;
    }
    return rlEvidenceAdvisory(bad);
}

bool rcdetail::rlEvidenceLooksLikePath(const QString &element) {
    if (element.contains(QLatin1Char('/')) || element.contains(QLatin1Char('\\')))
        return true;
    // A filename-style extension: a dot followed by a short alphanumeric run at
    // the very end. Keeps `shot.png` at the repo root, which carries no
    // separator and is legitimate evidence.
    static const QRegularExpression ext(QStringLiteral("\\.[A-Za-z0-9]{1,8}$"));
    return ext.match(element).hasMatch();
}

bool rcdetail::rlFillItemBody(const QJsonObject &bulletReq,
                           RoadmapStore::ItemWrite &w,
                           QStringList &scrubbedNames, QString *error,
                           int *unnamedRemovals,
                           QStringList *evidenceNotPathShaped,
                           QStringList *removedFragments) {
    // Scrubbed exactly as formatRoadmapBullet() scrubs it on the markdown path.
    QString body = bulletReq.value(QStringLiteral("body")).toString();
    rcScrubLeakedToolXml(body, scrubbedNames, unnamedRemovals,
                         removedFragments);   // ANTS-4938
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
            // ANTS-4955 — the column holds a Layman value without its stop.
            if (field == QLatin1String("layman"))
                scalar = RoadmapRender::laymanForStore(scalar);
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
        // ANTS-4527 — report, never refuse. Checked on the FINAL list, so it
        // covers both the `evidence` argument and the fallback to a body's own
        // `Evidence:` line.
        if (evidenceNotPathShaped && field == QLatin1String("evidence"))
            for (const QString &e : list)
                if (!rlEvidenceLooksLikePath(e))
                    evidenceNotPathShaped->append(e);
        if (!(k.list ? list.isEmpty() : scalar.isEmpty()))
            provenance.insert(field, QStringLiteral("asserted"));
    }
    if (!body.isEmpty())
        provenance.insert(QStringLiteral("body"), QStringLiteral("asserted"));
    w.provenance = provenance;
    return true;
}
