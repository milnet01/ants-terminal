// ANTS-3833 TU 4/15 — Changelog verbs.
#include "remotecontrol.h"
#include "remotecontrol_internal.h"
#include "paginationengine.h"
#include "roadmapfoldin.h"
#include "changeloglog.h"
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

using namespace rcdetail;  // ANTS-3833

// ANTS-1548 — changelog_log. Renders one Keep-a-Changelog bullet and
// splices it under the right category in `## [Unreleased]`. m_main-
// independent (caller_cwd + filesystem only), same as the flip path.
// ANTS-3533 — changelog_query: read-only structured CHANGELOG.md reader,
// the symmetric read side of changelog_log. Mirrors cmdRoadmapQuery's
// arg/mode/refusal discipline. See docs/specs/ANTS-3533.md.
QJsonDocument RemoteControl::cmdChangelogQuery(const QJsonObject &req) {
    auto err = [](const QString &code, const QString &message) {
        QJsonObject env;
        env[QStringLiteral("ok")]    = false;
        env[QStringLiteral("code")]  = code;
        env[QStringLiteral("error")] = message;
        return QJsonDocument(env);
    };

    const QString callerRaw = req.value(QStringLiteral("caller_cwd")).toString();
    if (callerRaw.isEmpty())
        return err(QStringLiteral("caller_cwd_required"),
                   QStringLiteral("changelog_query: caller_cwd is required"));
    const QString callerCanonical = QFileInfo(callerRaw).canonicalFilePath();
    if (callerCanonical.isEmpty())
        return err(QStringLiteral("no_changelog"),
                   QStringLiteral("changelog_query: caller_cwd \"%1\" does not "
                                  "canonicalise to an existing directory").arg(callerRaw));

    // (1) path resolution — no_changelog / format_mismatch (§ 2.3 precedence).
    const QString clPath = findChangelogUnder(callerCanonical);
    if (clPath.isEmpty()) {
        if (!findYamlChangelogUnder(callerCanonical).isEmpty())
            return err(QStringLiteral("format_mismatch"),
                       QStringLiteral("changelog_query: found a YAML changelog; "
                                      "only Keep-a-Changelog Markdown (CHANGELOG.md) "
                                      "is read"));
        return err(QStringLiteral("no_changelog"),
                   QStringLiteral("changelog_query: no CHANGELOG.md found under \"%1\"")
                       .arg(callerCanonical));
    }

    // (2) bad_mode.
    const QString mode =
        req.value(QStringLiteral("mode")).toString(QStringLiteral("entries"));
    static const QStringList kModes = { QStringLiteral("entries"),
                                        QStringLiteral("version_index"),
                                        QStringLiteral("headline_only") };
    if (!kModes.contains(mode)) {
        QJsonObject env;
        env[QStringLiteral("ok")]       = false;
        env[QStringLiteral("code")]     = QStringLiteral("bad_mode");
        env[QStringLiteral("error")]    =
            QStringLiteral("changelog_query: unknown mode \"%1\"").arg(mode.left(64));
        env[QStringLiteral("accepted")] = QJsonArray::fromStringList(kModes);
        return QJsonDocument(env);
    }

    // (4) id / ids parse — id+ids → bad_args; >100 → bad_args (§ 2.3).
    const QString singleId = req.value(QStringLiteral("id")).toString().trimmed();
    const bool hasId = !singleId.isEmpty();
    QStringList reqIds;
    bool hasIds = false;
    {
        const QJsonValue idsv = req.value(QStringLiteral("ids"));
        if (idsv.isArray()) {
            const QJsonArray a = idsv.toArray();
            hasIds = !a.isEmpty();
            for (const QJsonValue &v : a) {
                const QString s = v.toString().trimmed();
                if (!s.isEmpty() && !reqIds.contains(s)) reqIds << s;
            }
        } else if (idsv.isString() && !idsv.toString().trimmed().isEmpty()) {
            hasIds = true;
            const auto parts = idsv.toString().split(
                QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts);
            for (const QString &s : parts)
                if (!reqIds.contains(s)) reqIds << s;
        }
    }
    if (hasId && hasIds)
        return err(QStringLiteral("bad_args"),
                   QStringLiteral("changelog_query: pass either id or ids, not both"));
    if (reqIds.size() > 100)
        return err(QStringLiteral("bad_args"),
                   QStringLiteral("changelog_query: ids exceeds 100 entries"));
    const bool idMode = hasId || hasIds;

    // (5) bad_mode_combo — id/ids + version_index.
    if (idMode && mode == QLatin1String("version_index"))
        return err(QStringLiteral("bad_mode_combo"),
                   QStringLiteral("changelog_query: id/ids cannot combine with "
                                  "mode:version_index"));

    // --- parse (mtime+TTL cache, single-slot, path-keyed) ---
    const QFileInfo fi(clPath);
    const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool fresh = (m_changelogCachePath == clPath) &&
                       (m_changelogCacheMtimeMs == mtime) && mtime != 0 &&
                       (nowMs - m_changelogCacheStampMs <= kRoadmapCacheTtlMs);
    if (!fresh) {
        QString md;
        QFile f(clPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text))
            md = QString::fromUtf8(f.readAll());
        // Prefix P = project roadmap prefix (ANTS-3533 § 3). Sniff from
        // the ROADMAP.md at the repo root; fall back to "ANTS".
        const QString roadmapPath = findRoadmapUnder(callerCanonical);
        const QString roadmapDir = roadmapPath.isEmpty()
            ? callerCanonical
            : QFileInfo(roadmapPath).absolutePath();
        const QString prefix =
            RoadmapFoldIn::sniffIdPrefix(roadmapDir, QStringLiteral("ANTS"));
        m_changelogCache       = ChangelogQuery::parse(md, prefix);
        m_changelogCachePrefix = prefix;
        m_changelogCachePath   = clPath;
        m_changelogCacheMtimeMs = mtime;
        m_changelogCacheStampMs = nowMs;
    }
    const ChangelogQuery::ParseResult &pr = m_changelogCache;

    // (7) bad_version — version filter matching no block (normalise Unreleased).
    // ANTS-3618 — `section` is accepted as an alias for `version`. Callers
    // arriving from roadmap_query reach for its vocabulary and pass
    // `section:"Unreleased"`; that landed in ignored_args[] and the verb
    // cheerfully returned every entry, which reads as "the filter found
    // everything" rather than "the filter was never applied". A CHANGELOG's
    // `## [X.Y.Z]` blocks ARE its sections, so the alias is exact — and a
    // value that is not a version still self-corrects, because the
    // bad_version refusal below lists the versions that do exist.
    QString versionFilter = req.value(QStringLiteral("version")).toString();
    const QString sectionAlias = req.value(QStringLiteral("section")).toString();
    if (!sectionAlias.isEmpty()) {
        if (!versionFilter.isEmpty() && versionFilter != sectionAlias) {
            QJsonObject env;
            env[QStringLiteral("ok")]    = false;
            env[QStringLiteral("code")]  = QStringLiteral("bad_args");
            env[QStringLiteral("error")] = QStringLiteral(
                "changelog_query: `section` is an alias for `version`; pass "
                "one or the other, not two different values (version=\"%1\", "
                "section=\"%2\")").arg(versionFilter.left(64),
                                       sectionAlias.left(64));
            return QJsonDocument(env);
        }
        versionFilter = sectionAlias;
    }
    const bool hasVersionFilter = !versionFilter.isEmpty();
    if (hasVersionFilter &&
        versionFilter.compare(QStringLiteral("Unreleased"), Qt::CaseInsensitive) == 0)
        versionFilter = QStringLiteral("Unreleased");
    if (hasVersionFilter) {
        bool exists = false;
        QStringList tokens;
        for (const ChangelogQuery::VersionInfo &vi : pr.versions) {
            tokens << vi.version;
            if (vi.version == versionFilter) exists = true;
        }
        if (!exists) {
            QJsonObject env;
            env[QStringLiteral("ok")]       = false;
            env[QStringLiteral("code")]     = QStringLiteral("bad_version");
            env[QStringLiteral("error")]    =
                QStringLiteral("changelog_query: no version block \"%1\"")
                    .arg(versionFilter.left(64));
            env[QStringLiteral("versions")] = QJsonArray::fromStringList(tokens);
            return QJsonDocument(env);
        }
    }

    // (8) bad_category — validate in every mode (validate-then-ignore, § 2.4).
    const QString category = req.value(QStringLiteral("category")).toString();
    if (!category.isEmpty() && !ChangelogLog::isValidCategory(category)) {
        QJsonObject env;
        env[QStringLiteral("ok")]       = false;
        env[QStringLiteral("code")]     = QStringLiteral("bad_category");
        env[QStringLiteral("error")]    =
            QStringLiteral("changelog_query: unknown category \"%1\"")
                .arg(category.left(64));
        env[QStringLiteral("accepted")] =
            QJsonArray::fromStringList(ChangelogLog::canonicalCategories());
        return QJsonDocument(env);
    }

    // (9) offset / limit shape (bad_args).
    int offset = 0;
    bool passedOffset = false;
    if (req.contains(QStringLiteral("offset"))) {
        const QJsonValue v = req.value(QStringLiteral("offset"));
        const double d = v.toDouble(-1);
        if (!v.isDouble() || d < 0 || d != std::floor(d))
            return err(QStringLiteral("bad_args"),
                       QStringLiteral("changelog_query: offset must be a "
                                      "non-negative integer"));
        offset = static_cast<int>(d);
        passedOffset = true;
    }
    int limitArg = -1;
    bool passedLimit = false;
    if (req.contains(QStringLiteral("limit"))) {
        const QJsonValue v = req.value(QStringLiteral("limit"));
        const double d = v.toDouble(0);
        if (!v.isDouble() || d < 1 || d != std::floor(d))
            return err(QStringLiteral("bad_args"),
                       QStringLiteral("changelog_query: limit must be an "
                                      "integer >= 1"));
        limitArg = static_cast<int>(d);
        passedLimit = true;
    }
    Q_UNUSED(passedOffset);

    const bool headlineOnly = (mode == QLatin1String("headline_only"));
    const bool includeBody  = req.value(QStringLiteral("include_body")).toBool(false);

    auto entryToJson = [&](const ChangelogQuery::Entry &e) {
        QJsonObject o;
        o[QStringLiteral("version")]  = e.version;
        o[QStringLiteral("category")] = e.category;
        o[QStringLiteral("ids")]      = QJsonArray::fromStringList(e.ids);
        if (headlineOnly) {
            o[QStringLiteral("text_oneline")] = e.text.simplified();
        } else {
            o[QStringLiteral("date")]       = e.date;
            o[QStringLiteral("unreleased")] = e.unreleased;
            o[QStringLiteral("text")]       = e.text;
            if (includeBody && !e.body.isEmpty())
                o[QStringLiteral("body")] = e.body;
        }
        return o;
    };

    QJsonObject out;
    out[QStringLiteral("ok")]   = true;
    out[QStringLiteral("path")] = clPath;
    out[QStringLiteral("mode")] = mode;

    if (mode == QLatin1String("version_index")) {
        QJsonArray versions;
        for (const ChangelogQuery::VersionInfo &vi : pr.versions) {
            if (hasVersionFilter && vi.version != versionFilter) continue;
            QJsonObject o;
            o[QStringLiteral("version")]     = vi.version;
            o[QStringLiteral("date")]        = vi.date;
            o[QStringLiteral("unreleased")]  = vi.unreleased;
            o[QStringLiteral("entry_count")] = vi.entry_count;
            QJsonObject cats;
            for (const auto &p : vi.categories) cats[p.first] = p.second;
            o[QStringLiteral("categories")] = cats;
            versions << o;
        }
        const PaginationEngine::PageResult page =
            PaginationEngine::pageBullets(versions, offset,
                                          passedLimit ? limitArg : -1);
        out[QStringLiteral("versions")] = page.slice;
        out[QStringLiteral("count")]    = page.slice.size();
        out[QStringLiteral("total")]    = page.total;
        out[QStringLiteral("offset")]   = page.offset;
        if (page.truncated) {
            out[QStringLiteral("truncated")]   = true;
            out[QStringLiteral("next_offset")] = page.nextOffset;
        }
        if (hasVersionFilter) out[QStringLiteral("version")] = versionFilter;
        return QJsonDocument(out);
    }

    // entries / headline_only.
    QString needle = req.value(QStringLiteral("query")).toString();
    if (needle.size() > 200) needle.truncate(200);
    needle = needle.toLower();

    QJsonArray arr;
    QStringList matchedIds, missingIds;
    if (idMode) {
        const QStringList requested = hasId ? QStringList{ singleId } : reqIds;
        QSet<QString> realIds;
        QHash<QString, QString> ciToReal;
        for (const ChangelogQuery::Entry &e : pr.entries)
            for (const QString &id : e.ids) {
                realIds.insert(id);
                ciToReal.insert(id.toLower(), id);
            }
        // bad_case — SINGULAR id only, case-only mismatch (§ 2.3).
        if (hasId && !realIds.contains(singleId)) {
            const auto it = ciToReal.constFind(singleId.toLower());
            if (it != ciToReal.constEnd()) {
                QJsonObject env;
                env[QStringLiteral("ok")]           = false;
                env[QStringLiteral("code")]         = QStringLiteral("bad_case");
                env[QStringLiteral("error")]        =
                    QStringLiteral("changelog_query: id \"%1\" differs only by "
                                   "case from \"%2\"").arg(singleId, it.value());
                env[QStringLiteral("canonical_id")] = it.value();
                return QJsonDocument(env);
            }
        }
        QSet<QString> reqExact(requested.begin(), requested.end());
        for (const QString &r : requested)
            (realIds.contains(r) ? matchedIds : missingIds) << r;
        for (const ChangelogQuery::Entry &e : pr.entries) {
            bool hit = false;
            for (const QString &id : e.ids)
                if (reqExact.contains(id)) { hit = true; break; }
            if (hit) arr << entryToJson(e);
        }
    } else {
        for (const ChangelogQuery::Entry &e : pr.entries) {
            if (hasVersionFilter && e.version != versionFilter) continue;
            if (!category.isEmpty() && e.category != category) continue;
            if (!needle.isEmpty() &&
                !e.text.toLower().contains(needle) &&
                !e.body.toLower().contains(needle))
                continue;
            arr << entryToJson(e);
        }
    }

    // ANTS-3576 — auto-downshift the fat entries[] to the lean headline_only
    // shape when it would truncate (mirrors ANTS-3543 for roadmap_query), so a
    // scanning caller keeps every entry's version/ids instead of losing the
    // tail. Gated off when the caller already asked for lean rows
    // (headline_only) or explicitly wants bodies (include_body). The
    // version_index site above has no lean form and stays 3-arg.
    const bool wantDownshift = !headlineOnly && !includeBody;
    const PaginationEngine::PageResult page =
        PaginationEngine::pageBullets(
            arr, offset, passedLimit ? limitArg : -1,
            wantDownshift
                ? PaginationEngine::RowProjector(&rcProjectChangelogHeadlineOnly)
                : PaginationEngine::RowProjector{});
    out[QStringLiteral("entries")] = page.slice;
    out[QStringLiteral("count")]   = page.slice.size();
    out[QStringLiteral("total")]   = page.total;
    out[QStringLiteral("offset")]  = page.offset;
    if (page.truncated) {
        out[QStringLiteral("truncated")]   = true;
        out[QStringLiteral("next_offset")] = page.nextOffset;
    }
    // ANTS-3576 — emit only when a downshift fired (truthy-only, like
    // truncated). Rows are then headline-shaped and the tail is complete.
    if (page.downshifted) out[QStringLiteral("downshifted")] = true;
    if (hasVersionFilter)   out[QStringLiteral("version")]  = versionFilter;
    if (!category.isEmpty()) out[QStringLiteral("category")] = category;
    if (idMode) {
        if (hasId) out[QStringLiteral("id")]  = singleId;
        else       out[QStringLiteral("ids")] = QJsonArray::fromStringList(reqIds);
        out[QStringLiteral("matched_ids")] = QJsonArray::fromStringList(matchedIds);
        out[QStringLiteral("missing_ids")] = QJsonArray::fromStringList(missingIds);
        out[QStringLiteral("found")]       = (page.total > 0);
    }
    return QJsonDocument(out);
}

