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
#include <QMap>          // ANTS-4844 — Outcome::touchedBullets
#include <QSet>          // ANTS-4628 — Options::gateScope
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
    // ANTS-4628 / § 2.5 — which items the INV-5 Layman gate judges, as
    // item_pks. UNSET means every item, which is the whole-project rule as
    // originally written and the behaviour of every caller that does not set
    // it; an ENGAGED set judges only its members, and an engaged EMPTY set
    // therefore judges nothing and always passes.
    //
    // Unset-means-all is the load-bearing half. The alternative default —
    // empty means all — makes a caller that builds a scope and legitimately
    // finds nothing indistinguishable from one that never opted in, and the
    // `op:render` case is exactly that caller: it touches no item, so it MUST
    // be able to say "judge nothing" and be obeyed.
    std::optional<QSet<qint64>> gateScope;
    // ANTS-4803 — which dialect to EMIT. "" / "ants-v1" is the bullet form
    // every other project uses; "pass-headings" emits `#### Pass N.M` blocks.
    //
    // Only the per-ITEM emission differs. Sections, ordering, narration,
    // tables, the file split, the INV-5 gate and the drift report are the
    // same machinery, because the store's shape is the same and only the
    // surface spelling is not. A second render() would have been a second
    // place for all of that to drift.
    //
    // The caller passes the project's STORED source_format, so a project
    // cannot be published in a dialect it was not migrated from.
    QString dialect;
    // ANTS-5256 — make the INV-5 Layman gate ADVISORY for this render: the
    // offenders are still collected, into Outcome::laymanMissing instead of
    // Outcome::gateFailures, and the render proceeds and publishes.
    //
    // For op:"convert" and nothing else. The gate is a rule about AUTHORING —
    // do not land new work without a plain-English line — and a convert
    // authors nothing: the same bullets change representation. Worse, it
    // cannot satisfy the gate even in principle. Layman is an ants-v1 rule,
    // and a github-task-list source is a dialect where roadmap-format.md makes
    // it optional, so the gate applies the DESTINATION dialect's rule to items
    // that exist only in the source dialect, as a precondition of the one call
    // that would make that rule apply to them.
    //
    // Measured (Vestige, 2026-09-21): 460 open items with no Layman line, so
    // the op refused outright. ANTS-4628 scoped the gate to the items a write
    // TOUCHES specifically to unblock conversion; a convert touches every item
    // by construction — and ANTS-4500 gives every id-less bullet a synthesised
    // id, making it an INSERT the store has never seen — so touched-items
    // scoping degenerates to whole-project scoping on exactly the op it was
    // narrowed for. gateScope cannot express this; the scope is right and it
    // is the RULE that does not apply.
    //
    // Deliberately not reusable as "skip the gate": an advisory gate still
    // reports, and the exemption ends with the convert. Every ordinary write
    // afterwards judges these items normally, so the first edit to one still
    // owes its summary.
    bool laymanGateAdvisory = false;
};

struct Outcome {
    QStringList filesWritten;   // what landed (or, under dryRun, what would have)
    // ANTS-5016 — files whose rendered bytes were already on disk, so they were
    // not rewritten (their mtime is untouched). Disjoint from filesWritten; the
    // two together are every file the render owns.
    QStringList filesUnchanged;
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
    // ANTS-5256 — the same offenders, under Options::laymanGateAdvisory: items
    // that FAILED the gate on a render that published anyway. Disjoint from
    // gateFailures by construction — one render fills one of the two — so the
    // "non-empty ⇒ nothing was written" rule above still reads true.
    //
    // A separate field rather than a flag beside gateFailures, because the two
    // mean opposite things to a caller: gateFailures is "your write was
    // refused", this is "your write landed and here is what it owes". A caller
    // branching on emptiness alone must not be able to confuse them.
    //
    // Sorted like gateFailures. The convert's envelope reports it as
    // `layman_missing`.
    QStringList laymanMissing;

