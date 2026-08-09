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
        // ANTS-4065 § 2.1's four additions. They were missing because the
        // survey § 7.4's table was derived from had rxKind()'s own anchor, so
        // every value written inline was invisible to it — `bug` is the single
        // largest unmapped value in the corpus (29) and all 29 write it inline.
        {QStringLiteral("bug"),              QStringLiteral("fix")},
        {QStringLiteral("performance"),      QStringLiteral("perf")},
        {QStringLiteral("process + tooling"), QStringLiteral("chore")},
        {QStringLiteral("audit"),            QStringLiteral("audit-fix")},
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

// Cells split on UNESCAPED pipes, and `\|` restores a literal `|` — so the
// store holds the author's text rather than its GFM spelling, and this is the
// exact inverse of the render's escapeCell() (ANTS-3832). A naive split() put
// `a \` and `b` in the store for a cell reading `a | b`, which no render could
// have put back.
QStringList tableCells(const QString &line) {
    QString t = line.trimmed();
    t.chop(1);
    t.remove(0, 1);
    QStringList out;
    QString cur;
    for (int i = 0; i < t.size(); ++i) {
        const QChar c = t.at(i);
        if (c == QLatin1Char('\\') && i + 1 < t.size() && t.at(i + 1) == QLatin1Char('|')) {
            cur.append(QLatin1Char('|'));
            ++i;
        } else if (c == QLatin1Char('|')) {
            out.append(cur.trimmed());
            cur.clear();
        } else {
            cur.append(c);
        }
    }
    out.append(cur.trimmed());
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
             int line, int sourceIndex = 0) {
    notes.append(Note{QString::fromLatin1(code), detail, line, sourceIndex});
}

// ANTS-3808 § 2.1 — `item.body` is the bullet body with the render's own
// head-line PREFIX removed, not with its first line removed. The distinction is
// the whole of this helper: a native bullet takes its headline from the bold
// token only, so text after the closing `**` lives nowhere but `body`'s first
// line, and for a single-line bullet it is the item's entire substance.
// Dropping the line would delete it outright (INV-5). The share of this
// project's own ROADMAP.md affected is somewhere between a seventh and a third
// depending on how a soft-wrapped bold headline is counted — ANTS-3808 § 2.1
// measured 241 of 1646 on 2026-08-04, a re-count the same day 539 of 1666. The
// ratio is the durable claim; the denominator moves with every append.
//
// The boundary is the one the READER recorded (`rec.headlineEnd`), never a
// re-match of the stored headline against the line it came from: the reader
// normalises what it stores, so on the GFM prose path the headline is not a
// substring of its own source line and a text match would silently strip
// nothing.
QString bodyWithoutHeadPrefix(const BulletRecord &rec) {
    if (rec.headlineEnd < 0 || rec.headlineEnd > rec.body.size())
        return rec.body;   // no headline assigned, or not a prefix — strip nothing
    const QString rest = rec.body.mid(rec.headlineEnd);
    const qsizetype nl = rest.indexOf(QLatin1Char('\n'));
    // Both halves below are load-bearing. The separator space after `[<id>]`
    // survives the strip, so an untrimmed residual is `" <prose>"` rather than
    // `<prose>` — and an untrimmed EMPTY residual is `" "`, which is not empty,
    // so the drop would never fire and every bullet would gain a stray blank
    // indented line from renderBullet()'s `if (!it.body.isEmpty())` guard.
    const QString firstLine = (nl < 0 ? rest : rest.left(nl)).trimmed();
    const QString continuations = nl < 0 ? QString() : rest.mid(nl + 1);
    if (firstLine.isEmpty())
        return continuations;
    if (nl < 0)
        return firstLine;
    return firstLine + QLatin1Char('\n') + continuations;
}

