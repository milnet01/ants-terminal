#pragma once

// themedstylesheet — pure-function QSS builders extracted from
// MainWindow::applyTheme by ANTS-1147 (Bundle G Tier 3, sibling
// to ANTS-1146's ClaudeStatusBarController carve-out). See
// docs/specs/ANTS-1147.md for the full design rationale, the
// three-call-site chip-helper consolidation, and the
// updateStatusBar cache-and-compare optimization shape.
//
// Each helper is `(Theme, ...args) → QString` — stateless, no I/O,
// no widget instantiation. Side effects (qApp->setStyleSheet,
// dialog re-polish, setBackgroundFill, palette/update calls) stay
// in MainWindow.

#include <QString>

#include "themes.h"

class QColor;

namespace themedstylesheet {

// The big app-level cascade applied via qApp->setStyleSheet.
// Covers QMainWindow / QMenu* / QStatusBar / QTabBar / dialog
// chrome / QPushButton / QLineEdit / QCheckBox / QComboBox /
// QSpinBox / QGroupBox / QListWidget / QHeaderView / QScrollBar /
// QToolTip / QProgressBar. Includes the data-URI SVG tab-close
// glyph (with %23-pre-encoded stroke colours — see
// implementation comment).
QString buildAppStylesheet(const Theme &theme);

// Per-widget templates — each takes only the data its QSS needs.
QString buildMenuBarStylesheet(const Theme &theme);
QString buildStatusMessageStylesheet(const Theme &theme);
QString buildStatusProcessStylesheet(const Theme &theme);
QString buildGitSeparatorStylesheet(const Theme &theme);

// Chip stylesheet shared by m_statusGitBranch and
// m_repoVisibilityLabel. The only delta between the two pre-1147
// inlined templates was the left margin (4 px on the branch
// chip, 0 on the repo-visibility chip), so the helper takes it
// as a parameter. Foreground colour comes from the caller —
// branch chip picks green/amber by primary-branch flag, the
// visibility chip picks green/red by Public/Private. Different
// policies, same QSS shape.
QString buildChipStylesheet(const Theme &theme, const QColor &fgColor, int leftMarginPx);

}  // namespace themedstylesheet
