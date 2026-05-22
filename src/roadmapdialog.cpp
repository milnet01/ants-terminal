#include "roadmapdialog.h"

#include "coloredtabbar.h"     // for ClaudeTabIndicator::color (ToolUse yellow)
#include "config.h"
#include "dialogchrome.h"      // ANTS-1242 — frameless dialog chrome
#include "roadmapindex.h"      // ANTS-1287 — canonical home for heading/slug helpers
#include "roadmapshortcutsdialog.h"  // ANTS-1236 — `?` cheatsheet overlay
#include "themes.h"
#include "titlebar.h"

#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QApplication>
#include <QClipboard>
#include <QListWidget>
#include <QMenu>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSplitter>
#include <QStringBuilder>
#include <QTabBar>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {

// Status emojis the ROADMAP legend documents. Keep in lock-step with
// the legend block at the top of ROADMAP.md and with INV-3 of the
// roadmap_viewer feature test.
constexpr const char *kEmojiDone        = "✅";
constexpr const char *kEmojiPlanned     = "📋";
constexpr const char *kEmojiInProgress  = "🚧";
constexpr const char *kEmojiConsidered  = "💭";

// ANTS-1235 — screen-reader-readable label for each status emoji.
// QT_TR_NOOP marks the label strings for lupdate extraction; the
// runtime translation happens at RoadmapDialog::statusAccessibleLabel
// lookup time via the RoadmapDialog tr() context.
struct StatusLabel { const char *emoji; const char *label; };
constexpr StatusLabel kStatusLabels[] = {
    {kEmojiDone,       QT_TR_NOOP("shipped")},
    {kEmojiInProgress, QT_TR_NOOP("in progress")},
    {kEmojiPlanned,    QT_TR_NOOP("planned")},
    {kEmojiConsidered, QT_TR_NOOP("considered")},
};
static_assert(std::size(kStatusLabels) == 4,
              "Add a label here when introducing a new status emoji.");

// ANTS-1236 — file-scope cheatsheet data table. Single source of truth
// for the Roadmap dialog's keyboard surface; the cheatsheet sub-dialog
// reads through `roadmapShortcutRows()` (defined below) and the
// feature test source-greps these literal byte sequences (INV-1 / INV-2
// / INV-5 / INV-8 — see docs/specs/ANTS-1236.md § 4).
//
// Adding a shortcut: bump the row count below + the test's exact-10
// assertions in tests/features/roadmap_shortcuts_cheatsheet/ in the
// same commit. The static_assert guards against silent drift.
struct ShortcutRow { const char *keys; const char *action; };
constexpr ShortcutRow kRoadmapShortcuts[] = {
    {"?",              QT_TR_NOOP("Show this cheatsheet")},
    {"/",              QT_TR_NOOP("Focus search box")},
    {"Esc",            QT_TR_NOOP("Close dialog")},
    {"F5",             QT_TR_NOOP("Refresh from disk")},
    {"Ctrl+C",         QT_TR_NOOP("Copy selection")},
    {"Ctrl+A",         QT_TR_NOOP("Select all")},
    {"↑ ↓",            QT_TR_NOOP("Scroll one line")},
    {"PgUp PgDn",      QT_TR_NOOP("Scroll one page")},
    {"Home End",       QT_TR_NOOP("Jump to top / bottom")},
    {"Tab Shift+Tab",  QT_TR_NOOP("Move focus to next / previous control")},
};
static_assert(std::size(kRoadmapShortcuts) == 10,
              "ANTS-1234/1236 INV-1: cheatsheet row count must match the test's "
              "exact-10 assertion. Bump both in lock-step when adding a new "
              "shortcut.");

QString htmlEscape(QString s) {
    s.replace('&', QStringLiteral("&amp;"));
    s.replace('<', QStringLiteral("&lt;"));
    s.replace('>', QStringLiteral("&gt;"));
    return s;
}

// ANTS-1106 + ANTS-1150 — Kind facet entries. Lifted to file scope
// so the ctor's build loop and the persisted-Kind-filter restore
// iterate the same source-of-truth table. Adding a new Kind here
// is a one-touch change: a new checkbox appears + the new value
// participates in persistence. objectNames are kept as literal
// strings so existing source-grep tests
// (`tests/features/roadmap_kind_facets/`) can pin them.
struct KindEntry {
    const char *value;
    const char *objectName;
    const char *labelTxt;
};
constexpr KindEntry kKinds[] = {
    {"implement",  "roadmap-filter-kind-implement",  "✨ implement"},
    {"fix",        "roadmap-filter-kind-fix",        "🐛 fix"},
    {"audit-fix",  "roadmap-filter-kind-audit-fix",  "🔍 audit-fix"},
    {"review-fix", "roadmap-filter-kind-review-fix", "🔁 review-fix"},
    {"doc",        "roadmap-filter-kind-doc",        "📚 doc"},
    {"doc-fix",    "roadmap-filter-kind-doc-fix",    "📝 doc-fix"},
    {"refactor",   "roadmap-filter-kind-refactor",   "🏗 refactor"},
    {"test",       "roadmap-filter-kind-test",       "🧪 test"},
    {"chore",      "roadmap-filter-kind-chore",      "🧹 chore"},
    {"release",    "roadmap-filter-kind-release",    "🚢 release"},
    {"research",   "roadmap-filter-kind-research",   "🔬 research"},
    {"ux",         "roadmap-filter-kind-ux",         "🎨 ux"},
};

// ANTS-1238 — per-density-tier CSS px values + vertical-padding
// scale, looked up by renderCardsHtml when building the embedded
// `<style>` block. Cozy column preserves pre-1238 byte-equal
// rendering (INV-1); Compact / Comfortable are -2 / +2 shifts on
// the body / heading / code groups, with a 9 px floor on the
// meta tier (INV-8). Spec: docs/specs/ANTS-1238.md § 2.f.
//
// Field naming mirrors the spec: bodyPx covers `body`,
// `.rm-summary`, `.rm-state`, `.rm-section-toggle`,
// `.rm-body-first`, `.rm-body-line` (all at the same value per
// tier). h1Px..h4Px cover the four heading levels. codePx
// covers `code`, `.rm-toggle`, `.rm-id`, `td`, `th`. metaPx
// covers `.rm-kind`, `.rm-section-counts`, `.rm-parent`,
// `.rm-date`. labelPx is `.rm-state-label` (one step below
// metaPx, with the floor applying first). Vertical padding
// scales: pMargin is `p` `margin:Npx 0`; hMarginTop /
// hMarginBottom are h1-h4 `margin:Tpx 0 Bpx 0`; cardPaddingY /
// cardPaddingX are `.rm-card` `padding:Ypx Xpx`; cardMargin
// is `.rm-card` `margin:Npx 0`; bodyFirstPaddingTop /
// bodyFirstMarginTop are `.rm-body-first` top spacing.
struct DensityTier {
    int bodyPx;
    int h1Px, h2Px, h3Px, h4Px;
    int codePx;
    int metaPx;
    int labelPx;
    int pMargin;
    int hMarginTop, hMarginBottom;
    int cardPaddingY, cardPaddingX;
    int cardMargin;
    int bodyFirstPaddingTop;
    int bodyFirstMarginTop;
};
constexpr DensityTier kDensityTable[3] = {
    // Compact: -2 px tier with 9 px floor on label + meta groups.
    {/*bodyPx*/11, /*h1Px*/14, /*h2Px*/11, /*h3Px*/10, /*h4Px*/10,
     /*codePx*/10, /*metaPx*/9, /*labelPx*/9,
     /*pMargin*/1, /*hMarginTop*/2, /*hMarginBottom*/1,
     /*cardPaddingY*/2, /*cardPaddingX*/6, /*cardMargin*/1,
     /*bodyFirstPaddingTop*/2, /*bodyFirstMarginTop*/1},
    // Cozy: current default. Byte-equal to pre-1238 renderer.
    {/*bodyPx*/13, /*h1Px*/16, /*h2Px*/13, /*h3Px*/12, /*h4Px*/11,
     /*codePx*/12, /*metaPx*/11, /*labelPx*/10,
     /*pMargin*/3, /*hMarginTop*/4, /*hMarginBottom*/2,
     /*cardPaddingY*/4, /*cardPaddingX*/8, /*cardMargin*/2,
     /*bodyFirstPaddingTop*/4, /*bodyFirstMarginTop*/2},
    // Comfortable: +2 px tier, more vertical headroom.
    {/*bodyPx*/15, /*h1Px*/18, /*h2Px*/15, /*h3Px*/14, /*h4Px*/13,
     /*codePx*/14, /*metaPx*/13, /*labelPx*/12,
     /*pMargin*/5, /*hMarginTop*/6, /*hMarginBottom*/4,
     /*cardPaddingY*/6, /*cardPaddingX*/12, /*cardMargin*/4,
     /*bodyFirstPaddingTop*/6, /*bodyFirstMarginTop*/3},
};
static_assert(std::size(kDensityTable) == 3,
              "kDensityTable must keep one row per Density enum value.");

QString densityToString(RoadmapDialog::Density d) {
    switch (d) {
        case RoadmapDialog::Density::Compact:     return QStringLiteral("compact");
        case RoadmapDialog::Density::Cozy:        return QStringLiteral("cozy");
        case RoadmapDialog::Density::Comfortable: return QStringLiteral("comfortable");
    }
    return QStringLiteral("cozy");
}

RoadmapDialog::Density densityFromString(const QString &s) {
    if (s == QStringLiteral("compact"))     return RoadmapDialog::Density::Compact;
    if (s == QStringLiteral("comfortable")) return RoadmapDialog::Density::Comfortable;
    // Includes "cozy" + any unknown/empty value — INV-4 fallback.
    return RoadmapDialog::Density::Cozy;
}

int densityToIndex(RoadmapDialog::Density d) {
    return static_cast<int>(d);
}

// ANTS-1238 § 3.a — bounds-clamps out-of-range indices to Cozy.
// QComboBox::currentIndexChanged can fire with -1 when its model
// is cleared; defensive clamp avoids out-of-bounds access on
// kDensityTable.
RoadmapDialog::Density indexToDensity(int idx) {
    if (idx == 0) return RoadmapDialog::Density::Compact;
    if (idx == 2) return RoadmapDialog::Density::Comfortable;
    return RoadmapDialog::Density::Cozy;
}

const DensityTier &tierFor(RoadmapDialog::Density d) {
    return kDensityTable[static_cast<int>(d)];
}

// ANTS-1287: headingLevel moved to RoadmapIndex namespace. The
// using-declaration is at file scope (after the anonymous namespace
// closes) — see just before parseBullets.

QString tocAnchorAt(int index) {
    return QStringLiteral("roadmap-toc-%1").arg(index);
}

// Backtick → <code>…</code> + **bold** → <strong>…</strong>.
// ANTS-1139 (0.7.70) added the bold pass per indie-review L7
// H-6 — pre-fix code rendered the literal `**` characters in
// body prose, defeating the format-spec invariant that every
// bullet has a "**bold headline**". Order matters: htmlEscape
// first (so `&`/`<`/`>` in user content don't smuggle markup),
// then code (so `**` inside `` ` `` doesn't get bolded), then
// bold.
QString applyInline(const QString &line) {
    QString s = htmlEscape(line);
    static const QRegularExpression rxCode(QStringLiteral("`([^`]+)`"));
    s.replace(rxCode,
              QStringLiteral("<code style=\"font-family:monospace\">\\1</code>"));
    // ANTS-1547 — lazy `(.+?)` instead of `[^*]+` so the regex still
    // matches when the bold span wraps an inline-code segment that
    // itself contains a `*` (e.g. `**foo `bar_*` baz**`). The pre-1547
    // exclusion broke parsing for ANTS-1518 / 1529 -shaped bullets.
    static const QRegularExpression rxBold(QStringLiteral("\\*\\*(.+?)\\*\\*"));
    s.replace(rxBold, QStringLiteral("<strong>\\1</strong>"));
    return s;
}

// Normalise a string for fuzzy matching: lowercase, hyphens/underscores
// → space, runs of whitespace collapsed, trailing punctuation trimmed.
QString fuzzy(const QString &in) {
    QString out;
    out.reserve(in.size());
    QChar last;
    for (QChar c : in) {
        QChar n = c.toLower();
        if (n == '-' || n == '_' || n == '/' || n == '\\') n = ' ';
        if (!n.isLetterOrNumber() && n != ' ') continue;
        if (n == ' ' && last == ' ') continue;
        out.append(n);
        last = n;
    }
    return out.trimmed();
}

// Strip a leading status emoji + bold marker from a bullet body. Used
// before the substring match for current-work signals so a bullet that
// starts with `✅ **State-dot palette**` matches a CHANGELOG line that
// reads "State-dot palette".
QString bulletPayload(QString body) {
    auto stripPrefix = [&](const char *needle) {
        if (body.startsWith(QString::fromUtf8(needle))) {
            body.remove(0, QString::fromUtf8(needle).size());
            while (!body.isEmpty() && body.front().isSpace()) body.remove(0, 1);
        }
    };
    stripPrefix(kEmojiDone);
    stripPrefix(kEmojiPlanned);
    stripPrefix(kEmojiInProgress);
    stripPrefix(kEmojiConsidered);
    if (body.startsWith(QStringLiteral("**"))) body.remove(0, 2);
    int closeBold = body.indexOf(QStringLiteral("**"));
    if (closeBold > 0 && closeBold < 80) body.truncate(closeBold);
    if (body.size() > 80) body.truncate(80);
    return body;
}

// Read CHANGELOG.md `[Unreleased]` block bullets — first 80 characters
// of each `^- ` or `^  - ` line, no leading whitespace, no leading
// markers. Stops at the next `^## ` heading.
QStringList readUnreleasedBullets(const QString &changelogPath) {
    QStringList out;
    QFile f(changelogPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    bool inBlock = false;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith(QStringLiteral("## "))) {
            const bool isUnreleased = line.contains(
                QStringLiteral("[Unreleased]"), Qt::CaseInsensitive);
            inBlock = isUnreleased;
            continue;
        }
        if (!inBlock) continue;
        if (!line.startsWith(QStringLiteral("- ")) &&
            !line.startsWith(QStringLiteral("* "))) continue;
        QString body = line.mid(2).trimmed();
        // Drop common emphasis markers.
        body.remove(QStringLiteral("**"));
        if (body.size() > 80) body.truncate(80);
        if (!body.isEmpty()) out.append(body);
    }
    return out;
}

// `git log -n 5 --format=%s` from `repoRoot`. Best-effort — returns an
// empty list on any failure (no git in PATH, not a repo, etc.).
QStringList readRecentCommitSubjects(const QString &repoRoot) {
    QStringList out;
    QProcess git;
    git.setWorkingDirectory(repoRoot);
    git.start(QStringLiteral("git"),
              {QStringLiteral("log"), QStringLiteral("-n"), QStringLiteral("5"),
               QStringLiteral("--format=%s")});
    if (!git.waitForFinished(1500)) return out;
    if (git.exitStatus() != QProcess::NormalExit || git.exitCode() != 0)
        return out;
    const QString stdoutStr = QString::fromUtf8(git.readAllStandardOutput());
    for (const QString &raw : stdoutStr.split('\n', Qt::SkipEmptyParts)) {
        const QString s = raw.trimmed();
        // Skip mechanical commits (release bumps, merges, reverts).
        static const QRegularExpression rxRelease(QStringLiteral("^\\d+\\.\\d+\\.\\d+:"));
        if (rxRelease.match(s).hasMatch()) continue;
        if (s.startsWith(QStringLiteral("Merge "))) continue;
        if (s.startsWith(QStringLiteral("Revert "))) continue;
        // Trim trailing parens/citations and take the meaningful slice.
        QString slice = s;
        const int dashPos = slice.indexOf(QStringLiteral(" — "));
        const int colonPos = slice.indexOf(QStringLiteral(": "));
        int cut = -1;
        if (dashPos > 0 && (colonPos < 0 || dashPos < colonPos)) cut = dashPos + 3;
        else if (colonPos > 0) cut = colonPos + 2;
        if (cut > 0 && cut < slice.size()) slice = slice.mid(cut);
        if (slice.size() > 80) slice.truncate(80);
        if (!slice.isEmpty()) out.append(slice);
    }
    return out;
}