QJsonDocument RemoteControl::cmdChangelogLog(const QJsonObject &req) {
    auto clErr = [](const QString &code, const QString &message) {
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = code;
        env["error"] = message;
        return QJsonDocument(env);
    };

    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    if (callerRaw.isEmpty()) {
        return clErr(QStringLiteral("missing_field"),
            QStringLiteral("changelog_log: caller_cwd is required"));
    }
    QString op =
        req.value(QStringLiteral("op")).toString(QStringLiteral("add"));
    // ANTS-4475 — accept the sibling verb's spelling. roadmap_log calls this
    // act `append` and this verb calls it `add`, and each already accepts a
    // batch variant under the OTHER convention's stem (append_batch /
    // add_batch) — which is exactly what makes the cross-spelling feel
    // natural. A LocalWebServerManager session wrote both calls in one message
    // and was refused on the spelling alone. Aliasing on the ANTS-3698
    // precedent (roadmap_query accepts `filter` for `status`); the refusal
    // below keeps naming the canonical form, so nothing learns the alias as
    // the primary name.
    if (op == QStringLiteral("append"))       op = QStringLiteral("add");
    else if (op == QStringLiteral("append_batch"))
                                              op = QStringLiteral("add_batch");
    // ANTS-2136 — dry_run preview: compute the would-be insert without
    // touching CHANGELOG.md (parity with roadmap_log dry_run).
    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();
    // ANTS-2044 — batch path: N entries, one read + one atomic commit.
    if (op == QStringLiteral("add_batch")) {
        return cmdChangelogLogAddBatch(req);
    }
    if (op != QStringLiteral("add") &&
        op != QStringLiteral("add_from_roadmap") &&
        op != QStringLiteral("add_subsection") &&
        op != QStringLiteral("release") &&
        op != QStringLiteral("normalize")) {
        return clErr(QStringLiteral("bad_op_combo"),
            QStringLiteral("changelog_log: unknown op \"%1\" — expected "
                           "\"add\" (default), \"add_from_roadmap\", "
                           "\"add_batch\", \"add_subsection\", "
                           "\"release\", or \"normalize\"")
                .arg(op));
    }

    const QString callerCanonical =
        QFileInfo(callerRaw).canonicalFilePath();
    if (callerCanonical.isEmpty()) {
        return clErr(QStringLiteral("no_changelog"),
            QStringLiteral("changelog_log: caller_cwd \"%1\" does not "
                           "canonicalise to an existing directory")
                .arg(callerRaw));
    }
    const QString clPath = findChangelogUnder(callerCanonical);
    if (clPath.isEmpty()) {
        // ANTS-2040 — distinguish "no changelog at all" from "found a
        // YAML changelog the Keep-a-Changelog writer can't append to".
        // project_layout discovers data/changelog.yaml &c (ANTS-1574),
        // so a bare no_changelog would mislead a caller whose reader
        // already saw the file. Refuse with format_mismatch + an Edit
        // fallback so they don't chase a phantom-missing changelog.
        const QString yamlPath = findYamlChangelogUnder(callerCanonical);
        if (!yamlPath.isEmpty()) {
            QJsonObject env;
            env["ok"]     = false;
            env["code"]   = QStringLiteral("format_mismatch");
            env["error"]  = QStringLiteral(
                "changelog_log: \"%1\" is a YAML changelog; the writer "
                "only appends to Keep-a-Changelog Markdown (CHANGELOG.md) "
                "and can't emit this format yet")
                    .arg(yamlPath);
            env["path"]   = yamlPath;
            env["format"] = QStringLiteral("yaml");
            env["hint"]   = QStringLiteral(
                "Edit the YAML changelog directly — append a "
                "`- version:/date:/tags:/body:` block in its schema. "
                "YAML-changelog writes aren't supported by changelog_log "
                "yet (ANTS-2040).");
            return QJsonDocument(env);
        }
        return clErr(QStringLiteral("no_changelog"),
            QStringLiteral("changelog_log: no CHANGELOG.md under \"%1\"")
                .arg(callerCanonical));
    }

    // ANTS-3495 — op:"normalize": reorder the `### <category>` blocks in
    // `## [Unreleased]` into canonical Keep-a-Changelog order. No summary/
    // body/id/category resolution — it rewrites layout, not content.
    // ANTS-3381 completes it: the other half of the ANTS-2125 advisory,
    // relocating stray interleaved prose, now runs in the same atomic
    // write and is previewed line by line in `moves[]`.
    if (op == QStringLiteral("normalize")) {
        QFile cf(clPath);
        if (!cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return clErr(QStringLiteral("changelog_read_failed"),
                QStringLiteral("changelog_log: could not read \"%1\"")
                    .arg(clPath));
        }
        const QString clMarkdown = QString::fromUtf8(cf.readAll());
        cf.close();

        const auto res = ChangelogLog::normalizeUnreleased(clMarkdown);
        if (!res.ok) {
            return clErr(res.code, res.error);
        }

        auto orderArr = [](const QStringList &v) {
            QJsonArray a;
            for (const QString &s : v) a.append(s);
            return a;
        };
        // ANTS-3381 — normalize now folds stray prose into the bullet above
        // it, so anything the scanner still sees is prose a fold could not
        // reach: a heading sits between it and the nearest bullet, and
        // indenting it would not put it under that bullet at all. Say which
        // case this is rather than repeating the pre-3381 "separate
        // follow-up" line, which no longer describes anything pending.
        auto proseAdvisory = [](int line) {
            return QStringLiteral(
                "changelog_log: `## [Unreleased]` still interleaves "
                "non-heading prose between `### ` blocks (first at line %1). "
                "op:normalize folds stray prose into the bullet above it, but "
                "this line is separated from that bullet by a heading, so "
                "there is no bullet to fold it into — tidy it by hand or "
                "leave in place.").arg(line);
        };
        // ANTS-3381 — the fold preview. Unconditional (dry_run and write
        // alike): the failure mode the accepted policy tolerates is a
        // stand-alone paragraph being absorbed by the entry above it, and
        // this list is the only thing that surfaces it.
        auto movesArr = [](const QVector<ChangelogLog::ProseMove> &v) {
            QJsonArray a;
            for (const auto &m : v) {
                QJsonObject o;
                o["from_line"]  = m.from_line;
                o["under_line"] = m.under_line;
                o["text"]       = m.text;
                a.append(o);
            }
            return a;
        };

        // dry_run, or an already-canonical section, both write nothing.
        if (dryRun || !res.changed) {
            QJsonObject out;
            out["ok"]           = true;
            out["op"]           = op;
            out["file"]         = clPath.section('/', -1);
            out["changed"]      = res.changed;
            out["order_before"] = orderArr(res.order_before);
            out["order_after"]  = orderArr(res.order_after);
            out["bytes"]        =
                static_cast<qint64>(res.markdown.toUtf8().size());
            out["prose_moved"]  = res.prose_moves.size();
            if (!res.prose_moves.isEmpty())
                out["moves"] = movesArr(res.prose_moves);
            if (dryRun) out["dry_run"] = true;
            if (res.malformed_section)
                out["advisory"] = proseAdvisory(res.malformed_line);
            return QJsonDocument(out);
        }

        const qint64 clBefore = QFileInfo(clPath).size();   // ANTS-3723
        QSaveFile cw(clPath);
        if (!cw.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return clErr(QStringLiteral("changelog_write_failed"),
                QStringLiteral("changelog_log: could not open \"%1\" for "
                               "writing").arg(clPath));
        }
        const QByteArray utf8 = res.markdown.toUtf8();
        if (cw.write(utf8) != utf8.size() || !cw.commit()) {
            return clErr(QStringLiteral("changelog_write_failed"),
                QStringLiteral("changelog_log: atomic write of \"%1\" failed")
                    .arg(clPath));
        }

        QJsonObject out;
        out["ok"]            = true;
        out["op"]            = op;
        out["file"]          = clPath.section('/', -1);
        out["changed"]       = true;
        out["order_before"]  = orderArr(res.order_before);
        out["order_after"]   = orderArr(res.order_after);
        out["prose_moved"]   = res.prose_moves.size();
        if (!res.prose_moves.isEmpty())
            out["moves"] = movesArr(res.prose_moves);
        // ANTS-3723 — a reorder adds no content, so the honest delta is 0;
        // `file_bytes` carries the size the old field used to report.
        // (A fold adds the two indent spaces per relocated line, so a
        // prose-relocating normalize reports a small positive delta.)
        rcSetWriteBytes(out, clBefore, static_cast<qint64>(utf8.size()));
        if (res.malformed_section)
            out["advisory"] = proseAdvisory(res.malformed_line);
        return QJsonDocument(out);
    }

    // ANTS-4363 — op:"release": CLOSE `## [Unreleased]` into a version block
    // and open a fresh empty one above it. The one changelog edit every
    // release makes, and the only one the verb did not own — so a file the
    // verb otherwise writes had its version headings typed by hand, at the
    // highest-stakes moment. A project whose release workflow greps
    // `^## \[<version>\]` for release notes publishes an EMPTY body when
    // that heading is a character out.
    if (op == QStringLiteral("release")) {
        const QString version = req.value(QStringLiteral("version")).toString();
        const QString relDate = req.value(QStringLiteral("date")).toString();

        QFile cf(clPath);
        if (!cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return clErr(QStringLiteral("changelog_read_failed"),
                QStringLiteral("changelog_log: could not read \"%1\"")
                    .arg(clPath));
        }
        const QString clMarkdown = QString::fromUtf8(cf.readAll());
        cf.close();

        const auto res =
            ChangelogLog::closeUnreleased(clMarkdown, version, relDate);
        if (!res.ok) return clErr(res.code, res.error);

        QJsonObject out;
        out["ok"]            = true;
        out["op"]            = op;
        out["file"]          = clPath.section('/', -1);
        out["version"]       = version.trimmed();
        out["heading"]       = res.heading;
        out["line"]          = res.line;
        // The closed section's content, because a caller wants it as release
        // notes immediately — otherwise they re-read the file they just wrote.
        out["released_body"] = res.released_body;

        if (dryRun) {
            out["dry_run"] = true;
            out["written"] = false;
            out["bytes"]   =
                static_cast<qint64>(res.markdown.toUtf8().size());
            return QJsonDocument(out);
        }

        const qint64 clBefore = QFileInfo(clPath).size();
        QSaveFile cw(clPath);
        if (!cw.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return clErr(QStringLiteral("changelog_write_failed"),
                QStringLiteral("changelog_log: could not open \"%1\" for "
                               "writing").arg(clPath));
        }
        const QByteArray utf8 = res.markdown.toUtf8();
        if (cw.write(utf8) != utf8.size() || !cw.commit()) {
            return clErr(QStringLiteral("changelog_write_failed"),
                QStringLiteral("changelog_log: atomic write of \"%1\" failed")
                    .arg(clPath));
        }
        out["written"]       = true;
        out["bytes_written"] =
            static_cast<qint64>(QFileInfo(clPath).size() - clBefore);
        return QJsonDocument(out);
    }

    // ANTS-3584 — op:"add_subsection": write a DATED feature-grouped block
    // (`### <date> <Category> — <headline>` + prose + bullets) at the TOP of
    // `## [Unreleased]`, for changelogs grouped by dated topic rather than flat
    // categories (Vestige/3D_Engine house style). Opt-in; the flat add /
    // add_from_roadmap paths are unchanged.
    if (op == QStringLiteral("add_subsection")) {
        QString headline = req.value(QStringLiteral("headline")).toString();
        if (headline.trimmed().isEmpty()) {
            return clErr(QStringLiteral("missing_field"),
                QStringLiteral("changelog_log: op:\"add_subsection\" requires "
                               "`headline`"));
        }
        QString category = req.value(QStringLiteral("category")).toString();
        if (category.isEmpty()) {
            const QString kind = req.value(QStringLiteral("kind")).toString();
            if (kind.isEmpty()) {
                return clErr(QStringLiteral("missing_field"),
                    QStringLiteral("changelog_log: op:\"add_subsection\" "
                                   "requires `category` or `kind` (to derive "
                                   "it)"));
            }
            category = ChangelogLog::kindToCategory(kind);
        }
        // The block leads with the date (newest-first); default to today.
        QString date = req.value(QStringLiteral("date")).toString().trimmed();
        if (date.isEmpty())
            date = QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));
        QString subBody = req.value(QStringLiteral("body")).toString();

        // Render optional bullets — each a bare string summary, or an object
        // {summary (required), body?, id?}. Empty summaries are skipped.
        QStringList bulletBlocks;
        const QJsonArray bulletsArr =
            req.value(QStringLiteral("bullets")).toArray();
        for (const QJsonValue &bv : bulletsArr) {
            QString bs, bb, bid;
            if (bv.isString()) {
                bs = bv.toString();
            } else if (bv.isObject()) {
                const QJsonObject bo = bv.toObject();
                bs  = bo.value(QStringLiteral("summary")).toString();
                bb  = bo.value(QStringLiteral("body")).toString();
                bid = bo.value(QStringLiteral("id")).toString();
            }
            if (bs.trimmed().isEmpty()) continue;
            QStringList bscrub;
            rcScrubLeakedToolXml(bs, bscrub);
            rcScrubLeakedToolXml(bb, bscrub);
            bulletBlocks.append(ChangelogLog::formatBullet(bs, bb, bid));
        }

        QStringList scrubbed;
        rcScrubLeakedToolXml(headline, scrubbed);
        rcScrubLeakedToolXml(subBody, scrubbed);

        QFile cf(clPath);
        if (!cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return clErr(QStringLiteral("changelog_read_failed"),
                QStringLiteral("changelog_log: could not read \"%1\"")
                    .arg(clPath));
        }
        const QString clMarkdown = QString::fromUtf8(cf.readAll());
        cf.close();

        const auto res = ChangelogLog::insertUnreleasedSubsection(
            clMarkdown, date, category, headline, subBody, bulletBlocks);
        if (!res.ok) {
            return clErr(res.code, res.error);
        }

        const QString heading = QStringLiteral("### %1 %2 — %3")
                                    .arg(date, category, headline.trimmed());
        if (dryRun) {
            QJsonObject out;
            out["ok"]       = true;
            out["op"]       = op;
            out["dry_run"]  = true;
            out["written"]  = false;
            out["file"]     = clPath.section('/', -1);
            out["category"] = category;
            out["date"]     = date;
            out["heading"]  = heading;
            out["line"]     = res.line;
            out["bytes"]    =
                static_cast<qint64>(res.markdown.toUtf8().size());
            return QJsonDocument(out);
        }

        const qint64 clBefore = QFileInfo(clPath).size();   // ANTS-3723
        QSaveFile cw(clPath);
        if (!cw.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return clErr(QStringLiteral("changelog_write_failed"),
                QStringLiteral("changelog_log: could not open \"%1\" for "
                               "writing").arg(clPath));
        }
        const QByteArray utf8 = res.markdown.toUtf8();
        if (cw.write(utf8) != utf8.size() || !cw.commit()) {
            return clErr(QStringLiteral("changelog_write_failed"),
                QStringLiteral("changelog_log: atomic write of \"%1\" failed")
                    .arg(clPath));
        }

        QJsonObject out;
        out["ok"]            = true;
        out["op"]            = op;
        out["file"]          = clPath.section('/', -1);
        out["category"]      = category;
        out["date"]          = date;
        out["heading"]       = heading;
        out["line"]          = res.line;
        rcSetWriteBytes(out, clBefore, static_cast<qint64>(utf8.size()));
        return QJsonDocument(out);
    }

    // Resolve summary / body / id / category by mode.
    QString summary, body, id, category;
    const QString reqCategory =
        req.value(QStringLiteral("category")).toString();
    id = req.value(QStringLiteral("id")).toString();
    body = req.value(QStringLiteral("body")).toString();

    if (op == QStringLiteral("add")) {
        summary = req.value(QStringLiteral("summary")).toString();
        if (summary.isEmpty()) {
            return clErr(QStringLiteral("missing_field"),
                QStringLiteral("changelog_log: op:\"add\" requires "
                               "`summary`"));
        }
        if (!reqCategory.isEmpty()) {
            category = reqCategory;
        } else {
            const QString kind =
                req.value(QStringLiteral("kind")).toString();
            if (kind.isEmpty()) {
                return clErr(QStringLiteral("missing_field"),
                    QStringLiteral("changelog_log: op:\"add\" requires "
                                   "`category` or `kind` (to derive it)"));
            }
            category = ChangelogLog::kindToCategory(kind);
        }
    } else {  // add_from_roadmap — reuse the ROADMAP bullet's prose.
        if (id.isEmpty()) {
            return clErr(QStringLiteral("missing_field"),
                QStringLiteral("changelog_log: op:\"add_from_roadmap\" "
                               "requires `id` (the [PROJ-NNNN] to cite)"));
        }
        const QString rmPath = findRoadmapUnder(callerCanonical);
        if (rmPath.isEmpty()) {
            return clErr(QStringLiteral("no_roadmap"),
                QStringLiteral("changelog_log: op:\"add_from_roadmap\" "
                               "needs a ROADMAP.md under \"%1\"")
                    .arg(callerCanonical));
        }
        // ANTS-3863 — the read is the provider's now, and it happens only as
        // far as the path needs: this site wants a bullet's id, so on a
        // migrated project it never reaches the body at all. openFailed()
        // forces the open, which is what keeps this refusal where it was.
        auto rmText = RoadmapSource::RoadmapText::fromFile(rmPath);
        if (rmText.openFailed()) {
            return clErr(QStringLiteral("roadmap_read_failed"),
                QStringLiteral("changelog_log: could not read \"%1\"")
                    .arg(rmPath));
        }
        // ANTS-3793 — the store when this project is migrated, the file's text
        // when it is not. A refusal is surfaced rather than served from
        // markdown: citing a bullet resolved from a source we were told not to
        // trust is how a wrong id reaches the CHANGELOG.
        RoadmapSource::ReadError srcWhy = RoadmapSource::ReadError::None;
        QString srcErr;
        const auto bullets = roadmapBullets(callerCanonical, rmText,
                                            /*includeArchive=*/false,
                                            &srcWhy, &srcErr);
        if (srcWhy != RoadmapSource::ReadError::None) {
            QJsonObject srcOut;
            rcRoadmapSourceRefused(srcOut, srcWhy, srcErr);
            return QJsonDocument(srcOut);
        }
        const RoadmapDialog::BulletRecord *match = nullptr;
        for (const auto &cand : bullets) {
            if (cand.id == id) { match = &cand; break; }
        }
        if (!match) {
            return clErr(QStringLiteral("id_not_in_roadmap"),
                QStringLiteral("changelog_log: id \"%1\" not found in "
                               "%2 (case-sensitive)").arg(id, rmPath));
        }
        // ANTS-1868 — collapse multi-line headlines to a single line
        // (same fold roadmap_query applies to `headline_oneline`,
        // ANTS-1521). A wrapped ROADMAP headline cannot otherwise
        // leak a hard newline into the rendered CHANGELOG bullet's
        // bold summary.
        // ANTS-2127 — use the UNTRUNCATED headline (headlineFull, ANTS-2075):
        // match->headline is capped at 120 chars with a `…` ellipsis for the
        // Roadmap dialog (ANTS-1811), which would otherwise leak mid-word
        // into the rendered CHANGELOG bold summary.
        // ANTS-4360 — an explicit `summary` OVERRIDES the roadmap headline,
        // mirroring `body`'s long-standing behaviour. A 📋 bullet names the
        // DEFECT, because that is what a planned bullet is for; copied
        // verbatim under `### Fixed` it reads as if the bug is still present
        // — present tense, no "fixed" / "no longer". `Kind: fix` bullets are
        // the common case for this op, so that was the majority path, and it
        // failed quietly: the verb reported ok and the category routing was
        // correct, so nothing prompted a re-read. Callers hand-edited the
        // rendered line immediately after the write, which is exactly the
        // round-trip this op exists to remove.
        //
        // Reporter's preference (b): keep inheriting category and id from the
        // roadmap — the parts a caller has no reason to restate — and let the
        // prose be rewritten in changelog voice.
        const QString summaryOverride =
            req.value(QStringLiteral("summary")).toString().trimmed();
        summary = summaryOverride.isEmpty()
            ? rcHeadlineOneline(
                  match->headlineFull.isEmpty() ? match->headline
                                                : match->headlineFull)
            : summaryOverride;
        // Reuse the authored user-facing prose: the bullet's Layman
        // line is CHANGELOG-voice already. An explicit `body` arg
        // overrides it. (We deliberately do NOT splat the full bullet
        // body — it carries Kind:/Source:/Lanes: metadata.)
        //
        // ANTS-1933: rxBoldLayman on match->body is tried FIRST because
        // it captures the full sentence including the trailing period via
        // greedy (.+). match->layman has the period stripped (INV-4), so
        // using it directly produced period-less CHANGELOG bodies after
        // ANTS-1861 fixed parseBullets to recognise the bold form.
        if (body.isEmpty()) {
            static const QRegularExpression rxBoldLayman(
                QStringLiteral("(?:\\*\\*)?Layman:(?:\\*\\*)?\\s*(.+)"),
                QRegularExpression::CaseInsensitiveOption);
            const auto lm = rxBoldLayman.match(match->body);
            if (lm.hasMatch()) body = lm.captured(1).trimmed();
        }
        // Fallback: period-stripped layman field (covers plain-form Layman:
        // bullets where match->body won't contain the bold prefix).
        if (body.isEmpty()) body = match->layman;
        category = !reqCategory.isEmpty()
            ? reqCategory
            : ChangelogLog::kindToCategory(match->kind);
    }

    // Scrub leaked tool-call XML from both fields (parity with
    // roadmap_log body handling).
    QStringList scrubbed;
    rcScrubLeakedToolXml(summary, scrubbed);
    rcScrubLeakedToolXml(body, scrubbed);

    const QString bullet = ChangelogLog::formatBullet(summary, body, id);

    QFile cf(clPath);
    if (!cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return clErr(QStringLiteral("changelog_read_failed"),
            QStringLiteral("changelog_log: could not read \"%1\"")
                .arg(clPath));
    }
    const QString clMarkdown = QString::fromUtf8(cf.readAll());
    cf.close();

    const auto res = ChangelogLog::insertUnreleasedEntry(
        clMarkdown, category, bullet);
    if (!res.ok) {
        return clErr(res.code, res.error);
    }

    // ANTS-2136 — dry_run: return the resolved insert preview without
    // writing CHANGELOG.md. `bytes` is the would-be file size; `bullet`
    // is the rendered entry so the caller can eyeball category routing
    // and prose before committing.
    if (dryRun) {
        QJsonObject out;
        out["ok"]               = true;
        out["op"]               = op;
        out["dry_run"]          = true;
        out["file"]             = clPath.section('/', -1);
        out["category"]         = category;
        out["line"]             = res.line;
        out["bytes"]            = static_cast<qint64>(res.markdown.toUtf8().size());
        out["created_category"] = res.created_category;
        out["bullet"]           = bullet;
        if (!id.isEmpty()) out["id"] = id;
        if (res.malformed_section) {
            out["advisory"] = changelogMalformedAdvisory(
                res.malformed_line, /*plural=*/false, /*applied=*/false);
        }
        if (!scrubbed.isEmpty()) {
            QJsonArray dropped;
            for (const QString &n : scrubbed) dropped.append(n);
            out["scrubbed_params"] = dropped;
        }
        return QJsonDocument(out);
    }

    const qint64 clBefore = QFileInfo(clPath).size();   // ANTS-3723
    QSaveFile cw(clPath);
    if (!cw.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return clErr(QStringLiteral("changelog_write_failed"),
            QStringLiteral("changelog_log: could not open \"%1\" for "
                           "writing").arg(clPath));
    }
    const QByteArray utf8 = res.markdown.toUtf8();
    if (cw.write(utf8) != utf8.size() || !cw.commit()) {
        return clErr(QStringLiteral("changelog_write_failed"),
            QStringLiteral("changelog_log: atomic write of \"%1\" failed")
                .arg(clPath));
    }

    QJsonObject out;
    out["ok"]               = true;
    out["op"]               = op;
    out["file"]             = clPath.section('/', -1);
    out["category"]         = category;
    out["line"]             = res.line;
    rcSetWriteBytes(out, clBefore, static_cast<qint64>(utf8.size()));
    out["created_category"] = res.created_category;
    if (!id.isEmpty()) out["id"] = id;
    // ANTS-2125 — non-blocking advisory: the entry was inserted in
    // canonical order, but the Unreleased section already interleaves
    // non-heading prose between its `### ` category blocks, so the layout
    // is malformed. Surface it (parity with roadmap_log possible_duplicates)
    // so the caller can tidy the section before the insert compounds it.
    if (res.malformed_section) {
        out["advisory"] = changelogMalformedAdvisory(
            res.malformed_line, /*plural=*/false, /*applied=*/true);
    }
    if (!scrubbed.isEmpty()) {
        QJsonArray dropped;
        for (const QString &n : scrubbed) dropped.append(n);
        out["scrubbed_params"] = dropped;
    }
    return QJsonDocument(out);
}

