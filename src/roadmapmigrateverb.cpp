// ANTS-3855 — the `roadmap_migrate` seam: everything that happens to a store.
// Contract: docs/specs/ANTS-3855-roadmap-migrate-verb.md
//
// This is the ONLY production caller of the migration engine. Schema
// (ANTS-3756), read half (ANTS-3757), load half (ANTS-3765), archives
// (ANTS-3766), render (ANTS-3758), export (ANTS-3761), section provenance
// (ANTS-3782), consumer cutover (ANTS-3793) and write half (ANTS-3809) all
// shipped before it and none of them could be reached: every invoker was a test
// under tests/features/, so no project could be migrated at all. INV-1 pins
// that this file — and only this file — closes that gap.
//
// Qt6::Core + Qt6::Sql only. It names no RemoteControl and no MainWindow, which
// is what lets test_core link it: that bundle links ants_core_lib ALONE, and a
// static archive is pulled in at OBJECT granularity, so the handler sharing
// this object would drag the whole chrome stack in behind it (see
// roadmapmigrateverb.h).

#include "roadmapmigrateverb.h"

#include "projectsettings.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>

#include <algorithm>

namespace {

QJsonObject rmErr(const QString &code, const QString &message) {
    QJsonObject e;
    e[QStringLiteral("ok")]    = false;
    e[QStringLiteral("code")]  = code;
    e[QStringLiteral("error")] = message;
    return e;
}

// § 2.4 — notes[] is bounded on BOTH axes, because capping the element count
// alone bounds no bytes: RoadmapMigrate::Note::detail is a QString with no
// length rule of its own, and one note is emitted per offending source line.
// The detail bound is in characters, which is the unit the rest of this
// project's note handling uses (rcdetail::kRcMaxNoteChars).
constexpr int kMaxNoteEntries     = 200;
constexpr int kMaxNoteDetailChars = 2048;
// ANTS-4649 — enough to locate a repeated note without restating it. Three
// lines answer "where does this happen?"; the fourth answers nothing new.
constexpr int kMaxNoteSampleLines = 3;

QJsonObject noteToJson(const RoadmapMigrate::Note &n) {
    QJsonObject o;
    o[QStringLiteral("code")] = n.code;
    QString detail = n.detail;
    if (detail.size() > kMaxNoteDetailChars)
        detail = detail.left(kMaxNoteDetailChars - 1) + QStringLiteral("…");
    o[QStringLiteral("detail")] = detail;
    o[QStringLiteral("line")]   = n.line;
    // Verbatim, including the -1 sentinel (ANTS-3766 § 2.4): a note about a
    // file that is NOT a source indexes nothing, and defaulting it to 0 would
    // have it claim to be about the live roadmap.
    o[QStringLiteral("source_index")] = n.sourceIndex;
    return o;
}

// Carried by the success envelope and by `migrate_failed`, under one set of
// bounds. `notes_count` stays the TRUE total so a collapsed or truncated array
// cannot read as a complete one.
//
// ANTS-4649 — repetition is collapsed BEFORE the cap. A 357-item migration
// returned 357 `{code:"field_defaulted", detail:"source", line:N}` objects —
// roughly 6 KB — beside `defaulted_fields:{source:357}` and
// `notes_count:357`, which already state the same fact twice; and because both
// the dry run and the real call emit it, a careful caller who previews first
// paid it twice. The per-line detail was not actionable: the column defaults
// because the legacy bullets carry no `Source:` trailer at all, which is
// uniform across the file rather than a per-line problem.
//
// Rows are keyed by (code, detail, source_index) and keep first-appearance
// order. A row of one keeps the old shape exactly, so a genuine one-off is
// untouched; a merged row carries `count` + up to kMaxNoteSampleLines
// `sample_lines` and NO `line`, because it has no single line and must not
// claim one. Every row carries `count`, so the counts sum to `notes_count`
// and the collapse is checkably lossless in aggregate.
void setNotes(QJsonObject &env, const QVector<RoadmapMigrate::Note> &notes) {
    struct Group {
        RoadmapMigrate::Note first;
        int         count = 0;
        QJsonArray  sampleLines;
    };
    QVector<Group>      groups;
    QHash<QString, int> indexOf;          // key → position in `groups`

    for (const RoadmapMigrate::Note &n : notes) {
        const QString key = n.code + QLatin1Char('\x1f') + n.detail
                          + QLatin1Char('\x1f') + QString::number(n.sourceIndex);
        auto at = indexOf.constFind(key);
        if (at == indexOf.constEnd()) {
            indexOf.insert(key, groups.size());
            Group g;
            g.first = n;
            g.count = 1;
            g.sampleLines.append(n.line);
            groups.append(g);
            continue;
        }
        Group &g = groups[at.value()];
        ++g.count;
        if (g.sampleLines.size() < kMaxNoteSampleLines) g.sampleLines.append(n.line);
    }

    bool collapsed = false;
    QJsonArray arr;
    const int shown = std::min<int>(groups.size(), kMaxNoteEntries);
    for (int i = 0; i < shown; ++i) {
        const Group &g = groups.at(i);
        QJsonObject o  = noteToJson(g.first);
        o[QStringLiteral("count")] = g.count;
        if (g.count > 1) {
            collapsed = true;
            o.remove(QStringLiteral("line"));
            o[QStringLiteral("sample_lines")] = g.sampleLines;
        }
        arr.append(o);
    }
    env[QStringLiteral("notes")]       = arr;
    env[QStringLiteral("notes_count")] = notes.size();
    // Two different facts, deliberately two fields: `notes_truncated` means
    // rows were DROPPED and are unrecoverable; `notes_collapsed` means rows
    // were MERGED and every note is still accounted for by a count.
    env[QStringLiteral("notes_truncated")] = groups.size() > kMaxNoteEntries;
    env[QStringLiteral("notes_collapsed")] = collapsed;
}

// ANTS-4479 — WHICH items `items_updated` counted, and which columns moved.
// Under dry_run this turns a count into a reviewable plan, which is the whole
// value of a preview: `items_updated: 3` alone could not distinguish a
// reconciliation of real drift from a lossy re-parse flattening good rows.
//
// The cap is applied in the load, where the entries are collected, so this only
// reports it. `items_updated` stays the TRUE total — no count of its own is
// needed, and a truncated array therefore cannot read as a complete one.
void setUpdatedItems(QJsonObject &env, const RoadmapMigrateLoad::Outcome &out) {
    QJsonArray arr;
    for (const RoadmapMigrateLoad::Outcome::UpdatedItem &u : out.updatedItems) {
        QJsonObject o;
        o[QStringLiteral("id")]     = u.id;
        o[QStringLiteral("fields")] = QJsonArray::fromStringList(u.fields);
        arr.append(o);
    }
    env[QStringLiteral("updated_items")] = arr;
    env[QStringLiteral("updated_items_truncated")] =
        out.itemsUpdated > out.updatedItems.size();
}

// ANTS-4065 § 2.3 — the run-level tally, beside `notes_count`. A per-FIELD
// count, and it must be complete: `notes[]` is capped at kMaxNoteEntries, so a
// reader counting `field_defaulted` entries in the array would under-report
// exactly the run that needed reporting most.
//
// Counted off the PLAN's provenance rather than off the notes, because that is
// where the decision was made — one `defaulted` value per field per item, with
// no dependence on a note code being emitted or surviving the cap.
QJsonObject defaultedFieldTally(const RoadmapMigrate::MigrationPlan &plan) {
    QHash<QString, int> counts;
    for (const RoadmapMigrate::PlannedItem &it : plan.items) {
        for (auto f = it.provenance.constBegin(); f != it.provenance.constEnd(); ++f)
            if (f.value().toString() == QLatin1String("defaulted"))
                ++counts[f.key()];
    }
    QJsonObject out;
    for (auto i = counts.constBegin(); i != counts.constEnd(); ++i)
        out[i.key()] = i.value();
    return out;
}

// § 2.1 — the DDL's own CHECK, transcribed rather than paraphrased, because a
// second spelling of a charset is a second charset:
//
//   export_slug TEXT NOT NULL UNIQUE
//     CHECK (export_slug GLOB '[a-z0-9]*'
//        AND export_slug NOT GLOB '*[^a-z0-9-]*')
bool isLowerAlnum(QChar c) {
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
        || (c >= QLatin1Char('0') && c <= QLatin1Char('9'));
}

bool isValidExportSlug(const QString &slug) {
    if (slug.isEmpty())
        return false;
    if (!isLowerAlnum(slug.at(0)))
        return false;
    for (const QChar c : slug) {
        if (!isLowerAlnum(c) && c != QLatin1Char('-'))
            return false;
    }
    return true;
}

// § 2.5's mapping rule: a findRoadmaps() code maps onto a canonical code when
// one already means the same thing, and is added to the taxonomy when none
// does. `not_found` and `archive_format_mismatch` are the first case; minting
// synonyms would split one meaning across two codes.
QString discoveryRefusalCode(const QString &findRoadmapsCode) {
    if (findRoadmapsCode == QLatin1String("not_found"))
        return QStringLiteral("no_roadmap");
    if (findRoadmapsCode == QLatin1String("archive_format_mismatch"))
        return QStringLiteral("format_mismatch");
    return findRoadmapsCode;   // case_ambiguous, not_utf8 — no canonical twin
}

}  // namespace

