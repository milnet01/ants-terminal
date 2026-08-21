// ANTS-3793 — the read seam: one reader function, two backends.
// Spec: docs/specs/ANTS-3793-roadmap-consumer-cutover.md
//
// The store has been primary since ANTS-3756 and ANTS-3758 can write markdown
// back out of it, but until this file nothing READ it: every consumer parsed
// ROADMAP.md, so the store was a write-only copy that drifted the moment anyone
// edited the file.
//
// The cutover is not 26 rewrites. Every consumer read of bullet records already
// went through RoadmapParse::parseBullets(), so this adds a sibling PRODUCER of
// the same record type — sourced from the store — plus a resolver that picks
// between them. Each of the 26 sites swaps one call for its owner's wrapper.
//
// Qt6::Core + Qt6::Sql only, in ants_roadmapstore_lib, beside the render whose
// bulletText() § 2.1.1 is defined in terms of.

#pragma once

#include "roadmapparse.h"
#include "roadmapstore.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <optional>

class QFile;

namespace RoadmapSource {

using RoadmapParse::BulletRecord;

// Why the ceiling needs its own channel rather than an *error string:
// INV-3's refusal has to reach an MCP envelope as the code `too_large`, and
// RoadmapDialog — which is not a verb and emits no envelope — has to tell
// "too big, tell the user" apart from "the store is broken". A caller
// branching on error TEXT is a caller that breaks on a reworded message,
// which mcp-error-codes.md exists to forbid.
enum class ReadError {
    None,
    StoreFailed,        // the store file exists and will not open
    SourceUnrecognised, // migrated, but its ROADMAP.md is absent, empty
                        // or unparseable — the STORE is fine, and
                        // reporting this as StoreFailed would send the
                        // user to fix the wrong file (§ 2.2's table)
    TooLarge,
};

// INV-3 — the whole-project read ceiling, in items. Derived in § 4: ~4.5 KiB
// of records per item against a 16 MiB budget is ~3,678, rounded down. It is a
// runaway guard at ~2× the largest real project, not a working limit, and it is
// deliberately a COUNT rather than a byte total — the gate has to decide before
// materialising, which is the thing it exists to guard.
inline constexpr int kItemCeiling = 3500;

// ANTS-3863 § 2.1 — the detector's window, in both of its units.
//
// kDetectorLineCap counts NON-BLANK lines, which is detectRoadmapFormat()'s own
// rule: blank lines are kept in the list and are not counted. That alone bounds
// neither the read nor the list — a file of blank lines, or one with no '\n' at
// all, reaches EOF with the count never rising — so kDetectorByteCap bounds the
// bytes. 1 MiB is ~50x the largest prefix this corpus produces, so no real
// roadmap reaches it, and a pathological one costs a bounded read rather than
// the whole file.
//
// BOTH producers get both caps (§ 2.1). Capping only the file reader would fork
// it from the in-memory helper on exactly the input this item is about, and
// INV-2 — the two return the same QStringList — would be false by construction
// on an all-blank file. They are here rather than in the .cpp because the tests
// assert against them, which is why kItemCeiling above is here too.
inline constexpr int    kDetectorLineCap = 300;
inline constexpr qint64 kDetectorByteCap = 1 << 20;   // 1 MiB

// ANTS-3863 § 2.1 — the roadmap text, read as late as the caller's path needs.
//
// The dispatch needs only the detector's window; the unmigrated path needs the
// whole text. Splitting them behind one object is what lets the dispatch run
// BEFORE any body read, without each of the call sites having to know which of
// the two it will end up needing.
//
// The read contract, stated because three invariants rest on it: a file-backed
// text opens its file ONCE, with ReadOnly | Text — the mode every call site
// this replaced used, and the one that decides CRLF handling, so INV-5's
// byte-identity claim is meaningless without pinning it. It RETAINS the handle
// until full() has run or the object dies. detectionPrefix() reads forward to
// whichever cap it reaches first and stops; full() continues from where the
// prefix stopped and appends rather than seeking back. Hence every byte is read
// at most once (INV-6), prefix and body come from one file description, and a
// mid-read error leaves openFailed() true with full() returning what was read
// before it — which is what QFile::readAll() already does.
class RoadmapText {
public:
    // Move-only: a file-backed text owns an open handle, so a copy would either
    // read the file twice — breaking INV-6 — or double-close it.
    RoadmapText(const RoadmapText &) = delete;
    RoadmapText &operator=(const RoadmapText &) = delete;
    RoadmapText(RoadmapText &&) noexcept;
    RoadmapText &operator=(RoadmapText &&) noexcept;
    ~RoadmapText();

    // Nothing is read at construction. `path` is the project's LIVE roadmap.
    static RoadmapText fromFile(QString path);

    // The caller already holds the text: RoadmapDialog's archive concatenation,
    // every roadmap_log op that walks the file to locate a bullet, and every
    // test. Costs nothing and reads nothing — full() hands back what it was
    // given.
    static RoadmapText fromMemory(QString text);