// Reorder a markdown document so its top-level (`## `) sections appear
// in reverse order. Preamble (everything above the first `## `) and
// per-section content stay intact; only the section sequence flips.
// Used by renderHtml when SortOrder is DescendingChronological.
//
// ANTS-1140 — function-local cache keyed on the input string's
// (size, hash-of-prefix). reverseTopLevelSections is on the hot
// path for History-mode renders (every search keystroke +
// every filter toggle), and the input — particularly with
// archive markdown attached (ANTS-1125) — is up to 64 MiB. Cache
// hit rate is essentially 100% across consecutive renders of the
// same document; cache miss invalidates on any markdown content
// change.
QString reverseTopLevelSections(const QString &markdownText) {
    static thread_local QString s_lastInput;
    static thread_local QString s_lastOutput;
    if (markdownText.size() == s_lastInput.size() &&
            markdownText == s_lastInput) {
        return s_lastOutput;
    }
    const QStringList lines = markdownText.split('\n');
    QStringList preamble;
    QVector<QStringList> sections;
    QStringList *currentSection = nullptr;

    for (const QString &line : lines) {
        if (line.startsWith(QStringLiteral("## "))) {
            sections.push_back(QStringList());
            currentSection = &sections.last();
            currentSection->append(line);
            continue;
        }
        if (currentSection) {
            currentSection->append(line);
        } else {
            preamble.append(line);
        }
    }

    if (sections.isEmpty()) {
        // ANTS-1140 — populate cache even on the no-sections path
        // so subsequent identical calls hit it.
        s_lastInput = markdownText;
        s_lastOutput = markdownText;
        return markdownText;
    }

    QStringList out;
    out.reserve(lines.size());
    out += preamble;
    for (int i = sections.size() - 1; i >= 0; --i) {
        out += sections[i];
    }
    QString result = out.join('\n');
    // ANTS-1140 — cache the reversed output keyed on the input.
    s_lastInput = markdownText;
    s_lastOutput = result;
    return result;
}

// Extract the four-digit numeric suffix of an `[ANTS-NNNN]` token from
// `predicate` if it has the form `id:NNNN` (case-insensitive on the
// `id:` prefix). Returns -1 if not an id-shorthand predicate.
int parseIdShorthand(const QString &predicate) {
    if (predicate.size() < 4) return -1;
    if (!predicate.startsWith(QStringLiteral("id:"), Qt::CaseInsensitive))
        return -1;
    const QStringView digits = QStringView{predicate}.mid(3).trimmed();
    if (digits.isEmpty()) return -1;
    bool ok = false;
    const int n = digits.toString().toInt(&ok);
    return ok ? n : -1;
}

}  // namespace

unsigned RoadmapDialog::filterFor(Preset p) {
    switch (p) {
        case Preset::Full:
            return ShowDone | ShowPlanned | ShowInProgress |
                   ShowConsidered | ShowCurrent;
        case Preset::History:
            return ShowDone;
        case Preset::Current:
            return ShowInProgress | ShowCurrent;
        case Preset::Next:
            return ShowPlanned;
        case Preset::FarFuture:
            return ShowConsidered;
        case Preset::Custom:
            return 0;
    }
    return 0;
}

RoadmapDialog::SortOrder RoadmapDialog::sortFor(Preset p) {
    if (p == Preset::History) return SortOrder::DescendingChronological;
    return SortOrder::Document;
}

RoadmapDialog::Preset RoadmapDialog::presetMatching(unsigned filter,
                                                    SortOrder sort) {
    const Preset named[] = {
        Preset::Full, Preset::History, Preset::Current,
        Preset::Next, Preset::FarFuture,
    };
    for (Preset p : named) {
        if (filterFor(p) == filter && sortFor(p) == sort) return p;
    }
    return Preset::Custom;
}

QStringList RoadmapDialog::collectCurrentBullets() const {
    QStringList out;
    if (!m_changelogPath.isEmpty()) out += readUnreleasedBullets(m_changelogPath);
    const QFileInfo fi(m_roadmapPath);
    out += readRecentCommitSubjects(fi.absolutePath());
    return out;
}

// ANTS-1154-INV-5: slugify a heading string for section-tracking.
// Lowercase, non-alphanumeric runs collapse to `-`, leading/trailing
// dashes trimmed. Stable across heading reorders so persisted
// expand-state survives section moves.
// ANTS-1287: headingLevel + slugifyHeading + uniqueSlug moved to
// RoadmapIndex. File-scope using-declarations preserve the unqualified
// call surface for parseBullets, extractToc, renderCardsHtml.
// slugifyHeading is not called directly from this file (uniqueSlug
// wraps it inside the engine), but is kept here for symmetry and so
// any future direct caller resolves to the canonical engine version.
// See docs/specs/ANTS-1287.md § 7.
using RoadmapIndex::headingLevel;
using RoadmapIndex::uniqueSlug;

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
static QString idTokenPattern() {
    return QStringLiteral("[A-Za-z][A-Za-z0-9_-]*-\\d+");
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

// Detect format by scanning the head of the document for one of
// three shapes. ants-v1 marker wins; otherwise look for GFM
// task-list bullets, then the ANTS-1530 `#### Pass N.M (SEVERITY,
// SIZE) … - **Status**: <word>` shape used by RetroDB's roadmap.
// We scan a larger budget (300 lines) so Pass-style docs with a
// long preamble still trigger the right adapter.
QString detectRoadmapFormat(const QStringList &lines) {
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
            return QStringLiteral("ants-v1");
        }
        if (ln.trimmed().isEmpty()) continue;
        if (ln.startsWith(QStringLiteral("- [ ]")) ||
            ln.startsWith(QStringLiteral("- [x]")) ||
            ln.startsWith(QStringLiteral("- [X]"))) {
            hasGfm = true;
        }
        if (rxAntsV1Bullet.match(ln).hasMatch()) {
            hasAntsV1Emoji = true;
        }
        if (rxPassHeading.match(ln).hasMatch()) {
            ++passHeadings;
        }
        if (rxStatusMarker.match(ln).hasMatch()) {
            ++statusMarkers;
        }
        if (++seen >= 300) break;
    }
    if (hasGfm)          return QStringLiteral("github-task-list");
    // ANTS-1530 — pass-headings adapter. Triggered only when no
    // ants-v1 emoji bullets are present (so we never override a
    // native-format doc) and the document carries at least two
    // Pass-N.M headings AND two `- **Status**: …` markers (the
    // 2+2 threshold rules out an accidental fenced-code example).
    if (!hasAntsV1Emoji && passHeadings >= 2 && statusMarkers >= 2) {
        return QStringLiteral("pass-headings");
    }
    return QStringLiteral("ants-v1");
}

// ANTS-1530 — parse a doc in `pass-headings` format. Each `####
// Pass N.M (SEVERITY, SIZE) …` heading becomes one bullet; the
// status emoji is derived from the first `- **Status**: <word>`
// line that follows the heading (bounded by the next heading or 50
// lines, whichever first). ID synthesised as `PASS-<major>-<minor>`
// so the bullet survives the default `[PROJ-NNNN]` filter and the
// numbering reverses cleanly back to the source heading.
QVector<RoadmapDialog::BulletRecord>
parsePassHeadingBullets(const QStringList &lines) {
    QVector<RoadmapDialog::BulletRecord> out;
    static const QRegularExpression rxHead(
        QStringLiteral("^####\\s+Pass\\s+(\\d+)\\.(\\d+)\\s*"
                       "(?:\\(([^)]*)\\))?\\s*(.*?)\\s*$"));
    static const QRegularExpression rxStatusLine(
        QStringLiteral("^\\s*[-*]\\s*\\*\\*Status\\*\\*\\s*:\\s*"
                       "([A-Za-z0-9_-]+)"),
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
        const QString meta = m.captured(3).trimmed();   // "CRITICAL, S"
        const QString tail = m.captured(4).trimmed();
        // Status lookahead. 50-line cap keeps the scan bounded on
        // sparse docs; a heading without a Status marker within the
        // window defaults to planned (📋).
        QString statusWord;
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
                statusWord = sm.captured(1).trimmed().toLower();
                break;
            }
        }
        RoadmapDialog::BulletRecord rec;
        rec.id     = QStringLiteral("PASS-%1-%2").arg(major).arg(minor);
        rec.format = QStringLiteral("pass-headings");
        if (statusWord == QStringLiteral("done") ||
            statusWord == QStringLiteral("shipped") ||
            statusWord == QStringLiteral("completed")) {
            rec.status = QStringLiteral("✅");
        } else if (statusWord == QStringLiteral("in-progress") ||
                   statusWord == QStringLiteral("in_progress") ||
                   statusWord == QStringLiteral("inprogress") ||
                   statusWord == QStringLiteral("doing") ||
                   statusWord == QStringLiteral("wip")) {
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
        if (headline.size() > 120) {
            headline.truncate(120);
            headline.append(QStringLiteral("…"));
        }
        rec.headline       = headline;
        rec.body           = headline;
        rec.sectionHeading = currentSectionHeading;
        rec.sectionLevel   = currentSectionLevel;
        if (!currentSectionHeading.isEmpty()) {
            rec.sectionSlug = currentSectionSlug;
        }
        out.append(rec);
    }
    return out;
}

}  // namespace