// One reader record as migration will file it. § 2.1.1 accounts for the fields
// left empty; the notes this raises are § 2.10's.
PlannedItem makeItem(const BulletRecord &rec, const QString &sectionSlug,
                     int position, int sourceIndex, QVector<Note> &notes) {
    PlannedItem it;
    it.sourceIndex = sourceIndex;
    it.headline = rec.headlineFull.isEmpty() ? rec.headline : rec.headlineFull;
    it.body        = bodyWithoutHeadPrefix(rec);   // ANTS-3808 § 2.1
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
        // Every note below passes `sourceIndex`. It used to default to 0 —
        // ANTS-3766 § 2.4's sentinel for "the live roadmap" — so a note about an
        // archive bullet pointed its line number at the wrong file. Corrected
        // alongside ANTS-4065's own notes rather than left inconsistent beside
        // them; tracked as ANTS-4075.
        addNote(notes, "status_defaulted",
                QStringLiteral("no `- **Status**:` line in the block"),
                rec.firstLine, sourceIndex);
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
            addNote(notes, "status_defaulted", rec.sourceStatus, rec.firstLine,
                    sourceIndex);
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
            addNote(notes, "quarantined_id", rec.idToken, rec.firstLine, sourceIndex);
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
                rec.firstLine, sourceIndex);
    }

    // Kind (§ 2.8). Never a refusal: roughly half the corpus carries no
    // `Kind:`, and roadmap-data-model.md § 7.4's table was generated from a
    // corpus that grows.
    const QString rawKind = rec.kind.trimmed();
    if (rawKind.isEmpty()) {
        it.kind = QStringLiteral("implement");
        it.provenance.insert(QStringLiteral("kind"), QStringLiteral("defaulted"));
        // ANTS-4065 § 2.3 — import may default a field; it may not default one
        // silently. This branch is the one the rule was written for: it assigned
        // `implement` to 476 items on the first real migration and said nothing,
        // while its unmapped-value sibling below had emitted a note all along.
        addNote(notes, "field_defaulted", QStringLiteral("kind"), rec.firstLine,
                sourceIndex);
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
        // BOTH notes, and the second is not redundant: `kind_unmapped` reports
        // the value that was not recognised, `field_defaulted` reports that a
        // default was applied. § 2.3 counts the defaulted population as all 476
        // — "the note fires on the default itself rather than on whether
        // anything survived it" — so this branch's 38 belong in that tally too.
        addNote(notes, "kind_unmapped", rawKind, rec.firstLine, sourceIndex);
        addNote(notes, "field_defaulted", QStringLiteral("kind"), rec.firstLine,
                sourceIndex);
    }

    // Source (§ 2.8) — roadmap-format.md § 3.5.3's own default.
    if (rec.source.isEmpty()) {
        it.source = QStringLiteral("planned");
        it.provenance.insert(QStringLiteral("source"), QStringLiteral("defaulted"));
        addNote(notes, "field_defaulted", QStringLiteral("source"), rec.firstLine,
                sourceIndex);
    } else {
        it.source = rec.source;
        it.provenance.insert(QStringLiteral("source"), QStringLiteral("asserted"));
    }
    return it;
}


// Per-section walk state, parallel to the sections ONE source contributed.
struct Build {
    int  nextPosition = 0;   // § 2.11 — ONE sequence over items AND elements
    bool hasElement   = false;
    int  introFirst   = 0;
    int  introLast    = 0;   // last NON-BLANK intro line
    int  lastItemIndex = -1; // into MigrationPlan::items
};

// ANTS-3766 § 2.2's archive name regex, expressed ONCE. Deliberately tighter
// than roadmap-format.md § 3.9's stated `^[0-9]+\.[0-9]+\.md$`, which accepts
// the zero-padded `00.07.md` that its own prose forbids and that parses to the
// same (0, 7) tuple as `0.7.md`. Forbidding leading zeros makes the
// name→tuple map injective, so there is no duplicate-minor condition left to
// detect — the case is removed by construction rather than checked for.
const QRegularExpression &archiveNameRx() {
    static const QRegularExpression rx(
        QStringLiteral("\\A(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.md\\z"));
    return rx;
}

