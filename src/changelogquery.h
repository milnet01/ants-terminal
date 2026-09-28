// ANTS-3533: pure Keep-a-Changelog reader for the changelog_query MCP
// tool — parse CHANGELOG.md into structured {version, date, category,
// text, ids, body} records so drift-checks stop full-reading the log.
// Qt6::Core-only; lives in ants_core_lib so the remotecontrol handler
// and the feature test share one implementation.
// See docs/specs/ANTS-3533.md.

#pragma once

#include "docfinding.h"

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ChangelogQuery {

// One changelog entry inside a `## [<version>]` block: a `- ` bullet under a
// `### <category>`, or a dated topic heading (ANTS-5145). See ANTS-3533 § 2.2 / § 3.
struct Entry {
    QString     version;            // "0.7.100" | "Unreleased"
    QString     date;               // text after ']', separator stripped; "" if none
    bool        unreleased = false; // true iff version == "Unreleased"
    QString     category;           // "Added" | … (canonical)
    QString     text;               // bullet's first line, or a topic's headline; markdown kept
    QStringList ids;                // every <P>-NNNN cited in text+body, doc order
    QString     body;               // continuation lines (a topic's prose), de-indented, joined "\n"
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

// ANTS-5543 — READ-ONLY format check, so a push gate can catch a misplaced
// line without changing the file. One finding per offending line, in document
// order, verb "changelog_query", never autoFixable:
//
//   prose_before_category    prose between a `## ` heading and its first `###`
//                            (a section with no `###` at all may hold a note)
//   bullet_outside_category  a bullet there, filed under no category
//   unknown_category         a `###` that is neither one of the six canonical
//                            categories nor a dated topic
//   category_out_of_order    a category heading that comes before one it
//                            follows in Keep-a-Changelog order
//   prose_in_category        a flush-left prose line inside a flat category
//                            block (the line normalize would fold)
//
// Blank lines, HTML comments, link reference definitions and fenced code are
// never findings, nor is one `**Theme:**` line before a section's first
// category (changelog-format.md § 2). Text above the first `## ` (the title
// and preamble) is not a section. `version` non-empty checks only that
// `## [<version>]` section ("Unreleased" matches case-insensitively).
QList<DocFinding::Finding> lint(const QString &markdown, const QString &relPath,
                                const QString &version = QString());

}  // namespace ChangelogQuery
