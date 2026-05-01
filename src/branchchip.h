#pragma once

// ANTS-1109 — git-branch chip color cue. Pure helper kept in its
// own header so feature tests can drive the predicate without
// linking MainWindow + the status-bar widget tree.

#include <QString>

namespace branchchip {

// Primary branches get a green outline (matches the "Public" repo-
// visibility pill); everything else gets amber, signalling that
// the user is on a feature branch.
inline bool isPrimaryBranch(const QString &branch) {
    return branch == QLatin1String("main") ||
           branch == QLatin1String("master") ||
           branch == QLatin1String("trunk");
}

}  // namespace branchchip
