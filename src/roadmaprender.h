// ANTS-3758 — the roadmap render: generate ROADMAP.md from the store at full
// fidelity. Spec: docs/specs/ANTS-3758-roadmap-render.md
//
// The inverse of the migration (ANTS-3757/3765). Every item the render emits is
// written in full `roadmap-format.md` § 3.5 bullet form — status emoji, id,
// bold headline, Kind: and every optional field the item carries — so the
// generated file is the file that exists today, written by the store instead of
// by hand. The render is lossy in MEMBERSHIP only (§ 7.5 of
// roadmap-data-model.md excludes `internal` and `dropped`), never in detail.
//
// Qt6::Core + Qt6::Sql only, in ants_roadmapstore_lib, because ANTS-3794 will
// call it from a headless publish path.

#pragma once

// ANTS-3808 § 2.4 — bulletText() names RoadmapStore::ItemWrite, a NESTED type,
// which a forward declaration cannot satisfy. Same library, so this costs no
// new link edge; render()'s own `RoadmapStore &` parameter kept the forward
// declaration sufficient until now.
#include "roadmapstore.h"

#include <QString>
#include <QStringList>
#include <optional>

namespace RoadmapRender {

struct Options {
    // Where a NULL section.source_path routes. REQUIRED — this lib does not
    // link projectsettings.cpp, so it cannot read .ants/project.json and the
    // caller resolves the `roadmap` override (§ 2).
    QString liveRoadmapPath;
    // Computes everything and writes nothing. filesWritten lists the files a
    // real pass WOULD have written, and is empty when the gate fails, because
    // a real pass would have written nothing either.
    bool dryRun = false;
};

struct Outcome {
    QStringList filesWritten;   // what landed (or, under dryRun, what would have)
    // false together with a non-empty filesWritten is the partial-commit case
    // § 2.7 documents: QSaveFile::commit() is per file, so the commit phase is
    // the one window staging cannot close.
    bool committed = false;
    int  itemsRendered = 0, itemsExcluded = 0, sectionsRendered = 0;
    // ANTS-4141 — the ids of the items actually emitted, in emission order.
    // itemsRendered is the same set counted, and a count cannot answer the one
    // question ANTS-4141's divergence guard asks: is a bullet the live file
    // holds today absent from what this render would publish. Empty on the
    // gate-failure return, where nothing was emitted. An item the store holds
    // with no id contributes nothing.
    QStringList renderedIds;
    // Ids of public OPEN items with no `layman` (INV-5). Non-empty ⇒ nothing
    // was written. Populated on every engaged return, so a caller staring at a
    // gate failure can still see how many items would have rendered.
    QStringList gateFailures;
};

// ANTS-3808 § 2.4 — one bullet's markdown, byte-identical to what the file
// writer below emits for this item. Exported for ANTS-3793's reader seam, whose
// `BulletRecord::body` is defined as this text and which cannot reach a
// file-local function; the alternative is a second renderer that has to be kept
// in step by hand.
QString bulletText(const RoadmapStore::ItemWrite &it);

// roadmap-format.md § 3.3's four status emojis, by lifecycle word. `dropped`
// deliberately has no glyph (§ 3.11 makes a fifth an anti-pattern) and returns
// an empty string. Exported for the same reason bulletText() is: ANTS-3793
// § 2.3 has RoadmapDialog render a migrated project's STORED legend, whose JSON
// is keyed by those words while the dialog is keyed by emoji, and a second
// word→emoji table in the dialog is a correspondence someone has to keep true
// by hand.
QString emojiFor(const QString &status);

// roadmap-data-model.md § 3.4's open set — planned, in-progress AND considered.
// Exported for ANTS-4070's `minor_not_closed` guard, which must decide "is any
// item in this move set still open" using the codebase's own notion of open: a
// second predicate there would be free to invent a narrower one, and the draft
// that did (📋 / 🚧 only) would have archived work nobody has committed to.
bool isOpen(const QString &status);

// nullopt is reserved for failures BEFORE the commit phase — SQL errors, a
// render error, a path refusal — where there is genuinely nothing to report.
// A gate failure and a partial commit both return an ENGAGED Outcome, because
// a refusal that returned nullopt would throw away the one field the caller
// needs (INV-5, § 2.7).
std::optional<Outcome> render(RoadmapStore &store, qint64 projectId,
                              const QString &projectRoot, const Options &opts,
                              QString *error = nullptr);

} // namespace RoadmapRender
