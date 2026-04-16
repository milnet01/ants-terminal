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
    m_titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents); // Pass clicks to TitleBar
    layout->addStretch();
    layout->addWidget(m_titleLabel);
    layout->addStretch();

    // Center window button
    m_centerBtn = new QToolButton(this);
    m_centerBtn->setText("\u2725");  // ✥ crosshair
    m_centerBtn->setToolTip("Center window on screen (Ctrl+Shift+M)");
    m_centerBtn->setAutoRaise(true);
    m_centerBtn->setFixedSize(32, 28);
    layout->addWidget(m_centerBtn);

    // Minimize button
    m_minimizeBtn = new QToolButton(this);
    m_minimizeBtn->setText("\u2013");  // – en dash
    m_minimizeBtn->setToolTip("Minimize");
    m_minimizeBtn->setAutoRaise(true);
    m_minimizeBtn->setFixedSize(32, 28);
    connect(m_minimizeBtn, &QToolButton::clicked, this, &TitleBar::minimizeRequested);
    layout->addWidget(m_minimizeBtn);

    // Maximize/restore button
    m_maximizeBtn = new QToolButton(this);
    m_maximizeBtn->setText("\u25A1");  // □ white square
    m_maximizeBtn->setToolTip("Maximize");
    m_maximizeBtn->setAutoRaise(true);
    m_maximizeBtn->setFixedSize(32, 28);
    connect(m_maximizeBtn, &QToolButton::clicked, this, &TitleBar::maximizeRequested);
    layout->addWidget(m_maximizeBtn);

    // Close button
    m_closeBtn = new QToolButton(this);
    m_closeBtn->setText("\u2715");  // ✕
    m_closeBtn->setToolTip("Close");
    m_closeBtn->setAutoRaise(true);
    m_closeBtn->setFixedSize(32, 28);
    m_closeBtn->setObjectName("closeBtn");
    connect(m_closeBtn, &QToolButton::clicked, this, &TitleBar::closeRequested);
    layout->addWidget(m_closeBtn);
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
