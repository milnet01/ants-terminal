// ANTS-3533: Keep-a-Changelog reader — see changelogquery.h + docs/specs/ANTS-3533.md.

#include "changelogquery.h"
#include "markdownscan.h"

#include "changeloglog.h"  // canonicalCategories() (ANTS-3533 public)

#include <QHash>
#include <QRegularExpression>

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
// `hasInfo` stays LOCAL, deliberately. CommonMark § 4.5 also says a CLOSING
// fence carries no info string, and MarkdownScan::fenceCloses does not
// implement that rule — see ANTS-4987. Calling it here would close a block
// on a ```` ```json ```` line, which is looser than what this parser does
// today, so adopting it would be a regression rather than a fix.
struct FenceInfo {
    bool  isFence = false;
    QChar ch;
    int   len = 0;
    bool  hasInfo = false;
};

FenceInfo fenceInfoOf(const QString &line) {
    FenceInfo fi;
    int run = 0;
    const QChar c = MarkdownScan::fenceOpenerChar(line, 3, &run);
    if (c.isNull()) return fi;
    fi.isFence = true;
    fi.ch = c;
    fi.len = run;

    // The info string is whatever follows the fence run. Re-derive the run's
    // end from the same space-only indent rule MarkdownScan applied, so the
    // two cannot disagree about where the info string starts.
    int i = 0;
    while (i < line.size() && line[i] == QLatin1Char(' ')) ++i;
    i += run;
    fi.hasInfo = !line.mid(i).trimmed().isEmpty();
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
    static QHash<QString, QRegularExpression> cache;
    auto it = cache.find(idPrefix);
    if (it == cache.end()) {
        it = cache.insert(
            idPrefix,
            QRegularExpression(QStringLiteral("\\b") +
                               QRegularExpression::escape(idPrefix) +
                               QStringLiteral("-\\d+\\b")));
    }
    auto m = it->globalMatch(text);
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
    Entry curEntry;
    QStringList curBody;

    auto finalizeEntry = [&]() {
        if (!building) return;
        // Trim trailing blank continuation lines.
        while (!curBody.isEmpty() && curBody.last().trimmed().isEmpty())
            curBody.removeLast();
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
            } else if (fi.ch == fenceChar && fi.len >= fenceLen && !fi.hasInfo) {
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
            curCategory = ChangelogLog::isValidCategory(cat) ? cat : QString();
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
            if (isContinuation(line)) {
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
