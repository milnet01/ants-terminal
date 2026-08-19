// ANTS-3757 — the roadmap migration read half: parse a hand-written markdown
// roadmap into the plan ANTS-3765 loads into the store.
//
// Contract: docs/specs/ANTS-3757-roadmap-migration-read.md. Its § 2.1
// declarations are the single statement of shape and the types below are
// those declarations; every rule is stated in one of its sections and is
// cited here rather than restated.
//
// Two functions, and the split between them is the impurity: findRoadmaps()
// touches the filesystem, planFrom() does not. Qt6::Core only, in
// ants_core_lib, so the whole read half is testable without a database —
// ANTS-3765 consumes the plan and owns every write.
//
// ANTS-3766 widens this from one source per project to N: the live ROADMAP.md
// at index 0, followed by the rotated archives under docs/roadmap/ that
// roadmap-format.md § 3.9 moved out of it. Its § 2 is the contract for
// everything that carries a `sourceIndex` below; a line number is meaningless
// without the file it indexes, which is why every line-bearing carrier gained
// one rather than only the ones that looked like they needed it.
//
// The bullet grammar is NOT re-implemented here. RoadmapParse (ANTS-3764)
// classifies bullets and extracts their fields for all three formats;
// planFrom() adds the structural walk that reader never had (§ 2.11) —
// headings, tables, fences, narration, the legend, and a line span for every
// carrier. Two jobs against one reader; a second bullet parser is exactly
// what § 2.3 forbids.

#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace RoadmapMigrate {

// One item as migration will file it. Field names follow
// RoadmapStore::ItemWrite (ANTS-3756); § 2.1.1 accounts for every ItemWrite
// field and every `item` column, including the ones left empty here.
struct PlannedItem {
    QString     id;
    QString     idOrigin;      // "parsed" | "synthesised" | "quarantined"
    QString     status;        // one of the store's five; never a source string
    QString     headline, kind, source, layman, body;
    QStringList lanes, evidence;   // `Lanes:` / `Evidence:`
    QString     sectionSlug;   // the section this is filed under
    int         position = 0;  // § 2.11 — the ONE per-section sequence, shared
                               // with that section's PlannedElements
    QJsonObject extras;        // source_status, source_kind — verbatim (§ 2.7, § 2.8)
    QJsonObject provenance;    // per field: asserted | defaulted | migrated
                               // (roadmap-data-model.md § 7.7)
    // § 2.9 — the allocation obligation rides on the ITEM, not only on a note:
    // ANTS-3765 allocates, and a note keyed on a line number cannot be
    // correlated back to the item it belongs to.
    bool        idAllocationOwed = false;
    bool        closed = false;         // roadmap-data-model.md § 3.4's sense
    int         firstLine = 0, lastLine = 0;   // 1-based, inclusive
    // ANTS-4481 — the 1-based line each path-bearing trailer sits on, so an
    // `unresolved_path` note points at the path rather than at the bullet's
    // first line (measured 1152 vs 1166 on a real 3373-line roadmap). 0 means
    // the label was not found and the note falls back to `firstLine`. Carried
    // on the ITEM for ANTS-3765 § 2.9's reason: a note keyed on a line number
    // cannot be correlated back to the item it belongs to.
    int         sourceLine = 0, evidenceLine = 0;
    // ANTS-3766 § 2.4 — which source the two line numbers above index.
    // 0 = the live roadmap.
    int         sourceIndex = 0;
};

// A `##` / `###` heading. `section` is a table of its own in ANTS-3756, not an
// element kind, so a heading line is carried here and nowhere else. Content
// preceding the first heading belongs to a synthetic section with an empty
// slug and title. § 2.11 owns detection, `parentSlug`, slug uniquing and the
// intro boundary.
struct PlannedSection {
    QString slug, title;
    QString intro;                 // § 2.11 — prose and fences between the
                                   // heading and the section's first element
    int     level = 0;             // 2 or 3; 0 for the synthetic root
    QString parentSlug;            // "" at top level
    // Spans the HEADING LINE AND ITS INTRO ONLY, never the section's items and
    // elements — those carry their own spans and INV-11 is a partition.
    int     firstLine = 0, lastLine = 0;
    // ANTS-3766 § 2.4. Also what ANTS-3782 § 2.4 resolves through
    // MigrationPlan::sources to compute this section's stored `source_path`.
    int     sourceIndex = 0;
};