namespace rcdetail {
// ANTS-2044 — resolve one add_batch entry into (category, rendered
// bullet, id). Mode is auto-detected: a `summary` makes it an `add`
// (needs `category` or `kind`); an `id` with no `summary` makes it an
// `add_from_roadmap` (pulls the cited bullet's headline + Layman from
// `roadmapBullets`). This mirrors the single-op resolution in
// cmdChangelogLog rather than refactoring it, so the established single
// path stays byte-for-byte unchanged (the single path keys on the
// explicit top-level `op`, a deliberately different contract from this
// per-entry auto-detect). Rule-of-Three: two sites, so the small
// duplication is left in place with this note rather than extracted.
struct ClBatchEntryResult {
    bool    ok = false;
    QString code, error;       // per-entry refusal iff !ok
    QString id, category, bullet;  // valid iff ok
};

ClBatchEntryResult resolveClBatchEntry(
        const QJsonObject &e,
        const QVector<RoadmapDialog::BulletRecord> &roadmapBullets,
        bool roadmapPresent) {
    ClBatchEntryResult r;
    const QString summary0 = e.value(QStringLiteral("summary")).toString();
    const QString id0      = e.value(QStringLiteral("id")).toString();
    const QString reqCategory =
        e.value(QStringLiteral("category")).toString();
    QString body = e.value(QStringLiteral("body")).toString();
    QString summary, category, id = id0;

    if (!summary0.isEmpty()) {
        summary = summary0;
        if (!reqCategory.isEmpty()) {
            category = reqCategory;
        } else {
            const QString kind = e.value(QStringLiteral("kind")).toString();
            if (kind.isEmpty()) {
                r.code  = QStringLiteral("missing_field");
                r.error = QStringLiteral(
                    "add entry needs `category` or `kind` (to derive it)");
                return r;
            }
            category = ChangelogLog::kindToCategory(kind);
        }
    } else if (!id0.isEmpty()) {
        if (!roadmapPresent) {
            r.code  = QStringLiteral("no_roadmap");
            r.error = QStringLiteral(
                "entry cites id \"%1\" (add_from_roadmap) but no ROADMAP.md "
                "is present").arg(id0);
            return r;
        }
        const RoadmapDialog::BulletRecord *match = nullptr;
        for (const auto &cand : roadmapBullets) {
            if (cand.id == id0) { match = &cand; break; }
        }
        if (!match) {
            r.code  = QStringLiteral("id_not_in_roadmap");
            r.error = QStringLiteral(
                "id \"%1\" not found in ROADMAP (case-sensitive)").arg(id0);
            return r;
        }
        // Headline → one-line bold summary; Layman → body (parity with
        // the single op:add_from_roadmap path, ANTS-1868 / ANTS-1933).
        // ANTS-2127 — untruncated headline (see cmdChangelogLog).
        summary = rcHeadlineOneline(
            match->headlineFull.isEmpty() ? match->headline
                                          : match->headlineFull);
        if (body.isEmpty()) {
            static const QRegularExpression rxBoldLayman(
                QStringLiteral("(?:\\*\\*)?Layman:(?:\\*\\*)?\\s*(.+)"),
                QRegularExpression::CaseInsensitiveOption);
            const auto lm = rxBoldLayman.match(match->body);
            if (lm.hasMatch()) body = lm.captured(1).trimmed();
        }
        if (body.isEmpty()) body = match->layman;
        category = !reqCategory.isEmpty()
            ? reqCategory
            : ChangelogLog::kindToCategory(match->kind);
    } else {
        r.code  = QStringLiteral("missing_field");
        r.error = QStringLiteral(
            "entry needs `summary` (add) or `id` (add_from_roadmap)");
        return r;
    }

    // Pre-validate category so a bad explicit `category` reads as a clear
    // per-entry skip rather than surfacing later as an insert refusal.
    if (!ChangelogLog::isValidCategory(category)) {
        r.code  = QStringLiteral("bad_category");
        r.error = QStringLiteral(
            "\"%1\" is not a Keep-a-Changelog category").arg(category);
        return r;
    }

    QStringList scrubbed;  // parity with single op (drop leaked tool XML)
    rcScrubLeakedToolXml(summary, scrubbed);
    rcScrubLeakedToolXml(body, scrubbed);

    r.ok       = true;
    r.id       = id;
    r.category = category;
    r.bullet   = ChangelogLog::formatBullet(summary, body, id);
    return r;
}
}  // namespace rcdetail

