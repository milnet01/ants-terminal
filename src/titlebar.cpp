#include "titlebar.h"

#include <QMouseEvent>
#include <QWindow>
#include <QApplication>
#include <QIcon>
#include <QCursor>

TitleBar::TitleBar(QWidget *parent) : QWidget(parent) {
    setFixedHeight(36);
    // Required for stylesheet background to paint when WA_TranslucentBackground
    // is set on the top-level window (custom QWidget subclasses don't paint their
    // stylesheet background by default in that mode).
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 4, 0);
    layout->setSpacing(2);

    // Application icon
    m_iconLabel = new QLabel(this);
    m_iconLabel->setFixedSize(24, 24);
    m_iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    QIcon appIcon = QIcon::fromTheme("ants-terminal");
    if (appIcon.isNull()) {
        QString base = QApplication::applicationDirPath() + "/../assets/ants-terminal";
        appIcon.addFile(base + "-32.png", QSize(32, 32));
        appIcon.addFile(base + "-48.png", QSize(48, 48));
    }
    if (!appIcon.isNull())
        m_iconLabel->setPixmap(appIcon.pixmap(20, 20));
    layout->addWidget(m_iconLabel);
    layout->addSpacing(4);

    m_titleLabel = new ElidedLabel(this);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    m_titleLabel->setElideMode(Qt::ElideMiddle);   // preserve both "cmd" prefix and "cwd" suffix
    // Pass clicks to TitleBar. This also means ElidedLabel's full-title
    // tooltip never shows; that is intended (see the tooltip note below).
    m_titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addStretch();
    layout->addWidget(m_titleLabel);
    layout->addStretch();

    // NOTE: tooltips removed from titlebar buttons (2026-04-20).
    // A Qt / KWin / Wayland quirk fired the QTipLabel show/hide cycle
    // on every frame while the cursor was anywhere near a titlebar
    // button, animating the tooltip's geometry at ~60 Hz. That
    // animation cascaded a LayoutRequest through the whole widget
    // tree each frame, and the cascade is what the user saw as the
    // open-dropdown flicker. Diagnosed via the debug-log system — see
    // `~/.local/share/ants-terminal/debug.log` with `paint,events`
    // enabled. The button glyphs (✥ / ✕ / ⬜ / ⟩) + the hover-colour
    // feedback give enough affordance without tooltip text.

    // The four window buttons. Glyph-only, so each has an accessible name:
    // Orca/AT-SPI would otherwise speak the codepoint literally
    // ("eight-spoked asterisk"). 0.7.41.
    auto addButton = [&](const char *glyph, const char *objectName,
                         const QString &name, const QString &description) {
        auto *btn = new QToolButton(this);
        btn->setText(QString::fromUtf8(glyph));
        btn->setObjectName(objectName);
        btn->setAccessibleName(name);
        btn->setAccessibleDescription(description);
        btn->setAutoRaise(true);
        btn->setFixedSize(32, 28);
        layout->addWidget(btn);
        return btn;
    };
    // MainWindow connects the centre button, through centerButton().
    m_centerBtn = addButton("\u2725", "centerBtn",  // ✥ crosshair
                            tr("Center window"),
                            tr("Center this window on the active screen"));
    m_minimizeBtn = addButton("\u2013", "minimizeBtn",  // – en dash
                              tr("Minimize window"), tr("Minimize this window"));
    connect(m_minimizeBtn, &QToolButton::clicked, this, &TitleBar::minimizeRequested);
    m_maximizeBtn = addButton("\u25A1", "maximizeBtn",  // □ white square
                              tr("Maximize window"),
                              tr("Maximize or restore this window"));
    connect(m_maximizeBtn, &QToolButton::clicked, this, &TitleBar::maximizeRequested);
    m_closeBtn = addButton("\u2715", "closeBtn",  // ✕
                           tr("Close window"), tr("Close this window"));
    connect(m_closeBtn, &QToolButton::clicked, this, &TitleBar::closeRequested);
}

void TitleBar::setTitle(const QString &title) {
    m_titleLabel->setFullText(title);
}

void TitleBar::setThemeColors(const QColor &bg, const QColor &fg,
                               const QColor &accent, const QColor &border,
                               const QColor &danger) {
    QString ss = QString(
        "TitleBar { background-color: %1; border-bottom: 1px solid %4; }"
        "QLabel { color: %2; font-weight: bold; background: transparent; }"
        "QToolButton { color: %2; background: transparent; border: none;"
        "  border-radius: 4px; font-size: 14px; }"
        "QToolButton:hover { background-color: %3; color: %1; }"
        "QToolButton#closeBtn:hover { background-color: %5; color: white; }"
    ).arg(bg.name(), fg.name(), accent.name(), border.name(), danger.name());
    setStyleSheet(ss);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        emit maximizeRequested();
    }
}

void TitleBar::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        // Compute offset: cursor_global - known_window_pos
        // Can't use event->pos() — Qt computes it from X11 pos (0,0 on KWin compositor)
        m_cursorOffset = event->globalPosition().toPoint() - m_knownWindowPos;
        m_systemDragActive = true;

        // Let KWin handle the drag (only method that works for frameless windows)
        if (window()->windowHandle())
            window()->windowHandle()->startSystemMove();
    }
}

void TitleBar::finishSystemDrag() {
    if (!m_systemDragActive) return;
    m_systemDragActive = false;

    QPoint cursor = QCursor::pos();
    QPoint finalPos = cursor - m_cursorOffset;
    emit windowMoved(finalPos);
}
