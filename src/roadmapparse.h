// ANTS-3764: the roadmap markdown reader, lifted out of
// src/roadmapdialog.cpp so a headless caller can share it.
//
// It was three per-format readers plus a dispatcher with internal linkage in
// an anonymous namespace, returning a record nested inside a QDialog subclass
// — reachable only from ants_dialogs_lib. The roadmap migration (ANTS-3757)
// needs the same reader from ants_core_lib, and the two alternatives were both
// bad: link Widgets into a headless bulk import, or write a SECOND parser for
// the same three formats, whose disagreements would be silent and would be
// about the corpus itself.
//
// Qt6::Core only; lives in ants_core_lib so RoadmapDialog, the remotecontrol
// handlers, the feature tests and the migration all share one implementation.
// Mirrors the call ANTS-2126 already made in the other direction for the
// pass-headings WRITER (src/passheadingwrite.{h,cpp}).
//
// RoadmapDialog keeps its widget code and calls in: RoadmapDialog::BulletRecord
// is an alias for RoadmapParse::BulletRecord and RoadmapDialog::parseBullets()
// forwards here, so no existing call site changed.

#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

namespace RoadmapParse {

// The four status markers roadmap-format.md § 3.3 defines. They are the
// FORMAT's vocabulary rather than the dialog's, so they live with the parser
// and RoadmapDialog uses them from here — one definition, which is the same
// argument ANTS-3764 makes for one reader. The dialog keeps kStatusLabels,
// which is display text and genuinely its own.
inline constexpr const char *kEmojiDone       = "✅";
inline constexpr const char *kEmojiPlanned    = "📋";
inline constexpr const char *kEmojiInProgress = "🚧";
inline constexpr const char *kEmojiConsidered = "💭";

// ANTS-3771 — a project's DECLARED id format, as read from
// .ants/project.json's `id_format` (docs/specs/ANTS-3771-id-format-declaration.md).
// Default-constructed means UNDECLARED, which is every project that ships no
// key and is the zero-change path (INV-1).
//
// It arrives here as a VALUE, never as a path this namespace resolves. That is
// what keeps the reader Qt6::Core-only and free of ProjectSettings: the single
// load is ProjectSettings::idFormatFor(), in ants_core_lib, and every
// project-scoped caller passes the result down (INV-13).
struct IdFormat {
    QString prefix;    // canonical prefix grammar; empty = undeclared. Generative
                       // half — allocation and written-id refusal (§ 2.3). Never
                       // affects reading.
    QString pattern;   // PCRE2, matched against extractBoldId()'s output — the
                       // bold span with its one trailing `.` already chopped and
                       // trimmed, NOT the raw span (§ 2.2). Empty = undeclared.
                       // Recognitional half; never affects allocation.
    bool isDeclared() const { return !prefix.isEmpty() || !pattern.isEmpty(); }
};

// ANTS-3771 § 2.5 — the cap on a declared `pattern`, in bytes of UTF-8. Matches
// the cap spec_conformance already applies to a pattern (ANTS-4108 § 2.4) so
// the two agree on what an over-long pattern is. Exported because
// ProjectSettings::load() and project_settings op:set both enforce it and a
// second literal is a second answer.
inline constexpr int kIdFormatPatternMaxBytes = 512;

// Bullet record surfaced via the `roadmap-query` IPC verb (ANTS-1117).
// One entry per top-level status-emoji-prefixed bullet in document
// order; plain narration bullets without an emoji are omitted (they
// don't have stable IDs and so are out of contract). See
// `docs/specs/ANTS-1117.md` § Acceptance criteria.
struct BulletRecord {
    QString id;          // <PREFIX>-NNNN; empty if no `[<PREFIX>-NNNN]` token (ANTS-1405)
    QString status;      // "✅" | "🚧" | "📋" | "💭"
    QString headline;    // first **bold** chunk after the emoji (≤ 120 chars)
    QString headlineFull; // ANTS-2075 — untruncated headline for locator use
    QString kind;        // value from `Kind:` line; "" if absent
    QStringList lanes;   // values from `Lanes:` line; [] if absent
    QStringList evidence; // ANTS-3382 — file paths from `Evidence:` line; [] if absent
    // ANTS-1154 v2 card-renderer extensions. Additive — older
    // callers that only inspect id/status/headline/kind/lanes
    // see no behaviour change.
    QString layman;      // value from `Layman:` line; "" if absent
    QString body;        // full bullet body (post-emoji, pre-continuation-join)
    // ANTS-4557 / ANTS-4558 — `body` WITHOUT its head line, dedented by the
    // continuation block's common indent. What roadmap_query emits as `body`:
    // the head line is already returned as `headline` / `headline_oneline`,
    // and the indent below the common one is the author's structure.
    QString bodyProse;
    // ANTS-4813 — which trailer keys in `bodyProse` were COMPOSED by the render
    // from their own columns rather than written by an author, lowercased
    // ("layman", "kind", "source", "lanes", "evidence").
    //
    // The two halves of a bullet are edited by different ops — amend_body for
    // stored prose, amend_field for a column — and until this existed a reader
    // could not tell them apart in the text they had just been handed. Passing
    // a composed line back as amend_body's old_text refuses on a string the
    // caller has read verbatim, which two sessions hit independently.
    //
    // Store path only. On the markdown path every line WAS written by an
    // author, so the list is empty and its emptiness is the true answer rather
    // than a missing one.
    QStringList composedTrailers;
    QString sectionHeading;  // text of the most recent ## or ### heading
    int sectionLevel = 0;    // 2 for `##`, 3 for `###`, 0 if no section
    QString sectionSlug;     // sectionHeading → lowercase, non-alnum→`-`
    // ANTS-1428 (adapter mode) — fields populated when the parser
    // engages the GFM-task-list branch. native parses leave them
    // at default (empty/false).
    QString anchor;        // ^prefix-NNNN caret anchor; "" if absent
    bool    synthetic = false;  // id was content-hash-derived, not from a token
    QString format;        // "ants-v1" (default) | "github-task-list"
    // ANTS-1438 — bold-ID token, populated when the GFM-adapter
    // matched a `**...**` prefix at the start of the head. Distinct
    // from `id` because id may be a synthetic content-hash when
    // boldId is empty; when boldId is non-empty, id == boldId.
    // Surfaced through the envelope as `bold_id` so callers can
    // correlate with commit-message prefixes explicitly.
    QString boldId;        // "FW W5 (cont.)", "Sh4", "Audit/FW X2", …
    // ANTS-3764 step 2 — the five fields the roadmap migration needs and the
    // card renderer never did (docs/specs/ANTS-3757-roadmap-migration-read.md
    // § 2.3). Each would otherwise be re-derived by hand from `body`, which is
    // the second bullet parser § 2.3 exists to forbid. Additive: no existing
    // caller reads a field that does not yet exist, so RoadmapDialog,
    // roadmap_query and roadmap_log see no behaviour change.
    // Contract + measurements: tests/features/roadmap_parse_widening/spec.md.
    QString sourceStatus;  // pass-headings: the whole remainder of the winning
                           // `- **Status**:` line, VERBATIM — qualifier tail
                           // and asterisks kept. Matching folds case and
                           // absorbs a leading `*`; storage strips nothing.
                           // "" on the emoji / checkbox paths, where the
                           // marker itself is the status.
    QString source;        // value from `Source:` line; "" if absent
    QString idToken;       // the leading-slot id token AS WRITTEN, without its
                           // brackets and before any acceptance test — so an
                           // off-grammar `[Cl9]` can be quarantined rather
                           // than issued a second identity. Distinct from
                           // `id`, which is positionless (it takes the first
                           // `[<PREFIX>-NNNN]` anywhere in the body) and which
                           // conflates conforming with off-grammar ids. "" if
                           // the leading slot holds no token, or holds a
                           // markdown link (`[Doc](x)` / `[ref]:`).
    QString passDesignator;  // "43.5" / "43.5.B" — the pass heading's own
                             // designator, which the heading regex otherwise
                             // consumes and drops. The one field here with no
                             // consumer, kept because it is the only one
                             // unobtainable after the fact and it makes
                             // ANTS-3757 INV-10 assertable directly:
                             // passIdFromDesignator(passDesignator) == id.
                             // "" off the pass path.
    // ANTS-3808 — the offset into `body` just past the text this parse
    // CONSUMED to build the head line's id and headline; equivalently, the
    // first character of the head line the render will NOT reconstruct.
    // QString positions (UTF-16 code units), as TrailerMatch::offset is.
    // -1 when no headline was assigned OR when the consumed text is not a
    // PREFIX of the body (a bold span sitting mid-prose), which the migration
    // reads as "strip nothing" — see docs/specs/ANTS-3808-*.md § 2.1.
    int headlineEnd = -1;
    // 1-based inclusive span of the source lines this record was built from:
    // the bullet line through its last continuation line, or the `####`
    // heading through the last non-blank line of its block. Trailing blanks
    // are outside the span and two records never overlap — ANTS-3757's INV-11
    // is a partition over these.
    int firstLine = 0, lastLine = 0;
};
// ANTS-4575 — did the reader ADOPT this record's id from a bold prose
// lead-in, rather than read it from a bracket token the author wrote?
//
// Surfaced by roadmap_query as a gated `id_inferred: true`. The
// discriminator is `id == boldId`: that is exactly the branch in
// fillBulletRecord that adopts a bold span as the id, and it holds on the
// native branch too, where ANTS-1987 adopts a head-anchored ID-shaped bold
// token (`- 📋 **Cl9.**`). An adoption is an inference on either branch, so
// this deliberately does NOT test `format`.
//
// It is NOT a test on `idToken`: that field is filled FROM boldId when the
// leading slot holds no bracket, so it cannot separate the two cases.
//
// A synthetic content-hash id reads false — `synthetic` already says that id
// was manufactured, and the two answer different questions.
bool idWasInferred(const BulletRecord &rec);

// The `[PROJ-NNNN]` token shape, as a bare regex fragment (no anchors, no
// capture group). ANTS-1784 — exported rather than file-static because
// roadmapdialog.cpp's CHANGELOG scanner shares it with the bullet parser
// "so they can't drift"; that contract predates this extraction and has to
// survive it.
QString idTokenPattern();

// Classify a roadmap document: "ants-v1" | "github-task-list" |
// "pass-headings". Answers "ants-v1" for input it does not recognise,
// INCLUDING an empty file — so a returned format is no evidence that anything
// was understood (ANTS-3757 § 2.3 relies on this and reports zero items).
//
// ANTS-3766 § 2.1.1 — `sawSignal` reports whether the answer rested on
// EVIDENCE. That the return value alone cannot say so is the whole reason this
// out-parameter exists: a genuine ants-v1 file and a file with no signal at all
// return the identical string, and the caller has to tell them apart to decide
// which sources may take part in the archive_format_mismatch comparison.
//
// `false` ONLY when the scan fell through EVERY classification signal to the
// default. Matching any one of them — the `<!-- ants-roadmap-format: 1 -->`
// marker, a GFM checkbox, an ants-v1 emoji bullet, a `#### Pass N` heading, a
// `- **Status**:` marker — sets it true, whatever is returned. Writing the
// predicate as "matched no bullet" instead would report *no evidence* for a
// bullet-less file that explicitly DECLARES its format, and § 2.1.1's
// inheritance rule would then override that declaration.
//
// Out-parameter rather than a wider return type: RoadmapDialog and
// parseBullets() ask only for the format, and nullptr is the whole cost of
// leaving them untouched.
QString detectRoadmapFormat(const QStringList &lines, bool *sawSignal = nullptr);

// ANTS-3808 § 2.2 — the trailer matchers, asked rather than duplicated. INV-2
// is phrased as "RoadmapParse remains the only bullet grammar", and that is
// this namespace: a consumer that needs a trailer value calls in here instead
// of growing its own regex.
//
// Per key: the value as parseBullets() derives it (§ 2.2.1 pins each
// normalisation step), plus where the match sat and whether it began a line —
// its MATCH POSITION. Deliberately not called "provenance": the store already
// uses that word for per-field write origin, and one term with two meanings is
// how an implementer conflates them. The match position is not decoration —
// ANTS-3809's `body_shadowed` refusal has to name the shadowing sentence, and a
// bare value cannot answer "was this an un-anchored mid-prose match?".
struct TrailerMatch {
    QString value;         // empty when the key is absent
    // ANTS-4542 — `value` with code spans blanked, built by the same steps.
    // It exists because `value` stopped being a contiguous slice of `body`
    // when a WRAPPED value gained its continuation: callers that re-scanned
    // `masked.mid(offset, value.size())` were then reading a window of the
    // wrong length. Same string when the value did not wrap, so the
    // unwrapped path is byte-identical to what it was.
    QString maskedValue;
    // Index of the CAPTURE (`capturedStart(1)`) into `body`, in QString
    // positions — UTF-16 code units, NOT bytes. -1 when absent. Converting
    // this to a UTF-8 offset breaks ANTS-3809's sentence extraction on the
    // first non-ASCII bullet.
    int     offset   = -1;
    // True when the MATCH — `capturedStart(0)`, not `offset` — begins a line.
    //
    // The distinction is the whole field. Every one of the five patterns puts
    // literal text between the line start and group 1 (`^\s*Kind:\s*(…)`,
    // `Lanes:\s*(…)`), so computed off `offset` the field would be false on
    // essentially every key of every bullet.
    bool    anchored = false;
};
struct TrailerValues {
    TrailerMatch layman, kind, source;
    // `value` for these two is the text the split is applied TO, which is not
    // the same step for each: `lanes` splits the raw capture, `evidence`
    // splits after a trim and one trailing-period chop. They are not
    // symmetric — § 2.2.1 pins both exactly.
    TrailerMatch lanes, evidence;
    QStringList  lanesList, evidenceList;   // the split forms parseBullets() assigns
};
// The kind vocabulary (ANTS-4086). It lives here rather than in the migrator
// because trailerValuesIn() applies it, and applying it there is what lets
// every consumer inherit the guard without threading a predicate through five
// call sites — one of which would eventually be missed.
//
// `mappedKind()` keys on a LOWERCASED value and returns "" when unmapped.
// `isRecognisedKind()` is the test trailerValuesIn() uses: canonical or
// mappable, after a trim and a fold.
const QSet<QString> &canonicalKinds();
QString mappedKind(const QString &lower);
bool isRecognisedKind(const QString &raw);

// The five trailer keys as they appear in `body`. parseBullets() assigns its
// own record fields from this call, so the equality INV-4 asserts holds by
// construction rather than by two implementations agreeing.
//
// ANTS-4086: `kind` is resolved against isRecognisedKind(); a capture that is
// not recognised is no candidate, so prose that wraps onto the label cannot
// displace a real declaration. When NO capture is recognised the last match is
// returned raw, so makeItem()'s unmapped branch still sees it.
TrailerValues trailerValuesIn(const QString &body);

// ANTS-3808 § 2.1 (ANTS-4506) — `body` with its TRAILING run of trailer-only
// lines removed: the block § 2.4's render appends, which the next parse would
// otherwise file back into the body. Owned here rather than at the migration's
// call site because INV-2 makes this namespace the only bullet grammar in
// `src/`. A line is dropped only when it is one whole trailer declaration AND
// the only LINE-INITIAL declaration of its key in `body` — that second
// condition is what stops a migrated item rewriting its own column (§ 2.3.1).
// Case-sensitive: these lines are the render's own output.
QString stripTrailingTrailerLines(const QString &body);

// Pure helper: parse `markdownText` into top-level status-emoji bullets.
// Dispatches on detectRoadmapFormat(). Result is read-only; used by the
// `roadmap-query` IPC verb to feed Claude a structured snapshot without
// re-burning the file content as tokens.
// ANTS-3771 — `fmt` is the project's DECLARED id format. The default argument
// keeps a caller that holds no project root (a fixture, an ad-hoc string)
// working, and it is the hazard INV-13 names: a caller that HOLDS a root and
// omits it reads that project's roadmap as undeclared, so one bullet resolves
// to two ids depending on which path reached it. Pass
// ProjectSettings::idFormatFor(root) wherever a root is in hand.
QVector<BulletRecord> parseBullets(const QString &markdownText,
                                   const IdFormat &fmt = {});

// ANTS-3793 § 2.1.1 — the record parseBullets() would build for ONE ants-v1
// bullet, given exactly that bullet's text. The read seam's normative rule is
// "fill each record with what parseBullets() would assign if it parsed
// RoadmapRender::bulletText(item)", and this is the function that makes that
// literally true instead of approximately so: the store reader runs the same
// grammar rather than a second copy of it (ANTS-3808 INV-2).
//
// `bulletText` is one bullet: its `- <emoji> …` line plus any two-space-
// indented continuation lines. Trailing lines beyond the bullet are ignored.
//
// nullopt when the text is not an ants-v1 bullet — no `- `/`* ` marker, or no
// status emoji after it. The second case is load-bearing rather than defensive:
// a `dropped` item renders with an EMPTY status marker (roadmap-format.md
// § 3.11 makes a fifth glyph an anti-pattern), so it produces no record here,
// exactly as it produces none in a document walk. ANTS-3793 § 2.1.2's `dropped`
// exclusion is that behaviour, not a filter layered on top of it.
//
// Three groups of fields are NOT set, because a bullet in isolation cannot
// decide them and the caller can: `sectionHeading` / `sectionLevel` /
// `sectionSlug` (the document's, and the store walk's own — its slugger is
// stateful across sections), and `firstLine` / `lastLine` (a store has no
// lines, which is ANTS-3793 INV-2's one declared field difference).
//
// ANTS-3771 — takes the declaration for symmetry with parseBullets() and so
// INV-6 is TESTABLE rather than asserted: the store path can be driven with a
// declaration in hand and shown to produce the same records. It cannot change
// them — this text is RoadmapRender::bulletText()'s output, which is ants-v1,
// so `gfmHere` is false and the branch the declaration governs never runs.
std::optional<BulletRecord> parseAntsV1Bullet(const QString &bulletText,
                                              const IdFormat &fmt = {});

}  // namespace RoadmapParse
