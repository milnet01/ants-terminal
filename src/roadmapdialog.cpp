#include "roadmapdialog.h"

#include "coloredtabbar.h"     // for ClaudeTabIndicator::color (ToolUse yellow)
#include "config.h"
#include "dialogchrome.h"      // ANTS-1242 — frameless dialog chrome
#include "roadmapindex.h"      // ANTS-1287 — canonical home for heading/slug helpers
// ANTS-3793 — the read seam and the store behind it. Included HERE and not in
// roadmapdialog.h: roadmapstore.h pulls <QSqlDatabase>, and ants_dialogs_lib
// links the store PRIVATE.
#include "roadmapsource.h"
#include "roadmapstore.h"
#include "roadmapshortcutsdialog.h"  // ANTS-1236 — `?` cheatsheet overlay
#include "themes.h"
#include "titlebar.h"

#include <QAbstractTextDocumentLayout>
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
#include <QShowEvent>
#include <QTabBar>
#include <QToolButton>      // ANTS-4412 — collapsed filter controls
#include <QWidgetAction>    // ANTS-4412 — checkboxes hosted inside a QMenu
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextTable>
#include <QTextFrame>
#include <functional>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// Status emojis the ROADMAP legend documents. Keep in lock-step with
// the legend block at the top of ROADMAP.md and with INV-3 of the
// roadmap_viewer feature test.
// ANTS-3764 — the status-emoji vocabulary moved to roadmapparse.h with the
// parser that reads it. Pulled back into scope here so kStatusLabels and the
// renderHtml bullet classifier below read exactly as before.
using RoadmapParse::kEmojiDone;
using RoadmapParse::kEmojiPlanned;
using RoadmapParse::kEmojiInProgress;
using RoadmapParse::kEmojiConsidered;

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
    // Escape '"' too: htmlEscape output lands in HTML *attribute* position in
    // several card sinks (href="ants://…/%1", id="rm-%1") where rec.id is
    // attacker-influenceable via a hostile ROADMAP.md bold-ID. Without this a
    // '"' in the id breaks out of the attribute (CWE-79, markup-injection).
    s.replace('"', QStringLiteral("&quot;"));
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
// `<style>` block. Cozy is the default tier (INV-1 locks default==Cozy;
// ANTS-2211 raised its meta/label tier to 12 px, so Cozy is no longer
// byte-equal to the pre-1238 renderer for those classes). Compact /
// Comfortable are -2 / +2 shifts on
// the body / heading / code groups, with an 11 px floor on the
// meta + label tier (INV-8; raised from 9 px by ANTS-2211). Spec:
// docs/specs/ANTS-1238.md § 2.f.
//
// Field naming mirrors the spec: bodyPx covers `body`,
// `.rm-summary`, `.rm-state`, `.rm-section-toggle`,
// `.rm-body-first`, `.rm-body-line` (all at the same value per
// tier). h1Px..h4Px cover the four heading levels. codePx
// covers `code`, `.rm-toggle`, `.rm-id`, `td`, `th`. metaPx
// covers `.rm-kind`, `.rm-section-counts`, `.rm-parent`,
// `.rm-date`. labelPx is `.rm-state-label`; at Compact it shares the
// 11 px floor with metaPx (ANTS-2211). Vertical padding
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
    // ANTS-3762 — fixed widths for the three non-flexible card columns
    // (`.rm-col-state` / `.rm-col-kind` / `.rm-col-meta`); the summary column
    // takes the remainder. Every section renders its OWN `table.rm-cards`, so
    // without these Qt's auto-layout sizes each table from its own content and
    // the columns land somewhere different in every group — which is the
    // misalignment the item was filed for. They scale with the tier because
    // the text in them does; they live here (and reach the document only via
    // the <style> block) so ANTS-1238 INV-6 — non-<style> HTML byte-identical
    // across tiers — still holds.
    int colStatePx, colKindPx, colMetaPx;
};
constexpr DensityTier kDensityTable[3] = {
    // Compact: -2 px tier; label + meta groups floored at 11 px (ANTS-2211,
    // raised from 9 px for readability).
    {/*bodyPx*/11, /*h1Px*/14, /*h2Px*/11, /*h3Px*/10, /*h4Px*/10,
     /*codePx*/10, /*metaPx*/11, /*labelPx*/11,
     /*pMargin*/1, /*hMarginTop*/2, /*hMarginBottom*/1,
     /*cardPaddingY*/2, /*cardPaddingX*/6, /*cardMargin*/1,
     /*bodyFirstPaddingTop*/2, /*bodyFirstMarginTop*/1,
     /*colStatePx*/258, /*colKindPx*/94, /*colMetaPx*/68},
    // Cozy: current default. (ANTS-2211 raised the meta + label tier to
    // 12 px for readability, so Cozy is no longer byte-equal to the
    // pre-1238 renderer for those two classes; INV-1 default==Cozy holds.)
    {/*bodyPx*/13, /*h1Px*/16, /*h2Px*/13, /*h3Px*/12, /*h4Px*/11,
     /*codePx*/12, /*metaPx*/12, /*labelPx*/12,
     /*pMargin*/3, /*hMarginTop*/4, /*hMarginBottom*/2,
     /*cardPaddingY*/4, /*cardPaddingX*/8, /*cardMargin*/2,
     /*bodyFirstPaddingTop*/4, /*bodyFirstMarginTop*/2,
     /*colStatePx*/290, /*colKindPx*/106, /*colMetaPx*/76},
    // Comfortable: +2 px tier, more vertical headroom.
    {/*bodyPx*/15, /*h1Px*/18, /*h2Px*/15, /*h3Px*/14, /*h4Px*/13,
     /*codePx*/14, /*metaPx*/13, /*labelPx*/12,
     /*pMargin*/5, /*hMarginTop*/6, /*hMarginBottom*/4,
     /*cardPaddingY*/6, /*cardPaddingX*/12, /*cardMargin*/4,
     /*bodyFirstPaddingTop*/6, /*bodyFirstMarginTop*/3,
     /*colStatePx*/322, /*colKindPx*/118, /*colMetaPx*/86},
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
    // ANTS-2012 — readRecentCommitSubjects() shells out to a blocking `git
    // log` (up to 1.5 s). rebuild() runs on every search keystroke, so an
    // uncached call spawned git per keystroke — multi-second GUI jank while
    // typing a filter. The external signals (CHANGELOG unreleased bullets +
    // recent commit subjects) don't change during a typing burst, so cache
    // them with a short TTL: a keystroke storm reuses one result, and newly
    // landed commits still surface within a few seconds.
    constexpr qint64 kExternalSignalsTtlMs = 5000;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_externalSignalsCacheMs != 0
        && nowMs - m_externalSignalsCacheMs < kExternalSignalsTtlMs)
        return m_externalSignalsCache;

    QStringList out;
    if (!m_changelogPath.isEmpty()) out += readUnreleasedBullets(m_changelogPath);
    const QFileInfo fi(m_roadmapPath);
    out += readRecentCommitSubjects(fi.absolutePath());
    m_externalSignalsCache = out;
    m_externalSignalsCacheMs = nowMs;
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

// ANTS-3764 — the parser moved to RoadmapParse (ants_core_lib). This
// forwarder is what keeps every existing RoadmapDialog::parseBullets() call
// site — the roadmap-query IPC verb, renderCardsHtml, the feature tests —
// compiling unchanged. The memo the old body carried moved with it.
QVector<RoadmapDialog::BulletRecord>
RoadmapDialog::parseBullets(const QString &markdownText) {
    return RoadmapParse::parseBullets(markdownText);
}

