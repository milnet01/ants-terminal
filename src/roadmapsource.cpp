// ANTS-3793 — see roadmapsource.h for the seam's shape and why it exists.

#include "roadmapsource.h"

#include "roadmapindex.h"    // RoadmapIndex::uniqueSlug() — the stateful slugger
#include "roadmaprender.h"   // bulletText() — ANTS-3808 § 2.4's export

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace RoadmapSource {

namespace {

// UTF-8 length of a QString without materialising the QByteArray. The byte cap
// is a cap on DISK bytes, so the in-memory producer has to measure in the same
// unit as the file reader or the two cut at different places — which is INV-2's
// "applies kDetectorByteCap to one producer and not the other" break clause.
qint64 utf8Len(QStringView s) {
    qint64 n = 0;
    for (qsizetype i = 0; i < s.size(); ++i) {
        const char16_t c = s[i].unicode();
        if (c < 0x80)
            n += 1;
        else if (c < 0x800)
            n += 2;
        else if (QChar::isHighSurrogate(c) && i + 1 < s.size()
                 && QChar::isLowSurrogate(s[i + 1].unicode())) {
            n += 4;
            ++i;   // the pair is one code point
        } else
            n += 3;
    }
    return n;
}

// § 4 — detectRoadmapFormat() takes a QStringList while this seam holds a
// QString, so a naive markdown.split('\n') of a 3 MiB roadmap materialises
// another ~6 MiB plus ~34k QString headers for lines nothing reads. The
// detector stops at 300 NON-BLANK lines (and returns at the format marker long
// before that on a rendered file), so it is handed exactly that many.
//
// Blank lines are kept but not counted, which is the detector's own rule
// (`if (ln.trimmed().isEmpty()) continue;` sits above `if (++seen >= 300)`), so
// the prefix and the whole file classify identically.
//
// ANTS-3863 § 2.1 — the byte cap joins it, because the line cap bounds neither
// the read nor this list: blank lines are kept and not counted, so an all-blank
// file walks to EOF with `nonBlank` never rising. This is the ONE producer of
// the prefix — RoadmapText::detectionPrefix() feeds its bounded head straight
// back through here — so the two cannot cut at different places (INV-2).
//
// Capping here changes no classification: detectRoadmapFormat() skips blank
// lines and breaks at its own `++seen >= 300` (roadmapparse.cpp), so it never
// examines a line these caps would drop. The helper has been returning list
// elements no consumer reads.
QStringList detectionPrefixOf(const QString &markdown) {
    QStringList lines;
    int nonBlank = 0;
    qint64 bytes = 0;
    qsizetype start = 0;
    while (nonBlank < kDetectorLineCap) {
        const qsizetype nl = markdown.indexOf(QLatin1Char('\n'), start);
        const QString line = (nl < 0) ? markdown.mid(start)
                                      : markdown.mid(start, nl - start);
        // Truncation lands on the last COMPLETE line at or before the cap, so
        // the detector is never handed a half-line — it sees a short list,
        // which is the case it already handles. A first line that alone exceeds
        // the cap therefore yields an empty list, which is § 5's recorded
        // refusal class (sawSignal false -> SourceUnrecognised) rather than an
        // unbounded read.
        const qint64 lineBytes = utf8Len(line) + (nl < 0 ? 0 : 1);
        if (bytes + lineBytes > kDetectorByteCap)
            break;
        lines.append(line);
        bytes += lineBytes;
        if (!line.trimmed().isEmpty())
            ++nonBlank;
        if (nl < 0)
            break;
        start = nl + 1;
    }
    return lines;
}

// One item's record, built by the ONE rule § 2.1.1 makes normative: what
// parseBullets() would assign if it parsed this item's rendered bullet, under
// the section heading that item's section renders.
//
// Nothing is copied from a column that the head line also carries — not `id`,
// not `status`, not `idToken`. That is not fastidiousness: `rxId` takes the
// first `[<PREFIX>-NNNN]` anywhere in the body, an item with an empty `id`
// column renders no bracket at all, and `status` is a lifecycle WORD in the
// store and an emoji on the record. Copying any of the three fails INV-2 on
// bullets this corpus actually contains.
//
// Appends nothing for a `dropped` item, and that is the whole of § 2.1.2's
// exclusion: emojiFor() returns an empty string for `dropped` by design, so
// bulletText() emits a head line with no status marker and parseAntsV1Bullet()
// declines it exactly as a document walk would skip it.
void appendRecord(QVector<BulletRecord> &out, const RoadmapStore::ItemWrite &it,
                  const QString &heading, int level, const QString &slug,
                  const RoadmapParse::IdFormat &fmt) {
    auto rec = RoadmapParse::parseAntsV1Bullet(RoadmapRender::bulletText(it), fmt);
    if (!rec)
        return;
    rec->sectionHeading = heading;
    rec->sectionLevel   = level;
    // The parser assigns a slug only under a real heading, so a bullet above
    // the first one — every preamble item, under § 2.8's level-0 synthetic root
    // — keeps an empty slug and level 0. Copying SectionRow::title there is one
    // of INV-2's named break clauses.
    if (!heading.isEmpty())
        rec->sectionSlug = slug;
    // ANTS-4813 — same rule bulletText() applies when it decides to emit a
    // trailer line: a column is COMPOSED when it has a value and the stored
    // body does not declare that key at a line start. Computed from the store
    // row rather than by diffing the rendered text, so the record cannot
    // disagree with the renderer about which lines it wrote.
    {
        const RoadmapParse::TrailerValues tv =
            RoadmapParse::trailerValuesIn(it.body);
        const auto shadows = [](const RoadmapParse::TrailerMatch &m) {
            return m.offset >= 0 && m.anchored;
        };
        if (!it.layman.isEmpty()   && !shadows(tv.layman))
            rec->composedTrailers << QStringLiteral("layman");
        if (!it.kind.isEmpty()     && !shadows(tv.kind))
            rec->composedTrailers << QStringLiteral("kind");
        if (!it.source.isEmpty()   && !shadows(tv.source))
            rec->composedTrailers << QStringLiteral("source");
        if (!it.lanes.isEmpty()    && !shadows(tv.lanes))
            rec->composedTrailers << QStringLiteral("lanes");
        if (!it.evidence.isEmpty() && !shadows(tv.evidence))
            rec->composedTrailers << QStringLiteral("evidence");
    }
    // firstLine / lastLine stay 0: a store has no lines to number, and no walk
    // can invent them. INV-2's one declared field difference.
    out.append(*rec);
}

} // namespace

// ── ANTS-3863 § 2.1 — RoadmapText ────────────────────────────────────────────
//
// The header carries the contract; what is worth saying here is why each of the
// three reads below is shaped the way it is.

RoadmapText::RoadmapText(RoadmapText &&) noexcept = default;
RoadmapText &RoadmapText::operator=(RoadmapText &&) noexcept = default;
RoadmapText::~RoadmapText() = default;

RoadmapText RoadmapText::fromFile(QString path) {
    RoadmapText t;
    t.m_path       = std::move(path);
    t.m_fileBacked = true;
    return t;   // nothing is read here — that is the whole point of the type
}

RoadmapText RoadmapText::fromMemory(QString text) {
    RoadmapText t;
    t.m_text     = std::move(text);
    // It IS the whole text, so full() has nothing to do and detectionPrefix()
    // derives from it. No file, therefore openFailed() is always false and
    // bytesRead() is always 0.
    t.m_fullDone = true;
    return t;
}

void RoadmapText::ensureOpen() {
    if (!m_fileBacked || m_openAttempted)
        return;
    m_openAttempted = true;
    m_file = std::make_unique<QFile>(m_path);
    // ReadOnly | Text — the mode every call site this replaced used, and the
    // one that decides CRLF translation, which is what INV-5 compares against.
    if (!m_file->open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_openFailed = true;
        m_file.reset();
    }
}

void RoadmapText::readHead() {
    if (m_headDone)
        return;
    m_headDone = true;
    ensureOpen();
    if (!m_file)
        return;   // unopenable: an empty head, hence an empty prefix (INV-4)

    // The loop runs while the head is at or under the cap, so it exits having
    // read at least ONE BYTE PAST it — and that overshoot is load-bearing
    // rather than slack. detectionPrefixOf() cuts at the last COMPLETE line at
    // or before the cap, so it has to be able to see that the next line
    // crosses. Handed exactly the cap it would read the crossing line's
    // truncated head as a line that fitted and keep it, while the same helper
    // over the whole file would drop it — the two producers disagreeing, which
    // is INV-2. (Measured: capping the read at exactly the cap made the
    // all-blank fixture return 1048577 lines from the file and 1048576 from
    // memory, because the head then ended on a newline and the helper appended
    // the empty line that follows it.)
    int nonBlank = 0;
    while (nonBlank < kDetectorLineCap && m_head.size() <= kDetectorByteCap) {
        // A line at a time rather than a fixed chunk: a chunk overshoots the
        // 300th non-blank line by up to its own size, and INV-1's bound is
        // stated against that line's end plus one line. The per-call ceiling
        // bounds the OTHER pathological input — a file with no '\n' at all — so
        // the overshoot past the cap is one capped line and not the file.
        const QByteArray line = m_file->readLine(kDetectorByteCap + 2);
        if (line.isEmpty())
            break;   // EOF, or a read that failed — m_file->error() below
        m_head += line;
        // Decoded rather than byte-scanned, so "blank" means the same thing
        // here as in detectionPrefixOf()'s QString::trimmed(): a line of
        // U+00A0 is blank to one and not the other, and a reader that stops
        // early cuts the head before the helper's own cut point.
        if (!QString::fromUtf8(line).trimmed().isEmpty())
            ++nonBlank;
    }
    m_bytesRead = m_head.size();
    if (m_file->error() != QFileDevice::NoError)
        m_openFailed = true;
}

const QStringList &RoadmapText::detectionPrefix() {
    if (m_prefixDone)
        return m_prefix;
    m_prefixDone = true;
    if (m_fullDone) {
        // Accessor order is free in BOTH directions: with the body already in
        // hand the prefix comes from it through the same helper, reading no
        // disk at all. That is the half of INV-6 a prefix-then-body test never
        // reaches.
        m_prefix = detectionPrefixOf(m_text);
        return m_prefix;
    }
    readHead();
    if (m_fileBacked && !m_file) {
        // The open failed. An EMPTY list, explicitly — detectionPrefixOf("")
        // would return a one-element list holding "", and INV-4's refusal
        // chain is stated in terms of an empty prefix. A file that opens and
        // is empty still takes the line below, where it agrees with
        // fromMemory("") and INV-2 holds.
        m_prefix.clear();
        return m_prefix;
    }
    m_prefix = detectionPrefixOf(QString::fromUtf8(m_head));
    return m_prefix;
}

const QString &RoadmapText::full() {
    if (m_fullDone)
        return m_text;
    m_fullDone = true;
    ensureOpen();
    if (!m_file) {
        m_text.clear();   // what today's sites hand the seam on a failed open
        return m_text;
    }
    // Continue from where the prefix stopped rather than seeking back and
    // re-reading: that is what lets full() return the whole text (INV-5)
    // without reading the head twice (INV-6). A mid-read error leaves
    // openFailed() true and the partial bytes in hand, which is what
    // QFile::readAll() already does and what no current site checks for.
    const QByteArray rest = m_file->readAll();
    if (m_file->error() != QFileDevice::NoError)
        m_openFailed = true;
    m_bytesRead = m_head.size() + rest.size();
    // Decoded ONCE over the joined bytes, so a multi-byte sequence straddling
    // the prefix boundary is not split into two replacement characters.
    m_text = QString::fromUtf8(m_head + rest);
    m_head.clear();
    m_headDone = true;   // the handle has moved past it; it is never re-read
    m_file.reset();      // the retained handle is retained only until here
    return m_text;
}

bool RoadmapText::openFailed() {
    // FORCES the open. Consumers branch on this exactly where they used to
    // branch on QFile::open(), which is before any accessor has run, so a
    // version that merely reported a flag set by an earlier accessor would
    // answer false on the first call — and op:append would silently lose the
    // roadmap_read_failed refusal INV-4 says is preserved.
    ensureOpen();
    return m_openFailed;
}

std::unique_ptr<RoadmapStore> storeFor(const QString &defaultPath,
                                       ReadError *why, QString *error,
                                       qint64 historyCapBytes) {
    if (why)
        *why = ReadError::None;
    if (error)
        error->clear();

    // Absent is the common case and it is NOT an error: most machines running
    // this code have never migrated anything, and a refusal here would break
    // every unmigrated project on the day the store shipped.
    if (!QFileInfo::exists(defaultPath))
        return nullptr;

    auto store = std::make_unique<RoadmapStore>(
        // ANTS-3822 § 2.3.2 — from the caller, defaulted to
        // kDefaultHistoryCapBytes. The header says why it is injectable.
        defaultPath, historyCapBytes,
        // § 2.2's sixth rule — named, never defaulted. Interactive is a 5 s
        // busy deadline and SQLite's 2 MiB page cache; Bulk's 30 s is right for
        // a corpus load and reads as a freeze on a verb call.
        RoadmapStore::Access::Interactive);

    QString openErr;
    if (!store->open(&openErr)) {
        // Present and unopenable is the case a silent fallback would hide: a
        // corrupted store that quietly serves markdown is a store nobody
        // notices is corrupt.
        if (why)
            *why = ReadError::StoreFailed;
        if (error)
            *error = openErr.isEmpty()
                         ? QStringLiteral("could not open roadmap store %1").arg(defaultPath)
                         : openErr;
        return nullptr;
    }
    return store;
}

std::optional<qint64> migratedProject(RoadmapStore &store,
                                      const QString &projectRoot,
                                      RoadmapText &text,
                                      QString *error, ReadError *why) {
    if (why)
        *why = ReadError::None;
    if (error)
        error->clear();

    // ANTS-3756 INV-8 keys a project on its CANONICAL root, so the lookup has
    // to canonicalise too. A reader passing the caller's raw path would miss on
    // every symlinked or non-normalised root and report "not migrated" — the
    // silent fallback INV-1 exists to forbid, arriving through the one door the
    // invariant does not watch.
    const QString canonical = QFileInfo(projectRoot).canonicalFilePath();
    if (canonical.isEmpty()) {
        // Empty is what canonicalFilePath() returns for a path that does not
        // resolve. Refusing beats querying: '' would match no row and read as
        // "not migrated", which is the same silent fallback one step later.
        if (why)
            *why = ReadError::SourceUnrecognised;
        if (error)
            *error = QStringLiteral("project root does not resolve: %1").arg(projectRoot);
        return std::nullopt;
    }

    QString sqlError;
    const auto row = store.readProjectByRoot(canonical, &sqlError);
    if (!row) {
        if (!sqlError.isEmpty()) {
            if (why)
                *why = ReadError::StoreFailed;
            if (error)
                *error = sqlError;
            return std::nullopt;
        }
        return std::nullopt;   // no project row — markdown, and not an error
    }

    // § 2.2's fourth and fifth rules. The migration reads all three dialects, so
    // a project row existing does not imply this path can serve it — and the
    // distinction is `sawSignal`, not the returned dialect, because
    // detectRoadmapFormat() answers "ants-v1" for input it does not recognise.
    bool sawSignal = false;
    const QString format =
        RoadmapParse::detectRoadmapFormat(text.detectionPrefix(), &sawSignal);
    if (!sawSignal) {
        // A migrated project whose ROADMAP.md is absent, empty or mangled. Not
        // StoreFailed: the store is fine and the FILE is not, and the two send
        // the user to different places.
        if (why)
            *why = ReadError::SourceUnrecognised;
        if (error)
            *error = QStringLiteral(
                         "project %1 has a store row but its roadmap text is "
                         "unrecognisable").arg(canonical);
        return std::nullopt;
    }
    // ANTS-3815 § 2.4 — the stored dialect is the second witness, and it costs
    // no extra query: readProjectByRoot() above already returned it. '' is a
    // pre-bump row and means "not recorded", so it takes the version-1 path
    // unchanged; treating it as authoritative would turn every project migrated
    // before the bump into a refusal, the empty string matching no dialect a
    // file can produce.
    if (!row->sourceFormat.isEmpty() && row->sourceFormat != format) {
        // Same code as the unrecognisable-file case above, so the MESSAGE has to
        // separate them — ANTS-3793 chose a distinct code precisely so the store
        // and the file send the user to different places, and a second cause
        // under one code loses that. This one has a remedy and says so.
        if (why)
            *why = ReadError::SourceUnrecognised;
        if (error)
            *error = QStringLiteral(
                         "project %1: store records format '%2' but its roadmap now reads "
                         "as '%3'; re-run the migration to record the new format")
                         .arg(canonical, row->sourceFormat, format);
        return std::nullopt;
    }

    // ANTS-4803 — pass-headings joins ants-v1 as store-served. The DATA side
    // always worked: the migration reads the dialect, folds each heading to a
    // PASS-N-M id and stores sections, items and bodies correctly. What was
    // missing was the RENDER, so the store was WRITE-ONLY for this dialect —
    // rows nobody read, and every store-side benefit (source of truth,
    // op:"render", drift detection, cross-project reporting) unavailable. The
    // reporting project de-registered twice rather than leave stale rows in the
    // machine-global figures.
    //
    // RoadmapRender now emits the dialect (Options::dialect), so the gate can
    // open. Any OTHER dialect still returns here: this list and the render's
    // are the same set, and a format that can be stored but not published is
    // the trap this item was filed about.
    if (format != QStringLiteral("ants-v1")
        && format != QStringLiteral("pass-headings"))
        return std::nullopt;   // legitimately markdown-served (§ 5)

    return row->projectId;
}

std::optional<QVector<BulletRecord>>
bulletsFromStore(RoadmapStore &store, qint64 projectId, bool includeArchive,
                 ReadError *why, QString *error,
                 const RoadmapParse::IdFormat &fmt) {
    if (why)
        *why = ReadError::None;
    if (error)
        error->clear();

    const auto fail = [&](ReadError e, const QString &msg) {
        if (why)
            *why = e;
        if (error && error->isEmpty())
            *error = msg;
        return std::optional<QVector<BulletRecord>>{};
    };

    // Step 0 — the item keys, for the ceiling gate and for the unfiled tail.
    // ItemRef carries a headline and four scalars rather than a body, which is
    // what makes the pre-read count cheap.
    const auto refs = store.listItems(projectId, error);
    if (!refs)
        return fail(ReadError::StoreFailed,
                    QStringLiteral("could not list items for project %1").arg(projectId));

    // INV-3 — tested BEFORE any readItem() runs, so a project over the ceiling
    // never materialises the bodies the ceiling exists to bound. `>` and not
    // `>=`: exactly kItemCeiling is accepted.
    if (refs->size() > kItemCeiling)
        return fail(ReadError::TooLarge,
                    QStringLiteral("project %1 holds %2 items, over the %3-item read ceiling")
                        .arg(projectId).arg(refs->size()).arg(kItemCeiling));

    // Then every item body in ONE query. This was a readItem() per item until
    // INV-3's p95 case measured it: 83.4 ms of a 101.5 ms read on a
    // 1,839-item project, against a 50 ms budget, with the whole
    // render-and-parse half costing 8.6 ms (2026-08-04). § 4 named the batched
    // reader as the remedy in advance and said to build it only if the budget
    // red — it did, so ANTS-3816's first half lands here.
    auto itemOf = store.readItems(projectId, error);
    if (!itemOf)
        return fail(ReadError::StoreFailed,
                    QStringLiteral("could not read items for project %1").arg(projectId));

    QVector<RoadmapStore::ItemRef> unfiled;
    for (const RoadmapStore::ItemRef &ref : *refs) {
        if (ref.sectionId == 0)
            unfiled.append(ref);
    }

    // Step 1 — sections, scoped by the caller's archive flag. SectionRow::
    // sourcePath is nullopt for the live roadmap and set for an archive, which
    // is the same distinction ANTS-3758's render routes files by.
    // Step 2 — document order comes from the store surface (ANTS-3818), which
    // sorts with a C++ comparator and not an ORDER BY: QString::compare() is
    // UTF-16 code-unit order while SQLite's BINARY collation is UTF-8 byte
    // order, and the two disagree on the supplementary-plane characters an emoji
    // heading slug reaches. Filtering below preserves the order.
    const auto sections = store.listSectionsOrdered(projectId, error);
    if (!sections)
        return fail(ReadError::StoreFailed,
                    QStringLiteral("could not list sections for project %1").arg(projectId));

    QVector<RoadmapStore::SectionRow> ordered;
    ordered.reserve(sections->size());
    for (const RoadmapStore::SectionRow &s : *sections) {
        if (includeArchive || !s.sourcePath)
            ordered.append(s);
    }

    QVector<BulletRecord> out;
    // Step 4 — accumulated across the WHOLE walk and never per section: the
    // parser's uniqueSlug() is stateful, appending -2/-3 to a repeated heading
    // text, and a stateless per-section slug diverges on the first duplicated
    // title.
    QSet<QString> seenSlugs;

    for (const RoadmapStore::SectionRow &s : ordered) {
        QString heading;
        int level = 0;
        QString slug;
        // § 2.8's level-0 synthetic root emits no heading line at all, so the
        // parser attributes its bullets to no section.
        if (s.level != 0) {
            heading = s.title;
            level   = s.level;
            slug    = RoadmapIndex::uniqueSlug(seenSlugs, s.title);
        }

        // Step 3 — the id arrives on the row (ANTS-3817). This was a
        // findSection() per section, with a "vanished between reads" error path
        // for the row disappearing between the two queries; one read cannot race
        // itself, so both are gone.
        const auto elements = store.listElements(s.sectionId, error);
        if (!elements)
            return fail(ReadError::StoreFailed,
                        QStringLiteral("could not list elements of section '%1'").arg(s.slug));

        // Step 5 — position order, which listElements() supplies as its own
        // contract (UNIQUE (section_id, position) makes it total). `narration`
        // and `table` elements are skipped: they are not bullets, and
        // parseBullets() returns no records for them either.
        for (const RoadmapStore::ElementRow &e : *elements) {
            if (e.kind != QLatin1String("item"))
                continue;
            const auto it = itemOf->constFind(e.itemPk);
            if (it == itemOf->constEnd())
                continue;
            appendRecord(out, *it, heading, level, slug, fmt);
        }
    }

    // Step 6 — unfiled items last. They have no element row and therefore no
    // position, so a deterministic order is the only answer that is both total
    // and reproducible. They are returned under BOTH values of includeArchive:
    // an unfiled item has no section, therefore no sourcePath for the flag to
    // test, and hiding it from the live read would hide it from the only read
    // likely to surface the fault ANTS-3758's INV-4 refuses the render over.
    std::sort(unfiled.begin(), unfiled.end(),
              [](const RoadmapStore::ItemRef &a, const RoadmapStore::ItemRef &b) {
                  return a.idFold.compare(b.idFold) < 0;
              });
    for (const RoadmapStore::ItemRef &ref : unfiled)
        appendRecord(out, itemOf->value(ref.itemPk), QString(), 0, QString(), fmt);

    return out;
}

std::optional<QVector<BulletRecord>>
bulletsFor(RoadmapStore &store, const QString &projectRoot,
           RoadmapText &text, bool includeArchive, ReadError *why,
           QString *error, const RoadmapParse::IdFormat &fmt) {
    if (why)
        *why = ReadError::None;
    if (error)
        error->clear();

    ReadError markerWhy = ReadError::None;
    const auto projectId =
        migratedProject(store, projectRoot, text, error, &markerWhy);
    if (!projectId) {
        // None here is the unmigrated project — the caller parses `text.full()`
        // itself, which is the first body read on that path. Anything else is a
        // refusal that must never fall back.
        if (why)
            *why = markerWhy;
        return std::nullopt;
    }
    return bulletsFromStore(store, *projectId, includeArchive, why, error, fmt);
}

QHash<QString, QString> legendByEmoji(const QString &legendText) {
    QHash<QString, QString> out;
    if (legendText.isEmpty())
        return out;
    const QJsonObject legend =
        QJsonDocument::fromJson(legendText.toUtf8()).object();
    for (auto it = legend.constBegin(); it != legend.constEnd(); ++it) {
        // roadmaprender.cpp's own word→emoji map, exported rather than
        // duplicated: a second table here is a correspondence someone has to
        // keep true by hand. `dropped` returns empty and is skipped, which is
        // right — § 3.11 gives it no glyph and no rendered bullet carries it.
        const QString emoji = RoadmapRender::emojiFor(it.key());
        if (emoji.isEmpty())
            continue;
        out.insert(emoji, it.value().toString());
    }
    return out;
}

} // namespace RoadmapSource
