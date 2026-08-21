// ANTS-3833 — shared helpers for the remotecontrol translation units.
//
// src/remotecontrol.cpp was one 24,803-line TU. Splitting it into eleven
// per-family TUs breaks every helper that used to resolve through the shared
// anonymous namespace, so they live in `namespace rcdetail` instead and the
// cross-TU ones are declared here.
//
// The list is DERIVED, not hand-written (spec § 2.3): of the 165 symbols
// defined across the file's 24 anonymous namespaces, these 91 are referenced
// from more than one slice. The spec's "~74" was a deliberate estimate from a
// coarser regex; re-derive rather than trusting either number, because the
// set changes the first time a verb moves. The linker is the arbiter (INV-7).
//
// TWO RULES THIS HEADER EXISTS TO HOLD.
//
//  1. A definition stays in a .cpp. Only the three structs and nothing else
//     move their text here, and only because a type used by value across TUs
//     must have one definition visible to each. The test tree scrapes the
//     remotecontrol sources for helper text (337 sites), and anything whose
//     text leaves those files disappears from every such assertion silently.
//
//  2. A constant is declared `extern const`, never bare `const`. A
//     namespace-scope const has INTERNAL linkage, so a bare declaration gives
//     every TU its own private copy and still links clean — INV-7's linker
//     check is blind to it. The definitions keep their `constexpr` spelling
//     in the .cpp: an extern declaration seen first gives them external
//     linkage without touching a byte of the definition, which is what keeps
//     the three tests that pin `constexpr int kWorkspaceSearch…` green.
//
// INV-5: included ONLY by the files listed in ANTS_RC_SOURCES.

#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

#include "coldeyesengine.h"
#include "indiereviewengine.h"
#include "roadmapdialog.h"
#include "roadmapindex.h"
// ANTS-3833 commit 1b — the promoted roadmap helpers below take nested types
// (RoadmapWrite::Result, RoadmapRender::Outcome, RoadmapStore::ItemWrite,
// RoadmapParse::BulletRecord), and a nested type cannot be forward-declared
// without its enclosing definition. remotecontrol.cpp already included all
// four, so TU 1 pays nothing new; the other TUs pay for the shared header.
#include "roadmapparse.h"
#include "roadmaprender.h"
#include "roadmapsource.h"
#include "roadmapstore.h"
#include "roadmapwrite.h"