// ANTS-3793 § 2.2 — see roadmapdialog.h. The up-walk stops at the same `.git`
// boundary findRoadmapUnder()'s down-walk does, so the two are inverses on
// every layout that helper supports (`./`, `docs/`, `docs/private/`,
// `docs/internal/`, `.github/`).
QString RoadmapDialog::storeProjectRoot() const {
    if (m_roadmapPath.isEmpty()) return {};
    const QString canonical = QFileInfo(m_roadmapPath).canonicalFilePath();
    if (canonical.isEmpty()) return {};
    const QString startDir = QFileInfo(canonical).path();
    QString dir = startDir;
    for (int depth = 0; depth < 64 && !dir.isEmpty(); ++depth) {
        if (QFileInfo::exists(dir + QStringLiteral("/.git"))) return dir;
        const QString parent = QFileInfo(dir).path();
        if (parent == dir) break;
        dir = parent;
    }
    return startDir;
}

// ANTS-3793 § 2.1 — the dialog's owner wrapper. Same three outcomes as
// RemoteControl's, presented differently: a refusal has no envelope to fill, so
// it lands in m_sourceError for rebuild() to render.
QVector<RoadmapDialog::BulletRecord>
RoadmapDialog::roadmapBullets(const QString &markdown,
                              bool includeArchive) const {
    m_sourceError.clear();
    m_lastReadFromStore = false;
    const QString root = storeProjectRoot();
    if (root.isEmpty())
        return RoadmapParse::parseBullets(markdown);

    if (!m_roadmapStore) {
        // § 2.2 rules 1 and 2: absent → markdown (the common case, and no
        // error); present but unopenable → refuse.
        RoadmapSource::ReadError openWhy = RoadmapSource::ReadError::None;
        QString openErr;
        m_roadmapStore = RoadmapSource::storeFor(RoadmapStore::defaultPath(),
                                                 &openWhy, &openErr);
        if (openWhy != RoadmapSource::ReadError::None) {
            m_sourceError = openErr;
            return {};
        }
    }
    if (!m_roadmapStore)
        return RoadmapParse::parseBullets(markdown);

    RoadmapSource::ReadError why = RoadmapSource::ReadError::None;
    QString err;
    auto records = RoadmapSource::bulletsFor(*m_roadmapStore, root, markdown,
                                             includeArchive, &why, &err);
    if (records) {
        m_lastReadFromStore = true;
        return *records;
    }
    if (why != RoadmapSource::ReadError::None) {
        m_sourceError = err;
        return {};   // INV-1 — never the markdown behind the store's back
    }
    return RoadmapParse::parseBullets(markdown);
}

