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

#include <QHash>
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

    // ANTS-4462 / ANTS-4465 — the external-edit report. SET BY
    // RoadmapWrite::commitAndRender(), never by render() itself, and the
    // asymmetry is the point: render() is handed a store and asked to publish
    // it, so it cannot tell an edit it is about to destroy from the mutation it
    // was called to publish. Only the write sequence holds both sides — the
    // PRE-mutation render and the file on disk — and it stamps the answer here
    // because this Outcome is already the channel that reaches the envelope.
    // Left at the defaults on the render's own returns, which is what
    // `externalEditsChecked:false` means: not "clean", but "nobody looked".
    bool externalEditsChecked = false;
    // Lines by which the file differed from what the store alone would have
    // produced: lines the file held and the render does not reproduce, plus
    // lines the render holds that the file had lost. Zero with
    // externalEditsChecked ⇒ the file was exactly the store's render, so the
    // publish overwrote nothing but its own output.
    int externalEditLines = 0;

    // ANTS-4615 — the breakdown, because one number cannot be acted on. A flip
    // that changed nothing reported 84 drifted lines: 24 bullets moving from an
    // older bold-id form to the canonical bracketed one, and ONE sentence that
    // no longer existed anywhere. Mixing the two trains callers to wave the
    // flag through.
    //
    // These classify the FILE's own lines and so do NOT sum to
    // externalEditLines, which also counts lines the RENDER holds that the file
    // had deleted — a reverted deletion is not a loss and is not either of
    // these. The total's meaning is unchanged: this adds a breakdown, it does
    // not suppress anything (ANTS-4462 is explicit that deciding which
    // differences are cosmetic is not this check's judgement to make — so the
    // total keeps counting every one of them).
    //
    // `externalRestyledLines` is the SOFTER claim and `externalTextLines` the
    // one to act on, so an unclassifiable line counts as text: over-reporting
    // loss costs a look, under-reporting hides the thing the item is about.
    int         externalRestyledLines = 0;
    int         externalTextLines     = 0;
    QStringList externalLostText;              // capped; see kLostTextCap
    bool        externalLostTextTruncated = false;
};

// ANTS-3808 § 2.4 — one bullet's markdown, byte-identical to what the file
// writer below emits for this item. Exported for ANTS-3793's reader seam, whose
// `BulletRecord::body` is defined as this text and which cannot reach a
// file-local function; the alternative is a second renderer that has to be kept
// in step by hand.
//
// PRECONDITION (ANTS-3820): `it.status` is a RENDERABLE status. A `dropped` item
// has no markdown form at all, and the round trip is worse than a missing glyph:
// emojiFor("dropped") returns an empty string by design (§ 3.11 makes a fifth
// emoji an anti-pattern), so the head line emitted here carries NO status
// marker — and parseBullets()' native path then fails stripInlineEmoji() and
// SKIPS THE BULLET ENTIRELY. A dropped item rendered to markdown re-parses to
// nothing.
//
// This function does NOT enforce the precondition, and that is deliberate rather
// than an omission — both callers already exclude a dropped item, by two
// different mechanisms, and neither wants a refusal here:
//   - render()'s loop drops it at isRenderable() before reaching renderBullet(),
//     so the pairing holds by CALL ORDER.
//   - bulletsFromStore()'s appendRecord() (roadmapsource.cpp) relies on the
//     marker-less text failing parseAntsV1Bullet(), and skips the nullopt — it
//     wants exactly the text this produces, and its own comment says so.
// Making this refuse would break the second, which uses the unparseable output
// AS its exclusion signal.
//
// So the precondition is stated rather than asserted, and it is TESTED instead:
// see tests/features/roadmap_render's Ants3820 case, which pins both the
// unparseable-text property and both callers' exclusion. A third caller must
// pick one of those two mechanisms; it cannot rely on this function to stop it.
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
// `contentOut`, when given, receives the exact bytes each file would get, keyed
// by the same absolute paths as Outcome::filesWritten. It is populated on the
// engaged success return under EITHER dryRun setting and left untouched on the
// gate-failure return, where nothing was assembled. ANTS-4462 / ANTS-4465 need
// it: comparing a dry render's text against the file that exists is the only
// way to see a hand-edit before the next publish destroys it, and a second
// renderer written to answer that would be a copy of this one.
std::optional<Outcome> render(RoadmapStore &store, qint64 projectId,
                              const QString &projectRoot, const Options &opts,
                              QString *error = nullptr,
                              QHash<QString, QString> *contentOut = nullptr);

} // namespace RoadmapRender
