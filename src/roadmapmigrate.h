// ANTS-3757 — the roadmap migration read half: parse a hand-written markdown
// roadmap into the plan ANTS-3765 loads into the store.
//
// Contract: docs/specs/ANTS-3757-roadmap-migration-read.md. Its § 2.1
// declarations are the single statement of shape and the types below are
// those declarations; every rule is stated in one of its sections and is
// cited here rather than restated.
//
// Two functions, and the split between them is the impurity: findRoadmap()
// touches the filesystem, planFrom() does not. Qt6::Core only, in
// ants_core_lib, so the whole read half is testable without a database —
// ANTS-3765 consumes the plan and owns every write.
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
};

// The roadmap-data-model.md § 5.1 status legend, which belongs to the PROJECT
// and not to any section. Two of the ten projects carry one. § 2.11 owns
// recognition and how the lines become `entries`.
struct PlannedLegend {
    QJsonObject entries;       // status value -> that project's wording
    int         firstLine = 0, lastLine = 0;
};

// Anything a human must see. Never a silent drop, never a stderr line: the
// report is a value, so a test can assert on it. § 2.10 fixes the code set.
struct Note {
    QString code;              // § 2.10's closed set
    QString detail;
    int     line = 0;          // 1-based in the source file; 0 = whole-file
};

struct MigrationPlan {
    QString                 projectName, exportSlug, sourcePath;
    QString                 format;    // detectRoadmapFormat()'s own vocabulary:
                                       // "ants-v1" | "github-task-list" | "pass-headings"
    QVector<PlannedSection> sections;
    QVector<PlannedItem>    items;
    QVector<PlannedElement> elements;
    std::optional<PlannedLegend> legend;
    QVector<Note>           notes;
};

// The IMPURE half — the only function here that touches the filesystem, and
// the only one that reads. Resolves the roadmap under `projectRoot`, decodes
// it, and returns both; nullopt with `error` set on any § 2.2 refusal.
//
// `*error` receives the REFUSAL CODE alone — `not_found` | `case_ambiguous` |
// `not_utf8` — and no prose. INV-1 asserts which refusal happened and free
// text is not assertable; a caller wanting a sentence composes it from the
// code and the root it passed, both of which it already holds.
struct Source { QString path, markdown; };
std::optional<Source> findRoadmap(const QString &projectRoot, QString *error);

// The PURE half: no filesystem, no clock, no id counter (INV-9).
// `projectName` and `exportSlug` are supplied by the caller, not derived —
// `exportSlug` is ANTS-3756's `project.export_slug`, whose charset the store
// constrains and which nothing in a markdown file carries. `sourcePath` is
// recorded into the plan and never read.
MigrationPlan planFrom(const QString &markdown, const QString &sourcePath,
                       const QString &projectName, const QString &exportSlug);

}  // namespace RoadmapMigrate
