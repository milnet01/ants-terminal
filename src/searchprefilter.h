#pragma once

// ANTS-6245 — the pure half of workspace_search's timeout fallback: which
// files git has proved cannot hold a literal, so a second ripgrep scan may
// leave them out. Design and the measured hazards:
// docs/specs/ANTS-6245-search-timeout-git-fallback.md.
//
// Nothing here starts a process or opens a file. cmdWorkspaceSearch runs
// git and ripgrep and hands the raw output in.

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace SearchPrefilter {

// The shortest needle worth a git scan (§ 4.1).
constexpr int kMinNeedleBytes = 3;
// The most bytes of `--glob` skip arguments one scan is given (§ 4.4).
constexpr qint64 kSkipArgsCapBytes = qint64(1024) * 1024;

// § 4.2 — the longest run of pattern bytes that are printable ASCII and none
// of k, K, s, S; the first such run wins a tie. Empty when there is none.
QByteArray needle(const QByteArray &patternUtf8);

// § 4.3 — the content of the file `git grep -f` reads: the needle, then the
// two UTF-16 byte-order marks, one fixed string per line.
QByteArray needleFile(const QByteArray &needle);

// § 4.5 — how long the git steps and the second scan may take together, in
// ms. Zero or less means no fallback.
int fallbackBudgetMs(int budgetMs);

// § 4.4 — the root-relative paths a second scan may skip. Inputs are the raw
// NUL-separated output of `git ls-files -z -t -v -c -m -s`,
// `git grep --cached -l -z` and the attribute `git ls-files -z`.
QStringList skipList(const QByteArray &lsFilesTagged,
                     const QByteArray &grepHits,
                     const QByteArray &attributed);

// § 4.4 — `path` with ripgrep's glob metacharacters escaped.
QString globEscape(const QString &path);

// § 4.4 — the `--glob '!/<path>'` argument pairs for `skip`, in order, until
// the pairs total `capBytes`.
QStringList skipArgs(const QStringList &skip,
                     qint64 capBytes = kSkipArgsCapBytes);

}  // namespace SearchPrefilter
