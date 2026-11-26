#pragma once

// ANTS-1109 — git-branch chip color cue. Pure helper kept in its
// own header so feature tests can drive the predicate without
// linking MainWindow + the status-bar widget tree.

#include "themes.h"

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

// The chip's colour: theme.ansi[2] (green) on a primary branch, else
// theme.ansi[3] (amber). Every site that styles the chip asks here.
inline const QColor &chipColour(const Theme &theme, const QString &branch) {
    return isPrimaryBranch(branch) ? theme.ansi[2] : theme.ansi[3];
}

}  // namespace branchchip
