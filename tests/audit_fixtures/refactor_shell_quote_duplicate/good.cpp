// Legitimate CALLERS of the consolidated helper, plus prose that merely
// names it. None of these is a duplication, and every one of them was a
// false positive under the pre-ANTS-3830 rule.

#include "shellutils.h"      // shellQuote() for pasted file paths

void sshArgs(QStringList &args, const QString &identityFile, const QString &host) {
    args << "-i" << shellQuote(identityFile);
    args << shellQuote(host);
}

void pastePaths(QStringList &paths, const QString &path) {
    paths << shellQuote(path);
}

QString wrapped(const QString &projectPath) {
    QString quoted = shellQuote(projectPath);   // assignment from a call
    return QString("cd %1 && claude").arg(quoted);
}

// A rule description that mentions the helper by name:
//   "ssh(1) argv construction that shellQuotes a host token"