    // ANTS-4844 — id → the bullet the render WILL emit for it, for each item
    // this write touched. SET BY RoadmapWrite::commitAndRender(), like the
    // drift fields below and for the same kind of reason: only the write
    // sequence holds the one window in which the mutated row exists.
    //
    // That window is narrow and is why this cannot be computed by the caller.
    // A dry run rolls its transaction back before commitAndRender() returns, so
    // an envelope builder rendering the bullet afterwards would render the
    // PRE-write state — a preview that is confidently wrong, which is the
    // defect this exists to fix rather than reproduce.
    //
    // A QMap, so a multi-item write reports in a stable order and a test can
    // assert on it. Bounded where it is filled; `op:"flip"` touches one item
    // and a batch touches many.
    QMap<QString, QString> touchedBullets;

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
    // ANTS-4695 — a file line whose ONLY difference from the render is
    // terminal punctuation. It is neither a dialect restyle nor lost prose:
    // the author's words survive and their punctuation does not. Counted
    // apart so `externalTextLines == 0` keeps meaning "your text is
    // untouched", which is the claim a caller checks before letting a render
    // overwrite their file.
    int         externalRepunctuatedLines = 0;
    // ANTS-4965 — a file line differing from the render ONLY in whitespace:
    // indentation or column alignment. Words survive, structure does not,
    // and a benign restyle never lands here, so it is counted apart.
    int         externalRestructuredLines = 0;
    int         externalTextLines     = 0;
    QStringList externalLostText;              // capped; see kLostTextCap
    bool        externalLostTextTruncated = false;
    // ANTS-4947 — where the overwritten file was kept, absolute, one entry per
    // file. Only the LOST arm fills this: a restyled or repunctuated line
    // survives in the render, so the file is not its only copy and a backup
    // there would fire on every project's first post-migration write. Empty on
    // a dry run, which overwrites nothing (the ANTS-4463 tense rule).
    QStringList externalLostBackups;
};

// ANTS-3808 § 2.4 — one bullet's markdown, byte-identical to what the file
// writer below emits for this item. Exported for ANTS-3793's reader seam, whose
// `BulletRecord::body` is defined as this text and which cannot reach a
// file-local function; the alternative is a second renderer that has to be kept
// in step by hand.
//
// ANTS-4977 superseded ANTS-3820's precondition: every status has a marker
// now, `dropped` included (🚫), so the head line always re-parses.
QString bulletText(const RoadmapStore::ItemWrite &it);

// ANTS-5087 — which trailer lines bulletText() will write for this item, as
// data. ONE owner for the five decisions, because there were two.
//
// `BulletRecord::composedTrailers` must name exactly the lines the render
// wrote: `roadmap_log op:"amend_body"` cannot reach a composed line, so a
// caller uses the field to tell a value the author typed from one the store
// composed. The store-built record computed it from its own copy of these
// predicates, and two had drifted — `source` lacked the defaulted-provenance
// rider and `kind` lacked the unrecognised-value one — so the field could name
// a `Source:` line the render withheld and miss a `Kind:` line it wrote. The
// copy's own comment claimed it "cannot disagree with the renderer", which is
// the claim that was false.
//
// Deriving the keys by DIFFING the rendered text against the body would be the
// other single owner and is the wrong one: a value present in both is composed
// in neither, and the diff cannot tell which line came from where.
struct TrailerLines {
    bool layman   = false;
    bool kind     = false;
    bool source   = false;
    bool lanes    = false;
    bool evidence = false;
    // The composed keys in the order the render emits them.
    QStringList keys() const;
};
TrailerLines trailerLines(const RoadmapStore::ItemWrite &it);

// ANTS-4955 — roadmap-format.md § 3.5: a Layman value is STORED without its
// closing full stop and RENDERED with one. laymanForStore() drops one trailing
// '.' from text a caller supplied; laymanRendered() appends '.' to a stored
// value unless it ends in '!' or '?'. Not withStop(), which also exempts '.':
// a stored "Wait.." would then render "Wait.." and re-import as "Wait.".
QString laymanForStore(const QString &value);
QString laymanRendered(const QString &stored);

// roadmap-format.md § 3.3's five status emojis, by lifecycle word (🚫 for
// `dropped`, ANTS-4977). An unknown word returns an empty string. Exported for the same reason bulletText() is: ANTS-3793
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

// ANTS-4803 — one item in the `pass-headings` dialect: a `#### Pass N.M` block
// with a Status keyword and the body's prose, and NONE of the bullet form's
// trailers, which that format has no slot for.
//
// ANTS-5087 exported it for the read seam. Building a record for a
// pass-headings project out of bulletText() produced an ants-v1 bullet the
// project's file does not contain and never will — with `Kind:` / `Source:` /
// `**Layman:**` lines invented from columns the format cannot carry — so the
// store's records disagreed with the rendered file they are defined against.
QString passBlockText(const RoadmapStore::ItemWrite &it);

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
