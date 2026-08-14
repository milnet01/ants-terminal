// ANTS-3368 — co_change_family: the pure seam behind the MCP verb.
//
// Given one exemplar field stem, find every edit site that belongs to the
// same settings family — including the JSON string key and the affixed
// derived names (setX, m_X, XChanged) that a whole-word symbol search
// cannot reach. See docs/specs/ANTS-3368-co-change-family.md.
//
// Design constraints (INV-10): this TU is pure. No Qt Widgets, no
// RemoteControl, no MainWindow, no QProcess — so it links into test_core
// alone and the handler's ripgrep plumbing stays in the RC TU that owns it.

#ifndef ANTS_COCHANGEFAMILY_H
#define ANTS_COCHANGEFAMILY_H

#include <QString>
#include <QStringList>
#include <QVector>

namespace CoChangeFamily {

// INV-5 — the closed, lexical role vocabulary. Six values, no seventh.
// Deliberately NOT semantic: an "apply sink" is a mutator on a class that
// happens not to be the settings store, which needs type resolution this
// scanner does not have (spec § 2.3).
enum class Role { JsonKey, Member, Mutator, Signal, Type, Reference };

// Wire spelling of a Role. Never returns nullptr.
const char *roleStr(Role r);

// INV-1 — split an identifier or config key into lowercased words. Splits
// on `_`, `-` and `.`, and on every lower->upper boundary; drops empties.
//   "claude.mcp_enabled" / "claudeMcpEnabled" / "CLAUDE_MCP_ENABLED"
//     -> [claude, mcp, enabled]
QStringList splitWords(const QString &s);

// INV-4 — a run whose every word is a stopword carries no signal.
bool isStopword(const QString &word);
bool allStopwords(const QStringList &words);

// INV-9 — the stem charset. First gate before the pattern is assembled.
bool isValidStem(const QString &stem);

// INV-3 — min_run defaults to min(2, stemWords) and clamps to 1..stemWords.
int defaultMinRun(int stemWordCount);
int clampMinRun(int requested, int stemWordCount);

// INV-3 — the longest run of stem words shared with a candidate, contiguous
// in BOTH sequences. Returns 0 when they share no adjacent run.
struct Run {
    int len       = 0;  // words
    int stemStart = 0;  // index into the stem's words
};
Run longestRun(const QStringList &stemWords, const QStringList &candWords);

// INV-2 + INV-12 — the case-insensitive alternation handed to rg. At
// minRun >= 2 it is the stem's adjacent word PAIRS joined by an optional
// separator; at minRun == 1 (and for a one-word stem) each single word is
// alternated as well, so min_run widens the scan and not merely the filter.
// Every word is regex-quoted before assembly.
QString scanPattern(const QStringList &stemWords, int minRun);

// INV-13 — widen an rg match span to the candidate the filter reads.
// Inside a string literal the candidate is the literal's contents; outside
// one it is the maximal surrounding [A-Za-z0-9_] token.
struct Candidate {
    QString name;
    bool    inLiteral = false;
};
Candidate widenToCandidate(const QString &line, int matchStart, int matchEnd);

// INV-5 — role from the candidate's lexical shape, first match wins in the
// order of the enum's documentation (json_key > member > mutator > signal >
// type > reference).
Role classifyRole(const QString &line, const Candidate &cand);

// One resolved stem: its name as the caller spelled it, its words, and the
// min_run resolved against its OWN word count (INV-3).
struct Stem {
    QString     name;
    QStringList words;
    int         minRun = 1;
};

// A raw rg hit, before widening and filtering.
struct RawMatch {
    QString path;
    int     line       = 0;
    QString text;
    int     matchStart = 0;
    int     matchEnd   = 0;
};

struct Site {
    QString     path;
    int         line = 0;
    QString     stem;   // the owning stem's name
    QString     name;   // the widened candidate
    Role        role   = Role::Reference;
    QStringList run;
    int         runLen = 0;
    QString     text;
};

struct Options {
    int maxSites     = 200;   // clamped to 1..1000 (INV-7)
    int maxTextBytes = 512;   // same clip workspace_search applies
};

struct Result {
    QVector<Site> sites;      // already in INV-6 order
    bool          truncated = false;
};

int clampMaxSites(int requested);

// INV-3/4/6/7/13 — turn raw hits into the ordered, deduped, capped site
// list. One row per (path, line), owned by the stem with the longest run
// (ties by position in `stems`). Ordered by each file's maximum runLen
// descending, then path ascending, then line ascending. Retains the highest
// runLen when the cap binds, so `truncated` never hides the strongest hits.
Result assemble(const QVector<RawMatch> &matches,
                const QVector<Stem> &stems,
                const Options &opts = {});

}  // namespace CoChangeFamily

#endif
