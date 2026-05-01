#include "themedstylesheet.h"

#include <QColor>
#include <QString>
#include <QStringLiteral>

// ANTS-1147 — extracted from MainWindow::applyTheme. Pure
// functions; no Qt widget headers needed. See
// docs/specs/ANTS-1147.md for the contract.

namespace themedstylesheet {

QString buildAppStylesheet(const Theme &theme) {
    // UI chrome (title bar, menus, tabs, status bar) always uses opaque backgrounds.
    // The `opacity` config key only affects the terminal content area — this is
    // handled in TerminalWidget::paintEvent via m_windowOpacity (the variable
    // name is historical; it drives per-pixel terminal-area fillRect alpha,
    // not Qt's whole-window setWindowOpacity).
    //
    // Qt stylesheet cascade: a stylesheet set on QMainWindow applies to its
    // QObject descendants, which includes child QDialogs — so the dialog
    // selectors below reach every popup (Settings, Audit, AI, SSH, Claude*,
    // QMessageBox/QInputDialog/etc.) as long as they were created with the
    // main window as their parent. Untagged QDialog must therefore stay
    // anchor-selected here and not rely on dialog-local setStyleSheet().
    return QString(
        "QMainWindow { background-color: %1; }"
        "QMenuBar { background-color: %2; color: %3; border-bottom: 1px solid %4; }"
        // Explicit base ::item rule prevents Qt from falling back to the
        // native style for the non-selected state. Without it, Qt composites
        // native item drawing under the stylesheet's :selected overlay; on
        // hover transitions the native and stylesheet layers race and the
        // highlight appears to flash (user report 2026-04-19). Transparent
        // background + matching padding makes the non-selected state a no-op
        // so the :selected rule is the only visible state change.
        "QMenuBar::item { background-color: transparent; padding: 4px 10px;"
        "  margin: 0; border-radius: 4px; }"
        // :hover and :selected both map to the same highlight. Qt's
        // QStyleSheetStyle treats them as distinct pseudo-states on
        // QMenuBar::item — Breeze / Fusion hover-flash for one frame
        // before :selected engages, and on that frame only :hover
        // styling applies. Mirroring :selected into :hover removes
        // the one-frame gap (closed-menu hover flash, original
        // 2026-04-20 report). The dropdown-flicker-when-both-apply
        // regression that briefly came up mid-0.7.4 is fixed
        // structurally by Qt::WA_OpaquePaintEvent on the menubar
        // widget (see construction site) — not by dropping this
        // rule.
        "QMenuBar::item:hover { background-color: %5; }"
        "QMenuBar::item:selected { background-color: %5; }"
        "QMenuBar::item:pressed { background-color: %5; }"
        "QMenu { background-color: %2; color: %3; border: 1px solid %4; padding: 4px; }"
        // The item's :selected background is a rectangle filling the
        // full item rect (no border-radius). Prior to 0.7.4 the base
        // rule had `border-radius: 4px` on the item — rounded
        // corners force Qt to paint the :selected fill with a clip
        // shape, and the rows outside that clip shape (inside the
        // item's bounding rect but outside the rounded pill) get
        // painted with the menu's background. That compositing path
        // shows a visible flash as the "current item" shifts rows
        // on mouse move — reported 2026-04-20 after the paste-
        // dialog fix exposed the leftover flicker. Dropping the
        // radius means the :selected fill is the whole rect and
        // deselect just repaints the same rect to the menu's
        // background color. No compositing, no flash.
        "QMenu::item { padding: 6px 24px 6px 12px; }"
        "QMenu::item:selected { background-color: %5; color: %1; }"
        "QMenu::separator { height: 1px; background: %4; margin: 4px 8px; }"
        "QStatusBar { background-color: %2; color: %6; border-top: 1px solid %4; }"
        "QTabWidget::pane { border: none; }"
        "QTabBar { background-color: %2; }"
        "QTabBar::tab { background-color: %2; color: %6; padding: 6px 16px 6px 22px;"
        "  border: none; border-bottom: 2px solid transparent; }"
        "QTabBar::tab:selected { color: %3; border-bottom: 2px solid %5; }"
        "QTabBar::tab:hover { background-color: %1; color: %3; }"
        // 0.7.32 (user feedback 2026-04-25) — the platform-style fallback
        // (0.6.27) still rendered the × hover-only on Fusion / qt6ct /
        // some Plasma color schemes — users couldn't see where to click
        // without first mousing onto the tab. Force-render the glyph via
        // a data-URI SVG that's always visible regardless of platform
        // style. Hover variant uses textPrimary instead of textSecondary
        // for "lifting" feedback and keeps the ansi-red background (%7)
        // for the will-click cue.
        "QTabBar::close-button {"
        "  image: url(\"data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg'"
        " width='10' height='10' viewBox='0 0 10 10'>"
        "<line x1='2' y1='2' x2='8' y2='8' stroke='%6'"
        " stroke-width='1.5' stroke-linecap='round'/>"
        "<line x1='8' y1='2' x2='2' y2='8' stroke='%6'"
        " stroke-width='1.5' stroke-linecap='round'/></svg>\");"
        "  subcontrol-position: right; margin: 2px; padding: 1px;"
        "  width: 14px; height: 14px; border-radius: 3px; }"
        "QTabBar::close-button:hover {"
        "  image: url(\"data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg'"
        " width='10' height='10' viewBox='0 0 10 10'>"
        "<line x1='2' y1='2' x2='8' y2='8' stroke='%3'"
        " stroke-width='1.5' stroke-linecap='round'/>"
        "<line x1='8' y1='2' x2='2' y2='8' stroke='%3'"
        " stroke-width='1.5' stroke-linecap='round'/></svg>\");"
        "  background-color: %7; border-radius: 3px; }"
        "QSplitter::handle { background-color: %4; }"
        "QSplitter::handle:horizontal { width: 2px; }"
        "QSplitter::handle:vertical { height: 2px; }"
        "QWidget#commandPalette { background-color: %2; border: 1px solid %4;"
        "  border-radius: 8px; }"
        "QLineEdit#commandPaletteInput { background: %1; color: %3; border: none;"
        "  border-bottom: 1px solid %4; padding: 10px 14px; font-size: 14px;"
        "  border-radius: 0px; }"
        "QListWidget#commandPaletteList { background: %2; color: %3; border: none;"
        "  outline: none; padding: 4px 0; }"
        "QListWidget#commandPaletteList::item { padding: 6px 14px; }"
        "QListWidget#commandPaletteList::item:selected { background-color: %5; color: %1; }"

        // ---- Pop-up / dialog theming ----
        // QMessageBox, QInputDialog, QColorDialog, QFileDialog, and our own
        // QDialog subclasses (Settings/Audit/AI/SSH/Claude*) all cascade from
        // here. Colors match the terminal's active theme.
        "QDialog { background-color: %2; color: %3; }"
        "QLabel { color: %3; background: transparent; }"
        "QPushButton { background-color: %1; color: %3;"
        "  border: 1px solid %4; padding: 6px 14px; border-radius: 4px; min-width: 60px; }"
        // :hover must be gated by :enabled. Without the gate, a disabled
        // button still gets the hover highlight, which advertises it as
        // actionable even though Qt swallows clicks on disabled buttons
        // (QAbstractButton::mousePressEvent early-returns). The Review
        // Changes button on a clean repo is the canonical example: it's
        // visible-but-disabled to tell the user "the repo is clean,"
        // but without this gate the hover highlight lied and made the
        // user think the button should work. See
        // tests/features/review_changes_clickable/spec.md.
        "QPushButton:hover:enabled { background-color: %5; color: %2; border-color: %5; }"
        "QPushButton:pressed:enabled { background-color: %4; }"
        "QPushButton:default { border: 1px solid %5; }"
        "QPushButton:disabled { color: %6; border-color: %4; background-color: %2; }"
        "QLineEdit { background-color: %1; color: %3; border: 1px solid %4;"
        "  padding: 5px 8px; border-radius: 3px;"
        "  selection-background-color: %5; selection-color: %2; }"
        "QLineEdit:focus { border: 1px solid %5; }"
        "QLineEdit:disabled { color: %6; }"
        "QTextEdit, QPlainTextEdit, QTextBrowser { background-color: %1; color: %3;"
        "  border: 1px solid %4; selection-background-color: %5; selection-color: %2; }"
        "QCheckBox { color: %3; spacing: 6px; background: transparent; }"
        "QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid %4;"
        "  background: %1; border-radius: 3px; }"
        "QCheckBox::indicator:checked { background: %5; border-color: %5; }"
        "QRadioButton { color: %3; spacing: 6px; background: transparent; }"
        "QRadioButton::indicator { width: 14px; height: 14px; border: 1px solid %4;"
        "  background: %1; border-radius: 7px; }"
        "QRadioButton::indicator:checked { background: %5; border-color: %5; }"
        "QComboBox { background-color: %1; color: %3; border: 1px solid %4;"
        "  padding: 4px 8px; border-radius: 3px; min-width: 80px; }"
        "QComboBox:hover { border-color: %5; }"
        "QComboBox::drop-down { border: none; width: 20px; }"
        "QComboBox QAbstractItemView { background-color: %2; color: %3;"
        "  border: 1px solid %4; selection-background-color: %5; selection-color: %2;"
        "  outline: none; }"
        "QSpinBox, QDoubleSpinBox { background-color: %1; color: %3;"
        "  border: 1px solid %4; padding: 4px 6px; border-radius: 3px; }"
        "QSpinBox:focus, QDoubleSpinBox:focus { border-color: %5; }"
        "QGroupBox { color: %3; border: 1px solid %4; border-radius: 4px;"
        "  margin-top: 10px; padding-top: 8px; background: transparent; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px;"
        "  padding: 0 4px; color: %3; }"
        "QListWidget, QTreeWidget, QTableWidget { background-color: %1; color: %3;"
        "  border: 1px solid %4; selection-background-color: %5; selection-color: %2;"
        "  alternate-background-color: %2; outline: none; }"
        "QListWidget::item:hover, QTreeWidget::item:hover, QTableWidget::item:hover"
        "  { background: %2; }"
        "QHeaderView::section { background-color: %2; color: %3;"
        "  border: 1px solid %4; padding: 4px 8px; }"
        "QScrollBar:vertical { background-color: %2; width: 12px; margin: 0; border: none; }"
        "QScrollBar::handle:vertical { background-color: %4; border-radius: 6px;"
        "  min-height: 20px; margin: 2px; }"
        "QScrollBar::handle:vertical:hover { background-color: %5; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; border: none; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
        "QScrollBar:horizontal { background-color: %2; height: 12px; margin: 0; border: none; }"
        "QScrollBar::handle:horizontal { background-color: %4; border-radius: 6px;"
        "  min-width: 20px; margin: 2px; }"
        "QScrollBar::handle:horizontal:hover { background-color: %5; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; border: none; }"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }"
        "QToolTip { background-color: %2; color: %3; border: 1px solid %4; padding: 4px; }"
        "QProgressBar { background-color: %1; color: %3; border: 1px solid %4;"
        "  border-radius: 3px; text-align: center; }"
        "QProgressBar::chunk { background-color: %5; }"
        "QDialogButtonBox QPushButton { min-width: 80px; }"
    ).arg(theme.bgPrimary.name(),
          theme.bgSecondary.name(),
          // %3 — textPrimary; appears verbatim in most rules but ALSO
          // as a stroke color in the data-URI SVG for the tab-close
          // glyph hover variant. Pre-encode the leading `#` as %23 so
          // Qt's CSS parser doesn't truncate the URI at the fragment
          // delimiter; the `%23` literal would also collide with
          // QString::arg() placeholder numbering, so we splice it
          // here rather than embed it in the format string.
          QStringLiteral("%23") + theme.textPrimary.name().mid(1),
          theme.border.name(),
          theme.accent.name(),
          // %6 — textSecondary; same SVG-stroke pre-encoding as %3
          // above, used by the default (non-hover) tab close button.
          QStringLiteral("%23") + theme.textSecondary.name().mid(1),
          theme.ansi[1].name());  // ANSI red for close/danger
}