// Everything inside a section that is NOT an item and NOT the intro. `kind` is
// the store's own element vocabulary minus `item`, and nothing else:
// `element.kind` CHECKs exactly ('item','narration','table').
//
// roadmap-data-model.md § 5 is explicit that a fenced block is NOT a section
// element, so a fence inside an item's span is that item's `body` and a fence
// above the first element is the section's `intro`. `narration` is
// section-summary prose after the first element; the
// `<!-- ants-roadmap-format: 1 -->` marker and any other stray line are
// narration too, since the store has no other bucket and INV-11 forbids
// dropping them.
struct PlannedElement {
    QString kind;              // "narration" | "table"
    QString payload;           // narration: verbatim source text.
                               // table: § 2.11's {header, rows} JSON — the
                               // separator row is delimiter, not content, so
                               // it is never stored.
    QString sectionSlug;
    // § 2.11 — ONE 0-based sequence per section shared with that section's
    // items, because `element` CHECKs UNIQUE (section_id, position) across
    // item and non-item rows alike.
    int     position = 0;
    int     firstLine = 0, lastLine = 0;
    int     sourceIndex = 0;   // ANTS-3766 § 2.4
};

// The roadmap-data-model.md § 5.1 status legend, which belongs to the PROJECT
// and not to any section. Two of the ten projects carry one. § 2.11 owns
// recognition and how the lines become `entries`.
struct PlannedLegend {
    QJsonObject entries;       // status value -> that project's wording
    int         firstLine = 0, lastLine = 0;
    // ANTS-3766 § 2.4 — carried because the span needs a file, NOT because the
    // value is informative: only sources[0] may plan a legend, so this is
    // always 0. An archive's own legend run is planned as `narration` in that
    // archive's section instead, which keeps every line carried and INV-5's
    // per-source partition satisfiable.
    int         sourceIndex = 0;
};

// Anything a human must see. Never a silent drop, never a stderr line: the
// report is a value, so a test can assert on it. § 2.10 fixes the code set.
struct Note {
    QString code;              // § 2.10's closed set, widened by ANTS-3766 § 2.2
    QString detail;
    int     line = 0;          // 1-based in the source file; 0 = whole-file
    // ANTS-3766 § 2.4. -1 means the note is about a file that is NOT a source:
    // `archive_unrecognised` names an entry deliberately never read, so it
    // indexes nothing, its filename lives in `detail`, and its `line` is 0
    // (there is no file here to be inside). Without the -1 it would default to
    // 0 and claim to be about the live roadmap.
    int     sourceIndex = 0;
};

// ANTS-3766 § 2.1 — one file of a project, and the unit `sourceIndex` indexes.
//
// `format` lives HERE and not on the plan: detectRoadmapFormat() classifies one
// FILE, exactly as `path` names one, so a plan-level format would parse an
// archive under the live file's grammar and its bullets would vanish with no
// note — the loss class this whole lane exists to prevent, reintroduced by the
// field nobody moved.
struct Source {
    QString path, markdown;
    QString format;            // "ants-v1" | "github-task-list" | "pass-headings"
};

// ANTS-3766 § 2.1 — what discovery returns.
//
// A struct and not a bare QVector<Source> because of `notes`: a REJECTED entry
// never becomes a Source, so without a carrier nothing downstream could report
// it. ANTS-3757 § 2.2 is explicit that a file which does not resolve "produces
// no markdown, so there is no plan for a note to ride on" — true of its three
// whole-root refusals, and NOT true here, where the call SUCCEEDS and one entry
// was dropped.
struct Discovery {
    QVector<Source> sources;   // element 0 is always the live roadmap
    QVector<Note>   notes;     // discovery-time notes (§ 2.2)
};