QString RoadmapMigrateVerb::defaultExportSlug(const QString &leafDirName) {
    QString out;
    out.reserve(leafDirName.size());
    bool pendingDash = false;
    for (const QChar c : leafDirName) {
        const QChar lower = c.toLower();
        if (isLowerAlnum(lower)) {
            if (pendingDash && !out.isEmpty())
                out.append(QLatin1Char('-'));
            pendingDash = false;
            out.append(lower);
        } else {
            pendingDash = true;
        }
    }
    return out;
}

// ANTS-4600 — § 2.5 step 0b. registerProject()'s INV-8 refuses a root that does
// not CANONICALISE, which is the deleted-path case and arrives too late: a
// session scratchpad exists while it is being migrated and is removed after.
// Registration is what makes it permanent, so the guard is at registration.
//
// The rule is deliberately the narrowest one that covers the measured case:
// under the system temp dir. Anything cleverer — matching "scratchpad" or a
// session-id shape in the path — is guessing at a naming convention no
// component owns.
// ANTS-4740 — see the header for why this lives on the link-testable seam.
QString RoadmapMigrateVerb::initSkeleton(const QString &projectName) {
    // The marker is assembled rather than written whole so this source file can
    // be read back through tooling that neutralises comment markers without the
    // literal being corrupted on the way to a fixture.
    return QStringLiteral("<!") + QStringLiteral("-- ants-roadmap-format: 1 --")
         + QStringLiteral(">\n# %1 \u2014 Roadmap\n"
             "\n"
             "> **Format:** v1 \u2014 see\n"
             "> [roadmap-format.md](docs/standards/roadmap-format.md).\n"
             "> Every actionable bullet carries a stable ID; ID is identity,\n"
             "> position is priority.\n"
             "\n"
             "**Legend** \u2014 \U0001F4CB planned \u00B7 \U0001F6A7 in progress "
             "\u00B7 \u2705 shipped \u00B7 \U0001F4AD considered\n"
             "\n"
             "## Backlog\n").arg(projectName);
}