// ANTS-3766 § 2.3 — one source's slug namespace. `prefix` is empty for the
// live roadmap and "<M>-<N>" for an archive.
struct SourceCtx {
    int     index = 0;
    QString prefix;
};

// The structural walk of ONE source, appending into `plan`. This is
// ANTS-3757 § 2.11's walk unchanged except where a comment names ANTS-3766.
void walkSource(const Source &src, const SourceCtx &ctx, MigrationPlan &plan) {
    const QStringList lines = src.markdown.split(QLatin1Char('\n'));
    const int n = lines.size();

    const QVector<BulletRecord> records = RoadmapParse::parseBullets(src.markdown);
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

    // ANTS-3766 § 2.3 — uniquing runs PER SOURCE, over this source's own set,
    // never one shared across sources. That is what makes the live file's
    // slugs identical whether or not an archive is present (INV-3), and what
    // makes an archive's slug a function of its own file alone (INV-4).
    QVector<Build> builds;
    QSet<QString>  seenSlugs;
    QString        parentOfSubsection;
    int            headingOrdinal = 0;   // 1-based over this source's ##/###

    // Sections this source contributed start here; `cur` is an index into
    // `builds`, and `sectionBase + cur` the matching index into plan.sections.
    const int sectionBase = plan.sections.size();

    // Content before the first heading belongs to a synthetic section. Its
    // slug is "" for the live file and the bare "<M>-<N>" for an archive —
    // it is not a heading, so it never enters `seen` (§ 2.3).
    {
        PlannedSection root;
        root.slug        = ctx.prefix;
        root.sourceIndex = ctx.index;
        plan.sections.append(root);
        builds.append(Build{});
    }
    int cur = 0;

    const auto section = [&](int i) -> PlannedSection & {
        return plan.sections[sectionBase + i];
    };

    const auto closeSection = [&]() {
        PlannedSection &s = section(cur);
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
        e.sectionSlug = section(cur).slug;
        e.position    = builds[cur].nextPosition++;
        e.firstLine   = first;
        e.lastLine    = last;
        e.sourceIndex = ctx.index;
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
                plan.items.append(makeItem(*rec, section(cur).slug,
                                           builds[cur].nextPosition++,
                                           ctx.index, plan.notes));
                builds[cur].hasElement    = true;
                builds[cur].lastItemIndex = plan.items.size() - 1;
                ln = last + 1;
                continue;
            }
            // A maximal run of legend lines is the legend block, and it belongs
            // to the PROJECT rather than to this section (roadmap-data-model.md
            // § 5.1). Only the first such run: a later one has no carrier.
            //
            // ANTS-3766 § 2.4 — and only sources[0] may plan one, because
            // MigrationPlan holds a single optional<PlannedLegend> and the
            // legend belongs to the project rather than to a file. An
            // archive's own legend run therefore falls through to `narration`
            // below, which costs nothing: every line is still carried, so
            // INV-5's per-source partition holds. Dropping it instead would
            // leave its lines in no carrier and make INV-5 unsatisfiable.
            const QString text = rec->body.section(QLatin1Char('\n'), 0, 0).trimmed();
            if (ctx.index == 0 && !plan.legend && looksLikeLegendLine(text)) {
                PlannedLegend legend;
                legend.firstLine   = ln;
                legend.sourceIndex = ctx.index;
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
            ++headingOrdinal;
            PlannedSection s;
            s.title = headingText;

            // ANTS-3766 § 2.3. Two rules, in this order, and the order IS the
            // mechanism:
            //
            // 1. A heading that slugifies to empty takes `h<ordinal>` — but
            //    ONLY in an archive. Applying it at index 0 would change a
            //    live section's slug from "" to h<n> on any project with an
            //    emoji-only heading, which is exactly the live-slug shift
            //    INV-4 and INV-9 forbid; the live path's empty-slug hole is
            //    uniqueSlug()'s own pre-existing defect and is left alone.
            // 2. Uniquing runs on the UNPREFIXED base, before the prefix is
            //    applied. Reversing it breaks the scheme: `seen` holds
            //    unprefixed slugs, so a post-prefix `0-6-h3` would never
            //    compare against a real `### H3` (which slugifies to `h3` and
            //    prefixes to the same string) and the two would collide.
            //    Minting `h<ordinal>` outside `seen` reproduces the same
            //    bypass, which is why the substitution happens BEFORE the
            //    uniqueSlug() call rather than instead of it.
            QString base = RoadmapIndex::slugifyHeading(headingText);
            if (base.isEmpty() && ctx.index >= 1)
                base = QStringLiteral("h") + QString::number(headingOrdinal);
            const QString unique = RoadmapIndex::uniqueSlug(seenSlugs, base);
            s.slug = ctx.prefix.isEmpty()
                         ? unique
                         : ctx.prefix + QLatin1Char('-') + unique;

            s.level       = level;
            s.parentSlug  = level == 3 ? parentOfSubsection : QString();
            s.sourceIndex = ctx.index;
            if (level == 2) parentOfSubsection = s.slug;
            s.firstLine = s.lastLine = ln;
            plan.sections.append(s);
            builds.append(Build{});
            cur = builds.size() - 1;
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
    // section or element whose span it falls in. Per SOURCE, against that
    // source's own detected format (§ 2.1.1).
    if (src.format == QLatin1String("pass-headings")) {
        QVector<bool> insideItem(n + 2, false);
        for (const PlannedItem &it : plan.items)
            if (it.sourceIndex == ctx.index)
                for (int k = it.firstLine; k <= it.lastLine && k <= n; ++k)
                    insideItem[k] = true;
        static const QRegularExpression rxStatus(
            QStringLiteral("^\\s*[-*]\\s*\\*\\*Status\\*\\*\\s*:"),
            QRegularExpression::CaseInsensitiveOption);
        for (int k = 1; k <= n; ++k)
            if (!insideItem[k] && !inFence[k] &&
                rxStatus.match(lines.at(k - 1)).hasMatch())
                addNote(plan.notes, "orphan_status_line",
                        lines.at(k - 1).trimmed(), k, ctx.index);
    }

    // The synthetic root is dropped when this source put nothing in it. Safe
    // to erase mid-vector: items and elements reference a section by SLUG, not
    // by index, and this source's walk is finished.
    if (builds.at(0).nextPosition == 0 && section(0).intro.isEmpty())
        plan.sections.remove(sectionBase);
}

}  // namespace