struct MigrationPlan {
    QString                 projectName, exportSlug;
    // ANTS-3766 § 2.1 — REPLACES `QString sourcePath` AND `QString format`.
    // Ordered and index-stable within a run: a carrier's `sourceIndex` is only
    // meaningful against the plan that produced it.
    //
    // Element 0 is always the live roadmap — planFrom() preserves Discovery's
    // order, on which the same guarantee is stated. Recorded HERE too because it
    // is the sole precondition ANTS-3815 INV-2 rests on: the store's
    // project.source_format is index 0's format, and "the live roadmap's
    // dialect" follows from reading index 0 only if this holds.
    QVector<Source>         sources;
    QVector<PlannedSection> sections;
    QVector<PlannedItem>    items;
    QVector<PlannedElement> elements;
    std::optional<PlannedLegend> legend;
    QVector<Note>           notes;
};

// The IMPURE half — the only function here that touches the filesystem, and
// the only one that reads. Resolves the live roadmap under `projectRoot` and
// every rotated archive beside it, decodes each, detects each one's format,
// and returns them; nullopt with `error` set on any § 2.2 refusal.
//
// ANTS-3766 § 2.1 — REPLACES findRoadmap(); no forwarding overload is kept. At
// the measured call-site count a second entry point costs more in ambiguity
// than it saves in churn, and coding.md § 2 prefers the single surface.
//
// `*error` receives the REFUSAL CODE alone — `not_found` | `case_ambiguous` |
// `not_utf8` | `archive_format_mismatch` — and no prose. ANTS-3757 INV-1
// asserts which refusal happened and free text is not assertable; a caller
// wanting a sentence composes it from the code and the root it passed, both of
// which it already holds. A refusal NAMES NO FILE, deliberately: it aborts the
// whole project, so the operator's next step is to inspect docs/roadmap/, which
// holds a handful of files. Naming the entry matters only where the call
// SUCCEEDS and one was dropped — and that case, `archive_unrecognised`, does
// name it, in a note riding on the Discovery that exists.
std::optional<Discovery> findRoadmaps(const QString &projectRoot, QString *error);

// The PURE half: no filesystem, no clock, no id counter (ANTS-3757 INV-9).
// `projectName` and `exportSlug` are supplied by the caller, not derived —
// `exportSlug` is ANTS-3756's `project.export_slug`, whose charset the store
// constrains and which nothing in a markdown file carries.
//
// ANTS-3766 § 2.1 — takes the whole Discovery: all sources in, ONE plan out.
// One plan per project is forced rather than chosen — RoadmapMigrateLoad::load()
// is "one plan, one project, one transaction", so per-source plans would mean N
// transactions per project and would break that spec's § 2.5 atomicity.
// The discovery NOTES arrive as part of the argument, which is what lets them
// reach the plan without planFrom() touching the filesystem: INV-9's purity is
// preserved because they are passed in, not read.
MigrationPlan planFrom(const Discovery &discovery, const QString &projectName,
                       const QString &exportSlug);

// ANTS-4065 § 2.5 — resolve every `Source:` / `Evidence:` value that names a
// file, against `projectRoot`. A path that does not resolve is NOT a refusal:
// the item gains `extras.unresolved_path` (an ARRAY — one item can cite several
// paths and lose more than one) and the plan gains an `unresolved_path` note.
// Refusing would make a historical roadmap unimportable, which no reading of
// the problem asks for.
//
// A THIRD function rather than an argument to planFrom(), because resolving a
// path reads the filesystem and ANTS-3757 INV-9 makes planFrom() pure. The
// impure half of this namespace is where a filesystem touch belongs, and the
// seam is what lets the plan be inspected before anything is written.
void validatePaths(MigrationPlan &plan, const QString &projectRoot);

}  // namespace RoadmapMigrate