bool RoadmapMigrateVerb::isTransientRoot(const QString &canonicalRoot) {
    // The temp root is canonicalised too. `canonicalRoot` is canonical by
    // precondition, and on a system whose temp dir is itself a symlink the two
    // forms would never compare equal.
    const QString tmp = QFileInfo(QDir::tempPath()).canonicalFilePath();
    if (tmp.isEmpty() || canonicalRoot.isEmpty())
        return false;   // no temp dir to be under; the cwd refusal owns empty.
    // The separator is load-bearing: a bare startsWith() would also match a
    // sibling merely NAMED like the temp dir (/tmpfoo against /tmp) and refuse
    // a legitimate root.
    return canonicalRoot == tmp
           || canonicalRoot.startsWith(tmp + QLatin1Char('/'));
}

QJsonObject RoadmapMigrateVerb::run(const QString &storePath, const Request &req) {
    // 1 — project_name. `project.name` is TEXT NOT NULL, and an all-whitespace
    // name would satisfy the column and identify nothing.
    const QString name = req.projectName.trimmed();
    if (name.isEmpty()) {
        return rmErr(QStringLiteral("bad_args"),
                     QStringLiteral("roadmap_migrate: project_name must not be "
                                    "empty after trimming"));
    }

    // 2 — export_slug, verbatim. Steps 1-2 precede the store open deliberately:
    // an invalid slug that reached registerProject() would fail the DDL CHECK
    // *inside* the transaction and roll back a whole migration, reporting a
    // store error for what is an argument error. NOTHING is opened yet, which
    // is what INV-6 asserts by checking no file appeared.
    const QString slug = req.exportSlug;
    if (!isValidExportSlug(slug)) {
        return rmErr(QStringLiteral("bad_args"),
                     QStringLiteral("roadmap_migrate: export_slug \"%1\" must be "
                                    "non-empty, start with [a-z0-9] and contain "
                                    "only [a-z0-9-]").arg(slug));
    }

    // 3 — discovery. Reads only under the resolved root and <root>/docs/roadmap/
    // (ANTS-3757 § 2.2); this verb passes the root and reads nothing itself.
    QString discErr;
    const auto disc = RoadmapMigrate::findRoadmaps(req.projectRoot, &discErr);
    if (!disc) {
        const QString code = discoveryRefusalCode(discErr);
        return rmErr(code, QStringLiteral("roadmap_migrate: no usable roadmap "
                                          "under \"%1\" (%2)")
                               .arg(req.projectRoot, discErr));
    }

    // 3a — ANTS-3771. The project's DECLARED id format, loaded ONCE here and
    // handed to both halves: the read half classifies parsed-vs-quarantined
    // against it (§ 2.4) and the load half allocates under its `prefix`
    // (§ 2.3). This is the impure side, which is where a filesystem read
    // belongs — planFrom() is pure (ANTS-3757 INV-9) and the loader's library
    // cannot see ProjectSettings at all.
    const RoadmapParse::IdFormat idFormat =
        ProjectSettings::idFormatFor(req.projectRoot);

    // 4 — the plan. Pure: no filesystem, no clock, no id counter. Cannot fail.
    auto plan = RoadmapMigrate::planFrom(*disc, name, slug, idFormat);

    // 4a — ANTS-4065 § 2.5. Separate from planFrom() because resolving a cited
    // path reads the filesystem and planFrom() is pure (ANTS-3757 INV-9). Never
    // a refusal: a roadmap legitimately cites files that have since moved,
    // shipped or been archived, so this annotates and moves on.
    RoadmapMigrate::validatePaths(plan, req.projectRoot);

    // 5 — the verb's OWN connection, on Access::Bulk, for the duration of one
    // call (§ 2.2). NOT RemoteControl's process-owned Interactive connection,
    // which load() refuses outright (ANTS-3765 INV-12) — and which this
    // function's signature makes unreachable. Two live connections in one
    // process are safe by construction: RoadmapStore allocates a per-instance
    // connection name from an atomic counter, and the store runs in WAL.
    //
    // A stack local, so ~RoadmapStore() runs PRAGMA optimize, close() and
    // removeDatabase() before this function returns — the connection names do
    // not accumulate across calls and the WAL sidecars are checkpointed away,
    // which INV-9 depends on.
    RoadmapStore store(storePath, RoadmapStore::kDefaultHistoryCapBytes,
                       RoadmapStore::Access::Bulk);
    QString err;
    if (!store.open(&err)) {
        return rmErr(QStringLiteral("store_failed"),
                     QStringLiteral("roadmap_migrate: could not open the roadmap "
                                    "store at \"%1\": %2").arg(storePath, err));
    }

    // 6 — collision and re-run-identity guards, ahead of the transaction.
    //
    // Both lookups check their ERROR out-param and not just their optional:
    // readProjectBySlug() / readProjectByRoot() return nullopt for BOTH "no such
    // row" and "the query failed", and conflating them would read an SQL failure
    // as "not registered" — the guard below would be skipped and the failure
    // would resurface at step 8 as a UNIQUE violation wearing `migrate_failed`.
    // Same split RoadmapSource::migratedProject() already makes.
    QString sqlErr;
    const auto other = store.readProjectBySlug(slug, &sqlErr);
    if (!sqlErr.isEmpty()) {
        return rmErr(QStringLiteral("store_failed"),
                     QStringLiteral("roadmap_migrate: export_slug lookup failed: %1")
                         .arg(sqlErr));
    }
    // `export_slug` is UNIQUE and the default is DERIVED, so two roots whose
    // leaf directories slugify alike (…/foo and …/Foo!) would otherwise collide
    // inside registerProject() and surface as the catch-all `migrate_failed`.
    // A matching ROOT is the re-run case (INV-7), not a collision — which is
    // why this compares the row's root rather than merely finding a row.
    if (other && other->root != req.projectRoot) {
        return rmErr(QStringLiteral("slug_collision"),
                     QStringLiteral("roadmap_migrate: export_slug \"%1\" already "
                                    "belongs to \"%2\" — pass a different "
                                    "export_slug").arg(slug, other->root));
    }

    sqlErr.clear();
    const auto owner = store.readProjectByRoot(req.projectRoot, &sqlErr);
    if (!sqlErr.isEmpty()) {
        return rmErr(QStringLiteral("store_failed"),
                     QStringLiteral("roadmap_migrate: project lookup failed: %1")
                         .arg(sqlErr));
    }
    // A re-run may not silently change a registered project's identity, and
    // identity is BOTH fields — each is defaulted from the leaf directory, so
    // an argument omitted on the second run recomputes its default and a
    // project migrated under an explicit value would be quietly re-slugged or
    // renamed. Different codes because the consequences differ: an export path
    // hangs off the slug; the name is a display change nobody asked for.
    if (owner && owner->exportSlug != slug) {
        return rmErr(QStringLiteral("slug_collision"),
                     QStringLiteral("roadmap_migrate: \"%1\" is already migrated "
                                    "under export_slug \"%2\"; re-running with "
                                    "\"%3\" would re-slug it")
                         .arg(req.projectRoot, owner->exportSlug, slug));
    }
    if (owner && owner->name != name) {
        return rmErr(QStringLiteral("bad_args"),
                     QStringLiteral("roadmap_migrate: \"%1\" is already migrated "
                                    "as project_name \"%2\"; re-running with "
                                    "\"%3\" would rename it")
                         .arg(req.projectRoot, owner->name, name));
    }

    // 7-8 — one plan, one project, one transaction. `changedAt` is the caller's
    // single stamp, forwarded rather than re-derived (§ 2.3.2).
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = req.changedAt;
    opts.projectRoot = req.projectRoot;
    opts.dryRun      = req.dryRun;
    opts.idFormat    = idFormat;      // ANTS-3771 § 2.3
    const auto out = RoadmapMigrateLoad::load(store, plan, opts);

    // 9 — the envelope. Every count is Outcome's corresponding field renamed to
    // snake_case and NOT recomputed: the Outcome is "a value, not a log", and a
    // second tally would be a second answer.
    QJsonObject env;
    env[QStringLiteral("dry_run")] = req.dryRun;
    if (!out.ok) {
        // Including a lock timeout: Access::Bulk carries a 30 s busy deadline
        // and the write lock is held for a whole project, so a concurrent
        // migrate or export can outlast it. SQLite reports that to load(),
        // whose message reaches the caller intact. No code of its own — the
        // operator's next step is the one `migrate_failed` already implies.
        env[QStringLiteral("ok")]    = false;
        env[QStringLiteral("code")]  = QStringLiteral("migrate_failed");
        env[QStringLiteral("error")] = out.error;
        setNotes(env, out.notes);
        return env;
    }

    env[QStringLiteral("ok")] = true;
    // ANTS-4478 — the store's project_id for this ROOT, on both paths whenever
    // a row for it already exists: step 6 read it into `owner` before the
    // transaction opened, and that id is durable, pre-existing, and the very
    // thing this envelope's items_updated / items_unchanged counts were diffed
    // against.
    //
    // So 0 means one thing only: this root has no project row yet. Under
    // dry_run that is the truthful answer — the rowid registerProject() would
    // allocate is inside a transaction about to roll back, and a provisional id
    // a later real run need not reuse is worse than no id, because it looks
    // durable. Three projects read the old unconditional 0 as "not registered"
    // beside counts that proved the opposite.
    env[QStringLiteral("project_id")] =
        req.dryRun ? (owner ? owner->projectId : 0) : out.projectId;
    env[QStringLiteral("project_name")] = name;
    env[QStringLiteral("export_slug")]  = slug;
    env[QStringLiteral("store_path")]   = storePath;
    env[QStringLiteral("changed_at")]   = req.changedAt;

    // `markdown` is deliberately dropped — it is the multi-megabyte input the
    // caller already has — and the path is relative to the root the caller
    // supplied, because echoing that root back on every entry is noise.
    QJsonArray sources;
    const QDir rootDir(req.projectRoot);
    for (const RoadmapMigrate::Source &s : plan.sources) {
        QJsonObject o;
        o[QStringLiteral("path")]   = rootDir.relativeFilePath(s.path);
        o[QStringLiteral("format")] = s.format;
        sources.append(o);
    }
    env[QStringLiteral("sources")] = sources;

    // ANTS-4490 — will roadmap_query and roadmap_log serve this project from
    // the store after this call? RoadmapSource::migratedProject() returns
    // nullopt for every dialect but ants-v1 ("legitimately markdown-served"),
    // so a github-task-list project migrates ok:true with faithful counts and
    // is still answered from markdown. Vestige could detect that only by
    // noticing which fields a LATER roadmap_query response did not carry.
    //
    // Index 0 is the live roadmap and is what project.source_format records
    // (ANTS-3815 INV-2). Unchanged by dry_run on purpose: this answers a
    // question about the source DIALECT, which a rollback cannot change.
    const bool storeBacked =
        !plan.sources.isEmpty()
        && plan.sources.first().format == QLatin1String("ants-v1");
    env[QStringLiteral("store_backed")] = storeBacked;

    // ANTS-4802 — the bare flag was not enough. A reader sees ok:true, a
    // project_id and a faithful item count, and concludes the store is now the
    // source of truth; `store_backed:false` sits among a dozen other fields
    // with nothing saying what it costs. RetroDB migrated twice on that
    // reading, leaving two snapshots no read path consults, and diagnosed it
    // only by going to sqlite3 directly.
    //
    // Say the consequence, not the fact. The rows ARE written and they are
    // faithful; what does not follow is that anything reads them.
    if (!storeBacked) {
        env[QStringLiteral("store_backed_hint")] = QStringLiteral(
            "Rows were written and are faithful, but NOTHING READS THEM: only "
            "the ants-v1 dialect is served from the store, and this roadmap is "
            "%1. roadmap_query keeps answering from markdown, roadmap_log keeps "
            "editing the file, and roadmap_log op:\"render\" has nothing to "
            "publish. Re-running this verb will not change that — it writes the "
            "same inert snapshot again. Those rows are summed by "
            "roadmap_query mode:\"report\" scope:\"all\", so if the store was "
            "not wanted here, remove them with roadmap_migrate "
            "op:\"deregister\".")
                .arg(plan.sources.isEmpty()
                         ? QStringLiteral("in no recognised dialect")
                         : QStringLiteral("'%1'").arg(plan.sources.first().format));
    }

    // ANTS-4482 — always false, and a field rather than a sentence because a
    // caller cannot branch on prose. This verb reads the roadmap and never
    // writes it: the file is byte-identical afterwards and `git status` is
    // clean, which three sessions verified with checksums and read as a
    // migration that had not run. The first re-render is the next roadmap_log
    // write.
    env[QStringLiteral("markdown_rewritten")] = false;

    env[QStringLiteral("items_inserted")]   = out.itemsInserted;
    env[QStringLiteral("items_updated")]    = out.itemsUpdated;
    // ANTS-4065 § 2.6 — both figures, because INV-6 reads the governed one and
    // the row-level one moves for reasons the contract excludes.
    env[QStringLiteral("items_updated_governed")] = out.itemsUpdatedGoverned;
    env[QStringLiteral("items_unchanged")]  = out.itemsUnchanged;
    env[QStringLiteral("items_orphaned")]   = out.itemsOrphaned;
    env[QStringLiteral("ids_allocated")]    = out.idsAllocated;
    env[QStringLiteral("sections_written")] = out.sectionsWritten;
    // ANTS-4490 — sections_written's partner. "0 written, 236 unchanged" says
    // what happened; `0` alone reads as a broken counter, which is how Vestige
    // reported it.
    env[QStringLiteral("sections_unchanged")] = out.sectionsUnchanged;
    env[QStringLiteral("elements_written")] = out.elementsWritten;
    // ANTS-4694 — what those numbers MEAN, on the one shape where they mislead.
    //
    // A large milestone roadmap is mostly prose: a phase map, exit criteria,
    // research logs, and bullets in an older dialect. Only the ants-v1 bullets
    // become ITEMS; everything else is carried as `narration` / `table`
    // elements. So the envelope can honestly say "3 items" about a 34-section
    // document, and a caller reads that as three-of-N captured and declines to
    // adopt the store. That is what happened, and stopping was the right call
    // on the information available -- which is the defect.
    //
    // The document is NOT partly captured. INV-11 is a PARTITION and is tested
    // as one: every non-blank source line falls inside exactly one item,
    // element, section or legend span, and a stray line is narration precisely
    // because the store has no other bucket and dropping it is forbidden.
    //
    // Rides the arm where the misreading is possible -- more non-item elements
    // than items -- because a hint on every migration is one nobody reads.
    {
        const int itemRows = out.itemsInserted + out.itemsUpdated
                           + out.itemsUnchanged;
        if (out.elementsWritten > itemRows) {
            env[QStringLiteral("coverage_hint")] = QStringLiteral(
                "%1 item(s) and %2 non-item element(s). The item count is NOT "
                "a coverage figure: only bullets in this project's own item "
                "dialect become items, and every other non-blank line -- "
                "prose, older-dialect bullets, tables, legends -- is carried "
                "as a `narration` or `table` element under the section it sat "
                "in. The line partition is total and is tested as one, so "
                "nothing in the source is dropped by the migration.")
                    .arg(itemRows).arg(out.elementsWritten);
        }
    }
    env[QStringLiteral("history_rows")]     = out.historyRows;
    setNotes(env, out.notes);
    setUpdatedItems(env, out);
    env[QStringLiteral("defaulted_fields")] = defaultedFieldTally(plan);
    return env;
}