// ANTS-3793 § 2.3 — the stored legend, or nothing.
QHash<QString, QString> RoadmapDialog::storeLegend() const {
    QHash<QString, QString> out;
    if (!m_roadmapStore) return out;   // never opened ⇒ nothing migrated
    const QString root = storeProjectRoot();
    if (root.isEmpty()) return out;

    // The dispatch again, because migratedProject() yields only the id and
    // bulletsFor() does not hand it back — § 2.3 prices this second lookup.
    const QString markdown = loadRoadmapMarkdown(/*includeArchive=*/false);
    const auto projectId =
        RoadmapSource::migratedProject(*m_roadmapStore, root, markdown);
    if (!projectId) return out;
    const auto row = m_roadmapStore->readProject(*projectId);
    if (!row || row->legendText.isEmpty()) return out;

    // legendText is the RAW stored text — held that way so ANTS-3761's
    // byte-identity contract does not round-trip through a parse — so the
    // re-keying to status emoji happens in the seam, where a test can reach it.
    return RoadmapSource::legendByEmoji(row->legendText);
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
            // ANTS-3808 INV-2 — this used to construct its own `Kind:` regex,
            // the second bullet grammar under src/. It now asks
            // RoadmapParse::trailerValuesIn(), which is the one grammar.
            // NOT behaviour-preserving, and deliberately so: the local pattern
            // omitted CaseInsensitiveOption, so a hand-edited `kind:`/`KIND:`
            // bullet was silently skipped by the kind filter and now matches it
            // — the widening ANTS-3407 case-folded the anchored labels for.
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
                const QString kind =
                    RoadmapParse::trailerValuesIn(bodyFull).kind.value;
                if (!kind.isEmpty())
                    kindByLine.insert(j, kind);
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
    // ANTS-3793 — unless the dialog already resolved them through its owner
    // wrapper (store or markdown) and put them in `opts`. This helper stays a
    // pure markdown→HTML function for every caller that does not.
    const QVector<BulletRecord> allBullets =
        opts.bullets ? *opts.bullets : parseBullets(sourceText);

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
        // ANTS-1693 — lets RoadmapIndex::rollupCounts bubble a child
        // section's tally into its ancestors (same tree-walk the MCP
        // section_index uses), so a parent chip sums its descendants.
        SectionCounts &operator+=(const SectionCounts &o) {
            done += o.done;
            planned += o.planned;
            inProgress += o.inProgress;
            considered += o.considered;
            visible += o.visible;
            return *this;
        }
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

    // ANTS-1693 — parent (level-2) chips previously showed only their
    // *direct* bullets, so a parent count disagreed with the MCP
    // `roadmap_query` section_index `active_count` for the same slug
    // (which rolls descendants up via RoadmapIndex::rollupCounts). Feed
    // the dialog's per-slug flat tallies through the identical tree-walk
    // so parent chips sum self + descendants. Leaf (h3) sections have no
    // descendants, so their chips are unchanged. Rollup runs on the
    // *filtered* counts, matching the dialog's "chips reflect what's
    // shown" model; in the default (unfiltered) view this reproduces the
    // MCP's rollup exactly. The flat `countsBySection` is kept for the
    // INV-12 section-suppression predicate (a parent with no *direct*
    // visible bullets must still collapse on non-Full presets).
    const QVector<RoadmapIndex::Section> sectionIndex =
        RoadmapIndex::buildIndex(sourceText);
    const QHash<QString, SectionCounts> rolledCounts =
        RoadmapIndex::rollupCounts(sectionIndex, countsBySection);

    // ANTS-1694 — surface duplicate [PROJ-NNNN] IDs the MCP already
    // flags (roadmap_query's rcComputeDuplicateIds). A collision is
    // display-harmless (each card's own data is correct) but makes
    // roadmap_log flip/annotate locators ambiguous, so warn. Detected
    // over ALL bullets — a duplicate is independent of the active
    // status filter — keyed on the same canonical-ID predicate the MCP
    // uses (anchors / hash nonces / hyphen-less legacy IDs can't
    // collide). First-seen order keeps the banner stable across renders.
    QStringList duplicateIds;
    {
        QHash<QString, int> idCounts;
        QStringList firstSeen;
        for (const BulletRecord &rec : allBullets) {
            if (!RoadmapIndex::isCanonicalId(rec.id)) continue;
            if (!idCounts.contains(rec.id)) firstSeen.append(rec.id);
            ++idCounts[rec.id];
        }
        for (const QString &id : firstSeen)
            if (idCounts.value(id) >= 2) duplicateIds.append(id);
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
        // ANTS-3392 — each section's cards are one `<table class="rm-cards">`
        // (one `<tr class="rm-card">` per bullet, four `<td class="rm-col-*">`
        // cells) so state / kind / summary / meta line up in aligned columns
        // instead of fusing inline. Qt's rich-text engine paints cell — not
        // row — backgrounds/borders reliably, so the card background lives on
        // the bare `td` rule (the cards path emits no other table, so a bare
        // `td` selector == card cell) and the left accent lives on the first
        // cell (.rm-col-state). Density scales the cell padding (%18/%19) via
        // this <style> block only, so ANTS-1238 INV-6 (non-<style> HTML byte-
        // identical across tiers) still holds.
        // ANTS-3762 — the column WIDTHS are not here, deliberately. Measured
        // 2026-08-15 against Qt's rich-text engine: it ignores both
        // `table-layout:fixed` and a CSS `width` on a `td`. Two tables given
        // identical CSS widths still auto-sized from their own content, one
        // putting its summary column at x=285 and the other at x=75 — which is
        // exactly the misalignment being fixed, so a CSS fix here would have
        // looked right in the source and changed nothing on screen.
        // `applyCardColumnGrid()` sets QTextTableFormat column constraints
        // after the HTML is parsed instead. That also keeps ANTS-1238 INV-6
        // safe by construction rather than by care: no per-tier width ever
        // reaches the HTML.
        "table.rm-cards{border-collapse:collapse;margin:%17px 0;width:100%;}"
        "td{border:none;padding:%18px %19px;vertical-align:top;background:%3;}"
        ".rm-col-state{border-left:3px solid %5;white-space:nowrap;}"
        ".rm-col-kind{white-space:nowrap;}"
        ".rm-col-meta{text-align:right;white-space:nowrap;}"
        // Expanded body row's colspan cell: the <p>s carry their own
        // rm-body-first/line spacing, so drop the cell's top padding.
        ".rm-col-body{padding-top:0;}"
        // rm-current: tint every cell of the row (class beats the bare `td`
        // background by specificity) + swap the first cell's accent to the
        // current-work colour. rm-card-synthetic: dashed first-cell border
        // (ANTS-1428 INV-10 — GFM bullets with a content-hash ID).
        ".rm-cur{background:rgba(229,194,74,0.08);}"
        ".rm-col-cur{border-left-color:%4;}"
        ".rm-col-syn{border-left-style:dashed;}"
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
            // ANTS-3392 — Qt's rich-text engine is unreliable about
            // `padding-right` on an inline <span>, so the count chip
            // fused into the section title ("7 plannedPlanned Features").
            // A hard non-breaking-space pair renders regardless of CSS
            // padding support; the `.rm-section-counts` padding stays as
            // a belt-and-braces on Qt versions that do honour it.
            html += QStringLiteral("&#160;&#160;");
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
        // ANTS-3392 — each card is a `<tr class="rm-card">` of four
        // `<td class="rm-col-*">` cells inside the section's
        // `<table class="rm-cards">`. rm-current / rm-card-synthetic
        // (ANTS-1428 INV-10 — GFM bullets with a content-hash ID) go on
        // the <tr> for semantics + the INV-1 grep, AND on the cells for
        // the accent/tint Qt paints reliably (cell-level, not row-level).
        const bool current = isCurrent(rec.body);
        QString rowClasses;
        if (current)       rowClasses += QStringLiteral(" rm-current");
        if (rec.synthetic) rowClasses += QStringLiteral(" rm-card-synthetic");
        const QString curCell =
            current ? QStringLiteral(" rm-cur") : QString();
        QString stateAccent;
        if (current)       stateAccent += QStringLiteral(" rm-col-cur");
        if (rec.synthetic) stateAccent += QStringLiteral(" rm-col-syn");
        // ANTS-1154-INV-1
        html += QStringLiteral("<tr class=\"rm-card%1\" id=\"rm-%2\">")
                    .arg(rowClasses, htmlEscape(rec.id));

        // Col 1 (state) — status emoji + ANTS-1235 screen-reader label.
        // The label is empty for non-status bullets (statusAccessibleLabel
        // returns empty); skip the span rather than emit an empty one.
        html += QStringLiteral("<td class=\"rm-col-state%1%2\">")
                    .arg(curCell, stateAccent);
        // ANTS-2119 (roadmapdialog L-2) — escape for consistency with the
        // other emits (rec.status is one of four hardcoded emoji today, but
        // every sibling field defensively escapes; do the same so a future
        // parser change can't turn this into a CWE-79 vector).
        html += QStringLiteral("<span class=\"rm-state\">%1</span>")
                    .arg(htmlEscape(rec.status));
        // ANTS-3793 § 2.3 — a store-served project shows its OWN legend, and
        // shows none when it has stored none. Every markdown-served project
        // keeps the compile-time table, which is most of them during the
        // rollout and is why this is a branch and not a replacement.
        const QString stateLabel = opts.legendFromStore
                                       ? opts.legend.value(rec.status)
                                       : statusAccessibleLabel(rec.status);
        if (!stateLabel.isEmpty()) {
            html += QStringLiteral("<span class=\"rm-state-label\">%1</span>")
                        .arg(htmlEscape(stateLabel));
        }
        html += QStringLiteral("</td>");

        // Col 2 (kind) — Kind chip; empty cell when there's no Kind: line.
        html += QStringLiteral("<td class=\"rm-col-kind%1\">").arg(curCell);
        if (!rec.kind.isEmpty()) {
            const QString glyph = kindGlyph(rec.kind);
            // CWE-79: rec.kind is user-supplied ROADMAP.md text (rxKind
            // [^\.\n]+? admits <, >, &, "); escape before emitting.
            html += QStringLiteral("<span class=\"rm-kind\">%1 %2</span>")
                        .arg(glyph, htmlEscape(rec.kind));
        }
        html += QStringLiteral("</td>");

        // Col 3 (summary) — layman if set, else headline with a leading
        // <prefix>-NNNN — token stripped (the ID lives in the meta cell).
        // ANTS-1660 — match any project-ID prefix, not just ANTS-.
        QString summary = rec.layman;
        if (summary.isEmpty()) {
            summary = rec.headline;
            static const QRegularExpression rxLeadId(
                // ANTS-3492 — digit-led-but-letter-containing prefix.
                QStringLiteral("^(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9]"
                               "[A-Za-z0-9_-]*-\\d+\\s*[—-]\\s*"));
            summary.remove(rxLeadId);
        }
        html += QStringLiteral("<td class=\"rm-col-summary%1\">"
                               "<span class=\"rm-summary\">%2</span></td>")
                    .arg(curCell, htmlEscape(summary));

        // Col 4 (meta, right-aligned) — #id + optional date + toggle.
        // ANTS-1241 — inline spans, NOT a `<div class="rm-meta">` wrapper
        // (which painted a QPalette::Base band on dark themes). ANTS
        // abbreviates to #NNNN; foreign prefixes show in full (#VEST-0042).
        html += QStringLiteral("<td class=\"rm-col-meta%1\">").arg(curCell);
        const QString hashedId = rec.id.startsWith(QStringLiteral("ANTS-"))
            ? QStringLiteral("#") + rec.id.mid(5)
            : QStringLiteral("#") + rec.id;
        html += QStringLiteral("<span class=\"rm-id\">%1</span>")
                    .arg(htmlEscape(hashedId));
        if (rec.status == QStringLiteral("✅")) {
            const QString date = opts.shippedDates.value(rec.id);
            if (!date.isEmpty()) {
                // ANTS-2119 (roadmapdialog L-2) — escape for consistency
                // (date is a \d{4}-\d{2}-\d{2} capture today, but the sibling
                // emits all escape defensively).
                html += QStringLiteral(
                    "<span class=\"rm-date\">· %1</span>").arg(htmlEscape(date));
            }
        } else if (rec.status == QStringLiteral("🚧")) {
            // ANTS-1237 — "Updated Nd ago" on 🚧 cards; only when git
            // last-touch data is present (graceful on non-git checkouts).
            const auto it = opts.lastTouchDates.constFind(rec.id);
            if (it != opts.lastTouchDates.constEnd()) {
                const qint64 age =
                    QDateTime::currentSecsSinceEpoch() - *it;
                html += QStringLiteral("<span class=\"rm-date\">· %1</span>")
                    .arg(tr("Updated %1").arg(humanAge(age)));
            }
        }
        html += QStringLiteral(
            "<a class=\"rm-toggle\" href=\"ants://%1/%2\">[%3]</a>")
            .arg(verb, htmlEscape(rec.id), chevron);
        html += QStringLiteral("</td></tr>");

        // Expanded body — a full-width row beneath the summary row.
        // ANTS-1240 — body <p>s are direct children of the colspan cell
        // (no `<div class="rm-body">` wrapper, which painted a nested
        // QPalette::Base frame over the card bg). rm-body-first carries the
        // dotted divider + top padding; rm-body-line is indent only.
        if (expanded) {
            html += QStringLiteral(
                "<tr class=\"rm-card-body\"><td colspan=\"4\" "
                "class=\"rm-col-body%1\">").arg(curCell);
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
            html += QStringLiteral("</td></tr>");
        }
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

    // ANTS-1694 — duplicate-ID warning banner leads the body so it is
    // seen before any card. Inline style (not the shared stylesheet
    // block above) so we don't have to renumber its density-tier %N
    // placeholder chain. Reuses the accent border + secondary
    // background for theme consistency; the ⚠ glyph carries the warning
    // semantics without needing a bespoke error colour.
    if (!duplicateIds.isEmpty()) {
        html += QStringLiteral(
            "<div style=\"margin:8px 0;padding:6px 10px;"
            "border-left:3px solid %1;background:%2;color:%3;"
            "font-weight:bold;\">⚠ Duplicate roadmap IDs: %4</div>")
            .arg(currentColor,
                 th.bgSecondary.name(),
                 th.textPrimary.name(),
                 htmlEscape(duplicateIds.join(QStringLiteral(", "))));
    }

    // ANTS-1662 — bullets that appear before the first ##/### heading have an
    // empty section slug, so the heading-driven walk below never emits them and
    // they vanish from the cards view (v1 renderHtml shows them). Emit the
    // unsectioned bucket here at the top of the body, preserving the INV-16
    // superset contract. No section header — these bullets have none.
    {
        const QVector<const BulletRecord *> &unsectioned =
            bySection.value(QString());
        if (!unsectioned.isEmpty()) {
            html += QStringLiteral("<table class=\"rm-cards\">");
            for (const BulletRecord *rec : unsectioned)
                emitCard(*rec);
            html += QStringLiteral("</table>");
        }
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
            const SectionCounts flat = countsBySection.value(slug);
            // ANTS-1693 — chips show the rolled-up (self + descendants)
            // tally to match the MCP; suppression keys on the *direct*
            // count so an empty parent still collapses on non-Full.
            const SectionCounts rolled = rolledCounts.value(slug);
            // INV-12: on non-Full, suppress sections with 0 visible bullets.
            sectionVisible = (opts.activePreset == Preset::Full) || flat.visible > 0;
            if (sectionVisible) {
                emitSectionHeader(level, hText, slug, rolled,
                                  level == 3 ? currentH2Text : QString());
                // If expanded, emit its bullets as cards.
                if (sectionExpanded) {
                    const QVector<const BulletRecord *> &bullets = bySection.value(slug);
                    if (!bullets.isEmpty()) {
                        html += QStringLiteral("<table class=\"rm-cards\">");
                        for (const BulletRecord *rec : bullets) {
                            emitCard(*rec);
                        }
                        html += QStringLiteral("</table>");
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
        // ANTS-1275 — preamble prose before the first "## " section has no
        // owning section yet (currentSlug empty), so sectionExpanded is
        // still false at this point; gating it on sectionExpanded silently
        // dropped the document intro on the Full preset (R23 / §4.1 require
        // it shown). Apply the collapse gate only to *section-intro* prose
        // (inside a section); the top-of-document preamble always shows.
        if (!currentSlug.isEmpty() && (!sectionVisible || !sectionExpanded))
            continue;
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
        QStringLiteral("\\b(") + RoadmapParse::idTokenPattern() + QStringLiteral(")\\b"));
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

// ANTS-4414 — the blame ARGUMENTS, in one place so the synchronous helper below
// and the dialog's asynchronous path cannot drift into blaming different things.
QStringList RoadmapDialog::lastTouchBlameArgs(const QString &fileName) {
    // --line-porcelain repeats header fields (incl. author-time) for every
    // source line — easy to index by line number. QProcess pipes already
    // suppress git's stderr progress, so no --no-progress needed.
    //
    // Deliberately NOT -L-restricted to the 🚧 blocks. Measured 2026-08-17 on
    // this project: whole-file 3.71 s, four -L ranges 3.12 s — 16%. The cost is
    // history traversal, not line count, so restricting the range buys almost
    // nothing and costs the ability to blame one file in one call.
    return {QStringLiteral("blame"), QStringLiteral("--line-porcelain"),
            QStringLiteral("--"), fileName};
}

// ANTS-4414 — the PARSE half, split out of parseLastTouchDates() so the
// synchronous helper (which tests drive directly) and the dialog's async path
// share one implementation. A second copy here would be a copy of the block
// walk, which is the part with the rules in it.
QHash<QString, qint64>
RoadmapDialog::lastTouchFromBlame(const QByteArray &blameOut,
                                  const QString &roadmapPath) {
    QHash<QString, qint64> out;

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
        // ANTS-3492 — digit-led-but-letter-containing prefix.
        QStringLiteral("^- 🚧 \\[((?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9]"
                       "[A-Za-z0-9_-]*-\\d+)\\]"));
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

QHash<QString, qint64>
RoadmapDialog::parseLastTouchDates(const QString &roadmapPath) {
    QHash<QString, qint64> out;
    const QFileInfo fi(roadmapPath);
    if (!fi.exists()) return out;

    QProcess git;
    git.setWorkingDirectory(fi.absolutePath());
    git.start(QStringLiteral("git"), lastTouchBlameArgs(fi.fileName()));
    if (!git.waitForStarted(2000)) return out;
    // ANTS-1661 capped this at 5 s (was 30 s) because it ran on the GUI thread.
    // ANTS-4414 moved the DIALOG off this path entirely — startLastTouchRefresh()
    // is what the dialog calls now, and it blocks on nothing. This synchronous
    // form survives for the tests that drive it directly and for any caller
    // that genuinely wants the answer before returning; the budget stays
    // because such a caller still cannot afford an unbounded blame.
    if (!git.waitForFinished(5000)) {
        git.kill();
        return out;
    }
    if (git.exitStatus() != QProcess::NormalExit
        || git.exitCode() != 0) {
        // Not a git repo, file not tracked, etc. — graceful.
        return out;
    }
    return lastTouchFromBlame(git.readAllStandardOutput(), roadmapPath);
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
    // ANTS-2049 — objectName hook so the e2e harness can resolve this dialog
    // via e2eResolveTarget(findChild) and confirm it opened after an
    // inject-click on the Roadmap button (smoke case 3).
    setObjectName(QStringLiteral("RoadmapDialog"));
    setWindowTitle(tr("Roadmap — %1").arg(QFileInfo(roadmapPath).fileName()));
    // ANTS-1100 spec: 1200x800 default; DialogChrome restores the
    // user's persisted SIZE below (D3) if they have resized us before.
    resize(1200, 800);
    setMinimumSize(720, 480);

    // ANTS-1242 — frameless + custom TitleBar so the dialog's chrome
    // honours the active theme (KWin draws the standard title bar
    // from the system colour scheme and ignores Qt's QPalette). The
    // shared helper installs the bar, wires the close/min/max
    // signals, and gives back a content QWidget to use as the
    // parent for the rest of the ctor's layouts.
    //
    // ANTS-2012 — resizable=true opts into dialogs.md D2–D4 via the
    // shared chrome: a QSizeGrip (D2), re-centering over the parent's
    // current frame on every open (D4), and bare-QSize persistence
    // under the "RoadmapDialog" key (D3, width/height only — position
    // is never persisted). This replaced the hand-rolled
    // save/restore-geometry round-trip, which violated D4 by restoring
    // an absolute window position.
    auto chrome = DialogChrome::install(this, m_themeName,
                                        /*resizable=*/true,
                                        QStringLiteral("RoadmapDialog"));
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

    // ANTS-4415 — the contents pane's own switch, first in the row because it
    // controls the pane immediately below-left of it. Checkable rather than a
    // two-label toggle: the pressed state IS "the pane is showing", so the
    // control cannot disagree with what it controls.
    m_tocToggleBtn = new QToolButton(this);
    m_tocToggleBtn->setObjectName(QStringLiteral("roadmap-toc-toggle-button"));
    m_tocToggleBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_tocToggleBtn->setText(tr("Contents"));
    m_tocToggleBtn->setCheckable(true);
    m_tocToggleBtn->setFocusPolicy(Qt::StrongFocus);
    m_tocToggleBtn->setAccessibleName(tr("Show or hide the contents pane"));
    m_tocToggleBtn->setToolTip(tr("Show or hide the table of contents"));
    filterRow->addWidget(m_tocToggleBtn.data());

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
    // ANTS-4412 — the five status boxes go into a popup instead of edge to
    // edge. They are the SAME widgets, re-parented: every connect() above and
    // below, every objectName, every accessibleName and the whole persistence
    // path are untouched, and `findChild<QCheckBox*>("roadmap-filter-done")`
    // still resolves because a QMenu owned by the dialog is in its object
    // tree. That is the whole reason this is a re-parent and not a rewrite —
    // the busy row was chrome, and only chrome should change.
    auto *statusMenu = new QMenu(this);
    statusMenu->setObjectName(QStringLiteral("roadmap-filter-status-menu"));
    const auto addToMenu = [](QMenu *menu, QCheckBox *cb) {
        auto *wa = new QWidgetAction(menu);
        wa->setDefaultWidget(cb);
        menu->addAction(wa);
    };
    addToMenu(statusMenu, m_filterDone);
    addToMenu(statusMenu, m_filterPlanned);
    addToMenu(statusMenu, m_filterInProgress);
    addToMenu(statusMenu, m_filterConsidered);
    addToMenu(statusMenu, m_filterCurrent);

    m_statusFilterBtn = new QToolButton(this);
    m_statusFilterBtn->setObjectName(QStringLiteral("roadmap-filter-status-button"));
    m_statusFilterBtn->setPopupMode(QToolButton::InstantPopup);
    m_statusFilterBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_statusFilterBtn->setMenu(statusMenu);
    // Keyboard parity with the checkbox row it replaces: the button takes tab
    // focus, Space/Enter/Down opens the menu, and the boxes inside are
    // reachable with the arrow keys. A collapse that costs keyboard access
    // would be a regression dressed as tidying.
    m_statusFilterBtn->setFocusPolicy(Qt::StrongFocus);
    m_statusFilterBtn->setAccessibleName(tr("Status filter"));
    filterRow->addWidget(m_statusFilterBtn.data());
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
    // ANTS-4412 — density moves to the SEARCH row's trailing edge. It is a
    // view preference, not a filter, and sitting inside the filter row is
    // what made that row read as an unbounded list of toggles.
    searchRow->addWidget(m_densityCombo.data());

    // ANTS-1106 — Kind-faceted secondary filter. Empty by default
    // (no narrowing). Each checkbox toggles a Kind value in
    // m_kindFilter and triggers a re-render. Emoji prefixes are
    // visual cues — `audit-fix` ought to pop visually vs
    // `implement` so the user can distinguish at a glance which
    // categories of work are queued.
    // ANTS-4412 — eleven always-visible boxes become one summarising button,
    // by the same re-parent as the status set above. `kindLabel` is kept and
    // hidden rather than deleted: its objectName is a documented handle and
    // the button carries the label's text now.
    auto *kindMenu = new QMenu(this);
    kindMenu->setObjectName(QStringLiteral("roadmap-filter-kind-menu"));
    auto *kindLabel = new QLabel(tr("Kind:"), this);
    kindLabel->setObjectName(QStringLiteral("roadmap-filter-kind-label"));
    kindLabel->hide();
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
        addToMenu(kindMenu, cb);
    }

    m_kindFilterBtn = new QToolButton(this);
    m_kindFilterBtn->setObjectName(QStringLiteral("roadmap-filter-kind-button"));
    m_kindFilterBtn->setPopupMode(QToolButton::InstantPopup);
    m_kindFilterBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_kindFilterBtn->setMenu(kindMenu);
    m_kindFilterBtn->setFocusPolicy(Qt::StrongFocus);
    m_kindFilterBtn->setAccessibleName(tr("Kind filter"));
    filterRow->addWidget(m_kindFilterBtn.data());

    // ANTS-4412 — the affordance the collapse OWES. Two summarising buttons
    // can hide why a list looks short in a way sixteen visible checkboxes
    // never could, so a one-click restore ships in the same pass rather than
    // as a follow-up. Disabled while nothing is filtered, which doubles as
    // the at-a-glance "no filters active" signal.
    m_resetFiltersBtn = new QToolButton(this);
    m_resetFiltersBtn->setObjectName(QStringLiteral("roadmap-filter-reset-button"));
    m_resetFiltersBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_resetFiltersBtn->setText(tr("Reset filters"));
    m_resetFiltersBtn->setFocusPolicy(Qt::StrongFocus);
    m_resetFiltersBtn->setAccessibleName(tr("Clear all roadmap filters"));
    m_resetFiltersBtn->setToolTip(
        tr("Show every status and every kind again"));
    connect(m_resetFiltersBtn.data(), &QToolButton::clicked, this, [this] {
        // Drive the checkboxes rather than the model: each one's own toggled
        // handler owns the filter set, the config write and the rebuild, and
        // reaching past them is how a reset and a click stop agreeing.
        for (QCheckBox *cb : {m_filterDone.data(), m_filterPlanned.data(),
                              m_filterInProgress.data(),
                              m_filterConsidered.data(),
                              m_filterCurrent.data()})
            if (cb) cb->setChecked(true);
        for (QCheckBox *cb : std::as_const(m_kindCheckboxes))
            if (cb) cb->setChecked(false);   // empty kind set = show all
        if (m_searchBox) m_searchBox->clear();
        updateFilterSummaries();
    });
    filterRow->addWidget(m_resetFiltersBtn.data());
    filterRow->addStretch(1);
    root->addLayout(filterRow);

    // Keep the summaries honest on every toggle, whichever control moved.
    for (QCheckBox *cb : {m_filterDone.data(), m_filterPlanned.data(),
                          m_filterInProgress.data(), m_filterConsidered.data(),
                          m_filterCurrent.data()})
        if (cb) connect(cb, &QCheckBox::toggled, this,
                        [this] { updateFilterSummaries(); });
    for (QCheckBox *cb : std::as_const(m_kindCheckboxes))
        if (cb) connect(cb, &QCheckBox::toggled, this,
                        [this] { updateFilterSummaries(); });
    updateFilterSummaries();

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

    // ANTS-4415 — wire the toggle now the pane it hides exists. setCollapsible
    // as well as the explicit hide: without it the splitter refuses to drag the
    // pane below m_toc's 180px minimum, so the handle and the button would
    // disagree about whether the pane can go away.
    splitter->setCollapsible(0, true);
    m_tocVisible = m_config ? m_config->roadmapTocVisible() : true;
    m_toc->setVisible(m_tocVisible);
    m_tocToggleBtn->setChecked(m_tocVisible);
    connect(m_tocToggleBtn.data(), &QToolButton::toggled, this,
            [this](bool on) {
        m_tocVisible = on;
        if (m_toc) m_toc->setVisible(on);
        if (m_config) m_config->setRoadmapTocVisible(on);
        // Showing it again has to fill it: rebuild() skips the TOC walk while
        // hidden, so the pane's contents are whatever the last visible render
        // left — stale, or empty if it started hidden.
        //
        // Dropping the cached HTML is what makes that re-render actually run.
        // rebuild() returns early when the HTML it just built equals the last
        // one, and that guard sits BEFORE the TOC block — so with the viewer
        // unchanged (which is the normal case here, since showing a pane
        // changes no card) the re-render would skip the very work it was
        // scheduled for and the pane would come back empty. Caught by
        // roadmap_toc_toggle's HiddenPaneCostsNothingAndRefillsOnReturn.
        if (on) {
            if (m_lastHtml) m_lastHtml->clear();
            scheduleRebuild();
        }
    });

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

    // ANTS-1350 (lane-6 L-1) — explicit, deterministic keyboard tab order.
    // Without this the order is creation-order-incidental and shuffles silently
    // whenever a widget is inserted. Chain the focusable controls in reading
    // order; the Kind checkboxes follow the file-scope kKinds table so their
    // order is stable (not QHash-iteration order). Buttons come last.
    {
        QList<QWidget *> tabChain;
        if (m_tabs)             tabChain << m_tabs.data();
        if (m_searchBox)        tabChain << m_searchBox.data();
        if (m_filterDone)       tabChain << m_filterDone.data();
        if (m_filterPlanned)    tabChain << m_filterPlanned.data();
        if (m_filterInProgress) tabChain << m_filterInProgress.data();
        if (m_filterConsidered) tabChain << m_filterConsidered.data();
        if (m_filterCurrent)    tabChain << m_filterCurrent.data();
        if (m_densityCombo)     tabChain << m_densityCombo.data();
        for (const KindEntry &k : kKinds) {
            if (QCheckBox *cb = m_kindCheckboxes.value(QString::fromLatin1(k.value)))
                tabChain << cb;
        }
        if (m_toc)    tabChain << m_toc.data();
        if (m_viewer) tabChain << m_viewer.data();
        tabChain << refreshBtn << resetBtn << closeBtn;
        for (int i = 1; i < tabChain.size(); ++i)
            QWidget::setTabOrder(tabChain[i - 1], tabChain[i]);
    }

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
    // ANTS-4412 — the reset button reflects search too, and NOT through the
    // debounce: the button says whether the list is narrowed, and lagging
    // that by 120 ms would make it briefly lie about the state on screen.
    connect(m_searchBox, &QLineEdit::textChanged, this,
            [this]() { updateFilterSummaries(); });

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

    // Size restore (D3) is owned by DialogChrome::install's
    // "RoadmapDialog" sizeKey — see the ctor head. No hand-rolled
    // geometry round-trip here (ANTS-2012).

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

// ANTS-4414 — was `= default`, and had to stop being.
//
// The last-touch blame is a QProcess parented to this dialog, so ~QWidget's
// deleteChildren() destroys it. ~QProcess on a RUNNING process kills it and
// waits — and that wait pumps the finished signal, which reaches a lambda that
// calls scheduleRebuild() on a dialog whose derived half has already been
// destructed. Measured as heap corruption inside deleteChildren(), not as a
// clean null deref, which is what makes it worth this comment: closing the
// dialog while a blame was in flight aborted the process.
//
// Tear it down here, where `this` is still whole: disconnect first so nothing
// can call back, then kill.
RoadmapDialog::~RoadmapDialog() {
    if (m_lastTouchProc) {
        QProcess *git = m_lastTouchProc;
        m_lastTouchProc = nullptr;
        git->disconnect(this);
        git->kill();
        // Reap it, so the child does not outlive us as a zombie. Short budget:
        // the process has already been signalled and this runs on close.
        git->waitForFinished(2000);
    }
}

void RoadmapDialog::closeEvent(QCloseEvent *event) {
    // Size is persisted by DialogChrome on close (D3, "RoadmapDialog"
    // sizeKey); ANTS-2012 dropped the hand-rolled geometry blob, which
    // also stored window position (D4 violation).
    if (m_config) {
        // ANTS-1154 — persist card / section / table state on close.
        m_config->setRoadmapExpandedItems(
            QStringList(m_expandedItems.begin(), m_expandedItems.end()));
        m_config->setRoadmapExpandedSections(
            QStringList(m_expandedSections.begin(), m_expandedSections.end()));
        m_config->setRoadmapTableSections(
            QStringList(m_tableSections.begin(), m_tableSections.end()));
        // ANTS-1154 §4.5 / INV-13 — remember where the user scrolled to.
        captureScrollAnchor();
    }
    QDialog::closeEvent(event);
}

// ANTS-1264 — first-show restore. Deferred one event-loop turn (singleShot
// 0) so the QTextBrowser has been laid out against its real viewport size;
// restoring against a still-zero-height scrollbar in showEvent itself would
// clamp every target to 0. `this` as the timer context auto-cancels the
// callback if the dialog is destroyed before it fires.
void RoadmapDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    if (m_scrollRestored) return;
    m_scrollRestored = true;
    QTimer::singleShot(0, this, [this] { restoreScrollAnchor(); });
}

namespace {

// ANTS-1264 — scan the rendered roadmap document for card anchors
// (`rm-<id>`, emitted by renderCardsHtml as `<div id="rm-...">`),
// returning each card's id and its pixel top in document coordinates (the
// vertical-scrollbar value that puts that card at the viewport top).
// Section headers carry positional `roadmap-toc-N` anchors which are not
// edit-stable, so only the id-keyed card anchors are collected — the
// section fallback is resolved via these same card anchors (the first
// rendered card of a section), not the header.
struct RenderedCard {
    QString id;
    int top;
};
QVector<RenderedCard> scanRenderedCards(const QTextDocument *doc) {
    QVector<RenderedCard> cards;
    if (!doc) return cards;
    QAbstractTextDocumentLayout *layout = doc->documentLayout();
    if (!layout) return cards;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid()) continue;
            for (const QString &name : f.charFormat().anchorNames()) {
                if (!name.startsWith(QLatin1String("rm-"))) continue;
                cards.push_back(
                    {name.mid(3), qRound(layout->blockBoundingRect(b).top())});
            }
        }
    }
    return cards;
}

}  // namespace

// ANTS-1264 — pure INV-13 resolver: card → section → top.
RoadmapDialog::ScrollTarget
RoadmapDialog::resolveScrollAnchor(const ScrollAnchor &saved,
                                   const QSet<QString> &presentIds,
                                   const QSet<QString> &presentSlugs) {
    ScrollTarget t;
    if (!saved.id.isEmpty() && presentIds.contains(saved.id)) {
        t.kind = ScrollTarget::Card;
        t.id = saved.id;
        t.offsetPx = saved.offsetPx;
    } else if (!saved.sectionSlug.isEmpty()
               && presentSlugs.contains(saved.sectionSlug)) {
        t.kind = ScrollTarget::Section;
        t.sectionSlug = saved.sectionSlug;
    }  // else: default Top.
    return t;
}

void RoadmapDialog::captureScrollAnchor() {
    if (!m_config || !m_viewer) return;
    auto *vbar = m_viewer->verticalScrollBar();
    if (!vbar) return;
    const int scrollY = vbar->value();

    // Topmost card sitting at or above the viewport top.
    const QVector<RenderedCard> cards = scanRenderedCards(m_viewer->document());
    QString topId;
    int topCardTop = -1;
    for (const RenderedCard &c : cards) {
        if (c.top <= scrollY && c.top > topCardTop) {
            topCardTop = c.top;
            topId = c.id;
        }
    }

    QJsonObject anchors = m_config->roadmapScrollAnchors();
    QString key = m_config->roadmapActivePreset();
    if (key.isEmpty()) key = QStringLiteral("full");

    if (topId.isEmpty()) {
        // Scrolled above the first card (header region) — drop any saved
        // anchor for this tab so the next open lands at the top.
        anchors.remove(key);
    } else {
        // ID → section slug (the edit-stable fallback key) via the parsed
        // bullets. One parse on close is cheap relative to the user action.
        QString slug;
        // ANTS-3793 — through the owner wrapper, like the render. INV-2 keeps
        // sectionSlug — the anchor's fallback key — identical either way.
        const bool includeArchive = wantsHistoryLoad();
        const QVector<BulletRecord> recs =
            roadmapBullets(loadRoadmapMarkdown(includeArchive), includeArchive);
        for (const BulletRecord &r : recs) {
            if (r.id == topId) {
                slug = r.sectionSlug;
                break;
            }
        }
        QJsonObject a;
        a.insert(QStringLiteral("slug"), slug);
        a.insert(QStringLiteral("id"), topId);
        a.insert(QStringLiteral("offset"), scrollY - topCardTop);
        anchors.insert(key, a);
    }
    m_config->setRoadmapScrollAnchors(anchors);
}

void RoadmapDialog::restoreScrollAnchor() {
    if (!m_config || !m_viewer) return;
    QString key = m_config->roadmapActivePreset();
    if (key.isEmpty()) key = QStringLiteral("full");
    const QJsonObject a = m_config->roadmapScrollAnchors().value(key).toObject();
    if (a.isEmpty()) return;

    ScrollAnchor saved;
    saved.sectionSlug = a.value(QStringLiteral("slug")).toString();
    saved.id = a.value(QStringLiteral("id")).toString();
    saved.offsetPx = a.value(QStringLiteral("offset")).toInt();

    // Present id set + per-card pixel tops from the rendered document; the
    // section-slug set is the slugs of those rendered cards (a section is
    // reachable only if at least one of its cards is on screen).
    const QVector<RenderedCard> cards = scanRenderedCards(m_viewer->document());
    if (cards.isEmpty()) return;
    QSet<QString> presentIds;
    QHash<QString, int> topById;
    for (const RenderedCard &c : cards) {
        presentIds.insert(c.id);
        topById.insert(c.id, c.top);
    }
    QHash<QString, QString> slugById;
    QSet<QString> presentSlugs;
    // ANTS-3793 — same swap as captureScrollAnchor's.
    const bool includeArchive = wantsHistoryLoad();
    const QVector<BulletRecord> recs =
        roadmapBullets(loadRoadmapMarkdown(includeArchive), includeArchive);
    for (const BulletRecord &r : recs) {
        if (!presentIds.contains(r.id)) continue;
        slugById.insert(r.id, r.sectionSlug);
        presentSlugs.insert(r.sectionSlug);
    }

    const ScrollTarget t = resolveScrollAnchor(saved, presentIds, presentSlugs);
    auto *vbar = m_viewer->verticalScrollBar();
    if (!vbar) return;
    int target = -1;
    if (t.kind == ScrollTarget::Card) {
        target = topById.value(t.id, -1);
        if (target >= 0) target += t.offsetPx;
    } else if (t.kind == ScrollTarget::Section) {
        // First rendered card of the surviving section (smallest top).
        for (const RenderedCard &c : cards) {
            if (slugById.value(c.id) != t.sectionSlug) continue;
            if (target < 0 || c.top < target) target = c.top;
        }
    }
    if (target >= 0) vbar->setValue(qBound(0, target, vbar->maximum()));
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
bool RoadmapDialog::isValidAnchorTarget(const QString &target) {
    // ANTS-1276 — accept only the shape the dialog's own hrefs emit: a
    // roadmap item ID (e.g. ANTS-1145, MAME_CURATOR-7) or a section
    // slug (e.g. performance-2), both drawn from [A-Za-z0-9_-]. Bound
    // the length so a pathological-but-charset-clean target can't bloat
    // the persisted config either.
    if (target.isEmpty() || target.size() > 200) return false;
    for (const QChar ch : target) {
        const char16_t u = ch.unicode();
        const bool ok = (u >= u'A' && u <= u'Z')
                     || (u >= u'a' && u <= u'z')
                     || (u >= u'0' && u <= u'9')
                     || u == u'-' || u == u'_';
        if (!ok) return false;
    }
    return true;
}

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

    // ANTS-1276 — validate the target before it lands in the
    // expanded-state sets, which serialise verbatim to Config on disk.
    // The hrefs this dialog renders only ever carry a roadmap item ID
    // or a section slug, both drawn from [A-Za-z0-9_-]. A hostile
    // ROADMAP.md shipping `ants://expand/' OR 1=1 --` (htmlEscape only
    // strips & < >) would otherwise persist arbitrary attacker strings
    // into the user's config. Reject anything outside that charset or
    // implausibly long.
    if (!isValidAnchorTarget(target)) return;

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

// ANTS-4414 — starts the blame and returns immediately.
//
// This used to BE the blame: parseLastTouchDates() ran `git blame` over the
// whole roadmap synchronously, from rebuild(), on the GUI thread. Measured
// 2026-08-17 on this project it took 3.71 s of a 3.79 s open, and it ran on
// every open rather than once, because mainwindow.cpp constructs the dialog
// with WA_DeleteOnClose — so the mtime guard below was born stale every time
// and never survived a close.
//
// The dates decorate 🚧 cards only (4 of 2,031 items here), so there is nothing
// to wait for: the dialog renders without them and re-renders when they land.
void RoadmapDialog::refreshLastTouchDatesIfStale() {
    if (m_roadmapPath.isEmpty()) return;
    const qint64 mtime = QFileInfo(m_roadmapPath).lastModified()
                             .toMSecsSinceEpoch();
    // m_lastTouchRan and not `!m_lastTouchDates.isEmpty()`: an EMPTY result is
    // a real answer here — a roadmap with no 🚧 bullets, or a directory that is
    // not a git repo — and keying the guard on the hash being non-empty means
    // those two cases never satisfy it, so every render starts another blame.
    // The synchronous version had the same hole and it cost one re-parse; async
    // it would be a process storm during a typing burst.
    if (mtime == m_lastTouchDatesMtime && m_lastTouchRan) return;
    // One in flight at a time. rebuild() runs on every filter toggle and every
    // debounced search keystroke, so without this a typing burst would spawn a
    // blame per keystroke — the ANTS-2012 failure, one layer down.
    if (m_lastTouchProc) return;

    const QFileInfo fi(m_roadmapPath);
    if (!fi.exists()) return;

    auto *git = new QProcess(this);
    m_lastTouchProc = git;
    git->setWorkingDirectory(fi.absolutePath());
    // Claim the mtime up front, not on completion. A blame reads the working
    // tree as it is NOW, so the answer belongs to the file this run saw; a
    // later edit bumps the mtime and the next rebuild() starts a fresh run.
    // The public accessor documents itself as the mtime that last TRIGGERED a
    // refresh, which is what this is.
    m_lastTouchDatesMtime = mtime;
    m_lastTouchRan = false;

    connect(git, &QProcess::finished, this,
            [this, git](int code, QProcess::ExitStatus status) {
        m_lastTouchProc = nullptr;
        m_lastTouchRan = true;   // answered, even if the answer is "nothing"
        git->deleteLater();
        if (status != QProcess::NormalExit || code != 0) {
            // Not a git repo, file not tracked — the same graceful empty the
            // synchronous path returns. No re-render: nothing moved.
            return;
        }
        const auto dates = lastTouchFromBlame(git->readAllStandardOutput(),
                                              m_roadmapPath);
        if (dates == m_lastTouchDates) return;   // nothing to repaint
        m_lastTouchDates = dates;
        // Re-render through the debounce rather than calling rebuild()
        // directly, so a blame landing mid-typing coalesces with the keystroke
        // rebuild instead of racing it.
        scheduleRebuild();
    });
    connect(git, &QProcess::errorOccurred, this, [this, git] {
        m_lastTouchProc = nullptr;
        // Also "answered". git missing from PATH will not fix itself mid-
        // session, so retrying on every render would spawn a doomed process
        // per keystroke. A roadmap edit bumps the mtime and re-arms it.
        m_lastTouchRan = true;
        git->deleteLater();
    });
    git->start(QStringLiteral("git"), lastTouchBlameArgs(fi.fileName()));
}

// ANTS-4412 — the honesty half of the collapse. Sixteen visible checkboxes
// told you what was filtered by being visible; two buttons have to say it.
//
// The status set counts DOWN from all-on (its default is everything checked),
// the kind set counts UP from empty (an empty set means no narrowing, per
// ANTS-1106), so "all" is the right word for opposite states and neither
// summary can be derived from the other's shape.
void RoadmapDialog::updateFilterSummaries() {
    int statusOn = 0, statusTotal = 0;
    for (QCheckBox *cb : {m_filterDone.data(), m_filterPlanned.data(),
                          m_filterInProgress.data(), m_filterConsidered.data(),
                          m_filterCurrent.data()}) {
        if (!cb) continue;
        ++statusTotal;
        if (cb->isChecked()) ++statusOn;
    }
    const bool statusAll = statusOn == statusTotal;
    if (m_statusFilterBtn) {
        m_statusFilterBtn->setText(
            statusAll ? tr("Status: all ▾")
                      : tr("Status: %1 of %2 ▾").arg(statusOn).arg(statusTotal));
        m_statusFilterBtn->setToolTip(
            statusAll ? tr("Every status is shown")
                      : tr("%1 of %2 statuses hidden — click to change")
                            .arg(statusTotal - statusOn).arg(statusTotal));
    }

    const int kindOn    = int(m_kindFilter.size());
    const int kindTotal = int(m_kindCheckboxes.size());
    const bool kindAll  = kindOn == 0;   // empty set = no narrowing
    if (m_kindFilterBtn) {
        m_kindFilterBtn->setText(
            kindAll ? tr("Kind: all ▾")
                    : tr("Kind: %1 of %2 ▾").arg(kindOn).arg(kindTotal));
        m_kindFilterBtn->setToolTip(
            kindAll ? tr("Every kind is shown")
                    : tr("Showing only %1 of %2 kinds — click to change")
                          .arg(kindOn).arg(kindTotal));
    }

    // Enabled EXACTLY when something is narrowing the list, so the control
    // doubles as the at-a-glance answer to "why is this list short?". Search
    // counts: it narrows as hard as any checkbox and the reset clears it.
    const bool searching = m_searchBox && !m_searchBox->text().isEmpty();
    if (m_resetFiltersBtn)
        m_resetFiltersBtn->setEnabled(!statusAll || !kindAll || searching);
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

// ANTS-3762 — pin the card table columns to one grid the whole view obeys.
//
// Every section renders its OWN `table.rm-cards`, so Qt's auto-layout sizes
// each table from that section's content: the state, kind and id columns land
// somewhere different in every group, and reading down the list means
// re-finding each field on every row. An absent kind chip is the worst of it —
// the cell collapses to nothing and the headline slides left, which is how a
// single group ends up showing three different text left-edges.
//
// This runs AFTER setHtml on purpose. Measured 2026-08-15 against Qt's
// rich-text engine: it honours neither `table-layout:fixed` nor a CSS `width`
// on a `td`, so the obvious stylesheet fix parses cleanly and changes nothing
// on screen. Column width CONSTRAINTS on the parsed QTextTable are honoured —
// two tables with wildly different content then put every column at an
// identical x. Doing it here also keeps ANTS-1238 INV-6 (non-<style> HTML
// byte-identical across density tiers) safe by construction: the per-tier
// widths never reach the HTML at all.
//
// The summary column is the flexible one; the other three are fixed, so the
// kind column reserves its space whether or not the row has a kind.
void RoadmapDialog::applyCardColumnGrid(QTextDocument *doc, Density density) {
    if (!doc) return;

    const DensityTier &t = kDensityTable[densityToIndex(density)];
    const QVector<QTextLength> cols{
        QTextLength(QTextLength::FixedLength,      t.colStatePx),
        QTextLength(QTextLength::FixedLength,      t.colKindPx),
        QTextLength(QTextLength::PercentageLength, 100),
        QTextLength(QTextLength::FixedLength,      t.colMetaPx),
    };

    // Depth-first over the frame tree; a card table can nest inside the body
    // row of another (`rm-card-body` uses colspan, but a future body could
    // carry its own table, and recursing costs nothing).
    std::function<void(QTextFrame *)> walk = [&](QTextFrame *frame) {
        for (auto it = frame->begin(); !it.atEnd(); ++it) {
            QTextFrame *child = it.currentFrame();
            if (!child) continue;
            if (auto *table = qobject_cast<QTextTable *>(child)) {
                // Only the four-column card tables. A markdown table in an
                // expanded body has its own column count and must keep its
                // own natural layout.
                if (table->columns() == cols.size()) {
                    QTextTableFormat fmt = table->format();
                    fmt.setColumnWidthConstraints(cols);
                    table->setFormat(fmt);
                }
            }
            walk(child);
        }
    };
    walk(doc->rootFrame());
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
    // ANTS-3793 — resolve the records here, once per render, through the owner
    // wrapper. § 2.3's legend follows the same backend.
    opts.bullets = roadmapBullets(markdown, includeArchive);
    opts.legendFromStore = m_lastReadFromStore;
    if (m_lastReadFromStore) opts.legend = storeLegend();
    if (!m_sourceError.isEmpty()) {
        // A refusal is shown, never served from the markdown sitting right
        // here (INV-1). The notice is the dialog's error-presentation path.
        m_viewer->setHtml(QStringLiteral(
            "<div style=\"padding:16px;font-family:sans-serif\">"
            "<b>Could not read this roadmap.</b><br><br>%1</div>")
                              .arg(htmlEscape(m_sourceError)));
        m_lastHtml.reset();
        return;
    }
    const QString html = renderCardsHtml(markdown, filter, signals_, m_themeName,
                                         m_sortOrder, predicate, m_kindFilter,
                                         opts);

    if (m_lastHtml && *m_lastHtml == html) return;  // skip identical re-render

    auto *vbar = m_viewer->verticalScrollBar();
    const int saved = vbar ? vbar->value() : 0;
    const int oldMax = vbar ? vbar->maximum() : 0;
    const bool wasAtBottom = vbar && (oldMax - saved <= 4);

    m_viewer->setHtml(html);
    // ANTS-3762 — must follow setHtml: it constrains the PARSED tables.
    applyCardColumnGrid(m_viewer->document(), m_density);
    if (m_lastHtml) *m_lastHtml = html;

    if (vbar) {
        if (wasAtBottom) vbar->setValue(vbar->maximum());
        else vbar->setValue(qMin(saved, vbar->maximum()));
    }

    // Refresh the TOC sidebar from the same markdown so the
    // anchor indices line up with what renderHtml just emitted.
    // When the active sort reorders sections, the TOC must walk the
    // post-reorder markdown so anchor indices match.
    // ANTS-4415 — skip the whole walk while the pane is hidden. This runs on
    // every render, not just on open: a filter toggle and each debounced search
    // keystroke both land here, and populating a widget nobody can see is work
    // with no observer. The toggle's own handler re-renders when it comes back.
    if (m_toc && m_tocVisible) {
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