std::optional<Discovery> findRoadmaps(const QString &projectRoot, QString *error) {
    const auto fail = [error](const char *code) -> std::optional<Discovery> {
        if (error) *error = QString::fromLatin1(code);
        return std::nullopt;
    };

    // 1. The live roadmap — ANTS-3757 § 2.2's rule, unchanged. The file
    // DIRECTLY in the root, not recursively, whose name case-folds to
    // `roadmap.md` — so a `docs/ROADMAP.md` is not a candidate and cannot
    // silently outrank the real one. One project names its file `roadmap.md`,
    // and an uppercase-only glob excluded it from an entire survey.
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

    Discovery disc;
    {
        QFile f(hits.constFirst());
        if (!f.open(QIODevice::ReadOnly)) return fail("not_found");
        Source live;
        if (!decodeUtf8(f.readAll(), &live.markdown)) return fail("not_utf8");
        live.path = hits.constFirst();
        disc.sources.append(live);
    }

    // 2. Archives under <root>/docs/roadmap/ — ANTS-3766 § 2.2. The directory
    // and the descending sort are roadmap-format.md § 3.9's, adopted as they
    // stand; the name regex is deliberately tighter (see archiveNameRx()).
    const QFileInfo archiveDir(dir.filePath(QStringLiteral("docs/roadmap")));
    if (archiveDir.exists()) {
        if (!archiveDir.isDir() || !archiveDir.isReadable()) {
            // Absent and unreadable are NOT the same fact and must not share
            // an outcome. Folding this into the missing-directory branch would
            // make the one case that loses EVERY archive at once the quietest
            // thing this lane can do — strictly worse than a single misnamed
            // file, which does get a note.
            addNote(disc.notes, "archive_unrecognised",
                    QStringLiteral("docs/roadmap"), 0, -1);
        } else {
            QVector<QPair<QPair<int, int>, QString>> found;
            const QFileInfoList archiveEntries =
                QDir(archiveDir.absoluteFilePath())
                    // QDir::System is load-bearing and was found missing by
                    // INV-8's symlink leg. Measured: against this lane's own
                    // fixture, `AllEntries` lists four of five entries and
                    // omits the symlink `0.9.md` entirely, while
                    // `AllEntries | System` lists all five. AllEntries is
                    // Dirs|Files|Drives, and a symlink that does not resolve
                    // is none of those — so without System a broken or
                    // outward symlink under docs/roadmap/ is not skipped with
                    // a note, it is INVISIBLE, which is exactly the silent
                    // drop § 2.2 promises never to do.
                    .entryInfoList(QDir::AllEntries | QDir::System |
                                       QDir::Hidden | QDir::NoDotAndDotDot,
                                   QDir::Name);
            for (const QFileInfo &fi : archiveEntries) {
                const QString name = fi.fileName();
                const auto m = archiveNameRx().match(name);
                // Regular files only, and symlinks are NOT followed. BOTH
                // halves are written out because Qt will not give them for
                // free: isFile() and a QDir::Files filter FOLLOW symlinks and
                // report a symlink-to-file as a file, so the natural
                // implementation loads it — and a symlink under docs/roadmap/
                // can point outside the project root. A DIRECTORY named
                // `0.7.md` matches the regex too.
                if (!m.hasMatch() || !fi.isFile() || fi.isSymLink()) {
                    // Never a silent skip: a misnamed archive is exactly the
                    // shape whose silent loss this lane exists to prevent.
                    addNote(disc.notes, "archive_unrecognised", name, 0, -1);
                    continue;
                }
                found.append({{m.captured(1).toInt(), m.captured(2).toInt()},
                              fi.absoluteFilePath()});
            }
            // Descending by the (major, minor) INTEGER tuple. Lexical sort is
            // explicitly wrong here — "0.10" < "0.9" as strings — and this
            // project will reach minor 10.
            std::sort(found.begin(), found.end(),
                      [](const auto &a, const auto &b) { return b.first < a.first; });

            for (const auto &entry : std::as_const(found)) {
                QFile f(entry.second);
                if (!f.open(QIODevice::ReadOnly)) {
                    // An archive is skippable where the live file is not, so a
                    // permissions failure takes the note rather than the
                    // refusal findRoadmap() gives the live file.
                    addNote(disc.notes, "archive_unrecognised",
                            QFileInfo(entry.second).fileName(), 0, -1);
                    continue;
                }
                Source arc;
                // Partial migration of a project is worse than none: the load
                // half is one transaction, and a plan silently missing one
                // archive would commit as though complete.
                if (!decodeUtf8(f.readAll(), &arc.markdown)) return fail("not_utf8");
                arc.path = entry.second;
                disc.sources.append(arc);
            }
        }
    }

    // 3. Format, per source, then § 2.1.1's reference-format comparison.
    QVector<bool> evidenced(disc.sources.size(), false);
    for (int i = 0; i < disc.sources.size(); ++i) {
        bool sawSignal = false;
        disc.sources[i].format = RoadmapParse::detectRoadmapFormat(
            disc.sources.at(i).markdown.split(QLatin1Char('\n')), &sawSignal);
        evidenced[i] = sawSignal;
    }
    // The reference is the FIRST EVIDENCED source, not sources[0] — the rule
    // has to hold in both directions. Keyed on sources[0] it would exempt an
    // archive from a refusal it cannot deserve while leaving a prose-only live
    // ROADMAP.md exposed to that same refusal beside a github-task-list
    // archive: the evidence-free `ants-v1` default would mismatch and the whole
    // project would be refused on no evidence at all.
    int refIdx = -1;
    for (int i = 0; i < disc.sources.size(); ++i)
        if (evidenced.at(i)) { refIdx = i; break; }
    if (refIdx >= 0) {
        const QString reference = disc.sources.at(refIdx).format;
        for (int i = 0; i < disc.sources.size(); ++i) {
            if (!evidenced.at(i)) {
                // No signal: inherit, and never raise the refusal — there is
                // nothing here to disagree with.
                disc.sources[i].format = reference;
                continue;
            }
            if (disc.sources.at(i).format != reference)
                return fail("archive_format_mismatch");
        }
    }
    // refIdx < 0: no source carries a signal at all, so there is no reference.
    // Every source keeps detectRoadmapFormat()'s answer — its evidence-free
    // `ants-v1` default for all of them — so they agree and nothing is refused.

    if (error) error->clear();
    return disc;
}