QString buildMenuBarStylesheet(const Theme &theme) {
    return QStringLiteral(
        "QMenuBar { background-color: %1; color: %2; "
        "  border-bottom: 1px solid %3; }"
        "QMenuBar::item { background-color: transparent; "
        "  padding: 4px 10px; margin: 0; border-radius: 4px; }"
        "QMenuBar::item:hover { background-color: %4; }"
        "QMenuBar::item:selected { background-color: %4; }"
        "QMenuBar::item:pressed { background-color: %4; }"
    ).arg(theme.bgSecondary.name(),
          theme.textPrimary.name(),
          theme.border.name(),
          theme.accent.name());
}

QString buildStatusMessageStylesheet(const Theme &theme) {
    return QStringLiteral("color: %1; padding: 0 8px; font-size: 11px;")
        .arg(theme.textPrimary.name());
}

QString buildStatusProcessStylesheet(const Theme &theme) {
    return QStringLiteral("color: %1; padding: 0 8px; font-size: 11px;")
        .arg(theme.ansi[4].name());
}

QString buildGitSeparatorStylesheet(const Theme &theme) {
    // Hard divider between the branch chip and the transient-status slot.
    // textSecondary (not border) because border is often nearly invisible
    // against bgPrimary on low-contrast themes (Gruvbox-dark, Nord, Solarized);
    // textSecondary is the muted-but-readable role that every theme tunes
    // for foreground legibility. Using background-color (not palette) to
    // paint the QFrame::VLine works around Fusion's habit of drawing
    // VLines in the window's palette midlight/dark roles, which also
    // disappear on dark themes.
    return QStringLiteral("QFrame { background-color: %1; border: none; }")
        .arg(theme.textSecondary.name());
}

