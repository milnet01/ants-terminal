// ANTS-3757 — see roadmapmigrate.h for the seam. Section references below are
// docs/specs/ANTS-3757-roadmap-migration-read.md unless another document is
// named; where a rule and this code could disagree, the spec wins.

#include "roadmapmigrate.h"

#include "roadmapindex.h"
#include "roadmapparse.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>

#include <algorithm>
#include <utility>

namespace RoadmapMigrate {
namespace {

using RoadmapParse::BulletRecord;

// ------------------------------------------------------------- discovery ---

// § 2.2 refuses a file that is not valid UTF-8 rather than decoding it lossily:
// substituting U+FFFD leaves the line carried, the byte count intact and
// INV-11 green over content that had already been corrupted. Same idiom as
// doccitations.cpp's decodeChecked() — the decoder's error flag is tested, not
// trusted.
bool decodeUtf8(const QByteArray &bytes, QString *out) {
    QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString s = dec(bytes);
    if (dec.hasError()) return false;
    if (out) *out = s;
    return true;
}

// ---------------------------------------------------------------- status ---

QString statusFromMarker(const QString &marker) {
    if (marker == QString::fromUtf8(RoadmapParse::kEmojiDone))
        return QStringLiteral("shipped");
    if (marker == QString::fromUtf8(RoadmapParse::kEmojiInProgress))
        return QStringLiteral("in-progress");
    if (marker == QString::fromUtf8(RoadmapParse::kEmojiConsidered))
        return QStringLiteral("considered");
    return QStringLiteral("planned");
}

// The keyword the reader classified on, recovered from the value it kept.
// parsePassHeadingBullets() matches `([^\sA-Za-z0-9_-]+)?\s*([A-Za-z0-9_-]*)`
// after the colon, lowercases group 2, and — when group 2 is empty — maps a
// bare status emoji in group 1 to its keyword. It then discards the keyword,
// so § 2.7's asserted/defaulted split has to re-derive it from `sourceStatus`.
// This mirrors the reader's two steps exactly; it decides nothing the reader
// did not already decide, which is what keeps § 2.3's one-parser rule intact.
QString passKeyword(const QString &sourceStatus) {
    static const QRegularExpression rx(
        QStringLiteral("^([^\\sA-Za-z0-9_-]+)?\\s*([A-Za-z0-9_-]*)"));
    const auto m = rx.match(sourceStatus);
    if (!m.hasMatch()) return QString();
    QString word = m.captured(2).trimmed().toLower();
    if (!word.isEmpty()) return word;
    const QString glyph = m.captured(1);
    if (glyph == QString::fromUtf8(RoadmapParse::kEmojiDone))
        return QStringLiteral("done");
    if (glyph == QString::fromUtf8(RoadmapParse::kEmojiInProgress))
        return QStringLiteral("in-progress");
    if (glyph == QString::fromUtf8(RoadmapParse::kEmojiConsidered))
        return QStringLiteral("deferred");
    if (glyph == QString::fromUtf8(RoadmapParse::kEmojiPlanned))
        return QStringLiteral("todo");
    return QString();
}

// § 2.7's word table, as the set of words the reader NAMES. A named word is a
// faithful transcription (`asserted`); anything else reached `planned` through
// the reader's else-branch and is migration's guess (`defaulted`).
bool keywordIsNamed(const QString &kw) {
    static const QSet<QString> named = {
        QStringLiteral("done"),        QStringLiteral("shipped"),
        QStringLiteral("completed"),   QStringLiteral("in-progress"),
        QStringLiteral("in_progress"), QStringLiteral("inprogress"),
        QStringLiteral("doing"),       QStringLiteral("wip"),
        QStringLiteral("deferred"),    QStringLiteral("considered"),
        QStringLiteral("parked"),      QStringLiteral("todo"),
        QStringLiteral("planned"),
    };
    return named.contains(kw);
}

// ------------------------------------------------------------------ kind ---

// roadmap-format.md § 3.5.3's 21-value enum.
const QSet<QString> &canonicalKinds() {
    static const QSet<QString> k = {
        QStringLiteral("implement"),   QStringLiteral("fix"),
        QStringLiteral("audit-fix"),   QStringLiteral("review-fix"),
        QStringLiteral("doc"),         QStringLiteral("doc-fix"),
        QStringLiteral("refactor"),    QStringLiteral("test"),
        QStringLiteral("chore"),       QStringLiteral("release"),
        QStringLiteral("perf"),        QStringLiteral("security"),
        QStringLiteral("feature"),     QStringLiteral("enhancement"),
        QStringLiteral("investigate"), QStringLiteral("research"),
        QStringLiteral("accessibility"), QStringLiteral("optimize"),
        QStringLiteral("package"),     QStringLiteral("marketing"),
        QStringLiteral("ux"),
    };
    return k;
}

// roadmap-data-model.md § 7.4's normative migration mapping, applied rather
// than restated. The two compound entries map to their FIRST term, per that
// section: picking the second would silently reclassify performance work.
QString mappedKind(const QString &lower) {
    static const QHash<QString, QString> m = {
        {QStringLiteral("improve"),          QStringLiteral("enhancement")},
        {QStringLiteral("docs"),             QStringLiteral("doc")},
        {QStringLiteral("bugfix"),           QStringLiteral("fix")},
        {QStringLiteral("testing"),          QStringLiteral("test")},
        {QStringLiteral("spike"),            QStringLiteral("research")},
        {QStringLiteral("feat"),             QStringLiteral("feature")},
        {QStringLiteral("enhance"),          QStringLiteral("enhancement")},
        {QStringLiteral("perf / fix"),       QStringLiteral("perf")},
        {QStringLiteral("perf / optimize"),  QStringLiteral("perf")},
        {QStringLiteral("tooling"),          QStringLiteral("chore")},
        {QStringLiteral("behaviour-change"), QStringLiteral("enhancement")},
    };
    return m.value(lower);
}

// -------------------------------------------------------------- structure --

bool isTableRow(const QString &line) {
    const QString t = line.trimmed();
    return t.size() >= 2 && t.startsWith(QLatin1Char('|')) &&
           t.endsWith(QLatin1Char('|'));
}

bool isSeparatorRow(const QString &line) {
    static const QRegularExpression rx(QStringLiteral("^\\s*\\|[\\s:|-]+\\|\\s*$"));
    return rx.match(line).hasMatch();
}

QStringList tableCells(const QString &line) {
    QString t = line.trimmed();
    t.chop(1);
    t.remove(0, 1);
    QStringList out;
    const QStringList parts = t.split(QLatin1Char('|'));
    out.reserve(parts.size());
    for (const QString &p : parts) out.append(p.trimmed());
    return out;
}

// § 2.11 recognises the legend exactly as tools/roadmap-corpus-survey.py
// counts it, so the spec and its own oracle cannot disagree: a status-marked
// bullet § 2.4 rejects, whose text begins with a status word and is under 160
// characters.
bool looksLikeLegendLine(const QString &text) {
    if (text.size() >= 160) return false;
    static const QRegularExpression rx(
        QStringLiteral("^(Done|In progress|Planned|Considered)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return rx.match(text).hasMatch();
}

// § 2.4's conjunction, with this spec's own refinement: an id token in the
// LEADING SLOT stands in for the bold headline, since roadmap-data-model.md
// § 7.1 puts the id there and the corpus writes both together. The reader sets
// `headline` from a bold span for ants-v1, from the first body line for GFM,
// and from the heading for a pass block — so `headline` non-empty IS "carries
// the headline roadmap-format.md § 3.5 requires", and no bullet grammar is
// re-decided here.
bool isItem(const BulletRecord &rec) {
    if (rec.format == QLatin1String("pass-headings")) return true;
    return !rec.headline.isEmpty() || !rec.idToken.isEmpty();
}

// roadmap-format.md § 3.5.1's acceptance grammar, whole-token. Everything else
// in the leading slot is off-grammar and quarantined (§ 2.6) — position is the
// discriminator, not shape (§ 2.5), so `[ANTS-119&]` is quarantined for the
// same reason `[Cl9]` is rather than being handed a second identity.
bool isGrammaticalId(const QString &token) {
    static const QRegularExpression rx(
        QStringLiteral("\\A(?:") + RoadmapParse::idTokenPattern() +
        QStringLiteral(")\\z"));
    return rx.match(token).hasMatch();
}

void addNote(QVector<Note> &notes, const char *code, const QString &detail,
             int line) {
    notes.append(Note{QString::fromLatin1(code), detail, line});
}

// One reader record as migration will file it. § 2.1.1 accounts for the fields
// left empty; the notes this raises are § 2.10's.
PlannedItem makeItem(const BulletRecord &rec, const QString &sectionSlug,
                     int position, QVector<Note> &notes) {
    PlannedItem it;
    it.headline = rec.headlineFull.isEmpty() ? rec.headline : rec.headlineFull;
    it.body        = rec.body;
    it.layman      = rec.layman;
    it.lanes       = rec.lanes;
    it.evidence    = rec.evidence;
    it.sectionSlug = sectionSlug;
    it.position    = position;
    it.firstLine   = rec.firstLine;
    it.lastLine    = rec.lastLine;

    const bool isPass = rec.format == QLatin1String("pass-headings");

    // Status (§ 2.7). Two of the three formats transcribe roadmap-format.md
    // § 3.5's own markers, so the author chose the status and it is `asserted`;
    // only a pass block can carry a word the reader does not name.
    it.status = statusFromMarker(rec.status);
    it.closed = it.status == QLatin1String("shipped");
    if (!isPass) {
        it.provenance.insert(QStringLiteral("status"), QStringLiteral("asserted"));
    } else if (rec.sourceStatus.isEmpty()) {
        it.provenance.insert(QStringLiteral("status"), QStringLiteral("defaulted"));
        addNote(notes, "status_defaulted",
                QStringLiteral("no `- **Status**:` line in the block"),
                rec.firstLine);
    } else {
        // Held for both word rows, not only the lossy one: storing the matched
        // word would discard the qualifier tail, and storing the normalised
        // word would lose `completed` vs `done`, which the write-back being a
        // right-inverse makes unrecoverable.
        it.extras.insert(QStringLiteral("source_status"), rec.sourceStatus);
        const bool named = keywordIsNamed(passKeyword(rec.sourceStatus));
        it.provenance.insert(QStringLiteral("status"),
                             named ? QStringLiteral("asserted")
                                   : QStringLiteral("defaulted"));
        if (!named)
            addNote(notes, "status_defaulted", rec.sourceStatus, rec.firstLine);
    }

    // Identity (§ 2.5 / § 2.6 / § 2.9).
    if (isPass) {
        // Taken as-is: the reader synthesised the id the writer's
        // passIdFromDesignator() would have produced, from the author's own
        // heading, so reader, writer and migration agree (INV-10).
        it.id       = rec.id;
        it.idOrigin = QStringLiteral("synthesised");
        it.provenance.insert(QStringLiteral("id"), QStringLiteral("migrated"));
    } else if (!rec.idToken.isEmpty()) {
        it.id       = rec.idToken;
        it.idOrigin = isGrammaticalId(rec.idToken) ? QStringLiteral("parsed")
                                                   : QStringLiteral("quarantined");
        it.provenance.insert(QStringLiteral("id"), QStringLiteral("asserted"));
        if (it.idOrigin == QLatin1String("quarantined"))
            addNote(notes, "quarantined_id", rec.idToken, rec.firstLine);
    } else {
        // A reader-`synthetic` GFM content-hash id is an identity the dialog
        // invented so it could address a bullet; filing it would silently
        // remove the item from the id-less population roadmap-data-model.md
        // § 7.2's bulk allocation exists to serve. `idToken` is empty for it,
        // so it lands here by construction.
        it.idAllocationOwed = true;
        it.provenance.insert(QStringLiteral("id"), QStringLiteral("migrated"));
        addNote(notes, "id_allocation_owed",
                it.closed ? QStringLiteral("closed") : QStringLiteral("open"),
                rec.firstLine);
    }

    // Kind (§ 2.8). Never a refusal: roughly half the corpus carries no
    // `Kind:`, and roadmap-data-model.md § 7.4's table was generated from a
    // corpus that grows.
    const QString rawKind = rec.kind.trimmed();
    if (rawKind.isEmpty()) {
        it.kind = QStringLiteral("implement");
        it.provenance.insert(QStringLiteral("kind"), QStringLiteral("defaulted"));
    } else if (canonicalKinds().contains(rawKind.toLower())) {
        it.kind = rawKind.toLower();
        it.provenance.insert(QStringLiteral("kind"), QStringLiteral("asserted"));
    } else if (const QString mapped = mappedKind(rawKind.toLower());
               !mapped.isEmpty()) {
        it.kind = mapped;
        it.provenance.insert(QStringLiteral("kind"), QStringLiteral("asserted"));
        it.extras.insert(QStringLiteral("source_kind"), rawKind);
    } else {
        it.kind = QStringLiteral("implement");
        it.provenance.insert(QStringLiteral("kind"), QStringLiteral("defaulted"));
        it.extras.insert(QStringLiteral("source_kind"), rawKind);
        addNote(notes, "kind_unmapped", rawKind, rec.firstLine);
    }

    // Source (§ 2.8) — roadmap-format.md § 3.5.3's own default.
    if (rec.source.isEmpty()) {
        it.source = QStringLiteral("planned");
        it.provenance.insert(QStringLiteral("source"), QStringLiteral("defaulted"));
    } else {
        it.source = rec.source;
        it.provenance.insert(QStringLiteral("source"), QStringLiteral("asserted"));
    }
    return it;
}

// Per-section walk state, parallel to MigrationPlan::sections.
struct Build {
    int  nextPosition = 0;   // § 2.11 — ONE sequence over items AND elements
    bool hasElement   = false;
    int  introFirst   = 0;
    int  introLast    = 0;   // last NON-BLANK intro line
    int  lastItemIndex = -1; // into MigrationPlan::items
};

}  // namespace

std::optional<Source> findRoadmap(const QString &projectRoot, QString *error) {
    const auto fail = [error](const char *code) -> std::optional<Source> {
        if (error) *error = QString::fromLatin1(code);
        return std::nullopt;
    };
    // The roadmap is the file DIRECTLY in the root, not recursively, whose name
    // case-folds to `roadmap.md` — so a `docs/ROADMAP.md` is not a candidate
    // and cannot silently outrank the real one. One project names its file
    // `roadmap.md`, and an uppercase-only glob excluded it from an entire
    // survey (§ 2.2).
    const QDir dir(projectRoot);
    if (!dir.exists()) return fail("not_found");
    QStringList hits;
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Hidden,
                                                    QDir::Name);
    for (const QFileInfo &fi : entries)
        if (fi.fileName().compare(QStringLiteral("roadmap.md"),
                                  Qt::CaseInsensitive) == 0)
            hits.append(fi.absoluteFilePath());
    if (hits.isEmpty()) return fail("not_found");
    // Reachable on a case-sensitive filesystem, and either choice silently
    // discards a whole project's roadmap.
    if (hits.size() > 1) return fail("case_ambiguous");

    QFile f(hits.constFirst());
    if (!f.open(QIODevice::ReadOnly)) return fail("not_found");
    Source src;
    if (!decodeUtf8(f.readAll(), &src.markdown)) return fail("not_utf8");
    src.path = hits.constFirst();
    if (error) error->clear();
    return src;
}

MigrationPlan planFrom(const QString &markdown, const QString &sourcePath,
                       const QString &projectName, const QString &exportSlug) {
    MigrationPlan plan;
    plan.projectName = projectName;
    plan.exportSlug  = exportSlug;
    plan.sourcePath  = sourcePath;

    const QStringList lines = markdown.split(QLatin1Char('\n'));
    const int n = lines.size();
    plan.format = RoadmapParse::detectRoadmapFormat(lines);

    // The reader owns bullet classification and every field on an item; this
    // function owns the structural walk the reader never had (§ 2.3).
    const QVector<BulletRecord> records = RoadmapParse::parseBullets(markdown);
    QHash<int, const BulletRecord *> recordAt;
    for (const BulletRecord &rec : records)
        if (rec.firstLine >= 1) recordAt.insert(rec.firstLine, &rec);

    // Fence extents, matched by their own delimiters. This is the one place
    // the walk must not read a line at face value: a `##` inside a fence is
    // not a heading (§ 2.11).
    QVector<bool> inFence(n + 2, false);
    QHash<int, int> fenceEnd;      // opening line -> closing line
    {
        int openedAt = 0;
        for (int i = 0; i < n; ++i) {
            const int ln = i + 1;
            if (lines.at(i).trimmed().startsWith(QStringLiteral("```"))) {
                inFence[ln] = true;
                if (openedAt == 0) {
                    openedAt = ln;
                } else {
                    fenceEnd.insert(openedAt, ln);
                    openedAt = 0;
                }
            } else {
                inFence[ln] = openedAt != 0;
            }
        }
        if (openedAt != 0) fenceEnd.insert(openedAt, n);   // unterminated
    }

    QVector<Build> builds;
    QSet<QString>  seenSlugs;
    QString        parentOfSubsection;

    // Content before the first heading belongs to a synthetic section: empty
    // slug and title, level 0, no parent. Dropped at the end if it stays empty.
    plan.sections.append(PlannedSection{});
    builds.append(Build{});
    int cur = 0;

    const auto closeSection = [&]() {
        PlannedSection &s = plan.sections[cur];
        const Build &b = builds.at(cur);
        if (b.introLast <= 0) return;
        QStringList intro;
        intro.reserve(b.introLast - b.introFirst + 1);
        for (int k = b.introFirst; k <= b.introLast; ++k)
            intro.append(lines.at(k - 1));
        s.intro    = intro.join(QLatin1Char('\n'));
        s.lastLine = b.introLast;
        if (s.firstLine == 0) s.firstLine = b.introFirst;
    };
    const auto addIntroLine = [&](int ln) {
        Build &b = builds[cur];
        if (b.introFirst == 0) b.introFirst = ln;
        if (!lines.at(ln - 1).trimmed().isEmpty()) b.introLast = ln;
    };
    const auto addElement = [&](const QString &kind, const QString &payload,
                                int first, int last) {
        PlannedElement e;
        e.kind        = kind;
        e.payload     = payload;
        e.sectionSlug = plan.sections.at(cur).slug;
        e.position    = builds[cur].nextPosition++;
        e.firstLine   = first;
        e.lastLine    = last;
        plan.elements.append(e);
        builds[cur].hasElement = true;
    };
    const auto verbatim = [&](int first, int last) {
        QStringList out;
        out.reserve(last - first + 1);
        for (int k = first; k <= last && k <= n; ++k) out.append(lines.at(k - 1));
        return out.join(QLatin1Char('\n'));
    };
    const auto onlyBlankBetween = [&](int after, int before) {
        for (int k = after + 1; k < before; ++k)
            if (!lines.at(k - 1).trimmed().isEmpty()) return false;
        return true;
    };

    int ln = 1;
    while (ln <= n) {
        const QString &raw = lines.at(ln - 1);

        // A reader record starts here. Items are filed; the rest are the
        // legend block or narration — § 2.4 rejects them as items, and INV-11
        // forbids dropping them.
        const auto rit = recordAt.constFind(ln);
        if (rit != recordAt.constEnd() && !inFence[ln]) {
            const BulletRecord *rec = *rit;
            const int last = qMax(rec->lastLine, ln);
            if (isItem(*rec)) {
                plan.items.append(makeItem(*rec, plan.sections.at(cur).slug,
                                           builds[cur].nextPosition++,
                                           plan.notes));
                builds[cur].hasElement    = true;
                builds[cur].lastItemIndex = plan.items.size() - 1;
                ln = last + 1;
                continue;
            }
            // A maximal run of legend lines is the legend block, and it belongs
            // to the PROJECT rather than to this section (roadmap-data-model.md
            // § 5.1). Only the first such run: a later one has no carrier.
            const QString text = rec->body.section(QLatin1Char('\n'), 0, 0).trimmed();
            if (!plan.legend && looksLikeLegendLine(text)) {
                PlannedLegend legend;
                legend.firstLine = ln;
                int end = ln;
                const BulletRecord *run = rec;
                for (;;) {
                    legend.entries.insert(
                        statusFromMarker(run->status),
                        run->body.section(QLatin1Char('\n'), 0, 0).trimmed());
                    end = qMax(run->lastLine, end);
                    const auto next = recordAt.constFind(end + 1);
                    if (next == recordAt.constEnd()) break;
                    const BulletRecord *cand = *next;
                    if (isItem(*cand)) break;
                    const QString t =
                        cand->body.section(QLatin1Char('\n'), 0, 0).trimmed();
                    if (!looksLikeLegendLine(t)) break;
                    run = cand;
                }
                legend.lastLine = end;
                plan.legend = legend;
                // Not an element and it takes no `position` — the legend
                // belongs to the project. It does close the intro, though:
                // without that, prose after it would join an `intro` whose
                // contiguous range then covers these lines and INV-11 would
                // see an overlap rather than a partition.
                builds[cur].hasElement = true;
                ln = end + 1;
                continue;
            }
            addElement(QStringLiteral("narration"), verbatim(ln, last), ln, last);
            ln = last + 1;
            continue;
        }

        // Sections are `##` and `###`, and no deeper: the reader tracks levels
        // 2 and 3 only, so a larger section set would file items under slugs it
        // never assigns.
        QString headingText;
        const int level = inFence[ln] ? 0 : RoadmapIndex::headingLevel(raw, &headingText);
        if (level == 2 || level == 3) {
            closeSection();
            PlannedSection s;
            s.title = headingText;
            // The same function with the same running `seen` set the reader
            // uses, so a section's slug equals the `sectionSlug` the reader put
            // on every item inside it, and the store's UNIQUE (project_id,
            // slug) is satisfied by the uniquing it already does.
            s.slug       = RoadmapIndex::uniqueSlug(seenSlugs, headingText);
            s.level      = level;
            s.parentSlug = level == 3 ? parentOfSubsection : QString();
            if (level == 2) parentOfSubsection = s.slug;
            s.firstLine = s.lastLine = ln;
            plan.sections.append(s);
            builds.append(Build{});
            cur = plan.sections.size() - 1;
            ++ln;
            continue;
        }

        // A fenced block is never an element (roadmap-data-model.md § 5:
        // "fenced code blocks belong to a `body` or an `intro`. Neither is a
        // section element"). A fence inside an item's span was consumed with
        // that item above; one at section level joins the intro, or the
        // preceding item when it immediately follows it.
        if (inFence[ln] && fenceEnd.contains(ln)) {
            const int end = fenceEnd.value(ln);
            Build &b = builds[cur];
            if (!b.hasElement) {
                for (int k = ln; k <= end; ++k) addIntroLine(k);
            } else if (b.lastItemIndex >= 0 &&
                       onlyBlankBetween(plan.items.at(b.lastItemIndex).lastLine, ln)) {
                PlannedItem &owner = plan.items[b.lastItemIndex];
                owner.body.append(QLatin1Char('\n'));
                owner.body.append(verbatim(ln, end));
                owner.lastLine = end;
            } else {
                // Separated from the item by other content, so extending that
                // item's span would make INV-11 overlap rather than partition.
                addElement(QStringLiteral("narration"), verbatim(ln, end), ln, end);
            }
            ln = end + 1;
            continue;
        }

        // A table is a maximal run of contiguous rows. Its separator row is
        // delimiter, not content (roadmap-data-model.md § 5.2), so it is never
        // stored.
        if (!inFence[ln] && isTableRow(raw)) {
            int end = ln;
            while (end < n && !inFence[end + 1] && isTableRow(lines.at(end))) ++end;
            QJsonArray header, rows;
            for (int k = ln; k <= end; ++k) {
                if (isSeparatorRow(lines.at(k - 1))) continue;
                QJsonArray cells;
                for (const QString &c : tableCells(lines.at(k - 1)))
                    cells.append(c);
                if (header.isEmpty() && rows.isEmpty()) header = cells;
                else rows.append(cells);
            }
            QJsonObject payload;
            payload.insert(QStringLiteral("header"), header);
            payload.insert(QStringLiteral("rows"), rows);
            addElement(QStringLiteral("table"),
                       QString::fromUtf8(QJsonDocument(payload).toJson(
                           QJsonDocument::Compact)),
                       ln, end);
            ln = end + 1;
            continue;
        }

        if (raw.trimmed().isEmpty()) {
            if (!builds.at(cur).hasElement && builds.at(cur).introFirst != 0)
                addIntroLine(ln);
            ++ln;
            continue;
        }

        // Ordinary section-level text. Before the first element it is the
        // section's `intro`; after one it is a `narration` element — a position
        // rule, not a content rule (roadmap-data-model.md § 5).
        if (!builds.at(cur).hasElement) {
            addIntroLine(ln);
            ++ln;
            continue;
        }
        int end = ln;
        while (end < n) {
            const QString &next = lines.at(end);           // line end + 1
            if (next.trimmed().isEmpty()) break;
            if (recordAt.contains(end + 1)) break;
            if (inFence[end + 1] || isTableRow(next)) break;
            const int lvl = RoadmapIndex::headingLevel(next);
            if (lvl == 2 || lvl == 3) break;
            ++end;
        }
        addElement(QStringLiteral("narration"), verbatim(ln, end), ln, end);
        ln = end + 1;
    }
    closeSection();

    // § 2.4 — a `- **Status**:` line belonging to no pass block is not an item.
    // Reported, not imported and not dropped: the line is still carried by the
    // section or element whose span it falls in.
    if (plan.format == QLatin1String("pass-headings")) {
        QVector<bool> insideItem(n + 2, false);
        for (const PlannedItem &it : plan.items)
            for (int k = it.firstLine; k <= it.lastLine && k <= n; ++k)
                insideItem[k] = true;
        static const QRegularExpression rxStatus(
            QStringLiteral("^\\s*[-*]\\s*\\*\\*Status\\*\\*\\s*:"),
            QRegularExpression::CaseInsensitiveOption);
        for (int k = 1; k <= n; ++k)
            if (!insideItem[k] && !inFence[k] &&
                rxStatus.match(lines.at(k - 1)).hasMatch())
                addNote(plan.notes, "orphan_status_line",
                        lines.at(k - 1).trimmed(), k);
    }

    // § 2.5 — both items are kept and both are reported. Merging or renaming
    // them here, or keying items on the folded id, defers the failure to
    // ANTS-3765's UNIQUE (project_id, id_fold) insert, in the half that can no
    // longer see the source line.
    QHash<QString, QVector<int>> byFold;
    for (int k = 0; k < plan.items.size(); ++k)
        if (!plan.items.at(k).id.isEmpty())
            byFold[plan.items.at(k).id.toLower()].append(k);
    QVector<int> collided;
    for (auto it = byFold.constBegin(); it != byFold.constEnd(); ++it)
        if (it.value().size() > 1) collided += it.value();
    std::sort(collided.begin(), collided.end());
    for (int k : std::as_const(collided))
        addNote(plan.notes, "duplicate_id", plan.items.at(k).id,
                plan.items.at(k).firstLine);

    // § 2.3 — detectRoadmapFormat() answers `ants-v1` for input it does not
    // recognise, including an empty file, so a detected format is no evidence
    // that anything was understood. The condition turns on ITEMS: a prose-only
    // file legitimately plans elements, and "zero items and zero elements"
    // would be unreachable for exactly the file a parse regression produces.
    if (plan.items.isEmpty())
        addNote(plan.notes, "empty_source",
                QStringLiteral("the source yielded no items"), 0);

    if (builds.at(0).nextPosition == 0 && plan.sections.at(0).intro.isEmpty())
        plan.sections.removeFirst();
    return plan;
}

}  // namespace RoadmapMigrate