    // At most kDetectorLineCap non-blank lines OR kDetectorByteCap bytes,
    // whichever comes first, in the detector's own shape. Memoised — INV-6
    // forbids a second read. On a text whose open failed this is an empty
    // QStringList, which is what INV-4's refusal chain (empty prefix ->
    // sawSignal false -> SourceUnrecognised) rests on.
    const QStringList &detectionPrefix();

    // The whole text, memoised. Only the unmigrated path calls it. Empty on a
    // text whose open failed — what today's sites already hand the seam when
    // their own QFile::open() fails.
    const QString &full();

    // True when a file-backed text's file would not open, or when a read that
    // did open failed part-way. FORCES the open if it has not happened yet, and
    // that is load-bearing rather than incidental: every consumer branches on
    // this exactly where it used to branch on QFile::open(), which is BEFORE
    // any accessor has run. Merely reporting a flag some earlier accessor set
    // would return false on the first call and silently lose op:append's
    // roadmap_read_failed refusal. The open it forces is the SAME open
    // detectionPrefix() would have done, so it consumes no bytes of its own.
    //
    // ALWAYS false for a fromMemory() text: it has no file to fail on, so a
    // site that branches on it takes its success path unconditionally.
    bool openFailed();

    // Bytes consumed from the file so far — DISK bytes, not the QString's
    // UTF-16 in-memory size (§ 4 prices that separately, and in different
    // units). ALWAYS 0 for a fromMemory() text, which reads no disk at all, so
    // INV-1's assertion is trivially satisfied there rather than undefined.
    // Exists so INV-1 is assertable rather than argued.
    qint64 bytesRead() const { return m_bytesRead; }

private:
    RoadmapText() = default;
    void ensureOpen();
    void readHead();

    QString m_path;                    // empty ⇒ memory-backed
    bool    m_fileBacked   = false;
    std::unique_ptr<QFile> m_file;
    bool    m_openAttempted = false;
    bool    m_openFailed    = false;
    qint64  m_bytesRead     = 0;

