#pragma once

// source_scrapes (ANTS-5602) — the tests that read a given source file as
// text, and the strings each looks for in it, so a refactor knows which
// tests it breaks without running the suite. Contract:
// tests/features/mcp_source_scrapes/spec.md. Its own translation unit so a
// test links it without RemoteControl (docs/standards/mcp-tools.md § Tests).

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

namespace SourceScrapes {

// What a test file can read: tokens naming files, tokens naming directories
// (a source-shaped literal beside one names a file under it), and helper
// functions whose return reads files. Paths are root-relative.
struct Readers {
    QHash<QString, QStringList> fileTokens;
    QHash<QString, QString> dirTokens;
    QHash<QString, QSet<QString>> helpers;
};

struct Anchor {
    QString test;      // "Suite.Name", or empty outside a TEST block
    int line = 0;
    QString anchor;    // the literal's text as written between its quotes
    bool regex = false;
    QSet<QString> paths;
};

struct Read {
    QString test;
    int line = 0;
    QString via;       // the token, helper or literal that reads
    QSet<QString> paths;
};

struct FileResult {
    QList<Anchor> anchors;
    QList<Read> reads;
    QList<QSet<QString>> unresolved;   // the paths of each anchor that is a variable
};

// Functions in `text` whose return reads a path, added to `r->helpers`. Run
// over the shared headers before analysing any test file.
void collectHelpers(const QString &text, Readers *r);

// Every anchor and direct read in one test file. `existing(rel)` says
// whether a root-relative file exists, for the directory-token rule.
FileResult analyse(const QString &text, const Readers &r,
                   const std::function<bool(const QString &)> &existing);

// The whole call below the resolved project root: the envelope, or a
// refusal {ok:false, code, error}.
QJsonObject run(const QString &rootCanonical, const QJsonObject &args);

} // namespace SourceScrapes