QVector<RoadmapDialog::BulletRecord>
RoadmapDialog::parseBullets(const QString &markdownText) {
    QVector<BulletRecord> out;
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
    // MultilineOption so `^` anchors at the start of any line within
    // the bullet body — Kind: / Lanes: / Layman: live as continuation
    // lines, not at the start of the string.
    static const QRegularExpression rxKind(
        QStringLiteral("^\\s*Kind:\\s*([^\\.\\n]+?)\\s*[\\.\\n]"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression rxLanes(
        QStringLiteral("^\\s*Lanes:\\s*(.+?)\\s*[\\.\\n]"),
        QRegularExpression::MultilineOption);
    // ANTS-1154-INV-4: optional Layman: line — case-insensitive label,
    // takes the rest of the line up to a period or newline.
    static const QRegularExpression rxLayman(
        QStringLiteral("^\\s*Layman:\\s*(.+?)\\s*[\\.\\n]"),
        QRegularExpression::MultilineOption |
        QRegularExpression::CaseInsensitiveOption);

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
        QString boldId;            // multi-prefix bold-ID token, e.g. "Sh4"
        QString rowFormat;         // per-bullet format echo

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
            // emoji override won above.
            if (status == QStringLiteral("📋") &&
                currentSectionComplete) {
                status = QStringLiteral("✅");
            }
            // Extract caret anchor (if any) from the line. We
            // look at the raw line (not the post-emoji-strip
            // head) so trailing anchors after body text still
            // match.
            anchorValue = extractCaretAnchor(raw);
            // Bold-ID token preservation. Multi-prefix projects
            // get their existing ID respected.
            extractBoldId(head, &boldId);
            rowFormat = QStringLiteral("github-task-list");
        } else {
            // Native path — unchanged from pre-1428.
            if (!stripInlineEmoji(head, status)) {
                ++i;
                continue;
            }
            rowFormat = QStringLiteral("ants-v1");
        }

        BulletRecord rec;
        rec.status = status;
        rec.sectionHeading = currentSectionHeading;
        rec.sectionLevel = currentSectionLevel;
        if (!currentSectionHeading.isEmpty()) {
            rec.sectionSlug = currentSectionSlug;
        }
        rec.anchor = anchorValue;
        rec.format = rowFormat;

        // Collect the bullet body — first line + subsequent indented
        // continuation lines. ANTS-1426: a blank line inside the body
        // is tolerated when the next non-blank line is still an
        // indented continuation (CommonMark loose-list mode). Without
        // this rule, an author splitting the body into sub-blocks for
        // readability (as ANTS-1422 did) causes `Kind:` / `Lanes:` /
        // `Layman:` lines after the blank to be silently dropped.
        QString body = head;
        ++i;
        while (i < lines.size()) {
            const QString &cont = lines[i];
            if (cont.trimmed().isEmpty()) {
                // Peek to the next non-blank line. If it's still an
                // indented continuation (and not a new top-level
                // bullet), absorb the blank and keep collecting.
                int peek = i + 1;
                while (peek < lines.size() &&
                       lines[peek].trimmed().isEmpty()) {
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
                if (h.size() > 120) { h.truncate(120); h.append(QStringLiteral("…")); }
                rec.headline = h;
            }
        }
        const auto boldMatch = rxBold.match(body);
        if (!rec.headline.isEmpty()) {
            // INV-4 already set the headline from the em-dash split;
            // skip the bold-or-prose fallback for this row.
        } else if (boldMatch.hasMatch()) {
            QString h = boldMatch.captured(1).trimmed();
            // Strip a trailing `.` if the bold token is actually
            // a Bold-ID locator (e.g. `**Sh4.**`); the visible
            // headline of a bold-ID'd bullet is the *next* bold
            // chunk, but if there's only one, use the post-`.`
            // text after the token.
            if (!boldId.isEmpty() && h == boldId + QLatin1Char('.')) {
                // Find the next bold token after the bold-ID.
                const int after = boldMatch.capturedEnd();
                const auto m2 = rxBold.match(body, after);
                if (m2.hasMatch()) {
                    h = m2.captured(1).trimmed();
                } else {
                    // Use the prose immediately following the
                    // bold-ID token; trim to one line.
                    h = body.mid(after).trimmed();
                    const int nl = h.indexOf(QLatin1Char('\n'));
                    if (nl >= 0) h = h.left(nl).trimmed();
                }
            }
            if (h.size() > 120) { h.truncate(120); h.append(QStringLiteral("…")); }
            rec.headline = h;
        } else if (gfmHere) {
            // ANTS-1428 — GFM bullets often have no `**bold**`
            // formatting at all (Vestige's roadmap is mixed). Use
            // the first line of body, stripped of any trailing
            // caret anchor, as the headline.
            QString h = body;
            const int nl = h.indexOf(QLatin1Char('\n'));
            if (nl >= 0) h = h.left(nl);
            // Drop trailing caret anchor + surrounding whitespace.
            static const QRegularExpression rxTrailAnchor(
                QStringLiteral("\\s*\\^[a-z0-9-]+\\s*$"));
            h.replace(rxTrailAnchor, QString());
            h = h.trimmed();
            if (h.size() > 120) { h.truncate(120); h.append(QStringLiteral("…")); }
            rec.headline = h;
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
        const auto kindMatch = rxKind.match(body);
        if (kindMatch.hasMatch()) {
            rec.kind = kindMatch.captured(1).trimmed();
        }
        const auto lanesMatch = rxLanes.match(body);
        if (lanesMatch.hasMatch()) {
            const QString lanesRaw = lanesMatch.captured(1);
            // Bind split() result first — clazy `range-loop-detach`
            // (ANTS-1122 audit-fold-in r2 2026-04-30).
            const QStringList parts =
                lanesRaw.split(',', Qt::SkipEmptyParts);
            for (const QString &part : parts) {
                const QString trimmed = part.trimmed();
                if (!trimmed.isEmpty()) rec.lanes.append(trimmed);
            }
        }
        // ANTS-1154-INV-4
        const auto laymanMatch = rxLayman.match(body);
        if (laymanMatch.hasMatch()) {
            rec.layman = laymanMatch.captured(1).trimmed();
        }
        rec.body = body;

        out.append(rec);
    }
    return out;
}

QVector<RoadmapDialog::TocEntry>
RoadmapDialog::extractToc(const QString &markdownText) {
    QVector<TocEntry> out;
    const QStringList lines = markdownText.split('\n');
    int idx = 0;
    for (const QString &raw : lines) {
        QString text;
        const int level = headingLevel(raw, &text);
        if (level <= 0) continue;
        TocEntry e;
        e.level = level;
        e.text = text;
        e.anchor = tocAnchorAt(idx++);
        out.push_back(e);
    }
    return out;
}

// Pure renderer. See spec for the parsing rules. Returns a self-
// contained HTML fragment ready for QTextBrowser::setHtml.
QString RoadmapDialog::renderHtml(const QString &markdownText,
                                  unsigned filter,
                                  const QStringList &currentBullets,
                                  const QString &themeName,
                                  SortOrder sortOrder,
                                  const QString &searchPredicate,
                                  const QSet<QString> &kindFilter) {
    const QString sourceText =
        (sortOrder == SortOrder::DescendingChronological)
            ? reverseTopLevelSections(markdownText)
            : markdownText;
    const int idShorthand = parseIdShorthand(searchPredicate);
    const QString idMarker =
        idShorthand >= 0
            // ANTS-1660 — match `[<prefix>-NNNN]` for any prefix via the
            // `-NNNN]` suffix (the leading '-' + close bracket anchor it so
            // `-42]` won't match `-142]`/`-420]`). ANTS-NNNN still matches.
            ? QStringLiteral("-%1]").arg(idShorthand)
            : QString();
    const QString plainSearch =
        (idShorthand >= 0) ? QString() : searchPredicate.trimmed();

    const Theme &th = Themes::byName(themeName);
    const QString currentColor =
        ClaudeTabIndicator::color(ClaudeTabIndicator::Glyph::ToolUse).name();
    const bool wantDone = (filter & ShowDone) != 0;
    const bool wantPlanned = (filter & ShowPlanned) != 0;
    const bool wantInProgress = (filter & ShowInProgress) != 0;
    const bool wantConsidered = (filter & ShowConsidered) != 0;
    const bool wantCurrent = (filter & ShowCurrent) != 0;

    // Pre-fuzzied signal phrases for substring matching.
    QStringList signalsFuzzy;
    signalsFuzzy.reserve(currentBullets.size());
    for (const QString &s : currentBullets) {
        const QString f = fuzzy(s);
        if (f.size() >= 6) signalsFuzzy.append(f);
    }

    auto isCurrent = [&](const QString &bulletBody) {
        if (signalsFuzzy.isEmpty()) return false;
        const QString fHay = fuzzy(bulletPayload(bulletBody));
        if (fHay.size() < 6) return false;
        for (const QString &s : signalsFuzzy) {
            if (fHay.contains(s) || s.contains(fHay)) return true;
        }
        return false;
    };

    QString html;
    html.reserve(markdownText.size() * 2);
    html += QStringLiteral(
        "<html><head><style>"
        "body{font-family:sans-serif;color:%1;}"
        "h1,h2,h3,h4{color:%2;font-weight:bold;}"
        "h1{font-size:18px;} h2{font-size:16px;}"
        "h3{font-size:14px;} h4{font-size:13px;}"
        "code{background:%3;padding:0 4px;border-radius:3px;}"
        "ul{margin-top:2px;margin-bottom:2px;}"
        "li{margin-bottom:4px;}"
        ".cur{border-left:4px solid %4;padding-left:8px;background:rgba(229,194,74,0.08);}"
        "table{border-collapse:collapse;}"
        "td,th{border:1px solid %5;padding:2px 6px;}"
        "</style></head><body>")
        .arg(th.textPrimary.name(),
             th.textPrimary.name(),
             th.bgSecondary.name(),
             currentColor,
             th.border.name());

    enum class BulletKind { Other, Done, Planned, InProgress, Considered };
    auto classify = [](const QString &body) {
        if (body.startsWith(QString::fromUtf8(kEmojiDone))) return BulletKind::Done;
        if (body.startsWith(QString::fromUtf8(kEmojiPlanned))) return BulletKind::Planned;
        if (body.startsWith(QString::fromUtf8(kEmojiInProgress))) return BulletKind::InProgress;
        if (body.startsWith(QString::fromUtf8(kEmojiConsidered))) return BulletKind::Considered;
        return BulletKind::Other;
    };

    const QStringList lines = sourceText.split('\n');

    // ANTS-1140 — pre-walk Kind extraction (one pass; cached
    // across consecutive renderHtml calls on the same input).
    // Pre-fix code did a per-bullet peek-ahead inside the main
    // walk: O(bullets × continuation_lines) per render with a
    // regex match per bullet. With 270 bullets × ~3 cont lines
    // and 8-10 renders/sec while the user types into the
    // search box with a Kind filter active, this was the
    // dominant render cost. The cache + pre-walk pattern
    // mirrors `reverseTopLevelSections` (0.7.70).
    QHash<int, QString> kindByLine;
    if (!kindFilter.isEmpty()) {
        static thread_local QString s_lastInput;
        static thread_local QHash<int, QString> s_lastKindMap;
        if (sourceText.size() == s_lastInput.size() &&
                sourceText == s_lastInput) {
            kindByLine = s_lastKindMap;
        } else {
            static const QRegularExpression rxKind(
                QStringLiteral("^\\s*Kind:\\s*([^\\.\\n]+?)\\s*[\\.\\n]"),
                QRegularExpression::MultilineOption);
            int j = 0;
            while (j < lines.size()) {
                const QString &row = lines[j];
                const bool isBullet =
                    row.startsWith(QStringLiteral("- ")) ||
                    row.startsWith(QStringLiteral("* "));
                if (!isBullet) { ++j; continue; }
                // Assemble bullet body: head + indented
                // continuation lines until blank or next
                // top-level bullet.
                QString bodyFull = row.mid(2);
                int k = j + 1;
                while (k < lines.size()) {
                    const QString &cont = lines[k];
                    if (cont.trimmed().isEmpty()) break;
                    if (cont.startsWith(QStringLiteral("- ")) ||
                        cont.startsWith(QStringLiteral("* "))) break;
                    if (cont.startsWith(QStringLiteral("  "))) {
                        bodyFull.append('\n');
                        bodyFull.append(cont.trimmed());
                        ++k;
                        continue;
                    }
                    break;
                }
                const auto km = rxKind.match(bodyFull);
                if (km.hasMatch())
                    kindByLine.insert(j, km.captured(1).trimmed());
                j = k;  // skip past the continuation lines
            }
            s_lastInput = sourceText;
            s_lastKindMap = kindByLine;
        }
    }

    bool inList = false;
    bool skipBlock = false;       // dropping a filtered-out bullet's continuation
    int headingIdx = 0;           // increments per emitted heading; matches extractToc

    auto closeListIfOpen = [&]() {
        if (inList) {
            html += QStringLiteral("</ul>");
            inList = false;
        }
    };

    for (int i = 0; i < lines.size(); ++i) {
        const QString &raw = lines[i];

        // Headings — always rendered, regardless of filters. Each
        // gets a `<a name="roadmap-toc-N">` anchor so the TOC sidebar
        // can scroll to it via QTextBrowser::scrollToAnchor.
        QString hText;
        if (const int level = headingLevel(raw, &hText); level > 0) {
            closeListIfOpen();
            skipBlock = false;
            const QString anchor = tocAnchorAt(headingIdx++);
            html += QStringLiteral("<a name=\"%1\"></a>").arg(anchor);
            html += QStringLiteral("<h%1>").arg(level)
                  + applyInline(hText)
                  + QStringLiteral("</h%1>").arg(level);
            continue;
        }

        // Markdown table rows — render as a <pre> block so the columns
        // line up. Coalesce consecutive `|` lines into one block.
        if (raw.startsWith(QStringLiteral("|"))) {
            // ANTS-1139 — render markdown tables as `<table>` not
            // `<pre>` (indie-review L7 H-5). Pre-fix code wrapped
            // the raw row text in `<pre>` so the user saw a
            // monospace block of `|` characters instead of a
            // proper table — which the QTextBrowser HTML
            // renderer + the existing `table {…}` CSS in the
            // header would otherwise render correctly.
            //
            // Walk row 1: emit as <th>. Detect separator row
            // (cells are mostly dashes) and skip. Remaining rows
            // emit as <tr><td>. applyInline runs per cell so
            // backticks + bold work inside cells.
            closeListIfOpen();
            skipBlock = false;
            QStringList rows;
            rows.append(raw);
            while (i + 1 < lines.size() && lines[i + 1].startsWith(QStringLiteral("|"))) {
                ++i;
                rows.append(lines[i]);
            }
            const auto splitRow = [](const QString &row) {
                // `| a | b |` → `["a", "b"]`. Strip leading/
                // trailing empties from the leading/trailing
                // pipe.
                QStringList parts = row.split(QLatin1Char('|'));
                if (!parts.isEmpty() && parts.first().trimmed().isEmpty())
                    parts.removeFirst();
                if (!parts.isEmpty() && parts.last().trimmed().isEmpty())
                    parts.removeLast();
                for (QString &p : parts) p = p.trimmed();
                return parts;
            };
            const auto isSeparator = [](const QStringList &cells) {
                // Row is a separator if every cell is something
                // like `---` / `:---:` / `---:`.
                if (cells.isEmpty()) return false;
                for (const QString &c : cells) {
                    QString s = c;
                    s.remove(QLatin1Char(':')).remove(QLatin1Char(' '));
                    if (s.isEmpty()) return false;
                    for (QChar ch : s)
                        if (ch != QLatin1Char('-')) return false;
                }
                return true;
            };
            html += QStringLiteral("<table>");
            bool sawHeader = false;
            for (const QString &row : rows) {
                const QStringList cells = splitRow(row);
                if (cells.isEmpty()) continue;
                if (isSeparator(cells)) continue;
                const QString tag =
                    sawHeader ? QStringLiteral("td") : QStringLiteral("th");
                html += QStringLiteral("<tr>");
                for (const QString &cell : cells) {
                    html += '<' + tag + '>' + applyInline(cell)
                          + QStringLiteral("</") + tag + '>';
                }
                html += QStringLiteral("</tr>");
                sawHeader = true;
            }
            html += QStringLiteral("</table>");
            continue;
        }

        // Top-level bullet.
        if (raw.startsWith(QStringLiteral("- ")) ||
            raw.startsWith(QStringLiteral("* "))) {
            const QString body = raw.mid(2);
            const BulletKind kind = classify(body);
            const bool current = isCurrent(body);
            // Inclusive-OR over enabled categories. Plain narration
            // bullets (Other) always render — they carry document
            // context, not status.
            // ANTS-1423 — current-signal rescue gated on
            // (wantDone || !isDone). The signal is fuzzy-matched
            // against CHANGELOG [Unreleased] + recent commits, which
            // includes just-shipped items; without the gate, a ✅
            // bullet whose ID appears in [Unreleased] slips through
            // the Current preset even though that preset explicitly
            // excludes ShowDone.
            const bool currentRescue = current && wantCurrent &&
                (wantDone || kind != BulletKind::Done);
            const bool keepStatus =
                (kind == BulletKind::Other) ||
                (kind == BulletKind::Done && wantDone) ||
                (kind == BulletKind::Planned && wantPlanned) ||
                (kind == BulletKind::InProgress && wantInProgress) ||
                (kind == BulletKind::Considered && wantConsidered) ||
                currentRescue;
            // Search predicate: case-insensitive substring against the
            // bullet body, OR the `id:NNNN` shorthand against an
            // `[ANTS-NNNN]` token in the body. Empty predicate keeps
            // every bullet that survived the status filter.
            bool keepSearch = true;
            if (!idMarker.isEmpty()) {
                keepSearch = body.contains(idMarker);
            } else if (!plainSearch.isEmpty()) {
                keepSearch = body.contains(plainSearch, Qt::CaseInsensitive);
            }
            // ANTS-1106 + ANTS-1140 — Kind filter. Empty
            // filter = no narrowing. Non-empty filter requires
            // the bullet's Kind: line value to be a member of
            // the set; bullets without a Kind: line are
            // excluded under non-empty filters. ANTS-1140
            // (0.7.72) folds the per-bullet peek-ahead into a
            // single pre-walk + cache (above) — `kindByLine[i]`
            // is now O(1) lookup keyed by the bullet's line
            // index instead of an O(continuation_lines) regex
            // walk per render.
            bool keepKind = true;
            if (!kindFilter.isEmpty() && kind != BulletKind::Other) {
                const auto it = kindByLine.constFind(i);
                const QString thisKind =
                    (it != kindByLine.constEnd()) ? it.value() : QString();
                keepKind = !thisKind.isEmpty() &&
                           kindFilter.contains(thisKind);
            }
            const bool keep = keepStatus && keepSearch && keepKind;
            if (!keep) {
                skipBlock = true;
                continue;
            }
            skipBlock = false;
            if (!inList) {
                html += QStringLiteral("<ul>");
                inList = true;
            }
            const QString cls = current
                ? QStringLiteral(" class=\"cur\"") : QString();
            html += QStringLiteral("<li") + cls + QStringLiteral(">")
                  + applyInline(body);
            // The </li> is closed when we leave the bullet (next non-
            // continuation line). A continuation appends inline.
            continue;
        }

        // Continuation of a bullet (two-space indent or blank line).
        if (raw.startsWith(QStringLiteral("  ")) && inList) {
            if (skipBlock) continue;
            html += '\n' + applyInline(raw.trimmed());
            continue;
        }

        // Blank line — terminate the current bullet item / list.
        if (raw.trimmed().isEmpty()) {
            if (inList) html += QStringLiteral("</li>");
            closeListIfOpen();
            skipBlock = false;
            continue;
        }

        // Other prose — rendered as a paragraph.
        closeListIfOpen();
        skipBlock = false;
        html += QStringLiteral("<p>") + applyInline(raw) + QStringLiteral("</p>");
    }
    closeListIfOpen();

    html += QStringLiteral("</body></html>");
    return html;
}

// ANTS-1154 v2 — card-style renderer. Built alongside renderHtml
// (not as a replacement) so existing call sites and the
// `tests/features/roadmap_viewer*` test suites keep passing. The
// dialog's `rebuild()` switches to this renderer; everything else
// (tests, future IPC consumers) keeps the original markdown-to-HTML
// path.
//
// Implementation shape:
//   1. Reorder sections if SortOrder::DescendingChronological.
//   2. Pre-walk the source to build a per-section bullet list +
//      visible-count summary under the current filter.
//   3. Main walk emits headings (with count chips + collapse anchor)
//      and, when a section is expanded, the bullets inside it as
//      cards. Prose and section intros emit only on Preset::Full.
//   4. Cards lay out as:
//        row 1: state icon · kind chip · summary · expand toggle
//        row 2: id chip · shipped date (muted)
//        row 3+ (expanded only): body prose
QString RoadmapDialog::renderCardsHtml(const QString &markdownText,
                                       unsigned filter,
                                       const QStringList &currentBullets,
                                       const QString &themeName,
                                       SortOrder sortOrder,
                                       const QString &searchPredicate,
                                       const QSet<QString> &kindFilter,
                                       const CardRenderOptions &opts) {
    const QString sourceText =
        (sortOrder == SortOrder::DescendingChronological)
            ? reverseTopLevelSections(markdownText)
            : markdownText;
    const int idShorthand = parseIdShorthand(searchPredicate);
    const QString idMarker =
        idShorthand >= 0
            // ANTS-1660 — match `[<prefix>-NNNN]` for any prefix via the
            // `-NNNN]` suffix (the leading '-' + close bracket anchor it so
            // `-42]` won't match `-142]`/`-420]`). ANTS-NNNN still matches.
            ? QStringLiteral("-%1]").arg(idShorthand)
            : QString();
    const QString plainSearch =
        (idShorthand >= 0) ? QString() : searchPredicate.trimmed();

    const Theme &th = Themes::byName(themeName);
    const QString currentColor =
        ClaudeTabIndicator::color(ClaudeTabIndicator::Glyph::ToolUse).name();
    const bool wantDone = (filter & ShowDone) != 0;
    const bool wantPlanned = (filter & ShowPlanned) != 0;
    const bool wantInProgress = (filter & ShowInProgress) != 0;
    const bool wantConsidered = (filter & ShowConsidered) != 0;
    const bool wantCurrent = (filter & ShowCurrent) != 0;

    QStringList signalsFuzzy;
    signalsFuzzy.reserve(currentBullets.size());
    for (const QString &s : currentBullets) {
        const QString f = fuzzy(s);
        if (f.size() >= 6) signalsFuzzy.append(f);
    }
    auto isCurrent = [&](const QString &bulletBody) {
        if (signalsFuzzy.isEmpty()) return false;
        const QString fHay = fuzzy(bulletPayload(bulletBody));
        if (fHay.size() < 6) return false;
        for (const QString &s : signalsFuzzy) {
            if (fHay.contains(s) || s.contains(fHay)) return true;
        }
        return false;
    };

    // Kind emoji map — mirrors the kKinds table in the file-scope
    // anonymous namespace, but indexed by Kind value for O(1) lookup
    // during card emission.
    auto kindGlyph = [](const QString &k) -> QString {
        if (k == QStringLiteral("implement")) return QStringLiteral("✨");
        if (k == QStringLiteral("fix")) return QStringLiteral("🐛");
        if (k == QStringLiteral("audit-fix")) return QStringLiteral("🔍");
        if (k == QStringLiteral("review-fix")) return QStringLiteral("🔁");
        if (k == QStringLiteral("doc")) return QStringLiteral("📚");
        if (k == QStringLiteral("doc-fix")) return QStringLiteral("📝");
        if (k == QStringLiteral("refactor")) return QStringLiteral("🏗");
        if (k == QStringLiteral("test")) return QStringLiteral("🧪");
        if (k == QStringLiteral("chore")) return QStringLiteral("🧹");
        if (k == QStringLiteral("release")) return QStringLiteral("🚢");
        if (k == QStringLiteral("research")) return QStringLiteral("🔬");
        if (k == QStringLiteral("ux")) return QStringLiteral("🎨");
        return QString();
    };

    // Parse all bullets once so we can group by section and compute
    // count chips per section header. parseBullets sets sectionSlug,
    // status, kind, id, headline, layman, body for each record.
    const QVector<BulletRecord> allBullets = parseBullets(sourceText);

    auto passesFilter = [&](const BulletRecord &rec) -> bool {
        // Status filter
        bool statusOk = false;
        if (rec.status == QStringLiteral("✅") && wantDone) statusOk = true;
        else if (rec.status == QStringLiteral("📋") && wantPlanned) statusOk = true;
        else if (rec.status == QStringLiteral("🚧") && wantInProgress) statusOk = true;
        else if (rec.status == QStringLiteral("💭") && wantConsidered) statusOk = true;
        // Current signal: rec is "current" if its body matches a
        // CHANGELOG/[Unreleased] or recent-commit fuzzy hit.
        // ANTS-1423 — gated on (wantDone || status != "✅"). The
        // signal includes just-shipped items via CHANGELOG
        // [Unreleased]; without the gate, ✅ bullets slip through
        // the Current preset which explicitly excludes ShowDone.
        const bool isCur = wantCurrent && isCurrent(rec.body) &&
            (wantDone || rec.status != QStringLiteral("✅"));
        if (!statusOk && !isCur) return false;

        // Kind filter
        if (!kindFilter.isEmpty()) {
            if (rec.kind.isEmpty() || !kindFilter.contains(rec.kind))
                return false;
        }

        // Search predicate
        if (!idMarker.isEmpty()) {
            if (!rec.body.contains(idMarker)) return false;
        }
        if (!plainSearch.isEmpty()) {
            QString hay = rec.id + QStringLiteral(" ") + rec.headline
                        + QStringLiteral(" ") + rec.layman
                        + QStringLiteral(" ") + rec.body;
            if (!hay.contains(plainSearch, Qt::CaseInsensitive)) return false;
        }
        return true;
    };

    // Group bullets by sectionSlug + count per status.
    QHash<QString, QVector<const BulletRecord *>> bySection;
    struct SectionCounts {
        int done = 0, planned = 0, inProgress = 0, considered = 0;
        int visible = 0;
    };
    QHash<QString, SectionCounts> countsBySection;
    for (const BulletRecord &rec : allBullets) {
        const QString slug = rec.sectionSlug;
        if (!passesFilter(rec)) continue;
        bySection[slug].append(&rec);
        SectionCounts &c = countsBySection[slug];
        c.visible++;
        if (rec.status == QStringLiteral("✅")) c.done++;
        else if (rec.status == QStringLiteral("📋")) c.planned++;
        else if (rec.status == QStringLiteral("🚧")) c.inProgress++;
        else if (rec.status == QStringLiteral("💭")) c.considered++;
    }

    // Build the HTML.
    QString html;
    html.reserve(sourceText.size() * 2);
    // ANTS-1238 — density-tier lookup. Cozy reproduces the
    // pre-1238 px values byte-for-byte (INV-1); Compact /
    // Comfortable shift the body / heading / code / meta groups
    // and the vertical-padding scale. Inline-spacing constants
    // (e.g. `rm-state padding-right:6px`, `rm-id padding-left:8px`)
    // stay fixed across all tiers per spec § 2.f.
    const DensityTier &t = tierFor(opts.density);
    html += QStringLiteral(
        "<html><head><style>"
        "body{font-family:sans-serif;color:%1;font-size:%7px;}"
        "p{margin:%8px 0;}"
        "h1,h2,h3,h4{color:%2;font-weight:bold;margin:%9px 0 %10px 0;}"
        "h1{font-size:%11px;} h2{font-size:%12px;}"
        "h3{font-size:%13px;} h4{font-size:%14px;}"
        "code{background:%3;padding:0 4px;border-radius:3px;font-size:%15px;}"
        "a{color:%2;text-decoration:none;}"
        "a:hover{text-decoration:underline;}"
        // ANTS-1239 — Qt's text engine paints <a> foreground with the
        // widget's QPalette::Link role, ignoring the generic `a{color:}`
        // rule above and not propagating through `color:inherit`. We
        // also set QPalette::Link on the QTextBrowser at construction
        // (see RoadmapDialog ctor), but emit explicit class colors here
        // as belt-and-braces so dark-theme section headers stay legible
        // even if a future Qt regression flips the palette path again.
        ".rm-section-toggle{color:%2;font-weight:normal;font-size:%7px;padding-right:12px;padding-left:4px;}"
        ".rm-section-title{color:%2;text-decoration:none;}"
        ".rm-section-title:hover{text-decoration:underline;}"
        ".rm-section-counts{font-weight:normal;font-size:%16px;color:%6;padding-right:10px;white-space:nowrap;}"
        ".rm-parent{font-weight:normal;font-size:%16px;color:%6;padding-left:8px;}"
        ".rm-card{margin:%17px 0;padding:%18px %19px;border-left:3px solid %5;background:%3;}"
        ".rm-card.rm-current{border-left-color:%4;background:rgba(229,194,74,0.08);}"
        // ANTS-1428 INV-10 — synthetic-ID cards (GFM bullets with no
        // bold-ID) carry a dashed left border so hand-authored and
        // auto-anchored bullets are visually distinguishable at a
        // glance. Dashed (vs solid fill) so a missing-stylesheet
        // fallback still reads as a card, just without the marker.
        ".rm-card.rm-card-synthetic{border-left-style:dashed;}"
        ".rm-state{font-size:%7px;padding-right:6px;}"
        ".rm-state-label{font-size:%20px;color:%6;padding-right:6px;}"
        ".rm-kind{font-size:%16px;color:%6;padding-right:6px;}"
        ".rm-summary{font-size:%7px;}"
        ".rm-toggle{font-size:%15px;color:%6;padding-left:12px;padding-right:4px;}"
        // ANTS-1241 — id + shipped-date are now inline on the
        // summary row (after the summary text, before the toggle).
        // Bumping rm-id from the old 10 px to 12 px makes the
        // number scannable at a glance; the previous `<div
        // class="rm-meta">` wrapper triggered the same Qt nested-
        // block QPalette::Base frame issue that broke rm-body in
        // ANTS-1240, painting a darker band under the ID on dark
        // themes. Inline spans paint over the card's bgSecondary
        // without that frame.
        ".rm-id{font-family:monospace;font-size:%15px;color:%6;padding-left:8px;}"
        ".rm-date{font-size:%16px;color:%6;padding-left:6px;}"
        // ANTS-1240 — body paragraphs are emitted directly inside the
        // card (no wrapping <div>) so Qt's text engine doesn't paint
        // an inner block frame with `QPalette::Base`, which broke the
        // visual continuity with the card's bgSecondary on dark
        // themes. `rm-body-first` carries the divider + extra top
        // padding; `rm-body-line` is plain indent only.
        ".rm-body-first{padding-top:%21px;padding-left:20px;border-top:1px dotted %5;margin-top:%22px;font-size:%7px;}"
        ".rm-body-line{padding-left:20px;font-size:%7px;}"
        "table{border-collapse:collapse;}"
        "td,th{border:1px solid %5;padding:2px 6px;font-size:%15px;}"
        "</style></head><body>")
        .arg(th.textPrimary.name(),         // %1
             th.textPrimary.name(),         // %2
             th.bgSecondary.name(),         // %3
             currentColor,                  // %4
             th.border.name(),              // %5
             th.textSecondary.name())       // %6
        // Density-tier px values. Order maps the %N number → field
        // by hand so a future tier reorder doesn't silently swap two
        // CSS rules. Use QString::number to ensure no locale
        // surprises (QString::arg(int) is locale-aware in some Qt
        // setups; QString::number defaults to base 10 + C locale).
        .arg(QString::number(t.bodyPx),              // %7
             QString::number(t.pMargin),             // %8
             QString::number(t.hMarginTop),          // %9
             QString::number(t.hMarginBottom),       // %10
             QString::number(t.h1Px),                // %11
             QString::number(t.h2Px),                // %12
             QString::number(t.h3Px),                // %13
             QString::number(t.h4Px),                // %14
             QString::number(t.codePx))              // %15
        .arg(QString::number(t.metaPx),              // %16
             QString::number(t.cardMargin),          // %17
             QString::number(t.cardPaddingY),        // %18
             QString::number(t.cardPaddingX),        // %19
             QString::number(t.labelPx),             // %20
             QString::number(t.bodyFirstPaddingTop), // %21
             QString::number(t.bodyFirstMarginTop)); // %22

    const QStringList lines = sourceText.split('\n');
    QString currentSlug;
    QString currentH2Text;        // most recent h2 (parent for h3 breadcrumb)
    bool sectionVisible = true;   // section header emitted?
    bool sectionExpanded = false; // user has opened this section?
    int headingIdx = 0;
    // ANTS-1239 — must run in lockstep with parseBullets above so that
    // bySection[slug] keys match the slugs computed here. uniqueSlug
    // is called for *every* h2/h3 encountered (including the skipped
    // "Table of Contents" h2) so the counter advances identically in
    // both walks.
    QSet<QString> seenSlugs;

    auto emitSectionHeader = [&](int level, const QString &text,
                                 const QString &slug,
                                 const SectionCounts &c,
                                 const QString &parentH2 = QString()) {
        const QString anchor = tocAnchorAt(headingIdx++);
        html += QStringLiteral("<a name=\"%1\"></a>").arg(anchor);
        const QString chevron = sectionExpanded
            ? QStringLiteral("▾") : QStringLiteral("▸");
        const QString verb = sectionExpanded
            ? QStringLiteral("collapse-section")
            : QStringLiteral("expand-section");
        html += QStringLiteral("<h%1>").arg(level);
        html += QStringLiteral(
            "<a class=\"rm-section-toggle\" href=\"ants://%1/%2\">%3</a>")
            .arg(verb, slug, chevron);
        // ANTS-1154 — section status counts lead the title so a partially-
        // sighted scan reads "4 done / 1 in-progress" before parsing the
        // section name.
        // ANTS-1235 — every chip carries a trailing text label
        // (`47 shipped`, `2 in progress`, …) so a screen reader
        // doesn't announce "white heavy check mark 47 construction
        // sign 2 …". `.trimmed()` only strips ASCII whitespace, so
        // an explicit chop strips the trailing " · " separator.
        QString chips;
        if (c.done > 0)       chips += QStringLiteral("✅ %1 shipped · ").arg(c.done);
        if (c.inProgress > 0) chips += QStringLiteral("🚧 %1 in progress · ").arg(c.inProgress);
        if (c.planned > 0)    chips += QStringLiteral("📋 %1 planned · ").arg(c.planned);
        if (c.considered > 0) chips += QStringLiteral("💭 %1 considered · ").arg(c.considered);
        if (chips.endsWith(QStringLiteral(" · "))) chips.chop(3);
        if (!chips.isEmpty()) {
            html += QStringLiteral(
                "<span class=\"rm-section-counts\">%1</span>")
                .arg(chips);
        }
        // Title is also a toggle target — clicking the heading text
        // toggles expand/collapse, not just the chevron. Same href as
        // the chevron above. A11y win for low-precision pointing.
        html += QStringLiteral(
            "<a class=\"rm-section-title\" href=\"ants://%1/%2\">%3</a>")
            .arg(verb, slug, applyInline(text));
        // Parent-h2 breadcrumb on h3 headers — disambiguates orphan
        // sub-sections (e.g. three different "Performance" h3s) when
        // their parent h2 was suppressed on non-Full tabs.
        if (level == 3 && !parentH2.isEmpty()) {
            html += QStringLiteral(
                "<a class=\"rm-section-title\" href=\"ants://%1/%2\">"
                "<span class=\"rm-parent\">· %3</span></a>")
                .arg(verb, slug, htmlEscape(parentH2));
        }
        html += QStringLiteral("</h%1>").arg(level);
    };

    auto emitCard = [&](const BulletRecord &rec) {
        // ANTS-1234 — auto-expand cards whose match lives in body-only
        // (continuation prose) so the matched substring is visible
        // without a manual expand click. Render-time only: never
        // mutates m_expandedItems (INV-5/INV-8 in docs/specs/ANTS-1234.md).
        // Guard: only fires when the predicate matches body but NOT
        // any of id / headline / layman — those are already visible
        // on the collapsed card so expansion would be redundant.
        const bool predicateOnlyInBody =
            !plainSearch.isEmpty() &&
            rec.body.contains(plainSearch, Qt::CaseInsensitive) &&
            !rec.id.contains(plainSearch, Qt::CaseInsensitive) &&
            !rec.headline.contains(plainSearch, Qt::CaseInsensitive) &&
            !rec.layman.contains(plainSearch, Qt::CaseInsensitive);
        const bool idJumpToThisCard =
            !idMarker.isEmpty() && rec.body.contains(idMarker);
        const bool expanded = opts.expandedItems.contains(rec.id)
                              || predicateOnlyInBody
                              || idJumpToThisCard;
        const QString verb = expanded
            ? QStringLiteral("collapse")
            : QStringLiteral("expand");
        const QString chevron = expanded
            ? QStringLiteral("▴") : QStringLiteral("▾");
        // ANTS-1428 INV-10 — append rm-card-synthetic for GFM
        // bullets that received a content-hash ID (no bold-ID
        // token in source). Native parses leave rec.synthetic
        // false, so existing cards are unchanged.
        QString cardClasses;
        if (isCurrent(rec.body)) cardClasses += QStringLiteral(" rm-current");
        if (rec.synthetic)       cardClasses += QStringLiteral(" rm-card-synthetic");
        // ANTS-1154-INV-1
        html += QStringLiteral("<div class=\"rm-card%1\" id=\"rm-%2\">")
                    .arg(cardClasses, htmlEscape(rec.id));
        // Row 1
        html += QStringLiteral("<span class=\"rm-state\">%1</span>")
                    .arg(rec.status);
        // ANTS-1235 — screen-reader label next to the status glyph.
        // Empty for non-status bullets (rec.status is empty in that
        // case, so statusAccessibleLabel returns empty); skip the
        // span entirely rather than emitting an empty one.
        const QString stateLabel = statusAccessibleLabel(rec.status);
        if (!stateLabel.isEmpty()) {
            html += QStringLiteral("<span class=\"rm-state-label\">%1</span>")
                        .arg(htmlEscape(stateLabel));
        }
        if (!rec.kind.isEmpty()) {
            const QString glyph = kindGlyph(rec.kind);
            // CWE-79: rec.kind is captured from user-supplied ROADMAP.md
            // by rxKind ([^\.\n]+?), which admits <, >, &, ". Escape
            // before emitting into HTML.
            html += QStringLiteral("<span class=\"rm-kind\">%1 %2</span>")
                        .arg(glyph, htmlEscape(rec.kind));
        }
        // Summary: layman if set, else headline with leading "ANTS-NNNN — " stripped
        QString summary = rec.layman;
        if (summary.isEmpty()) {
            summary = rec.headline;
            // Strip leading <prefix>-NNNN — token (the ID is shown
            // separately on the meta row). ANTS-1660 — match any project-ID
            // prefix, not just ANTS-.
            static const QRegularExpression rxLeadId(
                QStringLiteral("^[A-Za-z][A-Za-z0-9_-]*-\\d+\\s*[—-]\\s*"));
            summary.remove(rxLeadId);
        }
        html += QStringLiteral("<span class=\"rm-summary\">%1</span>")
                    .arg(htmlEscape(summary));
        // ANTS-1241 — id + shipped date inline on the summary row
        // (was a separate `<div class="rm-meta">` below the row,
        // which painted a different bg under dark themes for the
        // same reason ANTS-1240 hit on rm-body). Larger 12 px font
        // so the number is scannable at a glance.
        // ANTS abbreviates to #NNNN (the local prefix); foreign project-ID
        // prefixes show in full (#VEST-0042) to disambiguate. This already
        // handles multi-prefix correctly — see AdapterRenderGfm.
        // BoldIdRenderedInIdSlot.
        const QString hashedId = rec.id.startsWith(QStringLiteral("ANTS-"))
            ? QStringLiteral("#") + rec.id.mid(5)
            : QStringLiteral("#") + rec.id;
        html += QStringLiteral("<span class=\"rm-id\">%1</span>")
                    .arg(htmlEscape(hashedId));
        if (rec.status == QStringLiteral("✅")) {
            const QString date = opts.shippedDates.value(rec.id);
            if (!date.isEmpty()) {
                html += QStringLiteral(
                    "<span class=\"rm-date\">· %1</span>").arg(date);
            }
        } else if (rec.status == QStringLiteral("🚧")) {
            // ANTS-1237 — "Updated Nd ago" on 🚧 cards. Reuses
            // .rm-date CSS for visual consistency; the "Updated "
            // prefix carries the semantic distinction. Span only
            // emitted when lastTouchDates has an entry — graceful
            // degradation on non-git checkouts.
            const auto it = opts.lastTouchDates.constFind(rec.id);
            if (it != opts.lastTouchDates.constEnd()) {
                const qint64 nowSec =
                    QDateTime::currentSecsSinceEpoch();
                const qint64 age = nowSec - *it;
                html += QStringLiteral(
                    "<span class=\"rm-date\">· %1</span>")
                    .arg(tr("Updated %1").arg(humanAge(age)));
            }
        }
        html += QStringLiteral(
            "<a class=\"rm-toggle\" href=\"ants://%1/%2\">[%3]</a>")
            .arg(verb, htmlEscape(rec.id), chevron);
        // Body (expanded only). ANTS-1240 — no `<div class="rm-body">`
        // wrapper: Qt's text engine renders nested <div> blocks with
        // their own QPalette::Base background frame, so the body
        // showed a different colour than the parent card. Emitting
        // <p> elements directly into the card lets each paragraph
        // paint over the card's bgSecondary, preserving the visual
        // continuity. First non-empty <p> carries `rm-body-first`
        // (dotted divider + padding-top); subsequent lines carry
        // `rm-body-line` (indent only).
        if (expanded) {
            const QStringList bodyLines = rec.body.split('\n');
            bool firstP = true;
            for (int bi = 0; bi < bodyLines.size(); ++bi) {
                if (bodyLines[bi].trimmed().isEmpty()) continue;
                const QString cls = firstP
                    ? QStringLiteral("rm-body-first")
                    : QStringLiteral("rm-body-line");
                html += QStringLiteral("<p class=\"%1\">%2</p>")
                            .arg(cls, applyInline(bodyLines[bi]));
                firstP = false;
            }
        }
        html += QStringLiteral("</div>");  // rm-card
    };

    auto skipBulletBlockAt = [&](int i) -> int {
        // Skip the bullet's continuation lines so the outer walk
        // doesn't re-emit them as prose.
        int j = i + 1;
        while (j < lines.size()) {
            const QString &cont = lines[j];
            if (cont.trimmed().isEmpty()) break;
            if (cont.startsWith(QStringLiteral("- ")) ||
                cont.startsWith(QStringLiteral("* "))) break;
            if (cont.startsWith(QStringLiteral("  "))) { ++j; continue; }
            break;
        }
        return j - 1;
    };

    // ANTS-1662 — bullets that appear before the first ##/### heading have an
    // empty section slug, so the heading-driven walk below never emits them and
    // they vanish from the cards view (v1 renderHtml shows them). Emit the
    // unsectioned bucket here at the top of the body, preserving the INV-16
    // superset contract. No section header — these bullets have none.
    {
        const QVector<const BulletRecord *> &unsectioned =
            bySection.value(QString());
        for (const BulletRecord *rec : unsectioned)
            emitCard(*rec);
    }

    for (int i = 0; i < lines.size(); ++i) {
        const QString &raw = lines[i];

        QString hText;
        const int level = headingLevel(raw, &hText);

        if (level == 1) {
            // File title — emit only on Full (large heading).
            const QString anchor = tocAnchorAt(headingIdx++);
            html += QStringLiteral("<a name=\"%1\"></a>").arg(anchor);
            html += QStringLiteral("<h1>") + applyInline(hText)
                  + QStringLiteral("</h1>");
            continue;
        }
        if (level == 2 || level == 3) {
            if (level == 2) {
                currentH2Text = hText;
            }
            // ANTS-1239 — compute the slug *before* the TOC skip so
            // seenSlugs advances in lockstep with parseBullets (which
            // has no TOC skip).
            const QString slug = uniqueSlug(seenSlugs, hText);
            if (level == 2 &&
                hText.compare(QStringLiteral("Table of Contents"),
                              Qt::CaseInsensitive) == 0) {
                // Skip the "Table of Contents" h2 — the QListWidget nav
                // pane is the canonical TOC; rendering it inline is
                // duplicate noise.
                sectionVisible = false;
                sectionExpanded = false;
                continue;
            }
            currentSlug = slug;
            sectionExpanded = opts.expandedSections.contains(slug);
            const SectionCounts c = countsBySection.value(slug);
            // INV-12: on non-Full, suppress sections with 0 visible bullets.
            sectionVisible = (opts.activePreset == Preset::Full) || c.visible > 0;
            if (sectionVisible) {
                emitSectionHeader(level, hText, slug, c,
                                  level == 3 ? currentH2Text : QString());
                // If expanded, emit its bullets as cards.
                if (sectionExpanded) {
                    const QVector<const BulletRecord *> &bullets = bySection.value(slug);
                    for (const BulletRecord *rec : bullets) {
                        emitCard(*rec);
                    }
                }
            }
            continue;
        }
        if (level == 4) {
            if (opts.activePreset == Preset::Full && sectionVisible
                && sectionExpanded) {
                const QString anchor = tocAnchorAt(headingIdx++);
                html += QStringLiteral("<a name=\"%1\"></a>").arg(anchor);
                html += QStringLiteral("<h4>") + applyInline(hText)
                      + QStringLiteral("</h4>");
            }
            continue;
        }

        // Top-level bullet — handled above via bySection map when its
        // section emits. Skip the line + its continuation here so the
        // outer walk doesn't double-emit it as prose.
        const bool isBullet = raw.startsWith(QStringLiteral("- ")) ||
                              raw.startsWith(QStringLiteral("* "));
        if (isBullet) {
            i = skipBulletBlockAt(i);
            continue;
        }

        // Prose / narration / section-intros. INV-11.
        if (opts.activePreset != Preset::Full) continue;
        if (!sectionVisible || !sectionExpanded) continue;
        if (raw.trimmed().isEmpty()) continue;
        html += QStringLiteral("<p>") + applyInline(raw)
              + QStringLiteral("</p>");
    }

    html += QStringLiteral("</body></html>");
    return html;
}

// ANTS-1154 — Walk CHANGELOG.md and build an ANTS-NNNN → date map.
// Section heading shape (Keep-a-Changelog convention):
//   ## [0.7.82] — 2026-05-11
//   ## [Unreleased]
// Every ANTS-NNNN token in the section body inherits that date. The
// [Unreleased] block has no date; its IDs map to an empty string and
// thus the card emits no date row (graceful for in-flight ✅).
// ANTS-1235 — screen-reader-readable label for a status emoji.
// File-scope `kStatusLabels` is the source of truth; this method
// looks the emoji up and resolves the label through RoadmapDialog's
// tr() context (so a future translation pass works).
QString RoadmapDialog::statusAccessibleLabel(const QString &emoji) {
    // kStatusLabels::emoji is a UTF-8 multi-byte const char * (the
    // status emojis are outside the Latin1 range), so QLatin1String
    // would mis-decode it. Compare via fromUtf8.
    for (const auto &row : kStatusLabels) {
        if (emoji == QString::fromUtf8(row.emoji))
            return tr(row.label);
    }
    return {};
}

// ANTS-1236 — render the file-scope `kRoadmapShortcuts` table into the
// two-column shape the cheatsheet sub-dialog consumes. Resolves the
// action column through `RoadmapDialog::tr()` (qualified because the
// strings were extracted with QT_TR_NOOP in this TU and so belong to
// this translation context, not the sub-dialog's); the keys column is
// pass-through. See spec § 3.c / INV-2.
QVector<QPair<QString, QString>> roadmapShortcutRows() {
    QVector<QPair<QString, QString>> out;
    out.reserve(std::size(kRoadmapShortcuts));
    for (const auto &row : kRoadmapShortcuts) {
        out.append({QString::fromUtf8(row.keys),
                    RoadmapDialog::tr(row.action)});
    }
    return out;
}

// ANTS-1237 — render an age in seconds as one of the ladder strings
// documented in spec § 2.f.5. External linkage (NOT in the anonymous
// namespace) so the feature test reaches it via the forward
// declaration in roadmapdialog.h. Guard against negative ages
// (clock skew where a freshly-committed bullet's author-time
// exceeds now by a few seconds).
QString humanAge(qint64 ageSeconds) {
    if (ageSeconds < 0) ageSeconds = 0;
    const qint64 d = ageSeconds / 86400;
    if (d < 1)   return RoadmapDialog::tr("today");
    if (d < 2)   return RoadmapDialog::tr("yesterday");
    if (d < 14)  return RoadmapDialog::tr("%1d ago").arg(d);
    if (d < 60)  return RoadmapDialog::tr("%1w ago").arg(d / 7);
    if (d < 365) return RoadmapDialog::tr("%1mo ago").arg(d / 30);
    return RoadmapDialog::tr("%1y ago").arg(d / 365);
}

QHash<QString, QString>
RoadmapDialog::parseShippedDates(const QString &changelogPath) {
    QHash<QString, QString> out;
    QFile f(changelogPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    static const QRegularExpression rxHeading(
        QStringLiteral("^##\\s*\\[([^\\]]+)\\]\\s*(?:[—-]\\s*(\\d{4}-\\d{2}-\\d{2}))?"));
    // ANTS-1660 — match any project-ID prefix, not just ANTS- (multi-prefix
    // repos are permitted per roadmap-format.md § 3.10.4; the viewer is reused
    // by other projects). ANTS-NNNN still matches. ANTS-1784 — token shape
    // shared with parseBullets via idTokenPattern() so they can't drift.
    static const QRegularExpression rxId(
        QStringLiteral("\\b(") + idTokenPattern() + QStringLiteral(")\\b"));
    QString currentDate;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        const auto hm = rxHeading.match(line);
        if (hm.hasMatch()) {
            currentDate = hm.captured(2);  // empty if [Unreleased]
            continue;
        }
        if (currentDate.isEmpty()) continue;
        // Match all ANTS-NNNN tokens in the line.
        auto it = rxId.globalMatch(line);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString id = m.captured(1);
            // First-wins: keep the earliest shipped date for any ID
            // that appears in multiple versions (e.g. a follow-on
            // patch references the original).
            if (!out.contains(id)) out.insert(id, currentDate);
        }
    }
    return out;
}

QHash<QString, qint64>
RoadmapDialog::parseLastTouchDates(const QString &roadmapPath) {
    QHash<QString, qint64> out;
    const QFileInfo fi(roadmapPath);
    if (!fi.exists()) return out;

    // One blame call. --line-porcelain repeats header fields
    // (incl. author-time) for every source line — easy to index by
    // line number. QProcess pipes already suppress git's stderr
    // progress, so no --no-progress needed.
    QProcess git;
    git.setWorkingDirectory(fi.absolutePath());
    git.start(QStringLiteral("git"),
              {QStringLiteral("blame"),
               QStringLiteral("--line-porcelain"),
               QStringLiteral("--"),
               fi.fileName()});
    if (!git.waitForStarted(2000)) return out;
    // ANTS-1661 — runs synchronously on the GUI thread; cap the budget at 5 s
    // (was 30 s) so a slow/large blame can't freeze the dialog for half a
    // minute on open / Refresh / search-debounce rebuild. Off-thread move via
    // QtConcurrent is the longer-term fix tracked separately.
    if (!git.waitForFinished(5000)) {
        git.kill();
        return out;
    }
    if (git.exitStatus() != QProcess::NormalExit
        || git.exitCode() != 0) {
        // Not a git repo, file not tracked, etc. — graceful.
        return out;
    }
    const QByteArray blameOut = git.readAllStandardOutput();

    // Build a 1-indexed vector of author-times per source line.
    QVector<qint64> lineAuthorTime;
    lineAuthorTime.append(0);  // placeholder so index 1 == line 1
    qint64 currentAuthorTime = 0;
    int currentSourceLine = 0;
    // Porcelain format: lines starting with a 40-char hex hash
    // begin a record. Header lines `author-time NNNN` follow.
    // Content line begins with TAB.
    for (const QByteArray &raw : blameOut.split('\n')) {
        if (raw.startsWith('\t')) {
            // Content line — assign the current author-time to
            // the recorded final-line number.
            while (lineAuthorTime.size() <= currentSourceLine)
                lineAuthorTime.append(0);
            if (currentSourceLine > 0)
                lineAuthorTime[currentSourceLine] = currentAuthorTime;
            continue;
        }
        if (raw.size() >= 41 && raw.at(40) == ' ') {
            // Hash header: "<hash> <orig> <final> [count]"
            const QList<QByteArray> parts = raw.split(' ');
            if (parts.size() >= 3) {
                currentSourceLine = parts.at(2).toInt();
            }
            continue;
        }
        if (raw.startsWith("author-time ")) {
            currentAuthorTime =
                QByteArray(raw.mid(12)).trimmed().toLongLong();
        }
    }

    // Walk the markdown, identify each `- 🚧 [ANTS-NNNN]` bullet,
    // take MAX over its block (the bullet line + every contiguous
    // 2-space-indented continuation line until blank or next-bullet
    // or EOF). See spec § 3.b.2.
    QFile f(roadmapPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    const QByteArray body = f.readAll();
    f.close();
    const QList<QByteArray> mdLines = body.split('\n');
    // ANTS-1660 — match any project-ID prefix, not just ANTS-.
    static const QRegularExpression rxInProgress(
        QStringLiteral("^- 🚧 \\[([A-Za-z][A-Za-z0-9_-]*-\\d+)\\]"));
    for (int i = 0; i < mdLines.size(); ++i) {
        const QString line = QString::fromUtf8(mdLines.at(i));
        const auto m = rxInProgress.match(line);
        if (!m.hasMatch()) continue;
        const QString id = m.captured(1);
        qint64 maxTime = 0;
        int j = i;
        while (j < mdLines.size()) {
            const QString cont = QString::fromUtf8(mdLines.at(j));
            if (j > i) {
                if (cont.trimmed().isEmpty()) break;
                if (cont.startsWith(QStringLiteral("- "))
                    || cont.startsWith(QStringLiteral("* "))) break;
                if (!cont.startsWith(QStringLiteral("  "))) break;
            }
            const int lineNo = j + 1;  // 1-indexed
            if (lineNo < lineAuthorTime.size()) {
                const qint64 t = lineAuthorTime.at(lineNo);
                if (t > maxTime) maxTime = t;
            }
            ++j;
        }
        if (maxTime > 0) out.insert(id, maxTime);
    }
    return out;
}

RoadmapDialog::RoadmapDialog(const QString &roadmapPath,
                             const QString &themeName,
                             QWidget *parent,
                             Config *cfg)
    : QDialog(parent),
      m_roadmapPath(roadmapPath),
      m_themeName(themeName),
      m_lastHtml(std::make_shared<QString>()),
      m_config(cfg) {
    setWindowTitle(tr("Roadmap — %1").arg(QFileInfo(roadmapPath).fileName()));
    // ANTS-1100 spec: 1200x800 default; restoreGeometry kicks in below
    // if the user has resized us before.
    resize(1200, 800);
    setMinimumSize(720, 480);

    // ANTS-1242 — frameless + custom TitleBar so the dialog's chrome
    // honours the active theme (KWin draws the standard title bar
    // from the system colour scheme and ignores Qt's QPalette). The
    // shared helper installs the bar, wires the close/min/max
    // signals, and gives back a content QWidget to use as the
    // parent for the rest of the ctor's layouts.
    auto chrome = DialogChrome::install(this, m_themeName);
    m_titleBar = chrome.titleBar;
    QWidget *content = chrome.contentArea;

    // Find a sibling CHANGELOG.md (case-insensitive) for the
    // current-work signal set.
    const QDir dir = QFileInfo(roadmapPath).absoluteDir();
    for (const QFileInfo &fi : dir.entryInfoList(QDir::Files)) {
        if (fi.fileName().compare(QStringLiteral("CHANGELOG.md"),
                                  Qt::CaseInsensitive) == 0) {
            m_changelogPath = fi.absoluteFilePath();
            break;
        }
    }

    auto *root = new QVBoxLayout(content);

    // ANTS-1100 INV-7: tab bar is the first widget in the layout.
    m_tabs = new QTabBar(this);
    m_tabs->setObjectName(QStringLiteral("roadmap-tabs"));
    m_tabs->setExpanding(false);
    m_tabs->setDrawBase(false);
    m_tabs->addTab(tr("Full roadmap"));     // 0 → Preset::Full
    m_tabs->addTab(tr("History"));          // 1 → Preset::History
    m_tabs->addTab(tr("Current"));          // 2 → Preset::Current
    m_tabs->addTab(tr("Next"));             // 3 → Preset::Next
    m_tabs->addTab(tr("Far Future"));       // 4 → Preset::FarFuture
    m_tabs->addTab(tr("Custom"));           // 5 → Preset::Custom
    root->addWidget(m_tabs);

    // Search box. Debounced 120 ms per spec — saves a re-render per
    // keystroke during fast typing.
    auto *searchRow = new QHBoxLayout();
    auto *searchLabel = new QLabel(tr("Search:"), this);
    m_searchBox = new QLineEdit(this);
    m_searchBox->setObjectName(QStringLiteral("roadmap-search-box"));
    // Indie-review-2026-05-14 lane-6 M-4: placeholderText is not
    // guaranteed to map to AT-SPI Name; set the accessible name
    // explicitly so Orca/NVDA announce the search box.
    m_searchBox->setAccessibleName(tr("Roadmap search"));
    m_searchBox->setPlaceholderText(
        tr("Substring match (or id:NNNN to jump to a specific ID)"));
    m_searchBox->setClearButtonEnabled(true);
    // ANTS-1234 — install the dialog itself as an event filter on the
    // search box so Esc clears + blurs the box instead of closing the
    // dialog (eventFilter override below). Without this filter, the
    // unhandled Esc would bubble to QDialog::reject.
    m_searchBox->installEventFilter(this);
    searchRow->addWidget(searchLabel);
    searchRow->addWidget(m_searchBox, 1);
    root->addLayout(searchRow);

    auto *filterRow = new QHBoxLayout();
    // ANTS-1235 — visible label is "Shipped" (not "Done") to align
    // with the rest of the roadmap-format vocabulary; accessibleName
    // is what Orca / NVDA speak, kept terse and verb-led.
    m_filterDone = new QCheckBox(tr("✅ Shipped"), this);
    m_filterDone->setObjectName(QStringLiteral("roadmap-filter-done"));
    m_filterDone->setAccessibleName(tr("Show shipped items"));
    m_filterDone->setChecked(true);
    m_filterPlanned = new QCheckBox(tr("📋 Planned"), this);
    m_filterPlanned->setObjectName(QStringLiteral("roadmap-filter-planned"));
    m_filterPlanned->setAccessibleName(tr("Show planned items"));
    m_filterPlanned->setChecked(true);
    m_filterInProgress = new QCheckBox(tr("🚧 In progress"), this);
    m_filterInProgress->setObjectName(QStringLiteral("roadmap-filter-in-progress"));
    m_filterInProgress->setAccessibleName(tr("Show in-progress items"));
    m_filterInProgress->setChecked(true);
    m_filterConsidered = new QCheckBox(tr("💭 Considered"), this);
    m_filterConsidered->setObjectName(QStringLiteral("roadmap-filter-considered"));
    m_filterConsidered->setAccessibleName(tr("Show considered items"));
    m_filterConsidered->setChecked(true);
    m_filterCurrent = new QCheckBox(tr("Currently being tackled"), this);
    m_filterCurrent->setObjectName(QStringLiteral("roadmap-filter-current"));
    m_filterCurrent->setAccessibleName(
        tr("Show only items currently being worked on"));
    m_filterCurrent->setChecked(true);
    filterRow->addWidget(m_filterDone);
    filterRow->addWidget(m_filterPlanned);
    filterRow->addWidget(m_filterInProgress);
    filterRow->addWidget(m_filterConsidered);
    filterRow->addWidget(m_filterCurrent);
    filterRow->addStretch(1);
    // ANTS-1238 — density combo at trailing edge of filterRow.
    // Order matches the Density enum (Compact=0, Cozy=1,
    // Comfortable=2). m_density is the live source of truth;
    // currentIndex is set from m_density. Combo signal flips
    // m_density, persists to Config (when present), triggers
    // rebuild(). Spec: docs/specs/ANTS-1238.md § 3.b.
    //
    // Config is optional (matches the existing `if (m_config)`
    // guard pattern in this ctor — tests construct RoadmapDialog
    // with cfg=nullptr). When null, m_density stays at its default
    // Cozy initialiser and combo changes don't persist.
    if (m_config) {
        m_density = densityFromString(m_config->roadmapDensity());
    }
    m_densityCombo = new QComboBox(this);
    m_densityCombo->setObjectName(QStringLiteral("roadmap-density-combo"));
    m_densityCombo->addItem(tr("Compact"));      // index 0
    m_densityCombo->addItem(tr("Cozy"));         // index 1 (default)
    m_densityCombo->addItem(tr("Comfortable"));  // index 2
    m_densityCombo->setCurrentIndex(densityToIndex(m_density));
    m_densityCombo->setAccessibleName(tr("Roadmap card density"));
    connect(m_densityCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                m_density = indexToDensity(idx);
                if (m_config) {
                    m_config->setRoadmapDensity(densityToString(m_density));
                }
                if (m_lastHtml) m_lastHtml->clear();  // bust dedup cache
                rebuild();
            });
    filterRow->addWidget(m_densityCombo.data());
    root->addLayout(filterRow);

    // ANTS-1106 — Kind-faceted secondary filter. Empty by default
    // (no narrowing). Each checkbox toggles a Kind value in
    // m_kindFilter and triggers a re-render. Emoji prefixes are
    // visual cues — `audit-fix` ought to pop visually vs
    // `implement` so the user can distinguish at a glance which
    // categories of work are queued.
    auto *kindRow = new QHBoxLayout();
    auto *kindLabel = new QLabel(tr("Kind:"), this);
    kindLabel->setObjectName(QStringLiteral("roadmap-filter-kind-label"));
    kindRow->addWidget(kindLabel);
    // KindEntry table is at file scope (see kKinds in the anon
    // namespace) so the ctor's build loop and the ANTS-1150
    // persisted-Kind-filter restore iterate the same source-of-
    // truth table.
    for (const KindEntry &k : kKinds) {
        auto *cb = new QCheckBox(tr(k.labelTxt), this);
        cb->setObjectName(QString::fromLatin1(k.objectName));
        cb->setChecked(false);  // empty filter by default = show all
        const QString kindValue = QString::fromLatin1(k.value);
        m_kindCheckboxes.insert(kindValue, cb);
        connect(cb, &QCheckBox::toggled, this,
                [this, kindValue](bool on) {
                    if (on) m_kindFilter.insert(kindValue);
                    else    m_kindFilter.remove(kindValue);
                    if (m_lastHtml) m_lastHtml->clear();  // force re-render
                    // ANTS-1150 — persist the Kind filter set on
                    // every toggle. setRoadmapKindFilters sorts on
                    // write for stable on-disk ordering.
                    if (m_config) {
                        m_config->setRoadmapKindFilters(QStringList(
                            m_kindFilter.begin(), m_kindFilter.end()));
                    }
                    rebuild();
                });
        kindRow->addWidget(cb);
    }
    kindRow->addStretch(1);
    root->addLayout(kindRow);

    // Body: TOC list (left) + rendered viewer (right) inside a
    // QSplitter so the user can resize the sidebar. QTextBrowser
    // (vs plain QTextEdit) for `scrollToAnchor` support — the TOC
    // entries jump to `<a name="roadmap-toc-N">` anchors emitted by
    // renderHtml.
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("roadmap-splitter"));

    m_toc = new QListWidget(splitter);
    m_toc->setObjectName(QStringLiteral("roadmap-toc"));
    // Indie-review-2026-05-14 lane-6 M-4: name the TOC + viewer.
    m_toc->setAccessibleName(tr("Roadmap table of contents"));
    m_toc->setUniformItemSizes(false);
    m_toc->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_toc->setSelectionMode(QAbstractItemView::SingleSelection);
    m_toc->setMinimumWidth(180);
    splitter->addWidget(m_toc);

    m_viewer = new QTextBrowser(splitter);
    m_viewer->setAccessibleName(tr("Roadmap content"));
    m_viewer->setReadOnly(true);
    // Internal anchors only — disable navigation on `<a href>` so a
    // stray markdown link can't replace the document.
    m_viewer->setOpenLinks(false);
    m_viewer->setOpenExternalLinks(false);
    // ANTS-1239 / ANTS-1240 — apply the active terminal theme to the
    // dialog's palette so the Roadmap window blends with the rest of
    // the app instead of falling through to Qt's default dark palette.
    //
    // - The dialog itself gets Window/WindowText/Base/Text from the
    //   theme so checkboxes, tabs, labels, splitter, and search box
    //   all inherit the right colours.
    // - m_viewer (QTextBrowser) gets Base/Text/Link/LinkVisited. Qt's
    //   text engine paints <a> foreground using `QPalette::Link`,
    //   *not* the CSS `color` property — on Qt 6's default palette
    //   that role is near-black against most dark themes (user report
    //   2026-05-11). Setting Link/LinkVisited to textPrimary keeps the
    //   section-header chevron + title links legible. Base = bgPrimary
    //   so the area around the cards matches the terminal background
    //   instead of Qt's generic dark gray.
    // - m_toc (QListWidget) gets Base/Text + Highlight/HighlightedText
    //   so the sidebar matches and the selection indicator stays
    //   visible on every theme.
    {
        const Theme &th = Themes::byName(m_themeName);

        QPalette dp = palette();
        dp.setColor(QPalette::Window, th.bgPrimary);
        dp.setColor(QPalette::WindowText, th.textPrimary);
        dp.setColor(QPalette::Base, th.bgPrimary);
        dp.setColor(QPalette::AlternateBase, th.bgSecondary);
        dp.setColor(QPalette::Text, th.textPrimary);
        dp.setColor(QPalette::ButtonText, th.textPrimary);
        dp.setColor(QPalette::Button, th.bgSecondary);
        setPalette(dp);
        setAutoFillBackground(true);

        QPalette vp = m_viewer->palette();
        vp.setColor(QPalette::Base, th.bgPrimary);
        vp.setColor(QPalette::Text, th.textPrimary);
        vp.setColor(QPalette::Link, th.textPrimary);
        vp.setColor(QPalette::LinkVisited, th.textPrimary);
        m_viewer->setPalette(vp);

        QPalette tp = m_toc->palette();
        tp.setColor(QPalette::Base, th.bgPrimary);
        tp.setColor(QPalette::Text, th.textPrimary);
        tp.setColor(QPalette::Highlight, th.accent);
        tp.setColor(QPalette::HighlightedText, th.bgPrimary);
        m_toc->setPalette(tp);

        // ANTS-1242 — the frameless TitleBar paints itself from the
        // active theme rather than KWin's system colour scheme.
        if (m_titleBar) {
            m_titleBar->setThemeColors(th.bgSecondary, th.textPrimary,
                                       th.accent, th.border);
        }
    }
    // ANTS-1154: card / section toggle anchors use the `ants://` URL
    // scheme. The handler dispatches by URL verb and mutates the
    // relevant Config state set.
    connect(m_viewer, &QTextBrowser::anchorClicked,
            this, &RoadmapDialog::handleAnchorClicked);
    // Custom context menu — the auto-generated one parents its popup
    // to the QTextBrowser, which on Wayland (frameless translucent
    // parent stack on this build) makes Copy a no-op and traps the
    // popup so outside-clicks don't dismiss it. See QTBUG-79126.
    m_viewer->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_viewer, &QWidget::customContextMenuRequested,
            this, &RoadmapDialog::showViewerContextMenu);
    splitter->addWidget(m_viewer);

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 940});
    root->addWidget(splitter, 1);

    connect(m_toc, &QListWidget::itemActivated, this,
            [this](QListWidgetItem *item) {
                if (!item || !m_viewer) return;
                const QString anchor =
                    item->data(Qt::UserRole).toString();
                if (anchor.isEmpty()) return;
                m_viewer->scrollToAnchor(anchor);
            });
    connect(m_toc, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) {
                if (!item || !m_viewer) return;
                const QString anchor =
                    item->data(Qt::UserRole).toString();
                if (anchor.isEmpty()) return;
                m_viewer->scrollToAnchor(anchor);
            });

    // Plain QPushButton in an HBox row, mirroring the bg-tasks dialog
    // pattern that closes reliably on KDE/KWin + Qt 6.11 + frameless
    // translucent parent on Wayland. Earlier 0.7.43 attempt used
    // QDialogButtonBox::Close with both `rejected` and direct `clicked`
    // wiring; user reports in 0.7.49 confirmed the QDialogButtonBox
    // path still drops the click — likely the same xdg-shell modal /
    // role-dispatch interaction documented in QTBUG-79126. The plain
    // QPushButton route (no role-based dispatch) is what bg-tasks /
    // settings dialogs use successfully on this stack.
    auto *btnRow = new QHBoxLayout;
    // ANTS-1154 — Refresh button: force a re-read + rebuild. Bound to
    // F5 via QShortcut below. Live updates already fire on file
    // changes via QFileSystemWatcher, so this is the safety net for
    // cases where the watcher misses a change (atomic-rename, NFS).
    auto *refreshBtn = new QPushButton(tr("Refresh"), this);
    refreshBtn->setObjectName(QStringLiteral("roadmap-refresh-button"));
    refreshBtn->setShortcut(QKeySequence(Qt::Key_F5));
    connect(refreshBtn, &QPushButton::clicked, this, [this]() {
        if (m_lastHtml) m_lastHtml->clear();  // bust the dedup cache
        refreshShippedDatesIfStale();
        refreshLastTouchDatesIfStale();  // ANTS-1237
        rebuild();
    });
    btnRow->addWidget(refreshBtn);
    // ANTS-1154 — Reset View button: clear active-tab expand state
    // and search, restore filter checkboxes to the active preset's
    // default. Does not change the active tab (INV-15). Asks for
    // confirmation via a small inline label change rather than a
    // modal — partially sighted users do better without surprise
    // modals.
    auto *resetBtn = new QPushButton(tr("Reset View"), this);
    resetBtn->setObjectName(QStringLiteral("roadmap-reset-button"));
    connect(resetBtn, &QPushButton::clicked, this, [this, resetBtn]() {
        // First click: warn. Second click: do it.
        const QString armed = tr("Reset View (click again to confirm)");
        if (resetBtn->text() != armed) {
            resetBtn->setText(armed);
            QTimer::singleShot(3000, resetBtn, [resetBtn]() {
                resetBtn->setText(tr("Reset View"));
            });
            return;
        }
        resetBtn->setText(tr("Reset View"));
        m_expandedItems.clear();
        m_expandedSections.clear();
        m_tableSections.clear();
        if (m_searchBox) m_searchBox->clear();
        m_kindFilter.clear();
        for (auto it = m_kindCheckboxes.constBegin();
                  it != m_kindCheckboxes.constEnd(); ++it) {
            QSignalBlocker block(it.value());
            it.value()->setChecked(false);
        }
        // Re-apply current preset's default status mask + sort. Same
        // path as a tab click, but without changing the tab.
        applyPreset(m_activePreset);
    });
    btnRow->addWidget(resetBtn);
    btnRow->addStretch();
    auto *closeBtn = new QPushButton(tr("Close"), this);
    closeBtn->setObjectName(QStringLiteral("roadmap-close-button"));
    closeBtn->setDefault(true);
    closeBtn->setAutoDefault(true);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    // Live-update plumbing: 200 ms file-change debounce shared with
    // sibling dialogs (review-changes, bg-tasks). Kept separate from
    // m_searchDebounce below — the two have different latency budgets:
    // file-change can ride a 200 ms editor-save burst, while typing
    // search needs the snappier 120 ms feel. Merging into one timer
    // (ANTS-1123 indie-review LOW-6) would force one budget to win and
    // hurt either watch-burst coalescing or per-keystroke latency.
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(200);
    connect(&m_debounce, &QTimer::timeout, this, &RoadmapDialog::rebuild);

    // ANTS-1100: 120 ms search debounce so a fast typist doesn't
    // re-render after every keystroke.
    m_searchDebounce.setSingleShot(true);
    m_searchDebounce.setInterval(120);
    connect(&m_searchDebounce, &QTimer::timeout, this, &RoadmapDialog::rebuild);
    connect(m_searchBox, &QLineEdit::textChanged, this,
            [this]() { m_searchDebounce.start(); });

    m_watcher.addPath(roadmapPath);
    if (!m_changelogPath.isEmpty()) m_watcher.addPath(m_changelogPath);
    // Watch the archive directory too, so a /bump rotation that adds
    // a new <MAJOR>.<MINOR>.md to docs/roadmap/ triggers a rebuild
    // while the dialog is open. Per-file watches happen lazily — only
    // when an archive file is read does the user benefit from change
    // detection on it; for now, dir-watch is enough to pick up adds.
    const QString archiveDir = historyArchiveDir();
    if (!archiveDir.isEmpty()) m_watcher.addPath(archiveDir);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, &RoadmapDialog::scheduleRebuild);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &RoadmapDialog::scheduleRebuild);

    connect(m_filterDone, &QCheckBox::toggled,
            this, &RoadmapDialog::onCheckboxToggled);
    connect(m_filterPlanned, &QCheckBox::toggled,
            this, &RoadmapDialog::onCheckboxToggled);
    connect(m_filterInProgress, &QCheckBox::toggled,
            this, &RoadmapDialog::onCheckboxToggled);
    connect(m_filterConsidered, &QCheckBox::toggled,
            this, &RoadmapDialog::onCheckboxToggled);
    connect(m_filterCurrent, &QCheckBox::toggled,
            this, &RoadmapDialog::onCheckboxToggled);

    connect(m_tabs, &QTabBar::currentChanged, this,
            [this](int index) {
                if (m_suppressTabSignal) return;
                static constexpr Preset order[] = {
                    Preset::Full, Preset::History, Preset::Current,
                    Preset::Next, Preset::FarFuture, Preset::Custom,
                };
                // ANTS-1123 indie-review LOW-2: catch any future
                // Preset enum addition that would silently extend
                // past the array — `static_cast<int>(Preset::Custom)`
                // is the highest-numbered preset by convention; if a
                // new value gets inserted before it, this trips and
                // forces the author to update the array.
                static_assert(sizeof(order) / sizeof(order[0]) ==
                              static_cast<size_t>(Preset::Custom) + 1,
                              "tab order[] must match Preset enum size");
                if (index < 0 ||
                    index >= int(sizeof(order) / sizeof(order[0]))) return;
                applyPreset(order[index]);
            });

    // Restore persisted geometry if Config has one — same shape as
    // setWindowGeometryBase64 / saveGeometry round-trip. ANTS-1123
    // indie-review LOW-1: clear the persisted blob if restoreGeometry
    // returns false (corrupt / wrong-Qt-version saveformat) so a bad
    // blob doesn't masquerade as valid forever — next open will fall
    // back to the 1200x800 default and persist a clean blob on close.
    if (m_config) {
        const QString stored = m_config->roadmapDialogGeometry();
        if (!stored.isEmpty()) {
            const QByteArray bytes =
                QByteArray::fromBase64(stored.toLatin1());
            const bool restored = !bytes.isEmpty() && restoreGeometry(bytes);
            if (!restored) m_config->setRoadmapDialogGeometry(QString());
        }
    }

    // ANTS-1150 — restore persisted UI state. Order matters
    // (cold-eyes CRITICAL #1):
    //   (1) Restore Kind filter set — always (Kind is preset-
    //       orthogonal).
    //   (2) Determine persisted preset enum.
    //   (3) For Custom only — restore status checkboxes silently.
    //       Named presets get applyPreset's canonical mask anyway
    //       so a status restore would be dead code.
    //   (4) Apply the persisted preset (fires rebuild).
    //
    // (1) Kind filter set.
    if (m_config) {
        const QStringList persistedKinds = m_config->roadmapKindFilters();
        m_kindFilter = QSet<QString>(persistedKinds.begin(),
                                     persistedKinds.end());
        for (auto it = m_kindCheckboxes.constBegin();
                  it != m_kindCheckboxes.constEnd(); ++it) {
            QSignalBlocker block(it.value());
            it.value()->setChecked(m_kindFilter.contains(it.key()));
        }
        // Belt-and-suspenders (cold-eyes HIGH #4): clear the
        // rendered-html cache so the first rebuild after restore
        // re-renders with the restored filter, even if a watcher
        // fire raced ahead.
        if (m_lastHtml) m_lastHtml->clear();

        // ANTS-1154: restore card / section / table expand state.
        const QStringList exItems = m_config->roadmapExpandedItems();
        m_expandedItems = QSet<QString>(exItems.begin(), exItems.end());
        const QStringList exSecs = m_config->roadmapExpandedSections();
        m_expandedSections = QSet<QString>(exSecs.begin(), exSecs.end());
        const QStringList tabSecs = m_config->roadmapTableSections();
        m_tableSections = QSet<QString>(tabSecs.begin(), tabSecs.end());
    }
    refreshShippedDatesIfStale();
    refreshLastTouchDatesIfStale();  // ANTS-1237

    // (2) Persisted preset.
    Preset persisted = Preset::Full;
    if (m_config) {
        const QString name = m_config->roadmapActivePreset();
        if      (name == QLatin1String("history"))    persisted = Preset::History;
        else if (name == QLatin1String("current"))    persisted = Preset::Current;
        else if (name == QLatin1String("next"))       persisted = Preset::Next;
        else if (name == QLatin1String("far_future")) persisted = Preset::FarFuture;
        else if (name == QLatin1String("custom"))     persisted = Preset::Custom;
        // Unknown / "full" → Preset::Full default.
    }

    // (3) Custom-only status restore. If sf.isEmpty() (user picked
    // Custom via tab click but never toggled a status checkbox, so
    // onCheckboxToggled never wrote roadmap_status_filters), treat
    // the missing object as "all on" — equivalent to a fresh-Custom
    // state. We must NOT silently flip persisted back to Full here:
    // doing so writes "full" to disk via persistActivePreset and
    // discards the user's Custom choice without their knowledge.
    // The .toBool(true) defaults handle the empty case naturally.
    if (persisted == Preset::Custom && m_config) {
        const QJsonObject sf = m_config->roadmapStatusFilters();
        m_suppressCheckboxSignal = true;
        if (m_filterDone)
            m_filterDone->setChecked(sf.value(QLatin1String("done")).toBool(true));
        if (m_filterPlanned)
            m_filterPlanned->setChecked(sf.value(QLatin1String("planned")).toBool(true));
        if (m_filterInProgress)
            m_filterInProgress->setChecked(sf.value(QLatin1String("in_progress")).toBool(true));
        if (m_filterConsidered)
            m_filterConsidered->setChecked(sf.value(QLatin1String("considered")).toBool(true));
        if (m_filterCurrent)
            m_filterCurrent->setChecked(sf.value(QLatin1String("current")).toBool(true));
        m_suppressCheckboxSignal = false;
    }

    // (4) Apply the persisted preset (fires rebuild).
    applyPreset(persisted);
}