MigrationPlan planFrom(const Discovery &discovery, const QString &projectName,
                       const QString &exportSlug) {
    MigrationPlan plan;
    plan.projectName = projectName;
    plan.exportSlug  = exportSlug;
    plan.sources     = discovery.sources;
    // Discovery's notes reach the plan by being PASSED IN, which is what keeps
    // ANTS-3757 INV-9's purity intact: this function still touches no
    // filesystem.
    plan.notes       = discovery.notes;

    for (int i = 0; i < plan.sources.size(); ++i) {
        SourceCtx ctx;
        ctx.index = i;
        if (i >= 1) {
            // "<M>-<N>" from the archive's own filename — § 2.3. Derived from
            // the name alone, which is what makes an archive slug stable under
            // every edit to every other source (INV-4).
            const auto m = archiveNameRx().match(
                QFileInfo(plan.sources.at(i).path).fileName());
            ctx.prefix = m.captured(1) + QLatin1Char('-') + m.captured(2);
        }
        walkSource(plan.sources.at(i), ctx, plan);
    }

    // § 2.5 — both items are kept and both are reported. Merging or renaming
    // them here, or keying items on the folded id, defers the failure to
    // ANTS-3765's UNIQUE (project_id, id_fold) insert, in the half that can no
    // longer see the source line. ANTS-3766 § 2.4: a duplicate id ACROSS
    // sources is not a new case — the store keys on (project_id, id_fold),
    // which is source-blind, so this rule already covers it.
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
                plan.items.at(k).firstLine, plan.items.at(k).sourceIndex);

    // ANTS-3766 § 2.5 — `empty_source` is raised PER SOURCE. Evaluated over
    // the merged plan it would silently stop firing: the plan holds the live
    // file's items, so it is not empty, and the fact that one archive parsed
    // to nothing would never be reported. That is a LOST signal rather than a
    // spurious one — the harder direction, because a prose-only archive and an
    // archive the parser failed on then become indistinguishable, and the
    // second is a real defect this note is the only detector for.
    for (int i = 0; i < plan.sources.size(); ++i) {
        bool any = false;
        for (const PlannedItem &it : plan.items)
            if (it.sourceIndex == i) { any = true; break; }
        if (!any)
            addNote(plan.notes, "empty_source",
                    QFileInfo(plan.sources.at(i).path).fileName(), 0, i);
    }

    // ANTS-3766 § 2.3 — a prefixed archive slug that still equals a LIVE slug
    // is a REFUSAL, never a rename: renaming would shift an archive slug in
    // response to a live-file edit, which is the orphan cascade § 2.3.1 rules
    // out, just more rarely. planFrom() is pure and total (it returns a plan,
    // not an optional), so it records the note and leaves the collision
    // visible; load() refuses a plan carrying it.
    if (plan.sources.size() > 1) {
        QSet<QString> liveSlugs;
        for (const PlannedSection &s : plan.sections)
            if (s.sourceIndex == 0) liveSlugs.insert(s.slug);
        for (const PlannedSection &s : plan.sections) {
            if (s.sourceIndex == 0) continue;
            if (liveSlugs.contains(s.slug))
                addNote(plan.notes, "archive_slug_collision", s.slug,
                        s.firstLine, s.sourceIndex);
        }
    }

    return plan;
}