    QByteArray  m_head;                // the bytes detectionPrefix() consumed
    bool        m_headDone   = false;
    QString     m_text;                // memoised body
    bool        m_fullDone   = false;
    QStringList m_prefix;
    bool        m_prefixDone = false;
};

// Stat `defaultPath()`, and construct-and-open only if it is there. Returns an
// owned open store, or nullptr with `*why` set to None (no store on this
// machine — parse markdown) or StoreFailed (present, unopenable).
//
// It is a free function and not a wrapper member for one reason: INV-1's two
// unmigrated-project cases assert exactly this decision, and a member of
// RemoteControl or RoadmapDialog is not reachable from § 6's bundle. The
// wrapper still OWNS the decision — it is the only caller — but the decision is
// testable on its own.
// ANTS-3822 § 2.3.2 — `historyCapBytes` is injectable, defaulted to production.
//
// Not a testing convenience: without it the history cap is reachable only BELOW
// this function, while the response envelope carrying `history_note` is built
// only ABOVE it, so the invariant guarding INV-14's "fails AND reports" could be
// made neither red nor green — the only honest route being 250 MiB of real
// history. ANTS-3756's INV-14 already refuses that trade for its own legs, which
// is why the store takes the bound as a constructor parameter; this passes the
// same choice one layer up. Defaulted, so no existing caller changes.
std::unique_ptr<RoadmapStore> storeFor(const QString &defaultPath,
                                       ReadError *why,
                                       QString *error = nullptr,
                                       qint64 historyCapBytes =
                                           RoadmapStore::kDefaultHistoryCapBytes);

// The records a consumer would have got from parsing this project's rendered
// markdown, sourced from the store instead. Document order (§ 2.1.3), in the
// SAME shape RoadmapParse::parseBullets returns.
//
// Two outcomes only, and NEITHER is a fallback: engaged with `*why` set to
// None, or nullopt with `*why` set to the reason (and `*error` carrying the
// human-readable detail). Reaching this function already means the project is
// migrated, so there is no "parse markdown instead" answer available to it.
// `why` is REQUIRED rather than defaulted, because a caller that drops it
// cannot tell TooLarge from StoreFailed, and it is written on EVERY path so a
// caller may branch on it first.
//
// `includeArchive` mirrors RoadmapDialog::loadMarkdown()'s flag of the same
// name and is REQUIRED for the reason § 2.1.2 gives: without it the store path
// spans archives the markdown path was told to skip, and the dialog's history
// toggle would stop mattering the moment a project migrated.
std::optional<QVector<BulletRecord>>
bulletsFromStore(RoadmapStore &store, qint64 projectId,
                 bool includeArchive, ReadError *why,
                 QString *error = nullptr,
                 const RoadmapParse::IdFormat &fmt = {});   // ANTS-3771 — see bulletsFor()

// § 2.2's dispatch. Three outcomes, not two:
//   engaged               → migrated; read the store at this projectId
//   nullopt, *error empty → not migrated; parse markdown as today
//   nullopt, *error set   → REFUSE; never fall back (INV-1)
//
// `markdown` is REQUIRED and is the project's live roadmap text: § 2.2's
// ants-v1 gate runs detectRoadmapFormat() over it. ANTS-3815 § 2.4 added
// project.source_format as a SECOND witness — the store's record of the dialect
// the migration read — and the file's testimony is still needed to compare
// against it. Four outcomes for a project that has a store row:
//
//   source_format   live detect          outcome
//   ''              any                  exactly as version 1 (§ 2.2's table)
//   set             sawSignal false      refuse, SourceUnrecognised
//   set, == live    sawSignal set        § 2.2's table: store if ants-v1, else markdown
//   set, != live    sawSignal set        refuse, SourceUnrecognised — the drift case
//
// The last row is new at ANTS-3815 and is a BEHAVIOUR CHANGE at all three
// callers on exactly one input class: a migrated project whose live file changed
// dialect is now refused where it was previously served markdown. § 2.2 already
// refuses "a file that no longer looks like what the store says it is"; with one
// witness that could only reach the UNRECOGNISABLE case, because an
// unrecognisable file is the only disagreement a lone witness can have with
// itself.
//
// ANTS-3863 BOUNDED that file read rather than removing it, and its § 6 makes
// the distinction permanent: removing the open outright would leave this
// comparison with one witness again and make ANTS-3815 INV-6 unenforceable, so
// a store-only dispatch is rejected rather than deferred. What it costs now is
// the detector's window — at most kDetectorLineCap non-blank lines or
// kDetectorByteCap bytes — instead of the whole file. This function must never
// call `text.full()`, and neither may any of its four callers; that is the
// whole of ANTS-3863's saving (its § 2.2 table binds all five).
//
// `why` is defaulted here and required on the two functions above, and the
// asymmetry is deliberate rather than an oversight. This function's refusals
// come in two kinds that send the user to different places — a roadmap that no
// longer looks like what the store says it is (SourceUnrecognised) and a store
// query that failed outright (StoreFailed) — so bulletsFor() has to be able to
// tell them apart to honour § 2.2's table. A caller content with "engaged or
// not" may still drop it.
std::optional<qint64> migratedProject(RoadmapStore &store,
                                      const QString &projectRoot,
                                      RoadmapText &text,
                                      QString *error = nullptr,
                                      ReadError *why = nullptr);

// The library seam the two owner wrappers call — the two above are its halves,
// exposed because the tests drive them separately.
//
// Its three outcomes are the dispatch's, one layer on:
//   engaged               → the store's records (migrated project)
//   nullopt, *why == None → NOT migrated. The caller parses `markdown`
//                           itself, exactly as it does today. This is the
//                           common case and it is not an error.
//   nullopt, *why != None → REFUSE and surface it. Never parse.
//
// `text` is the caller's roadmap text provider, used by the gate above and by
// the caller on the unmigrated path. ANTS-3863: it is READ here, lazily and
// under both caps, rather than arriving already read — but only ever through
// detectionPrefix(). This function never calls full(), so an unmigrated
// caller's own text.full() is the first and only body read on that path.
std::optional<QVector<BulletRecord>>
//
// ANTS-3771 — `fmt` is the project's DECLARED id format, from
// ProjectSettings::idFormatFor(projectRoot). It arrives as a VALUE and is not
// loaded here: this library links `Qt6::Core Qt6::Sql ants_roadmapparse_lib`
// and deliberately NOT ants_core_lib (ANTS-3808 § 4), which is where
// ProjectSettings lives — reading .ants/project.json inside the seam would
// invert that edge.
//
// It cannot change what this function returns, and carrying it is how INV-6
// says so testably: every record here is derived from
// RoadmapRender::bulletText(), which emits ants-v1, so the GFM bold branch the
// declaration governs never runs on the store path. A caller passes the same
// value it would pass to parseBullets() on the unmigrated path, so one call
// site holds one declaration for both outcomes (INV-13).
bulletsFor(RoadmapStore &store, const QString &projectRoot,
           RoadmapText &text, bool includeArchive, ReadError *why,
           QString *error = nullptr,
           const RoadmapParse::IdFormat &fmt = {});

// § 2.3 — a project's stored legend, re-keyed from the lifecycle WORDS the
// store holds ("planned", "shipped", …) to the status EMOJI every consumer of
// a BulletRecord is keyed by. Unparseable or empty text yields an empty hash,
// which § 2.3's second row makes meaningful: a migrated project with no stored
// legend shows none.
//
// It lives here rather than in the dialog because it is a store-read concern
// and because the dialog's copy would be unreachable from a test: ProjectRow
// carries `legendText` as the RAW stored JSON (ANTS-3761's byte-identity
// contract keeps it unparsed) and no declared reader returns it parsed.
QHash<QString, QString> legendByEmoji(const QString &legendText);

} // namespace RoadmapSource