RoadmapDialog::~RoadmapDialog() = default;

void RoadmapDialog::closeEvent(QCloseEvent *event) {
    // Persist geometry on close so the next open lands at the same
    // size + position. Mirrors the audit-dialog convention discussed
    // in the ANTS-1100 spec.
    if (m_config) {
        const QByteArray bytes = saveGeometry();
        m_config->setRoadmapDialogGeometry(
            QString::fromLatin1(bytes.toBase64()));
        // ANTS-1154 — persist card / section / table state on close.
        m_config->setRoadmapExpandedItems(
            QStringList(m_expandedItems.begin(), m_expandedItems.end()));
        m_config->setRoadmapExpandedSections(
            QStringList(m_expandedSections.begin(), m_expandedSections.end()));
        m_config->setRoadmapTableSections(
            QStringList(m_tableSections.begin(), m_tableSections.end()));
    }
    QDialog::closeEvent(event);
}

// ANTS-1236 — Roadmap dialog keyboard cheatsheet trigger. `?` opens
// `RoadmapShortcutsDialog` (lazy + reused via QPointer per INV-6);
// pressing `?` again on the cheatsheet itself closes it. The
// search-box-focus guard (INV-4) lets the user type `?` into the
// substring filter — when the QLineEdit owns focus its own
// keyPressEvent runs first and consumes the event, so the guard is
// belt-and-braces for the case where focus has logically escaped.
//
// Layout-robust match via `event->text()` rather than `Key_Question` —
// AltGr+something on Polish layouts, dead-key sequences, etc. still
// resolve to "?" at the text() level even where Qt omits Key_Question.
void RoadmapDialog::keyPressEvent(QKeyEvent *event) {
    // ANTS-1234 — `/` focuses the search box (Linear / GitHub / Notion
    // convention). Layout-robust via event->text() so AltGr / dead-key
    // routes still hit it. Gated on !m_searchBox->hasFocus() so the
    // user can type `/` into the predicate itself (URLs, paths, etc.).
    if (event
        && event->text() == QLatin1String("/")
        && !(event->modifiers() & Qt::ControlModifier)
        && !(m_searchBox && m_searchBox->hasFocus())) {
        focusSearchBox();
        event->accept();
        return;
    }
    if (event
        && event->text() == QLatin1String("?")
        && !(event->modifiers() & Qt::ControlModifier)
        && !(m_searchBox && m_searchBox->hasFocus())) {
        if (!m_shortcutsDialog) {
            m_shortcutsDialog = new RoadmapShortcutsDialog(m_themeName, this);
        }
        m_shortcutsDialog->show();
        m_shortcutsDialog->raise();
        m_shortcutsDialog->activateWindow();
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

// ANTS-1234 — focus the search box and select any pre-existing text
// so the next keystroke replaces it. Called from keyPressEvent on `/`.
void RoadmapDialog::focusSearchBox() {
    if (!m_searchBox) return;
    m_searchBox->setFocus(Qt::ShortcutFocusReason);
    m_searchBox->selectAll();
}

// ANTS-1234 — Esc handler for the search box. Consumes the event so
// QDialog::reject doesn't fire on a focused-search Esc. For every
// other key, falls through to QDialog::eventFilter so the QLineEdit
// receives PgUp / F5 / typing characters normally.
bool RoadmapDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == m_searchBox && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Escape) {
            m_searchBox->clear();
            m_searchBox->clearFocus();
            return true;
        }
    }
    return QDialog::eventFilter(obj, event);
}