class MainWindow;
namespace rcdetail {
// ---- types (definitions forced here: used by value across TUs) ----
// ANTS-1428 Tier 2 — GFM-format bullet walker for the flip locator.
// Single forward pass over the file, fence-tracked, recording each
// top-level `- [ ]` / `- [x]` bullet with the data the locator and
// the surgery step need:
//   - firstLine: 0-based index of the checkbox line (where the
//     status token lives).
//   - headlineLine: 0-based index of the last line of the bullet's
//     headline content (where an anchor would be appended). Equal to
//     firstLine for a single-line bullet; later for a multi-line
//     bullet whose headline spans continuation lines before any
//     metadata key (Lanes: / Kind: / Source: / Layman:) or blank.
//   - status / headline / boldId / anchor extracted per the same
//     rules as roadmapdialog.cpp's GFM branch.
//   - insideFenced: true iff the checkbox line was reached while
//     inside an open ```...``` block — surfaces the
//     anchor_unsafe_context refusal.
//   - fenceOpenLine: 0-based index of the line that opened that fence,
//     -1 when not fenced. ANTS-3640 — the refusal reports it, because
//     the opener is the line the caller has to repair.
struct GfmBullet {
    int     firstLine     = -1;
    int     headlineLine  = -1;
    QString boldId;
    QString anchor;
    QString status;        // "✅" | "📋" | "🚧" | "💭"
    QString headline;      // first non-empty line text (post-strip)
    bool    insideFenced   = false;
    int     fenceOpenLine  = -1;
};

// ANTS-1441 — ants-v1 native bullet support for op:"flip". GFM
// path (above) requires either a `**Bold-ID.**` token or a caret
// `^anchor`; ants-v1 bullets carry a canonical bracket-ID
// `[PREFIX-NNNN]` right after the status emoji, so no anchor
// injection is needed and no counter is consumed. Simpler than GFM.
//
// Recognised line shape:
//   `- <emoji> [<PREFIX-NNNN>] <headline...>`
// where <emoji> is one of ✅ 📋 🚧 💭.
struct AntsV1Bullet {
    int     firstLine    = -1;
    QString id;            // e.g. "ANTS-1394"
    QString status;        // "✅" | "📋" | "🚧" | "💭"
    QString headline;      // post-strip; "**" wrappers removed
    bool    insideFenced   = false;
    int     fenceOpenLine  = -1;   // ANTS-3640 — see GfmBullet
};

struct VerifyGitSnapshot {
    bool    valid = false;
    QString head;            // 40-hex commit SHA
    QString statusSha;       // SHA256-hex16 of `git status --porcelain=v1 -z`
    // ANTS-3373 — repo-relative source files newly added in the working
    // tree (index-add `A` or untracked `??`), parsed out of the same
    // porcelain output. Feeds VerifyEngine::findUnreferencedSources.
    QStringList addedSources;
};

// ---- constants (definitions stay in the .cpp; see rule 2 above) ----
extern const int kRoadmapQueryBodyCap;
extern const int kRoadmapQueryBodyStoreCap;
extern const QRegularExpression kIdPrefixShape;
extern const int kRcMaxNoteChars;
extern const int kWorkspaceSearchHardKillMs;
extern const int kWorkspaceSearchMinBudgetMs;
extern const int kWorkspaceSearchMaxBudgetMs;
extern const int kWorkspaceSearchKillGraceMs;
extern const int kWorkspaceSearchMaxResultsCap;
extern const int kWorkspaceSearchMaxColumns;
extern const int kWorkspaceSearchThreads;
extern const int kWorkspaceSearchStderrCapBytes;
extern const int kWorkspaceSearchGlobBytesCap;
extern const int kDefaultMaxMatchBytes;
extern const char kFeedbackSuffix[];

// ---- file-scope statics promoted by the cut (ANTS-3833 § 2.5 commit 1b) ----
//
// These were never in an anonymous namespace, so the first promotion pass —
// which walked the 24 anonymous blocks — did not see them. A file-scope
// `static` has internal linkage just the same, which is invisible while the
// implementation is ONE translation unit and an undefined reference the
// moment it is eleven. Found by cutting and reading the linker, which is what
// INV-7 says the arbiter is.
//
// Nineteen of the file's 36 file-scope statics are referenced from a slice
// other than the one defining them; only those are promoted. The definitions
// keep every byte except the `static` keyword, which becomes an `rcdetail::`
// qualification — so the cut that follows stays pure motion.
extern bool g_forceCounterCommitFail;

QString rlCanonicalToStatus(const QString &toStatus);
QJsonDocument cmdRoadmapLogPassAppend(const QJsonObject &req, const QString &roadmapPath, const QString &markdown);
QJsonDocument cmdRoadmapLogPassAppendBatch(const QJsonObject &req, const QString &roadmapPath, const QString &markdown);
QJsonDocument cmdRoadmapLogPassFlip(const QJsonObject &req, const QString &roadmapPath, const QString &markdown);
QJsonDocument cmdRoadmapLogPassFlipBatch(const QJsonObject &req, const QString &roadmapPath, const QString &markdown);
bool rcRoadmapIdLess(const QString &a, const QString &b);
QString rcExtractGateNote(const QString &body);
bool rcRoadmapSourceRefused(QJsonObject &out, RoadmapSource::ReadError why, const QString &err);
bool rcRoadmapWriteRefused(QJsonObject &out, RoadmapWrite::Result r, const QString &err, const RoadmapRender::Outcome &outcome);
// ANTS-4463 — the dry-run field policy, in ONE place because nine sites emit
// these fields and nine copies of a rule is nine chances to miss one.
//
// A dry run writes nothing, and the envelope said `files_written:[...]` and
// `note_appended:true` anyway — field names whose tense IS the assertion. The
// only signal that nothing happened was the `dry_run` flag beside them, so a
// caller branching on `files_written` (the obvious field) read a preview as a
// completed write. Reported by LocalWebServerManager, who verified three ways
// that nothing was written.
//
// On a dry run the past-tense names are ABSENT — their reporter's argument,
// that an absent field cannot be misread while a renamed one still has to be
// noticed — and the same information is carried under `would_write`, matching
// what changelog_log's dry_run already documents for `bytes`. Callers get both
// properties: the misleading name is gone, and the useful value is kept.
void rcRoadmapWriteFields(QJsonObject &out, const RoadmapRender::Outcome &outcome, bool dryRun);
// ANTS-3822 § 2.3.1 — one operation's history state, created at the top of
// mutate() and threaded through everything that writes an item column.
//
// It exists because the six rows of § 2.1's worked example (a body plus five
// re-derived trailer columns) are written from TWO places: the handler sets
// status/body directly, and rlDeriveTrailerColumns() below sets the rest. That
// helper is shared by three call sites and had no way to receive the stamp, so
// without a carrier the trailer rows either go unwritten — one row for an
// amend_body, when § 2.1 requires six — or land under a SECOND stamp, splitting
// one write into two revisions and breaking the last-touch reader ANTS-3822
// exists to unblock.
//
// Rows are COLLECTED and flushed once, never written as they are recorded.
// § 2.3: the store's cap predicate is per row, so writing eagerly lets a long
// old_value be refused while a shorter later row for the same item still fits,
// leaving a revision holding some of its fields and claiming all of them.
struct HistoryContext {
    struct Row {
        qint64  itemPk = 0;
        QString field;
        // Null (not empty) where the column held or holds nothing: appendHistory()
        // binds a null QVariant on isNull(), and ANTS-3761's column diff reads
        // '' and NULL as different values.
        QString oldValue, newValue;
    };

