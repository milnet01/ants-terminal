// ANTS-3533: Keep-a-Changelog reader — see changelogquery.h + docs/specs/ANTS-3533.md.

#include "changelogquery.h"
#include "markdownscan.h"

#include "changeloglog.h"  // canonicalCategories() (ANTS-3533 public)

#include <QDate>
#include <QHash>
#include <QRegularExpression>
#include <QMutexLocker>
#include <QMutex>

#include <algorithm>

namespace ChangelogQuery {

namespace {

// A code-fence marker line ( ``` / ~~~, optionally indented, optionally
// with an info string). Returns the marker char, run length, and whether
// an info string follows (CommonMark: a close fence carries no info).
//
// ANTS-4404 — the OPENER test is MarkdownScan's (ANTS-3603), not a local
// one. The hand-rolled version this replaces was wrong three ways: it
// admitted a TAB as fence indent, where CommonMark is space-only and a tab
// never opens a fence (ANTS-3598); it bounded the indent at nothing, where
// the allowance is three spaces; and it ignored § 4.5, which forbids a
// backtick in a BACKTICK fence's info string precisely so that
// ```` ```json ```` — how a document quotes fence syntax — stays a
// paragraph (ANTS-3655). That third fault is the one that hid a quarter of
// the roadmap in ANTS-4403.
//
// Measured 2026-09-09 before changing anything: this project's CHANGELOG.md
// carries NO line that any of the three faults misreads, so the defect here
// was latent, not active. Adopted anyway — the same class has now cost two
// real bugs (ANTS-4403, ANTS-4450), and the shared rule cannot drift.
//
// ANTS-4987 — a closer is judged by MarkdownScan::fenceCloses, which now also
// refuses a closing fence that carries an info string. This parser kept that
// rule locally until the shared predicate implemented it.
struct FenceInfo {
    bool  isFence = false;
    QChar ch;
    int   len = 0;
};

FenceInfo fenceInfoOf(const QString &line) {
    FenceInfo fi;
    int run = 0;
    const QChar c = MarkdownScan::fenceOpenerChar(line, 3, &run);
    if (c.isNull()) return fi;
    fi.isFence = true;
    fi.ch = c;
    fi.len = run;
    return fi;
}

// De-indent a continuation line: strip up to two leading spaces or one tab.
QString deindent(const QString &l) {
    if (l.startsWith(QLatin1String("  "))) return l.mid(2);
    if (l.startsWith(QLatin1Char('\t'))) return l.mid(1);
    return l;
}

// A continuation of an entry body: blank, or indented ≥2 spaces / a tab.
bool isContinuation(const QString &l) {
    if (l.trimmed().isEmpty()) return true;
    return l.startsWith(QLatin1String("  ")) || l.startsWith(QLatin1Char('\t'));
}

// Extract every <prefix>-NNNN token from `text`, document order, deduped.
QStringList extractIds(const QString &text, const QString &idPrefix) {
    QStringList out;
    if (idPrefix.isEmpty()) return out;
    // ANTS-5108 — locked, and the pattern is copied out (QRegularExpression
    // is implicitly shared, so the copy is cheap): one thread reaches this
    // today, and an unlocked QHash would not survive a second.
    static QHash<QString, QRegularExpression> cache;
    static QMutex cacheMutex;
    QRegularExpression rx;
    {
        QMutexLocker lock(&cacheMutex);
        auto it = cache.find(idPrefix);
        if (it == cache.end()) {
            it = cache.insert(
                idPrefix,
                QRegularExpression(QStringLiteral("\\b") +
                                   QRegularExpression::escape(idPrefix) +
                                   QStringLiteral("-\\d+\\b")));
        }
        rx = *it;
    }
    auto m = rx.globalMatch(text);
    while (m.hasNext()) {
        const QString id = m.next().captured(0);
        if (!out.contains(id)) out.append(id);
    }
    return out;
}

// Version heading `## [<ver>]<sep><date>`. Returns false if not a version line.
bool parseVersionHeading(const QString &line, QString &version, QString &date,
                         bool &unreleased) {
    if (!line.startsWith(QLatin1String("## ["))) return false;
    const int close = line.indexOf(QLatin1Char(']'), 4);
    if (close < 0) return false;
    version = line.mid(4, close - 4).trimmed();
    QString rest = line.mid(close + 1).trimmed();
    if (!rest.isEmpty()) {
        const QChar c = rest[0];
        if (c == QChar(0x2014) /* em-dash */ || c == QLatin1Char('-') ||
            c == QLatin1Char(':')) {
            rest = rest.mid(1).trimmed();
        }
    }
    date = rest;
    unreleased = (version.compare(QStringLiteral("Unreleased"), Qt::CaseInsensitive) == 0);
    if (unreleased) version = QStringLiteral("Unreleased");
    return true;
}

// A `### ` category heading (or a bare `###`). `cat` = the trimmed name ("" if none).
bool parseCategoryHeading(const QString &line, QString &cat) {
    if (line.startsWith(QLatin1String("### "))) {
        cat = line.mid(4).trimmed();
        return true;
    }
    if (line.trimmed() == QLatin1String("###")) {
        cat.clear();
        return true;
    }
    return false;
}

// ANTS-5071 — the category a `### ` heading sets, or "" for none. A dated topic
// `<YYYY-MM-DD> <Category> — <headline>` (changelog_log op:add_subsection)
// takes the word after the date. The date is PARSED, as
// ChangelogLog::classifyUnreleased does, so `2026-13-45` is not one.
QString categoryForHeading(const QString &cat) {
    if (ChangelogLog::isValidCategory(cat)) return cat;
    if (cat.size() > 10 && cat.at(10).isSpace() &&
        QDate::fromString(cat.left(10), QStringLiteral("yyyy-MM-dd")).isValid()) {
        const QString word =
            cat.mid(10).simplified().section(QLatin1Char(' '), 0, 0);
        if (ChangelogLog::isValidCategory(word)) return word;
    }
    return QString();
}

// ANTS-5145 — a dated topic's headline: the heading after its date and category
// word, with a single leading `—` or `-` and the whitespace around it stripped.
QString topicHeadline(const QString &cat) {
    QString rest = cat.mid(10).trimmed();
    int wordEnd = 0;
    while (wordEnd < rest.size() && !rest.at(wordEnd).isSpace()) ++wordEnd;
    rest = rest.mid(wordEnd).trimmed();
    if (rest.startsWith(QChar(0x2014)) || rest.startsWith(QLatin1Char('-')))
        rest = rest.mid(1).trimmed();
    return rest;
}

}  // namespace

ParseResult parse(const QString &markdown, const QString &idPrefix) {
    ParseResult result;
    QVector<QHash<QString, int>> counts;  // per-version category counts

    bool inFence = false;
    QChar fenceChar;
    int fenceLen = 0;

    bool haveVersion = false;
    QString curVersion, curDate;
    bool curUnreleased = false;
    QString curCategory;  // "" == none
    int curVersionIdx = -1;

    bool building = false;
    bool topic = false;  // ANTS-5145 — the entry being built is a dated topic
    Entry curEntry;
    QStringList curBody;

    auto finalizeEntry = [&]() {
        if (!building) return;
        // Trim trailing blank continuation lines.
        while (!curBody.isEmpty() && curBody.last().trimmed().isEmpty())
            curBody.removeLast();
        if (topic) {
            // ANTS-5145 — a topic's prose also drops its leading blank lines,
            // and a topic with neither a headline nor prose makes no entry.
            while (!curBody.isEmpty() && curBody.first().trimmed().isEmpty())
                curBody.removeFirst();
            topic = false;
            if (curEntry.text.isEmpty() && curBody.isEmpty()) {
                building = false;
                return;
            }
        }
        curEntry.body = curBody.join(QLatin1Char('\n'));
        curEntry.ids = extractIds(curEntry.text + QLatin1Char('\n') + curEntry.body,
                                  idPrefix);
        result.entries.append(curEntry);
        if (curVersionIdx >= 0) {
            counts[curVersionIdx][curEntry.category]++;
        }
        building = false;
        curBody.clear();
    };

    const QStringList lines = markdown.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        // --- fenced code state ---
        const FenceInfo fi = fenceInfoOf(line);
        if (fi.isFence) {
            if (!inFence) {
                inFence = true;
                fenceChar = fi.ch;
                fenceLen = fi.len;
            } else if (MarkdownScan::fenceCloses(line, fenceChar, fenceLen)) {
                inFence = false;
            }
            if (building) curBody.append(deindent(line));
            continue;
        }
        if (inFence) {
            // A column-0 `## [` is a hard block boundary: it closes any
            // still-open (unterminated) fence and starts a new version.
            if (line.startsWith(QLatin1String("## ["))) {
                inFence = false;
            } else {
                if (building) curBody.append(deindent(line));
                continue;
            }
        }

        // --- version heading ---
        QString v, d;
        bool u = false;
        if (parseVersionHeading(line, v, d, u)) {
            finalizeEntry();
            curVersion = v;
            curDate = d;
            curUnreleased = u;
            haveVersion = true;
            curCategory.clear();
            VersionInfo vi;
            vi.version = v;
            vi.date = d;
            vi.unreleased = u;
            result.versions.append(vi);
            counts.append(QHash<QString, int>{});
            curVersionIdx = result.versions.size() - 1;
            continue;
        }

        // --- category heading ---
        QString cat;
        if (parseCategoryHeading(line, cat)) {
            finalizeEntry();
            curCategory = categoryForHeading(cat);
            // ANTS-5145 — a dated topic heading that sets a category is an
            // entry of its own, ahead of its bullets. A canonical `### Fixed`
            // returns its own text, so it is not a topic.
            if (haveVersion && !curCategory.isEmpty() && curCategory != cat) {
                curEntry = Entry{};
                curEntry.version = curVersion;
                curEntry.date = curDate;
                curEntry.unreleased = curUnreleased;
                curEntry.category = curCategory;
                curEntry.text = topicHeadline(cat);
                building = true;
                topic = true;
                curBody.clear();
            }
            continue;
        }

        // --- entry bullet (column 0) ---
        if (line.startsWith(QLatin1String("- "))) {
            finalizeEntry();
            if (!haveVersion || curCategory.isEmpty()) {
                continue;  // no version context / no category → skip
            }
            curEntry = Entry{};
            curEntry.version = curVersion;
            curEntry.date = curDate;
            curEntry.unreleased = curUnreleased;
            curEntry.category = curCategory;
            curEntry.text = line.mid(2);
            building = true;
            curBody.clear();
            continue;
        }

        // --- continuation / stray ---
        if (building) {
            // ANTS-5145 — a topic's prose is flush-left, so every line up to
            // the next bullet or heading is its body.
            if (topic || isContinuation(line)) {
                curBody.append(deindent(line));
            } else {
                finalizeEntry();  // non-indented, non-structural line ends the entry
            }
        }
    }
    finalizeEntry();