namespace {

// ANTS-4065 § 2.5 — roadmap-format.md § 3.5.3's `Source:` vocabulary. Needed
// because that vocabulary is full of hyphenated tokens that must not be
// mistaken for filenames: `external-CVE-2026-1234` has no slash and no
// extension, but `upstream-qt6.7` ends in one and would otherwise be looked up
// on disk.
bool isRecognisedSourceForm(const QString &v) {
    static const QSet<QString> exact = {
        QStringLiteral("planned"), QStringLiteral("static-analysis"),
        QStringLiteral("regression"),
    };
    if (exact.contains(v))
        return true;
    static const char *const kPrefixes[] = {
        "user-", "audit-", "indie-review-", "debt-sweep-", "doc-review-",
        "external-CVE-", "upstream-",
    };
    for (const char *p : kPrefixes)
        if (v.startsWith(QLatin1String(p)))
            return true;
    return false;
}

// § 2.5's predicate, and the parentheses in it are load-bearing: unbracketed,
// `A or B and C` also parses as `A or (B and C)`, under which any recognised
// source form containing a slash would be treated as a path.
//
//   (contains `/` OR its final segment matches \.[A-Za-z0-9]{1,5}$)
//   AND it is not one of § 3.5.3's recognised source forms.
//
// Applied to the WHOLE value, which is what "its final segment" means — the
// segment after the last `/`. A path mentioned inside a prose value
// (`in-session-2026-07-29 (rpmlint.log, first successful build)`) is therefore
// NOT validated. That is the literal contract and the safe direction: a missed
// note costs nothing, a note invented against a word that merely contains a dot
// would be noise on real prose.
bool looksLikePath(const QString &value) {
    if (value.isEmpty() || isRecognisedSourceForm(value))
        return false;
    if (value.contains(QLatin1Char('/')))
        return true;
    static const QRegularExpression ext(QStringLiteral("\\.[A-Za-z0-9]{1,5}$"));
    return ext.match(value).hasMatch();
}

// Resolves under the project root only. A value escaping the root — absolute,
// or climbing out with `..` — is reported unresolved rather than probed, which
// keeps a roadmap from turning the importer into a filesystem oracle.
bool resolvesUnderRoot(const QString &root, const QString &rel) {
    if (QDir::isAbsolutePath(rel))
        return false;
    const QString base = QDir(root).absolutePath();
    const QString abs  = QDir::cleanPath(base + QLatin1Char('/') + rel);
    if (abs != base && !abs.startsWith(base + QLatin1Char('/')))
        return false;
    return QFileInfo::exists(abs);
}

}  // namespace

void validatePaths(MigrationPlan &plan, const QString &projectRoot) {
    for (PlannedItem &it : plan.items) {
        QStringList cited;
        // `Evidence:` needs no predicate — roadmap-format.md § 3.5 defines the
        // field AS file paths — and it is comma-separated, so each element is
        // validated independently.
        cited += it.evidence;
        if (looksLikePath(it.source))
            cited.append(it.source);

        QJsonArray unresolved;
        for (const QString &p : std::as_const(cited)) {
            if (resolvesUnderRoot(projectRoot, p))
                continue;
            unresolved.append(p);
            addNote(plan.notes, "unresolved_path",
                    QStringLiteral("%1: %2").arg(it.id.isEmpty()
                                                     ? QStringLiteral("<no id>")
                                                     : it.id,
                                                 p),
                    it.firstLine, it.sourceIndex);
        }
        // An ARRAY, not a scalar: one item can cite several paths and lose more
        // than one. Written only when something is actually unresolved, so an
        // item that cites nothing missing gains no extras key and its `extras`
        // column does not move under § 2.6's comparison.
        if (!unresolved.isEmpty())
            it.extras.insert(QStringLiteral("unresolved_path"), unresolved);
    }
}

}  // namespace RoadmapMigrate