QString buildChipStylesheet(const Theme &theme, const QColor &fgColor, int leftMarginPx) {
    // Shared chip QSS — three pre-1147 inlined call sites
    // (mainwindow.cpp:applyTheme, updateStatusBar, refreshRepoVisibility)
    // converged on this one helper. The only delta between the two
    // chip variants in the wild is the left margin: 4 px on the
    // branch chip (paired with the git-separator), 0 on the
    // repo-visibility badge (sits flush against the title bar).
    // Foreground colour is the caller's policy (green/amber for
    // primary-vs-feature branch, green/red for Public/Private).
    //
    // Margin formatting mirrors CSS-spec convention exactly: zero
    // is unit-free (`0`), non-zero carries the `px` suffix
    // (`4px`). Pre-1147 the two inlined templates already used this
    // asymmetry — the helper preserves it to keep the post-extraction
    // QSS byte-identical to either original variant.
    const QString marginLeft = (leftMarginPx == 0)
        ? QStringLiteral("0")
        : QStringLiteral("%1px").arg(leftMarginPx);
    return QStringLiteral(
        "QLabel { color: %1; background: %2; border: 1px solid %3; "
        "border-radius: 3px; padding: 1px 8px; margin: 2px 6px 2px %4; "
        "font-size: 11px; font-weight: 600; }")
        .arg(fgColor.name(),
             theme.bgSecondary.name(),
             theme.border.name(),
             marginLeft);
}

}  // namespace themedstylesheet