// ANTS-1154 — anchorClicked handler for `ants://` URLs emitted by
// renderCardsHtml. Dispatches by verb: expand / collapse toggle a
void RoadmapDialog::showViewerContextMenu(const QPoint &pos) {
    if (!m_viewer) return;
    // Parent the menu to the dialog top-level (not m_viewer) so the
    // Wayland xdg_popup attaches to the dialog's surface, which has a
    // stable role. Parenting to m_viewer can produce a popup whose
    // surface the compositor never grants input to.
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const bool hasSelection = m_viewer->textCursor().hasSelection();
    auto *copyAct = menu->addAction(tr("Copy"));
    copyAct->setEnabled(hasSelection);
    copyAct->setShortcut(QKeySequence::Copy);
    connect(copyAct, &QAction::triggered, this, [this]() {
        if (m_viewer) m_viewer->copy();
    });
    auto *selectAllAct = menu->addAction(tr("Select All"));
    selectAllAct->setShortcut(QKeySequence::SelectAll);
    connect(selectAllAct, &QAction::triggered, this, [this]() {
        if (m_viewer) m_viewer->selectAll();
    });
    menu->popup(m_viewer->viewport()->mapToGlobal(pos));
}

// card's ID in m_expandedItems; expand-section / collapse-section
// toggle a section's slug in m_expandedSections; table toggles a
// section's slug in m_tableSections. Each mutation triggers a
// rebuild so the new state renders immediately.
void RoadmapDialog::handleAnchorClicked(const QUrl &link) {
    if (link.scheme() != QLatin1String("ants")) {
        // Internal-anchor jumps (`#roadmap-toc-N`) come through here
        // too because setOpenLinks(false) routes ALL anchor clicks
        // to this signal. Pass them to scrollToAnchor.
        //
        // Indie-review-2026-05-14 lane-6 H-2: only treat the link as
        // an internal-anchor jump when it really is one — scheme +
        // host both empty, fragment non-empty. A hostile ROADMAP.md
        // shipping `[click](https://attacker.example/#evil)` would
        // otherwise feed an attacker-controlled fragment string into
        // scrollToAnchor. scrollToAnchor is safe today (no JS / no
        // IO) but accepting arbitrary external schemes silently
        // turns this handler into an attack surface the moment a
        // future maintainer extends it (e.g. logging or opening the
        // link "for convenience").
        const bool isInternalAnchor = link.scheme().isEmpty()
                                       && link.host().isEmpty();
        if (m_viewer && isInternalAnchor && !link.fragment().isEmpty()) {
            m_viewer->scrollToAnchor(link.fragment());
        }
        return;
    }
    const QString verb = link.host();
    // QUrl strips the leading `/` of the path — `ants://expand/ANTS-1145`
    // gives host="expand", path="/ANTS-1145".
    QString target = link.path();
    if (target.startsWith('/')) target.remove(0, 1);
    if (target.isEmpty()) return;

    if (verb == QLatin1String("expand")) {
        m_expandedItems.insert(target);
    } else if (verb == QLatin1String("collapse")) {
        m_expandedItems.remove(target);
    } else if (verb == QLatin1String("expand-section")) {
        m_expandedSections.insert(target);
    } else if (verb == QLatin1String("collapse-section")) {
        m_expandedSections.remove(target);
    } else if (verb == QLatin1String("table")) {
        if (m_tableSections.contains(target)) m_tableSections.remove(target);
        else m_tableSections.insert(target);
    } else {
        return;  // unknown verb
    }
    scheduleRebuild();
}

