// ANTS-3533: pure Keep-a-Changelog reader for the changelog_query MCP
// tool — parse CHANGELOG.md into structured {version, date, category,
// text, ids, body} records so drift-checks stop full-reading the log.
// Qt6::Core-only; lives in ants_core_lib so the remotecontrol handler
// and the feature test share one implementation.
// See docs/specs/ANTS-3533.md.

#pragma once

#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ChangelogQuery {

// One changelog entry (a `- ` bullet under a `### <category>` inside a
// `## [<version>]` block). See ANTS-3533 § 2.2 / § 3.
struct Entry {
    QString     version;            // "0.7.100" | "Unreleased"
    QString     date;               // text after ']', separator stripped; "" if none
    bool        unreleased = false; // true iff version == "Unreleased"
    QString     category;           // "Added" | … (canonical)
    QString     text;               // bullet's first line (bold summary), markdown kept
    QStringList ids;                // every <P>-NNNN cited in text+body, doc order
    QString     body;               // continuation lines, de-indented, joined "\n"
};

// A version block skeleton for `version_index` mode (§ 2.2). `categories`
// omits zero-count categories and is in canonical Keep-a-Changelog order;
// `entry_count` == sum of the category counts (uncategorised bullets excluded).
struct VersionInfo {
    QString                     version;
    QString                     date;
    bool                        unreleased = false;
    int                         entry_count = 0;
    QVector<QPair<QString, int>> categories;
};

struct ParseResult {
    QVector<Entry>       entries;   // flat, document order
    QVector<VersionInfo> versions;  // every `## [x]` block, document order
};

// Parse a Keep-a-Changelog markdown body. `idPrefix` is the project
// roadmap prefix P (e.g. "ANTS"); id extraction collects `<P>-NNNN`
// tokens from each entry's text+body (§ 3). An empty prefix yields no ids.
ParseResult parse(const QString &markdown, const QString &idPrefix);

}  // namespace ChangelogQuery