QJsonObject RoadmapMigrateVerb::deregister(const QString &storePath,
                                           const DeregisterRequest &req) {
    if (req.projectRoot.isEmpty() && req.exportSlug.isEmpty()) {
        return rmErr(QStringLiteral("bad_args"),
                     QStringLiteral("roadmap_migrate: deregister needs a "
                                    "project root or an export_slug to key on"));
    }

    // Its own Access::Bulk connection for the duration of one call, on run()'s
    // reasoning above: a stack local, so ~RoadmapStore() checkpoints the WAL
    // and removes the connection name before this returns.
    RoadmapStore store(storePath, RoadmapStore::kDefaultHistoryCapBytes,
                       RoadmapStore::Access::Bulk);
    QString err;
    if (!store.open(&err)) {
        return rmErr(QStringLiteral("store_failed"),
                     QStringLiteral("roadmap_migrate: could not open the roadmap "
                                    "store at \"%1\": %2").arg(storePath, err));
    }

    // Both lookups check their ERROR out-param and not just their optional, for
    // run()'s reason: nullopt means BOTH "no such row" and "the query failed",
    // and conflating them here would report a project as unregistered because
    // the query broke — then answer `not_found` to a caller whose project is
    // very much still in the store.
    err.clear();
    std::optional<RoadmapStore::ProjectRow> row;
    if (!req.exportSlug.isEmpty())
        row = store.readProjectBySlug(req.exportSlug, &err);
    else
        row = store.readProjectByRoot(req.projectRoot, &err);
    if (!row && !err.isEmpty()) {
        return rmErr(QStringLiteral("store_failed"),
                     QStringLiteral("roadmap_migrate: could not read the project "
                                    "row: %1").arg(err));
    }
    if (!row) {
        return rmErr(QStringLiteral("not_found"),
                     QStringLiteral("roadmap_migrate: the store holds no project "
                                    "for \"%1\"")
                         .arg(req.exportSlug.isEmpty() ? req.projectRoot
                                                       : req.exportSlug));
    }

    // THE GUARD. Deregistering a project whose files are still there is data
    // loss with no undo: the store is primary and ROADMAP.md is its render, so
    // the rows are the only copy of everything the file does not carry —
    // history, relationships, citations. An absent root is the case this was
    // filed for and needs no ceremony; anything else has to be asked for in so
    // many words. Checked against the STORED root, not the caller's argument,
    // so keying by slug cannot skip it.
    const bool rootPresent =
        !row->root.isEmpty() && QFileInfo::exists(row->root);
    if (rootPresent && !req.confirm) {
        QJsonObject e = rmErr(QStringLiteral("confirm_required"),
            QStringLiteral("roadmap_migrate: \"%1\" still exists on disk, so "
                           "deregistering it would discard the store's own copy "
                           "of this project's history, relationships and "
                           "citations — none of which ROADMAP.md carries. Pass "
                           "confirm:true if that is what you want.")
                .arg(row->root));
        e[QStringLiteral("project_root")] = row->root;
        e[QStringLiteral("export_slug")]  = row->exportSlug;
        return e;
    }

    QJsonObject env;
    env[QStringLiteral("ok")]           = true;
    env[QStringLiteral("op")]           = QStringLiteral("deregister");
    env[QStringLiteral("project_root")] = row->root;
    env[QStringLiteral("export_slug")]  = row->exportSlug;
    env[QStringLiteral("root_exists")]  = rootPresent;

    // A dry run reports what WOULD go and opens no transaction. Counting is a
    // second query rather than a rolled-back delete: this is the one verb whose
    // preview a caller runs precisely because they are afraid of the real call,
    // and a preview that performs the deletion to measure it would be the
    // opposite of reassuring.
    if (req.dryRun) {
        RoadmapStore::DeregisterCounts would;
        if (const auto items = store.readItems(row->projectId, &err))
            would.items = int(items->size());
        else if (!err.isEmpty())
            return rmErr(QStringLiteral("store_failed"), err);
        if (const auto secs = store.listSections(row->projectId, &err))
            would.sections = int(secs->size());
        else if (!err.isEmpty())
            return rmErr(QStringLiteral("store_failed"), err);
        env[QStringLiteral("dry_run")]        = true;
        env[QStringLiteral("would_delete")]   = true;
        env[QStringLiteral("items")]          = would.items;
        env[QStringLiteral("sections")]       = would.sections;
        return env;
    }

    RoadmapStore::DeregisterCounts counts;
    if (!store.deregisterProject(row->projectId, &counts, &err)) {
        return rmErr(QStringLiteral("store_failed"),
                     QStringLiteral("roadmap_migrate: deregister failed and was "
                                    "rolled back whole: %1").arg(err));
    }

    env[QStringLiteral("deregistered")]  = true;
    env[QStringLiteral("items")]         = counts.items;
    env[QStringLiteral("sections")]      = counts.sections;
    env[QStringLiteral("elements")]      = counts.elements;
    env[QStringLiteral("history_rows")]  = counts.history;
    env[QStringLiteral("relationships")] = counts.relationships;
    env[QStringLiteral("citations")]     = counts.citations;
    env[QStringLiteral("feedback_refs")] = counts.feedbackRefs;
    env[QStringLiteral("id_prefixes")]   = counts.idPrefixes;
    return env;
}