void RoadmapDialog::refreshShippedDatesIfStale() {
    if (m_changelogPath.isEmpty()) return;
    const QFileInfo fi(m_changelogPath);
    const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
    if (mtime == m_shippedDatesMtime && !m_shippedDates.isEmpty()) return;
    m_shippedDates = parseShippedDates(m_changelogPath);
    m_shippedDatesMtime = mtime;
}

void RoadmapDialog::refreshLastTouchDatesIfStale() {
    if (m_roadmapPath.isEmpty()) return;
    const qint64 mtime = QFileInfo(m_roadmapPath).lastModified()
                             .toMSecsSinceEpoch();
    if (mtime == m_lastTouchDatesMtime
        && !m_lastTouchDates.isEmpty()) return;
    m_lastTouchDates = parseLastTouchDates(m_roadmapPath);
    m_lastTouchDatesMtime = mtime;
}

void RoadmapDialog::applyPreset(Preset p) {
    // Custom is "leave the user's tuning alone" — both checkboxes and
    // sort order. ANTS-1123 indie-review LOW-3: previously this code
    // ran `m_sortOrder = sortFor(Custom) = Document` even on the
    // Custom branch, so clicking the Custom tab from History flipped
    // descending → document-order silently. Spec INV-13's "Custom →
    // Document" applies to the named-preset → Custom transition via
    // checkbox divergence (handled in onCheckboxToggled, which
    // doesn't call applyPreset). For an explicit Custom tab click we
    // preserve whatever the user has staged.
    if (p == Preset::Custom) {
        m_activePreset = p;
        persistActivePreset(p);  // ANTS-1150
        if (m_tabs) {
            const int idx = static_cast<int>(p);
            if (m_tabs->currentIndex() != idx) {
                m_suppressTabSignal = true;
                m_tabs->setCurrentIndex(idx);
                m_suppressTabSignal = false;
            }
        }
        rebuild();
        return;
    }

    m_activePreset = p;
    persistActivePreset(p);  // ANTS-1150
    const unsigned mask = filterFor(p);
    m_sortOrder = sortFor(p);

    // Sync the checkboxes to the named preset's mask without
    // re-firing onCheckboxToggled.
    {
        m_suppressCheckboxSignal = true;
        if (m_filterDone)
            m_filterDone->setChecked((mask & ShowDone) != 0);
        if (m_filterPlanned)
            m_filterPlanned->setChecked((mask & ShowPlanned) != 0);
        if (m_filterInProgress)
            m_filterInProgress->setChecked((mask & ShowInProgress) != 0);
        if (m_filterConsidered)
            m_filterConsidered->setChecked((mask & ShowConsidered) != 0);
        if (m_filterCurrent)
            m_filterCurrent->setChecked((mask & ShowCurrent) != 0);
        m_suppressCheckboxSignal = false;
    }

    // Sync the tab bar selection to the preset (silent — no
    // currentChanged loop).
    if (m_tabs) {
        const int idx = static_cast<int>(p);
        if (m_tabs->currentIndex() != idx) {
            m_suppressTabSignal = true;
            m_tabs->setCurrentIndex(idx);
            m_suppressTabSignal = false;
        }
    }

    rebuild();
}