// ANTS-2044 — changelog_log op:"add_batch". N entries, one read + one
// atomic QSaveFile commit (parity with roadmap_log op:append_batch,
// ANTS-1879). Each entry is resolved by resolveClBatchEntry above;
// per-entry failures land in skipped[] while valid entries still apply.
// Entries insert in input order, so the result is byte-identical to the
// same N sequential single-op calls. m_main-independent.
QJsonDocument RemoteControl::cmdChangelogLogAddBatch(const QJsonObject &req) {
    auto clErr = [](const QString &code, const QString &message) {
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = code;
        env["error"] = message;
        return QJsonDocument(env);
    };

    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    if (callerRaw.isEmpty()) {
        return clErr(QStringLiteral("missing_field"),
            QStringLiteral("changelog_log: caller_cwd is required"));
    }
    if (!req.value(QStringLiteral("entries")).isArray() ||
        req.value(QStringLiteral("entries")).toArray().isEmpty()) {
        return clErr(QStringLiteral("bad_args"),
            QStringLiteral("changelog_log: op:\"add_batch\" needs a "
                           "non-empty `entries` array"));
    }
    const QJsonArray entries =
        req.value(QStringLiteral("entries")).toArray();
    // ANTS-2136 — dry_run preview, parity with the single op.
    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();

    const QString callerCanonical =
        QFileInfo(callerRaw).canonicalFilePath();
    if (callerCanonical.isEmpty()) {
        return clErr(QStringLiteral("no_changelog"),
            QStringLiteral("changelog_log: caller_cwd \"%1\" does not "
                           "canonicalise to an existing directory")
                .arg(callerRaw));
    }
    // Resolve the changelog with the same Markdown/YAML split as the
    // single op (ANTS-2040 format_mismatch reused verbatim).
    const QString clPath = findChangelogUnder(callerCanonical);
    if (clPath.isEmpty()) {
        const QString yamlPath = findYamlChangelogUnder(callerCanonical);
        if (!yamlPath.isEmpty()) {
            QJsonObject env;
            env["ok"]     = false;
            env["code"]   = QStringLiteral("format_mismatch");
            env["error"]  = QStringLiteral(
                "changelog_log: \"%1\" is a YAML changelog; the writer "
                "only appends to Keep-a-Changelog Markdown (CHANGELOG.md) "
                "and can't emit this format yet")
                    .arg(yamlPath);
            env["path"]   = yamlPath;
            env["format"] = QStringLiteral("yaml");
            env["hint"]   = QStringLiteral(
                "Edit the YAML changelog directly — append a "
                "`- version:/date:/tags:/body:` block in its schema. "
                "YAML-changelog writes aren't supported by changelog_log "
                "yet (ANTS-2040).");
            return QJsonDocument(env);
        }
        return clErr(QStringLiteral("no_changelog"),
            QStringLiteral("changelog_log: no CHANGELOG.md under \"%1\"")
                .arg(callerCanonical));
    }

    // Parse ROADMAP once, only if any entry is id-only (add_from_roadmap).
    QVector<RoadmapDialog::BulletRecord> roadmapBullets;
    bool roadmapPresent = false;
    bool roadmapNeeded = false;
    for (const QJsonValue &v : entries) {
        const QJsonObject e = v.toObject();
        if (e.value(QStringLiteral("summary")).toString().isEmpty() &&
            !e.value(QStringLiteral("id")).toString().isEmpty()) {
            roadmapNeeded = true;
            break;
        }
    }
    if (roadmapNeeded) {
        const QString rmPath = findRoadmapUnder(callerCanonical);
        if (!rmPath.isEmpty()) {
            auto rmText = RoadmapSource::RoadmapText::fromFile(rmPath);
            // ANTS-3863 — the same shape the QFile::open() guard had: this
            // block is entered only when the roadmap is readable.
            if (!rmText.openFailed()) {
                // ANTS-3793 — store-or-markdown. `this->` because the local
                // above shadows the member with the same name.
                RoadmapSource::ReadError srcWhy =
                    RoadmapSource::ReadError::None;
                QString srcErr;
                roadmapBullets = this->roadmapBullets(
                    callerCanonical, rmText, /*includeArchive=*/false,
                    &srcWhy, &srcErr);
                if (srcWhy != RoadmapSource::ReadError::None) {
                    QJsonObject srcOut;
                    rcRoadmapSourceRefused(srcOut, srcWhy, srcErr);
                    return QJsonDocument(srcOut);
                }
                roadmapPresent = true;
            }
        }
    }

    QFile cf(clPath);
    if (!cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return clErr(QStringLiteral("changelog_read_failed"),
            QStringLiteral("changelog_log: could not read \"%1\"")
                .arg(clPath));
    }
    QString markdown = QString::fromUtf8(cf.readAll());
    cf.close();

    QJsonArray applied;   // [{index, id?, category, line}]
    QJsonArray skipped;   // [{index, code, error}]
    // ANTS-2125 — capture the malformed-section advisory from the first
    // applied insert; it sees the original (pre-batch) section, so its
    // verdict reflects the body the caller is about to compound.
    bool malformedSection = false;
    bool anyCreatedCategory = false;   // ANTS-3804 — envelope-level rollup
    int  malformedLine = -1;
    for (int i = 0; i < entries.size(); ++i) {
        const QJsonObject e = entries.at(i).toObject();
        const ClBatchEntryResult er =
            resolveClBatchEntry(e, roadmapBullets, roadmapPresent);
        if (!er.ok) {
            QJsonObject s;
            s["index"] = i;
            s["code"]  = er.code;
            s["error"] = er.error;
            skipped.append(s);
            continue;
        }
        const auto res = ChangelogLog::insertUnreleasedEntry(
            markdown, er.category, er.bullet);
        if (!res.ok) {
            QJsonObject s;
            s["index"] = i;
            s["code"]  = res.code;
            s["error"] = res.error;
            skipped.append(s);
            continue;
        }
        if (!malformedSection && res.malformed_section) {
            malformedSection = true;
            malformedLine = res.malformed_line;
        }
        markdown = res.markdown;  // accumulate; insert in input order
        QJsonObject a;
        a["index"]    = i;
        if (!er.id.isEmpty()) a["id"] = er.id;
        a["category"] = er.category;
        a["line"]     = res.line;
        // ANTS-3804 — report whether THIS entry created its `### <category>`
        // heading. The single-op path has always emitted created_category and
        // this one emitted nothing at all, so a caller reading the batch
        // envelope saw an absent field — which most JSON clients render as
        // false. Vestige read that as "the tool created no heading", deleted a
        // heading the tool had in fact created, and lost content that only
        // version control brought back.
        //
        // Per entry rather than only in aggregate: in a batch the FIRST entry
        // for a category creates the heading and later ones insert into it, so
        // a single envelope-level flag cannot say which entry did what.
        a["created_category"] = res.created_category;
        if (res.created_category) anyCreatedCategory = true;
        // ANTS-4395 — stash the bullet so the line can be RE-RESOLVED against
        // the final markdown below. `res.line` is correct at insert time and
        // goes stale the moment a later entry lands in the same category:
        // insertion is at the category head, so entry 1 takes entry 0's line
        // and pushes it down. A two-entry batch therefore reported the same
        // line for both, and only the first could ever be right.
        a["_bullet_head"] = er.bullet.split(QChar('\n')).value(0);
        applied.append(a);
    }

    // ANTS-4395 — resolve each applied entry's line against the FINAL
    // markdown. A caller using the returned `line` to cite or re-read the
    // entry it just wrote read the wrong one for every entry after the first;
    // the single-entry op:"add" path was always correct, which is why this
    // went unnoticed.
    {
        const QStringList finalLines = markdown.split(QChar('\n'));
        QSet<int> claimed;
        for (int k = 0; k < applied.size(); ++k) {
            QJsonObject a = applied.at(k).toObject();
            const QString head =
                a.take(QStringLiteral("_bullet_head")).toString();
            if (head.isEmpty()) { applied.replace(k, a); continue; }
            // First unclaimed exact match. Two entries CAN render identical
            // heads (the same summary filed twice), so claiming keeps them
            // from collapsing onto one line.
            for (int ln = 0; ln < finalLines.size(); ++ln) {
                if (claimed.contains(ln)) continue;
                if (finalLines.at(ln) != head) continue;
                a[QStringLiteral("line")] = ln + 1;   // 1-based
                claimed.insert(ln);
                break;
            }
            applied.replace(k, a);
        }
    }

    qint64 bytesWritten = 0;
    // ANTS-2136 — dry_run: skip the commit, report what WOULD apply.
    // `bytes` reflects the accumulated markdown size (0 when nothing
    // applied, parity with the write path's bytesWritten).
    if (dryRun) {
        QJsonObject out;
        out["ok"]              = true;
        out["op"]              = QStringLiteral("add_batch");
        out["dry_run"]         = true;
        out["file"]            = clPath.section('/', -1);
        out["applied"]         = applied;
        out["applied_count"]   = applied.size();
        out["skipped"]         = skipped;
        out["skipped_count"]   = skipped.size();
        out["bytes"]           = applied.isEmpty()
            ? static_cast<qint64>(0)
            : static_cast<qint64>(markdown.toUtf8().size());
        out["created_category"] = anyCreatedCategory;   // ANTS-3804
        if (malformedSection) {
            out["advisory"] = changelogMalformedAdvisory(
                malformedLine, /*plural=*/true, /*applied=*/false);
        }
        return QJsonDocument(out);
    }
    // Only touch the file when at least one entry applied — an all-skip
    // batch leaves CHANGELOG.md untouched.
    qint64 fileBytes = QFileInfo(clPath).size();        // ANTS-3723
    if (!applied.isEmpty()) {
        const qint64 clBefore = fileBytes;
        QSaveFile cw(clPath);
        if (!cw.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return clErr(QStringLiteral("changelog_write_failed"),
                QStringLiteral("changelog_log: could not open \"%1\" for "
                               "writing").arg(clPath));
        }
        const QByteArray utf8 = markdown.toUtf8();
        if (cw.write(utf8) != utf8.size() || !cw.commit()) {
            return clErr(QStringLiteral("changelog_write_failed"),
                QStringLiteral("changelog_log: atomic write of \"%1\" failed")
                    .arg(clPath));
        }
        fileBytes    = utf8.size();
        bytesWritten = fileBytes - clBefore;            // ANTS-3723 — delta
    }

    QJsonObject out;
    out["ok"]            = true;
    out["op"]            = QStringLiteral("add_batch");
    out["file"]          = clPath.section('/', -1);
    out["applied"]       = applied;
    out["applied_count"] = applied.size();
    out["skipped"]       = skipped;
    out["skipped_count"] = skipped.size();
    out["bytes_written"] = bytesWritten;
    out["file_bytes"]    = fileBytes;                   // ANTS-3723
    out["created_category"] = anyCreatedCategory;       // ANTS-3804
    if (malformedSection) {
        out["advisory"] = changelogMalformedAdvisory(
            malformedLine, /*plural=*/true, /*applied=*/true);
    }
    return QJsonDocument(out);
}