    QString        changedAt;      // § 2.5 — one stamp for the whole op
    QVector<Row>   pending;
    int            skippedRows = 0;   // what history_note reports

    void record(qint64 itemPk, const QString &field,
                const QString &oldValue, const QString &newValue) {
        pending.push_back({itemPk, field, oldValue, newValue});
    }
    qint64 pendingBytes() const {
        qint64 n = 0;
        for (const Row &r : pending)
            n += r.field.size() + r.oldValue.size() + r.newValue.size();
        return n;
    }
};

// ANTS-3822 § 2.5 — one operation's changed_at, in the shape the history DDL's
// CHECK ... GLOB pins (YYYY-MM-DDThh:mm:ssZ). Deliberately the SAME expression
// the migration's stamp uses (remotecontrol_roadmap_migrate.cpp), so the two
// producers of history rows cannot format one column two ways; verified against
// the GLOB rather than assumed.
QString rlHistoryStamp();

// ANTS-3822 § 2.3 — write the op's collected rows, or none of them.
//
// Asks historyWouldExceedCap() ONCE for the whole batch. Over the cap: writes
// nothing, sets hist.skippedRows, and returns TRUE — the item write stands and
// the op reports success with a history_note, because an audit trail that takes
// the roadmap down with it is the opposite of an audit trail.
//
// Returns FALSE only on a failure that is NOT the cap (a constraint violation, a
// closed database), which aborts the op. Both branches are required: swallowing
// the second is the silently-dropped revision INV-14 forbids, and refusing on
// the first turns a full table into a broken roadmap.
bool rlFlushHistory(RoadmapStore &store, HistoryContext &hist, QString *error);

// ANTS-3822 § 2.3.1 — attach `history_note` when the flush skipped rows, and
// nothing at all otherwise (absent, never empty).
//
// A helper rather than three spellings of the key: the note is a response-
// envelope contract that the feature test and any MCP consumer bind to, and
// three sites each formatting their own string is how they stop agreeing. The
// count is of ROWS — § 2.3.1 pins the unit, because § 2.1 makes "revision" a
// group of rows and the same capped amend_body is 1 revision, 1 item or 6 rows.
void rlAttachHistoryNote(QJsonObject &env, const RoadmapStore &store,
                         const HistoryContext &hist);

// ANTS-4549 — caller prose is opaque: refuse text that declares a trailer key
// anywhere but the shape the render writes one. `argName` is the argument the
// text arrived in, and is named in the message: the same rule guards `note`
// (annotate / flip / flip_batch) and `new_text` (amend_body, ANTS-4576).
bool rlNoteDeclaresTrailer(const QString &note, QString *error,
                           const char *argName = "note");
// `kept` (ANTS-4576, optional) collects the NOT NULL columns whose declaration
// the new body dropped and whose value therefore survives — the one outcome a
// caller cannot see in their own diff.
//
// `code` (ANTS-4577, optional) carries the refusal code for the one failure
// here that is the CALLER's and not the engine's: a body declaring a `Kind:`
// no vocabulary recognises. Every mutate failure reaches the envelope through
// commitAndRender() and rcRoadmapWriteRefused(), which maps the whole class to
// `store_failed` — so without this the caller reads an engine fault for their
// own text, and retry is the wrong response to both readings. Set only on that
// refusal; left untouched otherwise, so an unset value still means store_failed.
bool rlDeriveTrailerColumns(RoadmapStore &store, qint64 itemPk, const RoadmapStore::ItemWrite &before, const QString &newBody, const QSet<QString> &supplied, HistoryContext *hist, QString *error, QStringList *kept = nullptr, QString *code = nullptr);
QString rlAppendBodyNote(const QString &body, const QString &note);
std::optional<qint64> rlStoreItemPk(RoadmapStore &store, qint64 projectId, const RoadmapParse::BulletRecord &rec, QString *code, QString *error);
qint64 rlStoreIdHighWater(RoadmapStore &store, qint64 projectId, const QString &projectRoot, const QString &prefix);
// ANTS-3863 § 2.4 — the one helper BELOW the seam this item touches. `text` is
// consulted only on the last fallback, after an explicit id_prefix and the
// store's own idPrefixFor() have both come up empty, so the common migrated
// op:append pays no body read while the rare prefix-sniff still gets the whole
// file.
QString rlStoreCounterPrefix(RoadmapStore &store, qint64 projectId, const QString &idPrefixArg, RoadmapSource::RoadmapText &text, const QString &callerCanonical);
bool rlFillItemBody(const QJsonObject &bulletReq, RoadmapStore::ItemWrite &w, QStringList &scrubbedNames, QString *error);
QString changelogMalformedAdvisory(int line, bool plural, bool applied);
QStringList rcShortBareAltTerms(const QString &pattern);
bool rcLooksLikeRegexButLiteral(const QString &pattern);
bool rcContainsHtmlEntity(const QString &pattern);

// ---- functions (definitions stay in the .cpp) ----
QString resolveRootCanonical(MainWindow *main);
// The two-arg overload. Declared separately and deliberately: the first
// promotion pass derived this header by SYMBOL NAME, so an overload set with
// one member already declared read as complete. 45 of the 85 residual errors
// from the first cut attempt were this one function.
QString resolveRootCanonical(MainWindow *main, const QJsonObject &req);
QString findRoadmapUnder(const QString &canonicalRoot);
QString findChangelogUnder(const QString &canonicalRoot);
QString findYamlChangelogUnder(const QString &canonicalRoot);
const QString &kUnrecognisedFormatHint();
QJsonArray kUnrecognisedFormatExpected();
bool rcBulletsArePassHeadings(const QVector<RoadmapDialog::BulletRecord> &parsed);
QJsonDocument rcPassHeadingsWriteRefusal(const QString &path, const QString &op);
void rcSetBodyFields(QJsonObject &o, const QString &body, int cap = kRoadmapQueryBodyCap);
void rcCapBodyFields(QJsonArray &arr, int cap);
void rcStripBodyFields(QJsonArray &arr);
void rcClipMatchTextFields(QJsonArray &matches, int cap);
void rcApplyHeadlineOnly(QJsonArray &matches);
QString rlDetectStablePrefixId(const QString &markdown);
bool rlRoadmapHasAnyBulletId(const QString &markdown);
QString rlDetectCounterPrefix(const QString &markdown);
QString rlResolveCounterPrefix(const QString &idPrefixArg, const QString &markdown, const QString &callerCanonical);
qint64 rlMaxExistingIdForPrefix(const QVector<RoadmapDialog::BulletRecord> &bullets, const QString &pfx);
QStringList rcSectionChildSlugs(const QVector<RoadmapIndex::Section> &index, const RoadmapIndex::Section &sec);
QJsonDocument rcSectionHasSubsectionsRefusal(const QString &slug, const QStringList &children);
void rcProjectHeadlineOnly(QJsonArray &arr);
void rcProjectChangelogHeadlineOnly(QJsonArray &arr);
QString rcHeadlineOneline(const QString &headline);
void rcMaybeEmitHeadlineFull(QJsonObject &o, const RoadmapDialog::BulletRecord &b);
void rcMaybeEmitEvidence(QJsonObject &o, const RoadmapDialog::BulletRecord &b);
bool rcReturnHeadlineOnly(const QJsonObject &req);
QJsonObject rcCompactBullet(const QString &id, const QString &statusWord, const QString &headline);
QString rcStatusWord(const QString &emoji);
QString rcSanitizeBulletField(const QString &in, int maxLen);
QString rcComputeSectionEtag(const QString &slice);
QJsonObject rcSectionShape(const QString &slice);
QJsonArray rcFilterDuplicateIdsForSection(const QJsonArray &dupes, const QString &sectionSlug);
QJsonArray rcComputeDuplicateIds(const QJsonArray &bullets);
QString rcFenceOpenerHint(int fenceOpenLine);
void rcScrubLeakedToolXml(QString &text, QStringList &scrubbedNames);
QString rcRightStrip(QString s);
void rcSetWriteBytes(QJsonObject &out, qint64 before, qint64 after);
int appendBodyNote(QStringList &lines, int headlineLine, const QString &note, bool *alreadyPresent = nullptr);
int amendBodyExact(QStringList &lines, int headlineLine, const QString &oldText, const QString &newText, int *matchedLine, bool *wrapped = nullptr);
QJsonObject buildHeaderInventoryEnvelope(const QVector<RoadmapIndex::Section> &index, const QString &path, qint64 bytes);
quint64 rcFnv1a64(const QString &normalised);
QString rcNormaliseHeadline(const QString &raw);
QString rcStructuralStem(const QString &headline);
bool rcIsNonconformingIdToken(const QString &tok);
double rcHeadlineJaccard(const QSet<QString> &tokA, const QSet<QString> &tokB, int minShared = 2);
QJsonArray rcComputePossibleDuplicates(const QVector<RoadmapDialog::BulletRecord> &existing, const QString &newHeadline);
QString rcGfmCanonicalHeadline(const QString &rawHead);
QSet<quint64> rcGfmHeadlineMatchHashes(const QString &rawHead, const QString &boldId);
QVector<GfmBullet> walkGfmBullets(const QStringList &lines);
void applyGfmFlip(QStringList &lines, const GfmBullet &b, const QString &statusEmoji, const QString &anchorToInject);
QVector<AntsV1Bullet> walkAntsV1Bullets(const QStringList &lines);
void applyAntsV1Flip(QStringList &lines, const AntsV1Bullet &b, const QString &targetEmoji);
QJsonObject wsErr(const char *code, const QString &message);
bool isValidSpecId(const QString &id);
QString resolveSpecRelForId(const QString &rootCanonical, const QString &dirRel, const QString &id);
QJsonObject fbErr(const QString &code, const QString &message);
QString feedbackCallerLeaf(const QJsonObject &req);
QJsonObject fbNotFound(const QString &message, const QString &resolved, const QString &callerLeaf = QString());
bool resolveFeedbackPath(const QJsonObject &req, const QString &toolName, QString &resolvedOut, bool &existsOut, QJsonObject &err, bool *derivedOut = nullptr);
QJsonObject gitErr(const char *code, const QString &message, const QByteArray &stderrTail = {});
bool isValidRange(const QString &range);
void parseStatusHeader(const QString &headerLine, QJsonObject &out);
QJsonObject runStatusOp(MainWindow *main, const QJsonObject &req);
QJsonObject runLogOp(MainWindow *main, const QJsonObject &req);
QJsonObject runDiffOp(MainWindow *main, const QJsonObject &req);
QByteArray runGit(const QString &root, const QStringList &argv);
QJsonObject csErr(const QString &code, const QString &message);
QJsonObject irErr(const QString &code, const QString &message);
VerifyGitSnapshot collectGitSnapshot(const QString &root);
QJsonObject ceErr(const QString &code, const QString &msg);
QString ceSanitiseEcho(const QString &raw);
QJsonObject ceFindingToJson(const IndieReviewEngine::CorroboratedFinding &f);
QJsonArray ceLaneArrayToJson(const QList<ColdEyesEngine::Lane> &lanes);

}  // namespace rcdetail