void RoadmapDialog::onCheckboxToggled() {
    // ANTS-1123 indie-review LOW-4: m_suppressCheckboxSignal is NOT
    // redundant — Qt's QAbstractButton::toggled doesn't fire on a
    // no-op `setChecked(currentState)`, but applyPreset switches
    // between presets that have *different* mask shapes (e.g. Full
    // vs Current), so any one of those `setChecked` calls actively
    // flips state and would re-enter onCheckboxToggled and bounce
    // the tab back to Custom mid-preset-apply. Guard retained.
    if (m_suppressCheckboxSignal) return;
    // The user diverged from a named preset; flip the tab bar to
    // Custom (silent) and re-render with the current sort order.
    if (m_tabs) {
        unsigned mask = 0;
        if (m_filterDone && m_filterDone->isChecked()) mask |= ShowDone;
        if (m_filterPlanned && m_filterPlanned->isChecked()) mask |= ShowPlanned;
        if (m_filterInProgress && m_filterInProgress->isChecked()) mask |= ShowInProgress;
        if (m_filterConsidered && m_filterConsidered->isChecked()) mask |= ShowConsidered;
        if (m_filterCurrent && m_filterCurrent->isChecked()) mask |= ShowCurrent;
        const Preset p = presetMatching(mask, m_sortOrder);
        m_activePreset = p;
        persistActivePreset(p);  // ANTS-1150 — second m_activePreset
                                 // write site (cold-eyes CRITICAL #2)
        // ANTS-1150 — persist the status-checkbox mask too. Only
        // matters when p == Custom (named-preset reads from
        // applyPreset's canonical mask), but unconditional save is
        // simpler than branching and storeIfChanged short-circuits
        // on no-change anyway.
        if (m_config) {
            QJsonObject sf;
            sf[QLatin1String("done")]        = m_filterDone        && m_filterDone->isChecked();
            sf[QLatin1String("planned")]     = m_filterPlanned     && m_filterPlanned->isChecked();
            sf[QLatin1String("in_progress")] = m_filterInProgress  && m_filterInProgress->isChecked();
            sf[QLatin1String("considered")]  = m_filterConsidered  && m_filterConsidered->isChecked();
            sf[QLatin1String("current")]     = m_filterCurrent     && m_filterCurrent->isChecked();
            m_config->setRoadmapStatusFilters(sf);
        }
        const int idx = static_cast<int>(p);
        if (m_tabs->currentIndex() != idx) {
            m_suppressTabSignal = true;
            m_tabs->setCurrentIndex(idx);
            m_suppressTabSignal = false;
        }
    }
    rebuild();
}

