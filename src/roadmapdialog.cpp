#include "roadmapdialog.h"

#include "coloredtabbar.h"     // for ClaudeTabIndicator::color (ToolUse yellow)
#include "config.h"
#include "themes.h"

#include <QByteArray>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
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

// 1..4 if `raw` is a Markdown ATX heading, else 0. On a hit, `text`
// receives the heading content with the `# ` prefix stripped. Walk
// shape is shared between extractToc and renderHtml so the indices
// line up — anchor `roadmap-toc-N` refers to the N-th hit.
int headingLevel(const QString &raw, QString *text) {
    if (raw.startsWith(QStringLiteral("#### "))) {
        if (text) *text = raw.mid(5);
        return 4;
    }
    if (raw.startsWith(QStringLiteral("### "))) {
        if (text) *text = raw.mid(4);
        return 3;
    }
    if (raw.startsWith(QStringLiteral("## "))) {
        if (text) *text = raw.mid(3);
        return 2;
    }
    if (raw.startsWith(QStringLiteral("# "))) {
        if (text) *text = raw.mid(2);
        return 1;
    }
    return 0;
}

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
    static const QRegularExpression rxBold(QStringLiteral("\\*\\*([^*]+)\\*\\*"));
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

QVector<RoadmapDialog::BulletRecord>
RoadmapDialog::parseBullets(const QString &markdownText) {
    QVector<BulletRecord> out;
    static const QRegularExpression rxId(QStringLiteral("\\[ANTS-(\\d+)\\]"));
    static const QRegularExpression rxBold(QStringLiteral("\\*\\*([^*]+)\\*\\*"));
    // MultilineOption so `^` anchors at the start of any line within
    // the bullet body — Kind: / Lanes: live as continuation lines, not
    // at the start of the string.
    static const QRegularExpression rxKind(
        QStringLiteral("^\\s*Kind:\\s*([^\\.\\n]+?)\\s*[\\.\\n]"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression rxLanes(
        QStringLiteral("^\\s*Lanes:\\s*(.+?)\\s*[\\.\\n]"),
        QRegularExpression::MultilineOption);

    const QStringList lines = markdownText.split('\n');
    int i = 0;
    while (i < lines.size()) {
        const QString &raw = lines[i];
        // Top-level bullet: `^- ` or `^* ` (two-space indent is a
        // continuation, not a bullet — same rule as renderHtml).
        const bool isBullet = raw.startsWith(QStringLiteral("- ")) ||
                              raw.startsWith(QStringLiteral("* "));
        if (!isBullet) { ++i; continue; }
        QString head = raw.mid(2);
        // Strip leading status emoji; skip plain-narration bullets.
        QString status;
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
            ++i;
            continue;
        }
        while (!head.isEmpty() && head.front().isSpace()) head.remove(0, 1);

        BulletRecord rec;
        rec.status = status;

        // Collect the bullet body — first line + subsequent indented
        // continuation lines until a blank line or another top-level
        // bullet. Mirrors renderHtml's continuation walk.
        QString body = head;
        ++i;
        while (i < lines.size()) {
            const QString &cont = lines[i];
            if (cont.trimmed().isEmpty()) break;
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
            rec.id = QStringLiteral("ANTS-%1").arg(idMatch.captured(1));
        }
        const auto boldMatch = rxBold.match(body);
        if (boldMatch.hasMatch()) {
            QString h = boldMatch.captured(1).trimmed();
            if (h.size() > 120) { h.truncate(120); h.append(QStringLiteral("…")); }
            rec.headline = h;
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
            ? QStringLiteral("[ANTS-%1]").arg(idShorthand)
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
            const bool keepStatus =
                (kind == BulletKind::Other) ||
                (kind == BulletKind::Done && wantDone) ||
                (kind == BulletKind::Planned && wantPlanned) ||
                (kind == BulletKind::InProgress && wantInProgress) ||
                (kind == BulletKind::Considered && wantConsidered) ||
                (current && wantCurrent);
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

    auto *root = new QVBoxLayout(this);

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
    m_searchBox->setPlaceholderText(
        tr("Substring match (or id:NNNN to jump to a specific ID)"));
    m_searchBox->setClearButtonEnabled(true);
    searchRow->addWidget(searchLabel);
    searchRow->addWidget(m_searchBox, 1);
    root->addLayout(searchRow);

    auto *filterRow = new QHBoxLayout();
    m_filterDone = new QCheckBox(tr("✅ Done"), this);
    m_filterDone->setObjectName(QStringLiteral("roadmap-filter-done"));
    m_filterDone->setChecked(true);
    m_filterPlanned = new QCheckBox(tr("📋 Planned"), this);
    m_filterPlanned->setObjectName(QStringLiteral("roadmap-filter-planned"));
    m_filterPlanned->setChecked(true);
    m_filterInProgress = new QCheckBox(tr("🚧 In progress"), this);
    m_filterInProgress->setObjectName(QStringLiteral("roadmap-filter-in-progress"));
    m_filterInProgress->setChecked(true);
    m_filterConsidered = new QCheckBox(tr("💭 Considered"), this);
    m_filterConsidered->setObjectName(QStringLiteral("roadmap-filter-considered"));
    m_filterConsidered->setChecked(true);
    m_filterCurrent = new QCheckBox(tr("Currently being tackled"), this);
    m_filterCurrent->setObjectName(QStringLiteral("roadmap-filter-current"));
    m_filterCurrent->setChecked(true);
    filterRow->addWidget(m_filterDone);
    filterRow->addWidget(m_filterPlanned);
    filterRow->addWidget(m_filterInProgress);
    filterRow->addWidget(m_filterConsidered);
    filterRow->addWidget(m_filterCurrent);
    filterRow->addStretch(1);
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
    m_toc->setUniformItemSizes(false);
    m_toc->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_toc->setSelectionMode(QAbstractItemView::SingleSelection);
    m_toc->setMinimumWidth(180);
    splitter->addWidget(m_toc);

    m_viewer = new QTextBrowser(splitter);
    m_viewer->setReadOnly(true);
    // Internal anchors only — disable navigation on `<a href>` so a
    // stray markdown link can't replace the document.
    m_viewer->setOpenLinks(false);
    m_viewer->setOpenExternalLinks(false);
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
    }

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

    // (3) Custom-only status restore.
    if (persisted == Preset::Custom && m_config) {
        const QJsonObject sf = m_config->roadmapStatusFilters();
        if (sf.isEmpty()) {
            // Custom + empty mask = empty render dead-end. Fall
            // back to Full instead of leaving every checkbox off.
            // Same UX as today's first-launch behaviour.
            persisted = Preset::Full;
        } else {
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
    }
    QDialog::closeEvent(event);
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
    const QString html = renderHtml(markdown, filter, signals_, m_themeName,
                                    m_sortOrder, predicate, m_kindFilter);

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
