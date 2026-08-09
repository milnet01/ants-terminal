// ANTS-3764 — see roadmapparse.h for why this lives in ants_core_lib.
// The bodies below are moved verbatim from src/roadmapdialog.cpp. The only
// edits are the ones the move itself forces: the record is no longer nested in
// RoadmapDialog, idTokenPattern() and detectRoadmapFormat() leave the
// anonymous namespace because the header declares them (ANTS-1784 shares the
// first with roadmapdialog.cpp's CHANGELOG scanner), and everything is wrapped
// in namespace RoadmapParse. No parsing behaviour changed.

#include "roadmapparse.h"

#include "roadmapindex.h"      // ANTS-1287 — canonical heading/slug helpers

#include <QByteArray>
#include <QRegularExpression>
#include <QSet>
#include <QStringView>

#include <algorithm>

// ANTS-1287: headingLevel + slugifyHeading + uniqueSlug live in RoadmapIndex.
// File-scope using-declarations preserve the unqualified call surface the
// moved bodies were written against. See docs/specs/ANTS-1287.md § 7.
using RoadmapIndex::headingLevel;
using RoadmapIndex::uniqueSlug;

namespace RoadmapParse {

namespace {

// ANTS-1428 — GFM-task-list adapter helpers. All operate on
// already-extracted line content (post-`- ` prefix strip).

// 64-bit FNV-1a hash of the normalised headline. See
// docs/specs/ANTS-1428.md § Synthetic-ID fallback.
quint64 fnv1a64(const QString &normalised) {
    constexpr quint64 kFnvOffsetBasis = 0xcbf29ce484222325ULL;
    constexpr quint64 kFnvPrime       = 0x100000001b3ULL;
    quint64 h = kFnvOffsetBasis;
    const QByteArray bytes = normalised.toUtf8();
    for (char c : bytes) {
        h ^= static_cast<quint64>(static_cast<unsigned char>(c));
        h *= kFnvPrime;
    }
    return h;
}

// Lowercase, collapse whitespace, drop trailing punctuation.
QString normaliseHeadline(const QString &raw) {
    QString s = raw.toLower();
    QString out;
    out.reserve(s.size());
    bool prevSpace = false;
    for (QChar c : s) {
        if (c.isSpace()) {
            if (!out.isEmpty() && !prevSpace) out.append(QLatin1Char(' '));
            prevSpace = true;
        } else {
            out.append(c);
            prevSpace = false;
        }
    }
    while (!out.isEmpty()) {
        const QChar c = out.back();
        if (c == QLatin1Char('.') || c == QLatin1Char(',') ||
            c == QLatin1Char(';') || c == QLatin1Char(':') ||
            c == QLatin1Char('!') || c == QLatin1Char('?') ||
            c == QLatin1Char(' ')) {
            out.chop(1);
        } else {
            break;
        }
    }
    return out;
}

// Encode the low N base36 chars of a 64-bit value. Returns 10
// chars by default (sufficient distribution per spec collision
// math). Length is capped at the encode capacity (13 chars).
QString base36Lower(quint64 v, int n = 10) {
    QString out;
    out.reserve(n);
    static const char kChars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (v == 0) {
        out.append(QLatin1Char('0'));
    } else {
        while (v != 0 && out.size() < n + 6) {
            out.prepend(QLatin1Char(kChars[v % 36]));
            v /= 36;
        }
    }
    while (out.size() < n) out.prepend(QLatin1Char('0'));
    if (out.size() > n) out = out.right(n);
    return out;
}

// ANTS-1438 — widened to match multi-token bold prefixes
// (`**FW W5 (cont.)**`, `**Audit/FW X2**`, `**Terrain System**`)
// in addition to the original single-token ants shape (`**Sh4.**`,
// `**VEST-0042.**`). Trailing period inside the bold is stripped
// so `**Sh4**` and `**Sh4.**` both yield "Sh4". Capped at 80 chars
// to avoid swallowing the whole headline on a pathological
// `**all text is bold**` bullet. Non-greedy match (`.{1,80}?`)
// stops at the FIRST closing `**`, not the last.
bool extractBoldId(const QString &lineHead, QString *id) {
    static const QRegularExpression rx(QStringLiteral(
        "^\\*\\*(.{1,80}?)\\*\\*"));
    const auto m = rx.match(lineHead);
    if (!m.hasMatch()) return false;
    QString captured = m.captured(1);
    if (captured.endsWith(QLatin1Char('.'))) captured.chop(1);
    captured = captured.trimmed();
    if (captured.isEmpty()) return false;
    if (id) *id = captured;
    return true;
}


// ANTS-1811 — truncate to maxChars + "…" without splitting a UTF-16 surrogate
// pair. A headline whose maxChars boundary lands on the high half of an emoji
// (common — headlines carry status/theme emoji) would otherwise emit a lone
// surrogate that the card renderer shows as U+FFFD / a broken glyph.
static void truncateEllipsis(QString &s, int maxChars) {
    if (s.size() <= maxChars) return;
    int cut = maxChars;
    if (cut > 0 && s.at(cut - 1).isHighSurrogate()) --cut;
    s.truncate(cut);
    s.append(QStringLiteral("…"));
}

// ANTS-2075 — store both the untruncated headline (for locator use:
// roadmap_log's headline locator hashes the FULL disk headline, so a
// caller handed only the 120-char display cap can't re-target a long
// narrator bullet) and the display-capped headline. Single source of
// truth for the truncate-then-assign pattern repeated at every headline
// site in the parser.
// ANTS-3808 § 2.1 — `headlineEnd` is set HERE and not at the call sites, so a
// fifth headline site cannot be added without deciding what the render will
// reconstruct. It is an offset into `rec.body`, never a re-match of the stored
// headline: the reader NORMALISES what it stores (the GFM prose fallback
// removes `**` markers and strips a trailing caret anchor), so on exactly those
// bullets the headline is not a substring of the line it came from and a text
// match would silently strip nothing.
static void assignHeadline(BulletRecord &rec, QString h, int headlineEnd) {
    rec.headlineFull = h;
    truncateEllipsis(h, 120);  // ANTS-1811 — surrogate-safe
    rec.headline = h;
    rec.headlineEnd = headlineEnd;
}

// ANTS-3808 § 2.1 — the offset the migration strips, or -1 for "strip nothing".
// `end` is one past the text this parse consumed for the head line; it is only
// a PREFIX of the body when nothing but the leading `[id]` token and whitespace
// precedes `start`. A native bullet whose bold span sits mid-prose
// (`- ✅ fixed the **thing**.`) leaves that prose unconsumed, so stripping to
// `end` would DELETE it — INV-5 forbids losing text, and the render re-emitting
// the headline a second time is the lesser outcome.
//
// This guard is BEYOND § 2.1's table, which assumes the consumed span is always
// a prefix. Measured against this project's own ROADMAP.md on 2026-08-04 it
// fires on ZERO bullets — every apparent mid-prose bold there is a soft-wrapped
// headline whose closing `**` sits on the next line (ANTS-1561), which rxBold's
// DotMatchesEverythingOption matches as one head-anchored span. So it costs no
// duplication today and exists to keep the lossy case unreachable.
static int headlinePrefixEnd(const QString &body, int start, int end) {
    QStringView lead = QStringView(body).left(start).trimmed();
    if (lead.startsWith(QLatin1Char('['))) {
        const qsizetype close = lead.indexOf(QLatin1Char(']'));
        if (close >= 0) lead = lead.sliced(close + 1).trimmed();
    }
    return lead.isEmpty() ? end : -1;
}

// The end of `body`'s first line — the whole head line, for the branches that
// consume it entirely (the GFM em-dash split and the GFM prose fallback).
static int headLineEnd(const QString &body) {
    const int nl = int(body.indexOf(QLatin1Char('\n')));
    return nl < 0 ? int(body.size()) : nl;
}

QString splitOnEmDash(const QString &head) {
    static const QString sep1 = QString::fromUtf8(" \xE2\x80\x94 ");  // " — "
    int idx = head.indexOf(sep1);
    int sepLen = sep1.size();
    if (idx < 0) {
        const QString sep2 = QStringLiteral(" -- ");
        idx = head.indexOf(sep2);
        sepLen = sep2.size();
    }
    // ANTS-1785 — dropped the ` - ` (space-hyphen-space) fallback. It is
    // too ambiguous to be a reliable label/headline separator: a bold-ID
    // label or headline containing hyphenated prose (`**FW - W5** - …`,
    // `add X - and Y`) made indexOf split at the wrong hyphen and
    // truncate the headline. With only the em-dash and ` -- ` separators
    // (both unambiguous), an ambiguous ` - ` bullet now falls through to
    // the rxBold headline extractor at the caller instead of mis-splitting.
    if (idx < 0) return QString();
    return head.mid(idx + sepLen).trimmed();
}

// Try to extract a caret anchor at line end (`^vest-0042`).
// Returns the anchor body (without the caret) on match.
QString extractCaretAnchor(const QString &line) {
    static const QRegularExpression rx(QStringLiteral(
        "\\^([a-z0-9-]+)\\s*$"));
    const auto m = rx.match(line);
    if (!m.hasMatch()) return QString();
    return m.captured(1);
}

// `(COMPLETE)` / `(DONE)` heading-completion marker. Whole-word,
// case-insensitive. Returns true on match.
bool headingIsComplete(const QString &heading) {
    static const QRegularExpression rx(
        QStringLiteral("\\b(complete|done)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return rx.match(heading).hasMatch();
}

// Map a GFM checkbox char to the ants status emoji.
QString gfmStatusFromCheckbox(QChar ch) {
    if (ch == QLatin1Char('x') || ch == QLatin1Char('X'))
        return QStringLiteral("✅");
    return QStringLiteral("📋");
}

// Try to strip an inline status emoji prefix from the head; on
// match, updates `status` and returns true. The prefix wins over
// the checkbox state per spec INV-4.
bool stripInlineEmoji(QString &head, QString &status) {
    if (head.startsWith(QString::fromUtf8(kEmojiDone))) {
        status = QStringLiteral("✅");
        head.remove(0, QString::fromUtf8(kEmojiDone).size());
    } else if (head.startsWith(QString::fromUtf8(kEmojiPlanned))) {
        status = QStringLiteral("📋");
        head.remove(0, QString::fromUtf8(kEmojiPlanned).size());
    } else if (head.startsWith(QString::fromUtf8(kEmojiInProgress))) {
        status = QStringLiteral("🚧");
        head.remove(0, QString::fromUtf8(kEmojiInProgress).size());
    } else if (head.startsWith(QString::fromUtf8(kEmojiConsidered))) {
        status = QStringLiteral("💭");
        head.remove(0, QString::fromUtf8(kEmojiConsidered).size());
    } else {
        return false;
    }
    while (!head.isEmpty() && head.front().isSpace()) head.remove(0, 1);
    return true;
}


// ANTS-3808 § 2.2 — the six trailer matchers. They were `static const` inside
// parseBullets() and reachable from nowhere; trailerValuesIn() needs the same
// objects, so they move out behind accessors and are SHARED, not copied.
// Function-local statics rather than namespace-scope globals, following this
// project's own precedent for a shared compiled pattern (rxCommitSha() in
// remotecontrol.cpp) — construction stays lazy and ordered.
//
// Every comment below is the pattern's own, moved verbatim with it.

// ANTS-4065 § 2.2 — UN-ANCHORED, exactly as rxLanes() was by ANTS-2058 and for
// the same measured reason: 99 bullets in this project alone write the field
// inline (`… not in this fold. Kind: doc-fix.`), and the old `^\\s*` anchor read
// every one of them as declaring nothing. `makeItem()` then defaulted the kind
// to `implement` — silently, which is the loss that spec exists to stop.
//
// ANTS-3722's negative lookbehind comes with the un-anchoring, because the two
// are one change: a bullet that QUOTES the trailer key is talking ABOUT it,
// never declaring it, and without the guard "the `Kind:` trailer" parses as
// `kind = "trailer"`. rxLanes() already paid for that regression once.
//
// CaseInsensitiveOption is DROPPED, reversing ANTS-3407 for this one label.
// That option and the anchor were safe together; un-anchored, case tolerance
// matches prose ("…changed the kind: of work we do…") as a declaration, which
// is why rxLanes() has never had it. The cost is that a hand-typed `kind:` stops
// parsing, and § 2.2 accepts it: once a project is migrated the render is the
// sole writer, so only pre-migration files carry a hand-typed label. INV-9 pins
// the behaviour so the reversal is tested rather than assumed.
//
// MultilineOption is retained for parity with rxLanes() and is inert now that
// `^` is gone — the `\\n` stop comes from the character class.
//
// ANTS-4077 — the BOLD spelling is accepted too, exactly as rxLayman() has
// accepted it since ANTS-1861. 20 bullets in this project declare
// `**Kind:** fix.` and every one was read as declaring nothing; un-anchoring
// alone made that worse, matching inside the label and capturing the closing
// `**` plus the prose after it into extras.source_kind. The contract is PARITY
// with the plain spelling — a qualifier-bearing value still runs to the first
// period in both, so no qualifier-stripping rule is introduced here.
//
// THREE lookbehinds, because one cannot see past the asterisks. PCRE2 requires
// each to be fixed-length, so the backtick guard is spelled once per offset the
// label can sit at: `` `Kind: ``, `` `*Kind: `` and `` `**Kind: ``. Without the
// last two, ``  `**Kind:** implement.` `` — a bullet QUOTING the trailer, which
// the bullets documenting this format do constantly — parses as a declaration.
const QRegularExpression &rxKind() {
    static const QRegularExpression rx(
        QStringLiteral("(?<!`)(?<!`\\*)(?<!`\\*\\*)"
                       "(?:\\*\\*)?Kind:(?:\\*\\*)?\\s*([^\\.\\n]+?)\\s*[\\.\\n]"),
        QRegularExpression::MultilineOption);
    return rx;
}
// ANTS-2058 — no `^` anchor. Bullets routinely write their metadata
// inline as one prose sentence (`Kind: refactor. Lanes: backend tests.
// Source: …`), so `Lanes:` lands mid-line, not at a line start. The
// old `^\\s*` anchor matched only line-leading `Lanes:` and returned
// lanes:[] for every inline bullet — while rxKind worked purely because
// `Kind:` happened to sit first. Match `Lanes:` anywhere; the non-greedy
// capture still stops at the first period/newline.
// ANTS-3407 — deliberately NOT CaseInsensitive (unlike the anchored
// Kind:/Layman:/Evidence: labels). Because this pattern is un-anchored,
// case-insensitivity would match a lowercase "lanes:" occurring mid-prose
// (e.g. "split the review lanes: audit, tests.") and mis-extract it as
// metadata. Canonical `Lanes:` is the only form the writer emits.
// ANTS-3722 — but NOT when the label is inside backticks. Un-anchoring is
// still right (ANTS-2058's inline trailer is real), so the guard is the
// narrowest thing that separates the two: a bullet that QUOTES the trailer
// keys is talking ABOUT them, never declaring them. Reading ANTS-3696's own
// body back returned lanes:["`/`Source:` trailer below the"], captured out
// of the sentence "the `Layman:`/`Kind:`/`Lanes:`/`Source:` trailer" — and
// the corpus most likely to write that sentence is the one documenting the
// roadmap format, i.e. exactly the bullets a lane filter should trust.
// ANTS-4077 — the bold spelling, and the widened backtick guard, both for the
// same reasons rxKind() gained them; the two keys are written side by side on
// one line and only one of them parsing is the shape this fixes. Measured over
// the corpus 2026-08-09: 3 lines write `**Lanes:**` (which used to land the
// closing `**` in the first lane, so a bullet declaring `**Lanes:** core`
// reported a lane named `** core`), and 2 quote `` `**Lanes:**` `` while
// writing about the format.
const QRegularExpression &rxLanes() {
    static const QRegularExpression rx(
        QStringLiteral("(?<!`)(?<!`\\*)(?<!`\\*\\*)"
                       "(?:\\*\\*)?Lanes:(?:\\*\\*)?\\s*(.+?)\\s*[\\.\\n]"),
        QRegularExpression::MultilineOption);
    return rx;
}
// ANTS-1154-INV-4: optional Layman: line — case-insensitive label,
// takes the rest of the line up to a period or newline.
// ANTS-1861: match both plain "Layman:" and bold "**Layman:**" forms
// (roadmap_log writes the bold version).
// NOTE — `[\\.\\n]` is intentional: it strips the trailing sentence
// period so rec.layman is punctuation-free (INV-4 invariant). CHANGELOG
// consumers that need the full sentence should use match->body via
// rxBoldLayman (see remotecontrol.cpp cmdChangelogLog, ANTS-1933).
const QRegularExpression &rxLayman() {
    static const QRegularExpression rx(
        QStringLiteral("^\\s*(?:\\*\\*)?Layman:(?:\\*\\*)?\\s*(.+?)\\s*[\\.\\n]"),
        QRegularExpression::MultilineOption |
        QRegularExpression::CaseInsensitiveOption);
    return rx;
}
// ANTS-3382 — optional `Evidence:` line listing file paths. Unlike
// Lanes:, the capture runs to end-of-line (NOT to the first period):
// evidence paths routinely contain dots (`photos/IMG_2031.jpg`), so a
// `[^\\.\\n]` stop would truncate at the extension.
// ANTS-3407 — CaseInsensitiveOption for parity with rxLayman/rxKind, so a
// hand-edited `evidence:`/`EVIDENCE:` label is still recognised. The MCP
// writer always emits capital `Evidence:`, so the round-trip is unchanged.
const QRegularExpression &rxEvidence() {
    static const QRegularExpression rx(
        QStringLiteral("^\\s*Evidence:\\s*([^\\n]+)"),
        QRegularExpression::MultilineOption |
        QRegularExpression::CaseInsensitiveOption);
    return rx;
}
// ANTS-3764 — the `Source:` provenance line (ANTS-3757 § 2.1.1 / § 2.8).
// Three rules, each measured against ROADMAP.md on 2026-07-31 rather
// than inherited from whichever sibling key looked closest:
//  - Capture runs to END OF LINE, not to the first period. 61 of 1282
//    values carry an internal period — `in-session-2026-07-29
//    (rpmlint.log, first successful build).` — so rxKind's stop-at-period
//    rule would cut 4.8% of the corpus mid-value. rxEvidence already runs
//    to end-of-line for exactly this reason.
//  - UN-ANCHORED with rxLanes' backtick guard. 157 occurrences sit inline
//    in a prose trailer rather than at a line start, which is ANTS-2058's
//    finding for `Lanes:`; 22 are backticked mentions of the key itself,
//    which ANTS-3722's guard excludes. Deliberately NOT case-insensitive,
//    same reasoning as rxLanes: un-anchored + case-folded would match a
//    lowercase "source:" mid-prose.
//  - The label may be bold. 24 lines in the corpus write `**Source:**`,
//    and rxLayman already tolerates the same shape for its own key;
//    without the optional pair the closing `**` lands in the value.
// ANTS-4077 — the backtick guard widened to three fixed-length lookbehinds.
// The single `(?<!`)` could not see past the optional `**` this key had
// accepted since ANTS-3764, so a backticked `` `**Source:**` `` — 1 line in the
// corpus — matched at the label and was read as a declaration.
const QRegularExpression &rxSource() {
    static const QRegularExpression rx(
        QStringLiteral("(?<!`)(?<!`\\*)(?<!`\\*\\*)"
                       "(?:\\*\\*)?Source:(?:\\*\\*)?\\s*([^\\n]+)"));
    return rx;
}
//  - The value stops at a following trailer key: 10 lines write two keys
//    on one line (`Source: regression. Lanes: packaging.`). Capital-led
//    (a lowercase `lanes:` mid-prose is prose, per rxLanes) and
//    optionally bold, since roadmap_log writes `**Layman:**`.
const QRegularExpression &rxTrailerKey() {
    static const QRegularExpression rx(
        QStringLiteral("\\s(?:\\*\\*)?(?:Kind|Lanes|Layman|Evidence):"));
    return rx;
}

// ANTS-3808 § 2.2.1 — fill one TrailerMatch from a match of `rx` against
// `body`. `offset` is the CAPTURE's start, which is what a consumer quoting
// the value needs; `anchored` is computed off the MATCH's start, because every
// pattern puts literal text between the line start and group 1.
TrailerMatch matchIn(const QRegularExpression &rx, const QString &body) {
    TrailerMatch out;
    const QRegularExpressionMatch m = rx.match(body);
    if (!m.hasMatch())
        return out;
    out.value    = m.captured(1);
    out.offset   = m.capturedStart(1);
    const int at = m.capturedStart(0);
    out.anchored = (at == 0) || (at > 0 && body.at(at - 1) == QLatin1Char('\n'));
    return out;
}

// ANTS-4065 § 2.2 / INV-11 — the same fill, taking the LAST occurrence.
//
// Match precedence became load-bearing the moment rxKind() lost its anchor.
// bulletText() appends `it.body` BEFORE the trailer lines, so in a rendered
// bullet a mid-prose `Kind:` always sits ahead of the canonical one; a
// first-match read would adopt the stale prose value on every re-import and
// break § 2.6's round trip on `kind`, a governed column. The trailer wins
// because it is the occurrence the render authored.
//
// Only `kind` uses this. The other four keys stay on matchIn(): none of them is
// re-emitted by the render into a body that may already discuss it, so changing
// their precedence would be a behaviour change with no defect behind it.
TrailerMatch matchLastIn(const QRegularExpression &rx, const QString &body) {
    TrailerMatch out;
    auto i = rx.globalMatch(body);
    while (i.hasNext()) {
        const QRegularExpressionMatch m = i.next();
        out.value    = m.captured(1);
        out.offset   = m.capturedStart(1);
        const int at = m.capturedStart(0);
        out.anchored = (at == 0) || (at > 0 && body.at(at - 1) == QLatin1Char('\n'));
    }
    return out;
}

// Split a comma-separated trailer value the way parseBullets() always has:
// SkipEmptyParts, each part trimmed, whitespace-only parts dropped.
QStringList splitTrailerList(const QString &raw) {
    QStringList out;
    // Bind split() result first — clazy `range-loop-detach`
    // (ANTS-1122 audit-fold-in r2 2026-04-30).
    const QStringList parts = raw.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty()) out.append(trimmed);
    }
    return out;
}

// ANTS-1530 — parse a doc in `pass-headings` format. Each `####
// Pass N.M (SEVERITY, SIZE) …` heading becomes one bullet; the
// status emoji is derived from the first `- **Status**: <word>`
// line that follows the heading (bounded by the next heading or 50
// lines, whichever first). ID synthesised as `PASS-<major>-<minor>`
// so the bullet survives the default `[PROJ-NNNN]` filter and the
// numbering reverses cleanly back to the source heading.
QVector<BulletRecord>
parsePassHeadingBullets(const QStringList &lines) {
    QVector<BulletRecord> out;
    // ANTS-2035 — capture an optional letter-led `.<SUB>` sub-pass
    // suffix (`41.5.B`) so a parent and its sub-passes synthesise
    // DISTINCT ids. Before this the `.B` fell into the headline tail
    // and `Pass 41.5` / `Pass 41.5.B` both became `PASS-41-5`, which
    // the duplicate-ID detector flagged as a false collision. The
    // suffix is letter-led, so a purely numeric third level
    // (`Pass 3.1.2`) is left in the tail as before (surgical scope).
    static const QRegularExpression rxHead(
        QStringLiteral("^####\\s+Pass\\s+(\\d+)\\.(\\d+)"
                       "(?:\\.([A-Za-z][A-Za-z0-9]*))?\\s*"
                       "(?:\\(([^)]*)\\))?\\s*(.*?)\\s*$"));
    // ANTS-2039 — group 1 captures an optional leading status-emoji run
    // (any non-word, non-space glyph: ✅/📋/🚧/💭). Before this, a
    // leading emoji — not a word char and not whitespace — fell between
    // `:\s*` and the keyword class and blocked the capture entirely, so
    // `- **Status**: ✅ Done` left statusWord empty and defaulted to
    // planned (📋). Group 2 is the keyword, now optional so a bare emoji
    // (`- **Status**: ✅`) still matches and is mapped below.
    static const QRegularExpression rxStatusLine(
        QStringLiteral("^\\s*[-*]\\s*\\*\\*Status\\*\\*\\s*:\\s*"
                       "([^\\sA-Za-z0-9_-]+)?\\s*([A-Za-z0-9_-]*)"),
        QRegularExpression::CaseInsensitiveOption);
    QString currentSectionHeading;
    QString currentSectionSlug;
    int     currentSectionLevel = 0;
    QSet<QString> seenSlugs;
    for (int i = 0; i < lines.size(); ++i) {
        const QString &raw = lines[i];
        QString headingText;
        const int level = headingLevel(raw, &headingText);
        if (level == 2 || level == 3) {
            currentSectionHeading = headingText;
            currentSectionLevel   = level;
            currentSectionSlug    = uniqueSlug(seenSlugs, headingText);
            continue;
        }
        if (level != 4) continue;
        const QRegularExpressionMatch m = rxHead.match(raw);
        if (!m.hasMatch()) continue;
        const int major = m.captured(1).toInt();
        const int minor = m.captured(2).toInt();
        const QString sub  = m.captured(3).trimmed();   // "B" (sub-pass)
        const QString meta = m.captured(4).trimmed();   // "CRITICAL, S"
        const QString tail = m.captured(5).trimmed();
        // Status lookahead. 50-line cap keeps the scan bounded on
        // sparse docs; a heading without a Status marker within the
        // window defaults to planned (📋).
        QString statusWord;
        // ANTS-3764 — the author's Status value, verbatim. The reader
        // classifies on a lowercased keyword and throws the rest away;
        // ANTS-3757 § 2.7 stores the whole remainder of the line so a
        // qualifier tail (`done (v3.20.0, …). Adds catalogs for …`) is not
        // lost, and re-deriving it from `body` is the second parser § 2.3
        // forbids. Matching still folds case and absorbs a leading `*`;
        // STORAGE strips nothing, so `**un-gated (2026-07-05).**` keeps its
        // asterisks.
        QString statusValue;
        const int probeCap = std::min<int>(
            lines.size(), i + 51);
        for (int j = i + 1; j < probeCap; ++j) {
            const QString &peek = lines[j];
            // Stop at any heading level ≤ 4 (next sibling/parent).
            if (peek.startsWith(QStringLiteral("#")) &&
                !peek.startsWith(QStringLiteral("#####"))) {
                break;
            }
            const QRegularExpressionMatch sm = rxStatusLine.match(peek);
            if (sm.hasMatch()) {
                const QString emoji = sm.captured(1);
                statusWord = sm.captured(2).trimmed().toLower();
                // ANTS-2039 — a bare status emoji with no trailing
                // keyword (`- **Status**: ✅`) is authoritative: map the
                // glyph straight to its canonical keyword so the
                // emoji→status branch below classifies it. Keyword wins
                // when both are present (`✅ Done` → "done").
                if (statusWord.isEmpty() && !emoji.isEmpty()) {
                    if      (emoji == QString::fromUtf8("\xE2\x9C\x85"))     statusWord = QStringLiteral("done");        // ✅
                    else if (emoji == QString::fromUtf8("\xF0\x9F\x9A\xA7")) statusWord = QStringLiteral("in-progress"); // 🚧
                    else if (emoji == QString::fromUtf8("\xF0\x9F\x92\xAD")) statusWord = QStringLiteral("deferred");    // 💭
                    else if (emoji == QString::fromUtf8("\xF0\x9F\x93\x8B")) statusWord = QStringLiteral("todo");        // 📋
                }
                // A content-free `- **Status**:` line (both groups
                // empty) is not a classification — keep scanning for a
                // real status marker within the window.
                if (!statusWord.isEmpty()) {
                    // ANTS-3764 — the value starts at the first capture
                    // that participated: group 1 when a leading emoji or
                    // `*` run is present, group 2 otherwise. Both sit
                    // after the regex's `:\s*`, so this is the first
                    // non-space character of the value.
                    int valueStart = sm.capturedStart(1);
                    if (valueStart < 0) valueStart = sm.capturedStart(2);
                    statusValue = peek.mid(valueStart).trimmed();
                    break;
                }
            }
        }
        // ANTS-2030 — collect the under-heading prose (the
        // `- **Status**:` / `- **Finding**:` / `- **Decision**:` /
        // `- **Items**:` bullets) as the bullet body, bounded by the
        // next heading level ≤ 4. Before this the body was set to the
        // headline, so `roadmap_query include_body:true` was a no-op on
        // pass-headings roadmaps (body == headline). The 2 KiB
        // truncation + body_truncated flag is applied downstream at
        // emit by rcSetBodyFields, so no cap is needed here.
        QStringList bodyLines;
        // ANTS-3764 — the block's last non-blank line, which is where its
        // span ends: the trailing blanks trimmed below are outside it, so
        // two records never overlap (ANTS-3757 INV-11 partitions on this).
        int lastIdx = i;
        for (int j = i + 1; j < lines.size(); ++j) {
            const QString &peek = lines[j];
            if (peek.startsWith(QStringLiteral("#")) &&
                !peek.startsWith(QStringLiteral("#####"))) {
                break;
            }
            bodyLines.append(peek);
            if (!peek.trimmed().isEmpty()) lastIdx = j;
        }
        // Trim leading/trailing blank lines without disturbing interior
        // blanks (a Status/Finding block may be paragraph-separated).
        while (!bodyLines.isEmpty() && bodyLines.first().trimmed().isEmpty())
            bodyLines.removeFirst();
        while (!bodyLines.isEmpty() && bodyLines.last().trimmed().isEmpty())
            bodyLines.removeLast();
        const QString passBody = bodyLines.join(QChar('\n'));
        BulletRecord rec;
        rec.id     = sub.isEmpty()
            ? QStringLiteral("PASS-%1-%2").arg(major).arg(minor)
            : QStringLiteral("PASS-%1-%2-%3").arg(major).arg(minor).arg(sub);
        rec.format = QStringLiteral("pass-headings");
        // ANTS-3764 — the designator the heading regex just consumed. `id`
        // is synthesised from it and the heading is gone by the time a
        // record exists, so recovering it afterwards would mean re-matching
        // rxHead. major/minor go through toInt() here exactly as they do
        // for the id, so passIdFromDesignator(passDesignator) == id — the
        // agreement ANTS-3757 INV-10 asserts.
        rec.passDesignator = sub.isEmpty()
            ? QStringLiteral("%1.%2").arg(major).arg(minor)
            : QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(sub);
        rec.sourceStatus = statusValue;
        rec.firstLine    = i + 1;
        rec.lastLine     = lastIdx + 1;
        if (statusWord == QStringLiteral("done") ||
            statusWord == QStringLiteral("shipped") ||
            statusWord == QStringLiteral("completed")) {
            rec.status = QStringLiteral("✅");
        } else if (statusWord == QStringLiteral("in-progress") ||
                   statusWord == QStringLiteral("in_progress") ||
                   statusWord == QStringLiteral("inprogress") ||
                   statusWord == QStringLiteral("doing") ||
                   statusWord == QStringLiteral("wip") ||
                   // ANTS-4071 — `partial` means begun, not not-started. It
                   // used to reach the else-branch and import as 📋, which says
                   // "nothing has happened here" about a block whose own Status
                   // line reads `partial (v3.6.20). Phase A landed`.
                   statusWord == QStringLiteral("partial")) {
            rec.status = QStringLiteral("🚧");
        } else if (statusWord == QStringLiteral("deferred") ||
                   statusWord == QStringLiteral("considered") ||
                   statusWord == QStringLiteral("parked")) {
            rec.status = QStringLiteral("💭");
        } else {
            // todo / planned / unknown / absent
            rec.status = QStringLiteral("📋");
        }
        // Headline: re-emit the meta in parens so SEVERITY+SIZE
        // survive in headline_oneline (no dedicated bullet fields
        // for them yet; cheapest fidelity-preserving option).
        QString headline = tail;
        if (!meta.isEmpty()) {
            headline = QStringLiteral("(%1) %2").arg(meta, tail).trimmed();
        }
        // ANTS-3808 — a pass block's headline comes from the `####` HEADING,
        // which is not part of `body` at all, so the render reconstructs it
        // out of nothing the body holds and there is nothing to strip. The one
        // exception is the fallback on the next line: when the block has no
        // prose, `body` IS the headline, and the whole of it is consumed.
        assignHeadline(rec, headline,  // ANTS-2075 / ANTS-1811
                       passBody.isEmpty() ? int(headline.size()) : 0);
        // ANTS-2030 — real under-heading prose; fall back to the
        // headline only when the heading has no content beneath it
        // (keeps body non-empty, matching the prior contract).
        rec.body           = passBody.isEmpty() ? headline : passBody;
        rec.sectionHeading = currentSectionHeading;
        rec.sectionLevel   = currentSectionLevel;
        if (!currentSectionHeading.isEmpty()) {
            rec.sectionSlug = currentSectionSlug;
        }
        out.append(rec);
    }
    return out;
}

// ANTS-3793 § 2.1.1 — the two halves of the per-bullet grammar, lifted out of
// parseBullets()'s document walk so the store reader can run the SAME code
// rather than a second copy of it.
//
// The seam needs "exactly what parseBullets() would assign if it parsed the
// bullet RoadmapRender::bulletText(item) returns", and reproducing that field
// by field would have meant re-implementing eight things that live in this
// file and nowhere else — rxId, rxBold, extractBoldId, rxIdShaped,
// rxLeadToken, rxLeadBracketId, headlinePrefixEnd and stripInlineEmoji. That
// is the second bullet grammar ANTS-3808's INV-2 forbids, so the split is here
// instead. What stays with the caller is what a bullet's own text cannot
// decide: section attribution and the line span, both properties of the
// DOCUMENT — which is exactly why a store, having neither, supplies its own.

// The body collection walk. `i` enters as the index of the bullet line and
// leaves as the first line that is NOT part of the bullet, so the caller's
// `lastLine` is `i` on every exit. ANTS-1426: a blank line inside the body is
// tolerated when the next non-blank line is still an indented continuation.
QString collectBulletBody(const QStringList &lines, int &i, const QString &head) {
    QString body = head;
    ++i;
    while (i < lines.size()) {
        const QString &cont = lines[i];
        if (cont.trimmed().isEmpty()) {
            // Peek to the next non-blank line. If it's still an
            // indented continuation (and not a new top-level
            // bullet), absorb the blank and keep collecting.
            int peek = i + 1;
            while (peek < lines.size() && lines[peek].trimmed().isEmpty()) {
                ++peek;
            }
            if (peek >= lines.size()) break;          // INV-5
            const QString &nextLine = lines[peek];
            // INV-3: a top-level bullet (col 0) is a new entry.
            if (nextLine.startsWith(QStringLiteral("- ")) ||
                nextLine.startsWith(QStringLiteral("* "))) break;
            // INV-4: a heading at col 0 ends the body.
            if (nextLine.startsWith(QStringLiteral("#"))) break;
            // INV-2: indented continuation — absorb the blank
            // line(s) (as a single '\n' in the body) and skip
            // forward to the continuation line.
            if (nextLine.startsWith(QStringLiteral("  "))) {
                body.append('\n');
                i = peek;
                continue;
            }
            // Anything else at col 0 ends the body.
            break;
        }
        if (cont.startsWith(QStringLiteral("- ")) ||
            cont.startsWith(QStringLiteral("* "))) break;
        if (cont.startsWith(QStringLiteral("  "))) {
            body.append('\n');
            body.append(cont.trimmed());
            ++i;
            continue;
        }
        break;
    }
    return body;
}

// Everything the bullet's own text decides. `head` is the bullet line with its
// `- ` marker and status marker already removed — the caller strips those,
// because the STRIP is what tells the two document formats apart and, on the
// native path, its FAILURE is what makes a marker-less bullet produce no
// record at all. `rec.status`, the section fields and the line span are the
// caller's; every other member is assigned here.
void fillBulletRecord(BulletRecord &rec, const QString &head, const QString &body,
                      bool gfmHere) {
    // ANTS-1405 — widened from `\[ANTS-(\d+)\]` to accept any
    // `[PROJ-NNNN]` token shape per the shareable
    // docs/standards/roadmap-format.md § 3.5.1 spec. Captures the full
    // token (e.g. "ANTS-1234", "MAME-CURATOR-42", "mame-curator-7"),
    // which is assigned verbatim to rec.id below. Back-compat for
    // ANTS bullets is preserved because the captured string matches
    // the previous "ANTS-%1".arg(N) shape byte-for-byte.
    static const QRegularExpression rxId(
        QStringLiteral("\\[(") + idTokenPattern() + QStringLiteral(")\\]"));
    // ANTS-1547 — lazy `(.+?)` instead of `[^*]+`. Bullets whose bold
    // span wraps inline-code with a literal `*` (e.g. `**Three review
    // surfaces (`cold_eyes_*` / ...)** `) would otherwise fall through
    // boldMatch.hasMatch() with `rec.headline` left empty, and
    // roadmap_query would emit headline:"" / headline_oneline:"".
    // ANTS-1561 — DotMatchesEverythingOption lets `.` match `\n` so
    // bullets whose bold headline soft-wraps across two source lines
    // (`**foo\n  bar.**`) are captured as one span instead of being
    // skipped, which previously caused rxBold to fall through to the
    // next `**Layman:**` bold token and emit headline:"Layman:" — the
    // contract `headline_oneline` advertises (one-line summary) was
    // violated for every multi-line-headline bullet.
    static const QRegularExpression rxBold(
        QStringLiteral("\\*\\*(.+?)\\*\\*"),
        QRegularExpression::DotMatchesEverythingOption);
    // ANTS-4066 — rxBold has no notion of Markdown code spans, so a headline
    // that legitimately QUOTES a bold marker inside backticks used to end at
    // the first marker inside that span and lose every character after it,
    // silently. ANTS-1702's headline quotes the C signature
    // `runMain(int argc, char **argv)`: those asterisks are pointer syntax, and
    // the parser stopped dead on them. 12 bullets corpus-wide, and the source is
    // correct in all 12 — rewriting the markdown would mean corrupting a C
    // signature to suit a regex, so the fix is here.
    //
    // The mask is LENGTH-PRESERVING (only `*` inside a span becomes `x`), which
    // is what lets every match offset index the original string. Captures are
    // sliced from `body`, never taken from the mask, so the stored headline is
    // the author's bytes.
    //
    // An UNTERMINATED backtick masks nothing: swallowing the tail would hide the
    // headline's own closing `**` and lose the headline entirely, which is worse
    // than the truncation this removes. A ``double-backtick`` span is likewise
    // left alone — the pairing is per backtick — which is a miss, not a
    // corruption, and no corpus bullet writes one.
    const QString maskedBody = [&body] {
        QString out = body;
        qsizetype i = 0;
        while (true) {
            const qsizetype open = out.indexOf(QLatin1Char('`'), i);
            if (open < 0) break;
            const qsizetype close = out.indexOf(QLatin1Char('`'), open + 1);
            if (close < 0) break;
            for (qsizetype k = open + 1; k < close; ++k)
                if (out.at(k) == QLatin1Char('*')) out[k] = QLatin1Char('x');
            i = close + 1;
        }
        return out;
    }();
    // The capture, taken from the ORIGINAL at the mask's offsets.
    const auto boldText = [&body](const QRegularExpressionMatch &m) {
        return body.mid(m.capturedStart(1), m.capturedLength(1)).trimmed();
    };

    QString boldId;      // multi-prefix bold-ID token, e.g. "Sh4"
    QString bracketId;   // ANTS-1987 — head-anchored bare-bracket id, e.g. "Cl9"
    QString boldCand;
    if (gfmHere) {
        // Bold-ID token preservation. Multi-prefix projects
        // get their existing ID respected.
        //
        // ANTS-3773 — but only when the bold run LOOKS like an id. This
        // branch used to adopt any bold run at the head of a bullet, on
        // the stated convention that in GFM the leading bold span is an
        // ID-label. Measured over the corpus 2026-08-01, that convention
        // is half true: one project writes this shape, with 288 real ids
        // (`MT1`, `FW W5`, `Audit X1`) and 166 bold runs that are plain
        // prose (`Photo mode`, `Aerodynamics`, `Asset-pipeline tooling`).
        // Prose adopted as an id is not cosmetic — two bullets sharing a
        // lead-in fold to one identity, which fails ANTS-3756's
        // UNIQUE (project_id, id_fold) and refuses that project's whole
        // migration (ANTS-3765 § 2.5). It hit 17 bullets in that project.
        //
        // The guard is a TRAILING COLON and nothing more, and the
        // narrowness is the point. It is not a heuristic:
        // idTokenPattern() is `[A-Za-z0-9][A-Za-z0-9_-]*-\d+`, so a colon
        // cannot occur in an id at all, and `**Phase 9E-2:**` is a label
        // introducing prose rather than a name for the item.
        //
        // Two wider rules were tried against the corpus and both are
        // wrong. The ants-v1 guard below demands a whitespace-free token,
        // which would lose the real ids `FW W5` and `Audit X1`. Requiring
        // a DIGIT would drop 133 further prose lead-ins and looked right
        // on the ten roadmaps measured — but ANTS-1438's own tests carry
        // a Vestige fixture whose ids are `Terrain System` and
        // `JustBoldNoSeparator`, digitless and deliberate, so it broke a
        // shipped contract that measurement of those ten files could not
        // see.
        //
        // So a digitless prose lead-in is still adopted, and two bullets
        // sharing one still collide (3D_Engine has two `**Photo mode**`
        // bullets). Nothing in the text can separate that from a real
        // multi-word id, which is the argument for ANTS-3771: let a
        // project DECLARE its id format instead of inferring one.
        if (extractBoldId(head, &boldCand) &&
            !boldCand.endsWith(QLatin1Char(':'))) {
            boldId = boldCand;
        }
    } else {
        // ANTS-1987 — extract a leading bold-ID token here too, not
        // only on the GFM branch. A native emoji bullet whose ID is
        // a bold-dotted token (`- 📋 **Cl9.**`) otherwise produced an
        // empty id (rxId below only matches a bracketed `[PROJ-NNNN]`
        // with a `-<digits>` tail), so the bullet fell out as a
        // narrator and vanished from roadmap_query. Unlike the GFM
        // branch (where the leading bold span is an ID-label by
        // convention), in ants-v1 the bold span is normally the
        // HEADLINE (`[ID] **headline**`), so adopt a bold token as
        // the id ONLY when it is ID-SHAPED — a single whitespace-free
        // token (`Cl9` / `Sh4` / `Ts20-DE1`). This keeps a bold-prose
        // narrator bullet (`- 🚧 **In-progress thing.**`) id-less.
        // extractBoldId is head-anchored, so the standard
        // `[ID] **headline**` form (head starts with `[`) never fires.
        if (extractBoldId(head, &boldCand)) {
            static const QRegularExpression rxIdShaped(
                QStringLiteral("^[A-Za-z][A-Za-z0-9_.-]*$"));
            if (rxIdShaped.match(boldCand).hasMatch()) boldId = boldCand;
        }
    }
    rec.format = gfmHere ? QStringLiteral("github-task-list")
                         : QStringLiteral("ants-v1");

    // ANTS-1987 (bracket-ID completion) — index the
    // `- <emoji> [Cl9] **headline**` form Vestige actually authors
    // (Cl9 / Cl10 / CE18). After the checkbox/emoji prefix is
    // stripped, `head` begins with the bracket; the body-wide rxId
    // only matches a dashed `[PROJ-NNNN]`, so a bare project-local id
    // (`[Cl9]`) produced an empty id and the bullet fell out as a
    // narrator — invisible to roadmap_query id/ids/section. Match a
    // HEAD-ANCHORED bracket whose contents are ID-shaped (single
    // whitespace-free token) and NOT a markdown link (`](`), so a
    // mid-prose `[ref]` or a `[label](url)` link can never qualify.
    // This is a positional signal — it does NOT widen the shared
    // idTokenPattern (rxId still rejects dash-less brackets in body
    // prose). Only fills in when no bold-ID was found.
    if (boldId.isEmpty()) {
        static const QRegularExpression rxLeadBracketId(
            QStringLiteral("^\\[([A-Za-z][A-Za-z0-9_.-]*)\\](?!\\()"));
        const auto lb = rxLeadBracketId.match(head);
        if (lb.hasMatch()) bracketId = lb.captured(1);
    }

    // ANTS-3764 — the leading-slot token AS WRITTEN, before any grammar
    // test (ANTS-3757 § 2.5 / § 2.6). Deliberately more permissive than
    // rxLeadBracketId above, which feeds `id` and must not change: any
    // bracketed run without whitespace qualifies, so a digit-led prefix
    // (`[3D_E-0042]`) and an off-grammar `[Cl9]` both survive to be
    // classified downstream rather than being decided here. This is the
    // field `id` cannot stand in for — `id` takes the first
    // `[<PREFIX>-NNNN]` found ANYWHERE in the body, so a bullet whose
    // slot says `[Cl9]` and whose prose cites `[ANTS-9999]` reports the
    // citation, and it conflates conforming with off-grammar ids.
    // The one thing this DOES decide is the `(?![(:])` guard: a token
    // followed by `(` or `:` is a markdown link, not an id.
    static const QRegularExpression rxLeadToken(
        QStringLiteral("^\\[([^\\]\\s]+)\\](?![(:])"));
    const auto leadTok = rxLeadToken.match(head);
    if (leadTok.hasMatch()) {
        rec.idToken = leadTok.captured(1);
    } else if (!boldId.isEmpty()) {
        // The other shape the leading slot takes: `**Sh4.**` / `**FW W5**`
        // (extractBoldId has already dropped the token's trailing `.`).
        rec.idToken = boldId;
    }

    // Extract structured fields from body.
    const auto idMatch = rxId.match(body);
    if (idMatch.hasMatch()) {
        // ANTS-1405 — capture group is the full token; assign
        // verbatim. For `[ANTS-NNNN]` the value is byte-identical
        // to the pre-1405 `"ANTS-%1".arg(\d+)` form.
        rec.id = idMatch.captured(1);
    } else if (!boldId.isEmpty()) {
        // ANTS-1428 — multi-prefix bold-ID preservation.
        rec.id = boldId;
    } else if (!bracketId.isEmpty()) {
        // ANTS-1987 — head-anchored bare-bracket id (`[Cl9]`).
        rec.id = bracketId;
    }
    // ANTS-1438 — surface the bold-ID on the record so the
    // envelope can emit a dedicated `bold_id` field. Keeps the
    // `id` semantics back-compatible (matches boldId here, but
    // a synthetic content-hash if boldId was empty further down).
    if (!boldId.isEmpty()) rec.boldId = boldId;
    // ANTS-1438-INV-4 — GFM bullets with a bold-ID *and* an
    // em-dash separator have a natural (id, headline) split:
    // `**FW W5 (cont.)** — add a reference-regression spec ...`
    // The em-dash convention is used by every Vestige bullet
    // and any other project that wraps an ID-as-label. When the
    // split matches, set the headline directly from the post-
    // separator prose; the rxBold-based fallback below stays
    // for INV-5 (no separator) and ants-v1 (no boldId).
    if (gfmHere && !boldId.isEmpty()) {
        const QString afterSep = splitOnEmDash(head);
        if (!afterSep.isEmpty()) {
            QString h = afterSep;
            static const QRegularExpression rxTrailAnchor(
                QStringLiteral("\\s*\\^[a-z0-9-]+\\s*$"));
            h.replace(rxTrailAnchor, QString());
            h = h.trimmed();
            // ANTS-3808 — the bold-ID label and the post-separator
            // headline are both re-emitted by the render (as `[id]` and
            // `**headline**`), so the whole head line is consumed.
            assignHeadline(rec, h, headLineEnd(body));  // ANTS-2075 / ANTS-1811
        }
    }
    const auto boldMatch = rxBold.match(maskedBody);   // ANTS-4066
    // ANTS-2046 — a bold span only stands in as the headline when it is
    // HEAD-ANCHORED (the bullet text *starts* with it). A GFM task item
    // like `Ambient soundscapes (…) — **deferred to Phase 10.**` carries
    // its real title as leading prose and a TRAILING bold deferral
    // marker; grabbing that trailing bold collapsed every such sibling
    // to the same "deferred…" headline — and, because the synthetic id
    // is a content-hash of the headline, to the same fabricated id.
    // Mirrors the head-anchored rule ANTS-1987 applied to bracket IDs.
    // Native (ants-v1) bullets are `[ID] **headline**`, where the bold
    // IS the headline, so the gate only constrains the GFM path.
    bool boldIsHeadAnchored = false;
    if (boldMatch.hasMatch()) {
        boldIsHeadAnchored =
            QStringView(body).left(boldMatch.capturedStart())
                .trimmed().isEmpty();
    }
    if (!rec.headline.isEmpty()) {
        // INV-4 already set the headline from the em-dash split;
        // skip the bold-or-prose fallback for this row.
    } else if (boldMatch.hasMatch() &&
               (!gfmHere || boldIsHeadAnchored)) {
        QString h = boldText(boldMatch);   // ANTS-4066 — sliced from `body`
        // ANTS-3808 — one past the text this branch consumes for the head
        // line, tracked alongside `h` because each sub-case below ends
        // somewhere different.
        int consumedEnd = boldMatch.capturedEnd();
        // Strip a trailing `.` if the bold token is actually
        // a Bold-ID locator (e.g. `**Sh4.**`); the visible
        // headline of a bold-ID'd bullet is the *next* bold
        // chunk, but if there's only one, use the post-`.`
        // text after the token.
        if (!boldId.isEmpty() && h == boldId + QLatin1Char('.')) {
            // Find the next bold token after the bold-ID.
            const int after = boldMatch.capturedEnd();
            const auto m2 = rxBold.match(maskedBody, after);   // ANTS-4066
            if (m2.hasMatch()) {
                h = boldText(m2);
                consumedEnd = m2.capturedEnd();
            } else {
                // Use the prose immediately following the
                // bold-ID token; trim to one line.
                h = body.mid(after).trimmed();
                const int nl = h.indexOf(QLatin1Char('\n'));
                if (nl >= 0) h = h.left(nl).trimmed();
                // The rest of the head line went into the headline.
                const qsizetype nlAt = body.indexOf(QLatin1Char('\n'), after);
                consumedEnd = nlAt < 0 ? int(body.size()) : int(nlAt);
            }
        }
        assignHeadline(rec, h,  // ANTS-2075 / ANTS-1811
                       headlinePrefixEnd(body, boldMatch.capturedStart(),
                                         consumedEnd));
    } else if (gfmHere) {
        // ANTS-1428 — GFM bullets often have no `**bold**`
        // formatting at all (Vestige's roadmap is mixed). Use
        // the first line of body, stripped of any trailing
        // caret anchor, as the headline. ANTS-2046 — this also
        // catches the trailing-bold case routed here by the
        // head-anchored gate above; strip the `**` emphasis
        // markers so the headline reads as clean prose (the real
        // item text + any trailing deferral note), not literal
        // markdown, and stays unique per sibling.
        QString h = body;
        const int nl = h.indexOf(QLatin1Char('\n'));
        if (nl >= 0) h = h.left(nl);
        // Drop trailing caret anchor + surrounding whitespace.
        static const QRegularExpression rxTrailAnchor(
            QStringLiteral("\\s*\\^[a-z0-9-]+\\s*$"));
        h.replace(rxTrailAnchor, QString());
        h.remove(QStringLiteral("**"));  // ANTS-2046 — de-markup
        h = h.trimmed();
        // ANTS-3808 — the first line IS the headline here, so all of it is
        // consumed. This is the branch a headline-text match could not
        // strip: `h` has had its `**` markers and caret anchor removed, so
        // it is not a substring of the line it came from.
        assignHeadline(rec, h, headLineEnd(body));  // ANTS-2075 / ANTS-1811
    }
    // ANTS-1428 — synthetic ID when GFM bullet has no bold-ID
    // and no `[ANTS-NNNN]` legacy token. Stable across line
    // reorders (depends only on the normalised headline).
    if (gfmHere && rec.id.isEmpty()) {
        const QString norm = normaliseHeadline(rec.headline);
        if (!norm.isEmpty()) {
            rec.id = base36Lower(fnv1a64(norm), 10);
            rec.synthetic = true;
        }
    }
    // ANTS-3808 § 2.2.1 — one pass over the five trailer keys, through the
    // accessor the render also uses. Assigning the record's fields FROM it
    // is what makes INV-4's equality hold by construction rather than by
    // two implementations happening to agree; an absent key yields an
    // empty value / empty list, which is what the per-key `hasMatch()`
    // guards used to leave behind.
    const TrailerValues tv = trailerValuesIn(body);
    rec.kind     = tv.kind.value;       // ANTS-3407
    rec.lanes    = tv.lanesList;        // ANTS-2058 / ANTS-3722
    rec.layman   = tv.layman.value;     // ANTS-1154-INV-4
    rec.evidence = tv.evidenceList;     // ANTS-3382
    rec.source   = tv.source.value;     // ANTS-3764
    rec.body = body;
}
}  // namespace

// ANTS-1438 — split `head` on the first em-dash separator (` — `,
// preferred; ` -- ` / ` - ` accepted as lenient fallbacks for
// projects that haven't typographic-em-dashed). Returns the trimmed
// post-separator text on match, empty QString otherwise. Whitespace
// brackets the separator so mid-word hits (e.g. `re-render`) don't
// match.
// ANTS-1784 — single source of truth for the project-ID token shape
// (`PROJ-NNNN`: a letter-led prefix + `-` + digits, per
// docs/standards/roadmap-format.md § 3.5.1). parseBullets anchors it in
// `[...]`; parseShippedDates anchors it on word boundaries (CHANGELOG
// prose). Both derive from this so the accepted ID shape can't drift
// between the two parsers.
QString idTokenPattern() {
    // ANTS-3492 — prefix may be digit-led if it contains ≥1 letter
    // (3D_E-0042). Bare, unanchored fragment shared by parseBullets
    // (bracket-anchored) and parseShippedDates (\b-anchored); the leading
    // lookahead splices safely into both.
    return QStringLiteral("(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*-\\d+");
}

// Detect format by scanning the head of the document for one of
// three shapes. ants-v1 marker wins; otherwise look for GFM
// task-list bullets, then the ANTS-1530 `#### Pass N.M (SEVERITY,
// SIZE) … - **Status**: <word>` shape used by RetroDB's roadmap.
// We scan a larger budget (300 lines) so Pass-style docs with a
// long preamble still trigger the right adapter.
QString detectRoadmapFormat(const QStringList &lines, bool *sawSignal) {
    // ANTS-3766 § 2.1.1 — start at "no evidence" and raise it at each signal's
    // own match site. Doing it that way rather than deriving the flag from the
    // accumulated booleans at the bottom is what covers the early return
    // below: that path never reaches the bottom, and it is the one whose
    // mis-classification § 2.1.1 spends a paragraph on.
    if (sawSignal) *sawSignal = false;
    const auto signalled = [sawSignal] { if (sawSignal) *sawSignal = true; };
    if (lines.isEmpty()) return QStringLiteral("ants-v1");
    int seen = 0;
    bool hasGfm           = false;
    bool hasAntsV1Emoji   = false;
    int  passHeadings     = 0;
    int  statusMarkers    = 0;
    static const QRegularExpression rxAntsV1Bullet(
        QStringLiteral("^- (✅|📋|🚧|💭)"));
    static const QRegularExpression rxPassHeading(
        QStringLiteral("^####\\s+Pass\\s+\\d"));
    static const QRegularExpression rxStatusMarker(
        QStringLiteral("^\\s*[-*]\\s*\\*\\*Status\\*\\*\\s*:"),
        QRegularExpression::CaseInsensitiveOption);
    for (const auto &ln : lines) {
        if (ln.contains(QStringLiteral("<!-- ants-roadmap-format: 1 -->"))) {
            // An explicit declaration, matched before any bullet is examined —
            // so this is evidence, and the strongest kind.
            signalled();
            return QStringLiteral("ants-v1");
        }
        if (ln.trimmed().isEmpty()) continue;
        if (ln.startsWith(QStringLiteral("- [ ]")) ||
            ln.startsWith(QStringLiteral("- [x]")) ||
            ln.startsWith(QStringLiteral("- [X]"))) {
            hasGfm = true;
            signalled();
        }
        if (rxAntsV1Bullet.match(ln).hasMatch()) {
            hasAntsV1Emoji = true;
            signalled();
        }
        if (rxPassHeading.match(ln).hasMatch()) {
            ++passHeadings;
            signalled();
        }
        if (rxStatusMarker.match(ln).hasMatch()) {
            ++statusMarkers;
            signalled();
        }
        if (++seen >= 300) break;
    }
    // ANTS-1530 — pass-headings adapter. Triggered only when no ants-v1
    // emoji bullets are present (so we never override a native-format doc)
    // and the document carries at least two Pass-N.M headings AND two
    // `- **Status**: …` markers (the 2+2 threshold rules out an accidental
    // fenced-code example).
    //
    // ANTS-2048 — this MUST be checked before the `hasGfm` fallback. A
    // pass-headings roadmap routinely contains a stray `- [ ]` sub-task
    // checklist line, which flipped `hasGfm` true and returned
    // github-task-list before this strong, far-more-specific 2+2 signal
    // ever ran. roadmap_log flip_batch then misdetected the format as gfm
    // and returned bullet_not_found instead of the helpful pass-headings
    // format_mismatch (ANTS-2031). The 2+2 pass-headings signal is the
    // authoritative shape; a lone checkbox does not override it.
    if (!hasAntsV1Emoji && passHeadings >= 2 && statusMarkers >= 2) {
        return QStringLiteral("pass-headings");
    }
    if (hasGfm)          return QStringLiteral("github-task-list");
    return QStringLiteral("ants-v1");
}
// ANTS-3808 § 2.2.1 — the normalisation contract. Every step below is the one
// parseBullets() has always applied; the values are returned post-match, NOT as
// raw captures, because the render compares them against the store's columns
// and raw captures would never compare equal — the suppression would never
// fire and this verb would be documented as working while the duplication
// stayed live.
TrailerValues trailerValuesIn(const QString &body) {
    TrailerValues out;

    // ANTS-4065 INV-11 — LAST match, not first. See matchLastIn().
    out.kind   = matchLastIn(rxKind(), body);
    out.kind.value = out.kind.value.trimmed();

    out.layman = matchIn(rxLayman(), body);
    out.layman.value = out.layman.value.trimmed();

    // `lanes.value` is the RAW capture: parseBullets() applies neither a trim
    // nor a period chop before splitting it.
    out.lanes     = matchIn(rxLanes(), body);
    out.lanesList = splitTrailerList(out.lanes.value);

    // ANTS-3382 — `evidence.value` is the text the split is applied TO, which
    // for this key is trimmed and period-chopped first. Dots inside paths are
    // kept, so only ONE trailing period goes, and not when the value ends `..`.
    out.evidence = matchIn(rxEvidence(), body);
    if (out.evidence.offset >= 0) {
        QString evRaw = out.evidence.value.trimmed();
        if (evRaw.endsWith(QLatin1Char('.')) && !evRaw.endsWith(QStringLiteral("..")))
            evRaw.chop(1);
        out.evidence.value = evRaw;
        out.evidenceList   = splitTrailerList(evRaw);
    }

    // ANTS-3764 — Source: stops at a following trailer key (10 corpus lines
    // write two keys on one line), then trims, drops one trailing period, and
    // trims again.
    out.source = matchIn(rxSource(), body);
    if (out.source.offset >= 0) {
        QString srcRaw = out.source.value;
        const auto keyAt = rxTrailerKey().match(srcRaw);
        if (keyAt.hasMatch()) srcRaw = srcRaw.left(keyAt.capturedStart());
        srcRaw = srcRaw.trimmed();
        if (srcRaw.endsWith(QLatin1Char('.'))) srcRaw.chop(1);
        out.source.value = srcRaw.trimmed();
    }

    return out;
}

QVector<BulletRecord>
parseBullets(const QString &markdownText) {
    // ANTS-2119 (roadmapdialog M-1) — function-local memo, mirroring
    // reverseTopLevelSections' (size, ==) guard. parseBullets is the dominant
    // uncached per-render cost (full split('\n') + per-bullet regex +
    // detectRoadmapFormat pre-scan over up to 64 MiB of History-mode archive
    // markdown) and re-runs on every debounced keystroke / filter toggle in
    // renderCardsHtml. The input is identical across consecutive renders of the
    // same document, so the hit rate is ~100%; a content change invalidates it.
    static thread_local QString s_lastInput;
    static thread_local QVector<BulletRecord> s_lastResult;
    if (markdownText.size() == s_lastInput.size() && markdownText == s_lastInput)
        return s_lastResult;

    QVector<BulletRecord> out;
    // ANTS-3808 — the five trailer matchers moved out to file scope so
    // trailerValuesIn() could share them. ANTS-3793 — and the rest of the
    // per-bullet grammar followed them into fillBulletRecord(), one level up
    // and for the same reason: the store reader has to produce these records
    // too, and a second copy of this logic is what ANTS-3808's INV-2 forbids.
    // What is left in this loop is what only a DOCUMENT can decide — which
    // format it is in, which section a bullet sits under, and which lines it
    // spans.
    const QStringList lines = markdownText.split('\n');
    // ANTS-1428 — format detection runs once per parse. On
    // detection of github-task-list, the bullet-line matcher
    // accepts `- [ ]` / `- [x]` shapes and synthesises IDs via
    // FNV-1a 64-bit content-hash when no `**Bold-ID.**` token is
    // present. Native parse path is byte-identical to pre-1428
    // behaviour when format == "ants-v1".
    const QString docFormat = detectRoadmapFormat(lines);
    // ANTS-1530 — `pass-headings` is a fundamentally different
    // shape (bullets come from #### headings, not `- ` lines), so
    // it routes to a dedicated parser instead of slotting into the
    // main loop. Returns immediately; the rest of parseBullets
    // never sees pass-headings input.
    if (docFormat == QStringLiteral("pass-headings")) {
        return parsePassHeadingBullets(lines);
    }
    const bool isGfm = (docFormat == QStringLiteral("github-task-list"));

    QString currentSectionHeading;
    QString currentSectionSlug;
    int currentSectionLevel = 0;
    bool currentSectionComplete = false;
    // ANTS-1239 — seen-set for unique heading slugs. Mirror the
    // renderCardsHtml walk: both call uniqueSlug() in the same
    // document order, so the Nth "Performance" h3 here gets the same
    // slug as the Nth "Performance" h3 in the renderer.
    QSet<QString> seenSlugs;
    int i = 0;
    while (i < lines.size()) {
        const QString &raw = lines[i];
        // ANTS-1154-INV-5: track the most recent ## or ### heading so
        // each bullet can be attributed to its owning section without
        // a second walk.
        QString headingText;
        const int level = headingLevel(raw, &headingText);
        if (level == 2 || level == 3) {
            currentSectionHeading = headingText;
            currentSectionLevel = level;
            currentSectionSlug = uniqueSlug(seenSlugs, headingText);
            currentSectionComplete = isGfm && headingIsComplete(headingText);
            ++i;
            continue;
        }
        // Top-level bullet: `^- ` or `^* ` (two-space indent is a
        // continuation, not a bullet — same rule as renderHtml).
        const bool isBullet = raw.startsWith(QStringLiteral("- ")) ||
                              raw.startsWith(QStringLiteral("* "));
        if (!isBullet) { ++i; continue; }
        QString head = raw.mid(2);
        QString status;
        QString anchorValue;       // ANTS-1428 — caret anchor at line end

        // ANTS-1428 / Tier 1 GFM branch. When the document is in
        // github-task-list format AND this line starts with the
        // checkbox token, parse via the adapter; otherwise fall
        // through to the native emoji-prefix path. Native mode
        // (docFormat == "ants-v1") never engages the GFM branch.
        const bool gfmHere = isGfm &&
            (head.startsWith(QStringLiteral("[ ]")) ||
             head.startsWith(QStringLiteral("[x]")) ||
             head.startsWith(QStringLiteral("[X]")));
        if (gfmHere) {
            const QChar boxCh = head.at(1);
            status = gfmStatusFromCheckbox(boxCh);
            head.remove(0, 3);
            while (!head.isEmpty() && head.front().isSpace())
                head.remove(0, 1);
            // Inline emoji prefix wins over checkbox state (INV-4).
            QString inlineStatus;
            if (stripInlineEmoji(head, inlineStatus)) {
                status = inlineStatus;
            }
            // Heading `(COMPLETE)` inheritance — bullets in a
            // marked-complete section inherit ✅ unless an inline
            // emoji override won above. Here and not in
            // fillBulletRecord(): the heading is the document's, and a
            // bullet read on its own has no section to inherit from.
            if (status == QStringLiteral("📋") &&
                currentSectionComplete) {
                status = QStringLiteral("✅");
            }
            // Extract caret anchor (if any) from the line. We
            // look at the raw line (not the post-emoji-strip
            // head) so trailing anchors after body text still
            // match.
            anchorValue = extractCaretAnchor(raw);
        } else {
            // Native path. The strip's FAILURE is the skip, and that is the
            // rule ANTS-3793 § 2.1.2 leans on from the other side: a head line
            // carrying no status marker yields no record at all, which is what
            // keeps a `dropped` store item — whose emoji is the empty string by
            // design — out of the reader's results.
            if (!stripInlineEmoji(head, status)) {
                ++i;
                continue;
            }
        }

        BulletRecord rec;
        rec.status = status;
        rec.sectionHeading = currentSectionHeading;
        rec.sectionLevel = currentSectionLevel;
        if (!currentSectionHeading.isEmpty()) {
            rec.sectionSlug = currentSectionSlug;
        }
        rec.anchor = anchorValue;

        // Collect the bullet body — first line + subsequent indented
        // continuation lines — then let the shared grammar fill everything
        // the bullet's own text decides.
        const int bulletIdx = i;   // ANTS-3764 — span start, before the walk
        const QString body = collectBulletBody(lines, i, head);
        fillBulletRecord(rec, head, body, gfmHere);

        // ANTS-3764 — the span. `i` is the index of the first line that is
        // NOT part of this bullet on every exit from the walk above, so the
        // 1-based inclusive last line is `i` itself; absorbed blank lines are
        // always interior (the walk only absorbs one when an indented
        // continuation follows), so no trailing blank is ever claimed.
        rec.firstLine = bulletIdx + 1;
        rec.lastLine  = i;

        out.append(rec);
    }
    // ANTS-2119 (roadmapdialog M-1) — populate the memo for the next render.
    s_lastInput  = markdownText;
    s_lastResult = out;
    return out;
}

std::optional<BulletRecord> parseAntsV1Bullet(const QString &bulletText) {
    const QStringList lines = bulletText.split('\n');
    if (lines.isEmpty())
        return std::nullopt;
    const QString &raw = lines.front();
    // Same two markers the document walk accepts, and deliberately not a
    // relaxed version of them: a caller handing this function a line the walk
    // would have skipped must get the walk's answer.
    if (!raw.startsWith(QStringLiteral("- ")) &&
        !raw.startsWith(QStringLiteral("* ")))
        return std::nullopt;
    QString head = raw.mid(2);
    QString status;
    if (!stripInlineEmoji(head, status))
        return std::nullopt;   // no status marker — see the header comment

    BulletRecord rec;
    rec.status = status;
    int i = 0;
    const QString body = collectBulletBody(lines, i, head);
    fillBulletRecord(rec, head, body, /*gfmHere=*/false);
    return rec;
}
}  // namespace RoadmapParse