void RoadmapDialog::persistActivePreset(Preset p) {
    if (!m_config) return;
    const char *name = nullptr;
    switch (p) {
        case Preset::Full:      name = "full";       break;
        case Preset::History:   name = "history";    break;
        case Preset::Current:   name = "current";    break;
        case Preset::Next:      name = "next";       break;
        case Preset::FarFuture: name = "far_future"; break;
        case Preset::Custom:    name = "custom";     break;
    }
    if (name) m_config->setRoadmapActivePreset(QString::fromLatin1(name));
}

void RoadmapDialog::scheduleRebuild() {
    // Re-add the watch — QFileSystemWatcher drops watches after atomic
    // editor saves. Same dance as the review-changes / bg-tasks
    // dialogs.
    if (!m_watcher.files().contains(m_roadmapPath))
        m_watcher.addPath(m_roadmapPath);
    if (!m_changelogPath.isEmpty() &&
        !m_watcher.files().contains(m_changelogPath)) {
        m_watcher.addPath(m_changelogPath);
    }
    m_debounce.start();
}

QString RoadmapDialog::archiveDirFor(const QString &roadmapPath) {
    // ANTS-1125 INV-1 / INV-1a: derive the archive path from the
    // canonical (symlink-resolved) roadmapPath, then check that
    // <dir>/docs/roadmap/ exists, is a directory, and is readable.
    // Empty string in *every* failure mode (missing, regular-file,
    // broken symlink, symlink cycle, unreadable, non-directory).
    if (roadmapPath.isEmpty()) return QString();
    const QString canonical =
        QFileInfo(roadmapPath).canonicalFilePath();
    if (canonical.isEmpty()) return QString();   // broken symlink / cycle
    const QString candidatePath =
        QFileInfo(canonical).absoluteDir().absolutePath()
        + QStringLiteral("/docs/roadmap");
    const QFileInfo candidateInfo(candidatePath);
    if (!candidateInfo.exists()) return QString();
    if (!candidateInfo.isDir()) return QString(); // regular file shadowing
    if (!candidateInfo.isReadable()) return QString();
    const QString resolved = candidateInfo.canonicalFilePath();
    if (resolved.isEmpty()) return QString();    // archive dir is itself a cycle
    return resolved;
}

namespace {
// ANTS-1125 INV-4a: archive filename is exactly <MAJOR>.<MINOR>.md
// (case-sensitive). 0.7.0.md / latest.md / 0.7.MD / hidden / .bak
// are silently skipped.
bool parseArchiveFilename(const QString &name, int *majorOut, int *minorOut) {
    static const QRegularExpression re(
        QStringLiteral(R"(^(\d+)\.(\d+)\.md$)"));
    const auto m = re.match(name);
    if (!m.hasMatch()) return false;
    if (majorOut) *majorOut = m.captured(1).toInt();
    if (minorOut) *minorOut = m.captured(2).toInt();
    return true;
}
} // namespace

QString RoadmapDialog::loadMarkdown(const QString &roadmapPath,
                                    bool includeArchive) {
    // ANTS-1012 indie-review-2026-04-27 + ANTS-1125 INV-5: per-file
    // 8 MiB cap on every QFile::read() call inside this helper.
    // Defends against /dev/zero symlinks and accidental binary
    // content. Real archives top out under 1 MiB.
    constexpr qint64 kPerFileCap = 8 * 1024 * 1024;
    // ANTS-1125 INV-5a: total assembled-buffer cap of 64 MiB. After
    // concatenation, if the assembled buffer would exceed this, the
    // loader stops adding archives and emits a truncation sentinel.
    constexpr qint64 kAssembledCap = 64 * 1024 * 1024;

    QString markdown;
    QFile f(roadmapPath);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        markdown = QString::fromUtf8(f.read(kPerFileCap));
    }
    if (!includeArchive) return markdown;

    const QString dir = archiveDirFor(roadmapPath);
    if (dir.isEmpty()) return markdown;

    // INV-4a: filter entries through the case-sensitive
    // <MAJOR>.<MINOR>.md regex. Non-conforming entries (latest.md,
    // 0.7.0.md, *.bak, hidden, non-.md) are skipped silently.
    // INV-4 / INV-11: numeric descending sort by the parsed
    // (major, minor) tuple — lexical sort breaks at minor 10
    // (0.10 < 0.9 lexically).
    QDir d(dir);
    const QStringList rawEntries =
        d.entryList(QDir::Files | QDir::Readable, QDir::NoSort);
    struct Entry { int major; int minor; QString name; };
    QVector<Entry> entries;
    entries.reserve(rawEntries.size());
    for (const QString &name : rawEntries) {
        int major = 0, minor = 0;
        if (parseArchiveFilename(name, &major, &minor)) {
            entries.push_back({major, minor, name});
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) {
        if (a.major != b.major) return a.major > b.major;
        return a.minor > b.minor;
    });

    for (const Entry &e : entries) {
        // INV-5a: stop adding archives once the assembled buffer
        // would exceed the total cap. Emit a single sentinel marking
        // the truncation point and break out of the loop.
        if (markdown.size() >= kAssembledCap) {
            markdown += QStringLiteral(
                "\n\n---\n\n<!-- archive: truncated past 64 MiB cap -->\n\n");
            break;
        }
        QFile af(d.filePath(e.name));
        if (!af.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        // INV-4: thematic-break + HTML-comment sentinel separator
        // before each archive's content. Markdown's `---` thematic
        // break terminates any open list/heading context the prior
        // file's truncated tail might have left dangling.
        markdown += QStringLiteral("\n\n---\n\n<!-- archive: ");
        markdown += e.name;
        markdown += QStringLiteral(" -->\n\n");
        markdown += QString::fromUtf8(af.read(kPerFileCap));
    }
    return markdown;
}

bool RoadmapDialog::shouldLoadHistory(Preset activePreset,
                                      const QString &searchText) {
    // ANTS-1125 INV-6: trigger on Preset::History the *enumerator*
    // (not a tab-index literal — the array order at construction may
    // change), OR a non-empty trimmed search predicate.
    if (activePreset == Preset::History) return true;
    if (!searchText.trimmed().isEmpty()) return true;
    return false;
}

bool RoadmapDialog::wantsHistoryLoad() const {
    return shouldLoadHistory(
        m_activePreset,
        m_searchBox ? m_searchBox->text() : QString());
}

void RoadmapDialog::rebuild() {
    if (!m_viewer) return;

    const bool includeArchive = wantsHistoryLoad();
    const QString markdown = loadRoadmapMarkdown(includeArchive);

    unsigned filter = 0;
    if (m_filterDone && m_filterDone->isChecked()) filter |= ShowDone;
    if (m_filterPlanned && m_filterPlanned->isChecked()) filter |= ShowPlanned;
    if (m_filterInProgress && m_filterInProgress->isChecked()) filter |= ShowInProgress;
    if (m_filterConsidered && m_filterConsidered->isChecked()) filter |= ShowConsidered;
    if (m_filterCurrent && m_filterCurrent->isChecked()) filter |= ShowCurrent;

    const QStringList signals_ = collectCurrentBullets();
    const QString predicate = m_searchBox ? m_searchBox->text() : QString();
    // ANTS-1154: refresh the shipped-date cache if CHANGELOG.md has
    // changed since the last render. Cheap stat-only check.
    refreshShippedDatesIfStale();
    refreshLastTouchDatesIfStale();  // ANTS-1237
    CardRenderOptions opts;
    opts.activePreset = m_activePreset;
    opts.expandedItems = m_expandedItems;
    opts.expandedSections = m_expandedSections;
    opts.tableSections = m_tableSections;
    opts.shippedDates = m_shippedDates;
    opts.lastTouchDates = m_lastTouchDates;
    opts.density = m_density;  // ANTS-1238
    const QString html = renderCardsHtml(markdown, filter, signals_, m_themeName,
                                         m_sortOrder, predicate, m_kindFilter,
                                         opts);

    if (m_lastHtml && *m_lastHtml == html) return;  // skip identical re-render

    auto *vbar = m_viewer->verticalScrollBar();
    const int saved = vbar ? vbar->value() : 0;
    const int oldMax = vbar ? vbar->maximum() : 0;
    const bool wasAtBottom = vbar && (oldMax - saved <= 4);

    m_viewer->setHtml(html);
    if (m_lastHtml) *m_lastHtml = html;

    if (vbar) {
        if (wasAtBottom) vbar->setValue(vbar->maximum());
        else vbar->setValue(qMin(saved, vbar->maximum()));
    }

    // Refresh the TOC sidebar from the same markdown so the
    // anchor indices line up with what renderHtml just emitted.
    // When the active sort reorders sections, the TOC must walk the
    // post-reorder markdown so anchor indices match.
    if (m_toc) {
        const QString prevAnchor =
            m_toc->currentItem()
                ? m_toc->currentItem()->data(Qt::UserRole).toString()
                : QString();
        m_toc->clear();
        const QString tocSource =
            (m_sortOrder == SortOrder::DescendingChronological)
                ? reverseTopLevelSections(markdown)
                : markdown;
        const QVector<TocEntry> entries = extractToc(tocSource);
        for (const TocEntry &e : entries) {
            // Indent by level — flat QListWidget shows hierarchy via
            // leading spaces (two per level above 1).
            QString prefix;
            for (int i = 1; i < e.level; ++i) prefix.append(QStringLiteral("  "));
            auto *item = new QListWidgetItem(prefix + e.text, m_toc);
            item->setData(Qt::UserRole, e.anchor);
            QFont itemFont = item->font();
            itemFont.setBold(e.level == 1);
            item->setFont(itemFont);
            if (e.anchor == prevAnchor) m_toc->setCurrentItem(item);
        }
    }
}