    // Build per-version category rollups in canonical order (zero-count omitted).
    const QStringList &canon = ChangelogLog::canonicalCategories();
    for (int i = 0; i < result.versions.size(); ++i) {
        const QHash<QString, int> &c = counts[i];
        int total = 0;
        for (const QString &cat : canon) {
            const int n = c.value(cat, 0);
            if (n > 0) {
                result.versions[i].categories.append(qMakePair(cat, n));
                total += n;
            }
        }
        result.versions[i].entry_count = total;
    }

    return result;
}

}  // namespace ChangelogQuery

namespace ChangelogQuery {

QList<DocFinding::Finding> lint(const QString &markdown, const QString &relPath,
                                const QString &version) {
    static const QRegularExpression linkRef(QStringLiteral(R"(^ {0,3}\[[^\]]+\]:\s)"));
    static const QRegularExpression listItem(
        QStringLiteral(R"(^ {0,3}(?:[-*+]|[0-9]+[.)])(?:\s|$))"));

    QStringList lines = markdown.split(QLatin1Char('\n'));
    for (QString &l : lines)
        if (l.endsWith(QLatin1Char('\r'))) l.chop(1);
    const QVector<bool> fence = MarkdownScan::fenceMask(lines);
    const QString wanted =
        version.compare(QStringLiteral("Unreleased"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("Unreleased") : version;

    QList<DocFinding::Finding> out;
    const auto add = [&out, &relPath](int line, const QString &kind, const QString &msg) {
        DocFinding::Finding f;
        f.verb          = QStringLiteral("changelog_query");
        f.kind          = kind;
        f.file          = relPath;
        f.line          = line;
        f.message       = msg;
        f.emissionIndex = int(out.size());
        out.append(f);
    };

    bool inSection = false, selected = false, seenCategory = false;
    // Prose above a section's first `###` is reported only once that heading
    // arrives: in a section with no category at all it is a placeholder note
    // ("(Nothing yet.)" in the skeleton's empty [Unreleased]), not a line
    // filed in the wrong place.
    QList<int> pendingProse;
    bool inFlatCategory = false, inComment = false, themeSeen = false;
    int  lastCanon = -1;
    QString lastCanonName;
    for (int i = 0; i < lines.size(); ++i) {
        const QString &l = lines.at(i);
        if (fence.at(i)) continue;  // fenced code is never a finding
        if (l.startsWith(QLatin1String("## "))) {
            QString ver, date;
            bool unreleased = false;
            if (!parseVersionHeading(l, ver, date, unreleased)) ver = l.mid(3).trimmed();
            inSection = true;
            selected = wanted.isEmpty() || ver == wanted;
            seenCategory = inFlatCategory = inComment = themeSeen = false;
            pendingProse.clear();
            lastCanon = -1;
            continue;
        }
        if (!inSection || !selected) continue;  // the preamble is not a section

        const QString t = l.trimmed();
        if (inComment) {
            if (t.contains(QLatin1String("-->"))) inComment = false;
            continue;
        }
        if (t.startsWith(QLatin1String("<!--"))) {
            inComment = !t.contains(QLatin1String("-->"));
            continue;
        }
        if (t.isEmpty() || linkRef.match(l).hasMatch()) continue;

        QString cat;
        if (parseCategoryHeading(l, cat)) {
            if (!seenCategory)
                for (int p : std::as_const(pendingProse))
                    add(p, QStringLiteral("prose_before_category"),
                        QStringLiteral("prose before this section's first ### category heading"));
            pendingProse.clear();
            seenCategory = true;
            const QString c = categoryForHeading(cat);
            if (c.isEmpty()) {
                inFlatCategory = false;
                add(i + 1, QStringLiteral("unknown_category"),
                    QStringLiteral("\"### %1\" is not a Keep-a-Changelog category")
                        .arg(cat.left(64)));
                continue;
            }
            // A dated topic's body is prose by design; only a flat block is
            // bullets alone.
            inFlatCategory = ChangelogLog::isValidCategory(cat);
            if (inFlatCategory) {
                const int idx = ChangelogLog::canonicalCategories().indexOf(c);
                if (idx < lastCanon)
                    add(i + 1, QStringLiteral("category_out_of_order"),
                        QStringLiteral("### %1 comes after ### %2; the order is %3")
                            .arg(c, lastCanonName,
                                 ChangelogLog::canonicalCategories().join(QStringLiteral(", "))));
                else { lastCanon = idx; lastCanonName = c; }
            }
            continue;
        }
        if (l.startsWith(QLatin1Char('#'))) continue;  // a deeper heading

        const bool item = listItem.match(l).hasMatch();
        // changelog-format.md § 2: a released section opens with ONE
        // `**Theme:**` line before its categories. That line, and only it.
        if (!seenCategory && !item && !themeSeen &&
            t.startsWith(QLatin1String("**Theme:**"))) {
            themeSeen = true;
            continue;
        }
        if (!seenCategory) {
            if (item)
                add(i + 1, QStringLiteral("bullet_outside_category"),
                    QStringLiteral("a bullet before this section's first ### category "
                                   "heading is filed under no category"));
            else
                pendingProse << i + 1;
            continue;
        }
        if (inFlatCategory && !item && !l.startsWith(QLatin1Char(' ')) &&
            !l.startsWith(QLatin1Char('\t')))
            add(i + 1, QStringLiteral("prose_in_category"),
                QStringLiteral("a flush-left prose line inside a category block; "
                               "indent it under its bullet or make it a bullet"));
    }
    // Deferred prose lands after later lines; findings are in document order.
    std::stable_sort(out.begin(), out.end(),
                     [](const DocFinding::Finding &x, const DocFinding::Finding &y) {
                         return x.line < y.line;
                     });
    for (int k = 0; k < out.size(); ++k) out[k].emissionIndex = k;
    return out;
}

}  // namespace ChangelogQuery
