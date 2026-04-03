#include "terminalwidget.h"

#include <QPainter>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QInputMethodEvent>
#include <QScrollBar>
#include <cmath>

TerminalWidget::TerminalWidget(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Pick a good monospace font
    QStringList families = {"JetBrains Mono", "Fira Code", "Source Code Pro",
                            "DejaVu Sans Mono", "Liberation Mono", "Monospace"};
    m_font = QFont(families);
    m_font.setPointSize(11);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);

    QFontMetrics fm(m_font);
    m_cellWidth = fm.horizontalAdvance('M');
    m_cellHeight = fm.height();
    m_fontAscent = fm.ascent();

    // Initial grid
    m_grid = new TerminalGrid(24, 80);
    m_parser = new VtParser([this](const VtAction &a) {
        m_grid->processAction(a);
    });

    // Cursor blink timer
    m_cursorTimer.setInterval(530);
    connect(&m_cursorTimer, &QTimer::timeout, this, &TerminalWidget::blinkCursor);
    m_cursorTimer.start();

    setMinimumSize(m_cellWidth * 20 + m_padding * 2, m_cellHeight * 5 + m_padding * 2);
}

TerminalWidget::~TerminalWidget() {
    delete m_parser;
    delete m_grid;
    // m_pty is a QObject child, deleted automatically
}

bool TerminalWidget::startShell() {
    m_pty = new Pty(this);
    connect(m_pty, &Pty::dataReceived, this, &TerminalWidget::onPtyData);
    connect(m_pty, &Pty::finished, this, &TerminalWidget::onPtyFinished);

    recalcGridSize();

    if (!m_pty->start()) {
        return false;
    }

    m_pty->resize(m_grid->rows(), m_grid->cols());
    return true;
}

void TerminalWidget::applyThemeColors(const QColor &fg, const QColor &bg,
                                       const QColor &cursorColor) {
    m_grid->setDefaultFg(fg);
    m_grid->setDefaultBg(bg);
    m_cursorColor = cursorColor;
    update();
}

void TerminalWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setFont(m_font);

    const int rows = m_grid->rows();
    const int cols = m_grid->cols();
    const QColor defaultBg = m_grid->defaultBg();

    // Fill background
    p.fillRect(rect(), defaultBg);

    // Viewport model:
    //   Combined buffer = [scrollback lines...] + [screen lines...]
    //   viewStart = scrollbackSize - scrollOffset
    //   For viewport row `vr`: globalLine = viewStart + vr
    //     if globalLine < scrollbackSize -> scrollback line
    //     else -> screen line (index = globalLine - scrollbackSize)
    int scrollbackSize = m_grid->scrollbackSize();
    int viewStart = scrollbackSize - m_scrollOffset;

    static Cell blankCell;

    for (int vr = 0; vr < rows; ++vr) {
        int globalLine = viewStart + vr;
        int px_y = m_padding + vr * m_cellHeight;

        for (int col = 0; col < cols; ++col) {
            int px_x = m_padding + col * m_cellWidth;

            // Get the cell for this position
            const Cell *cp = &blankCell;
            if (globalLine >= 0 && globalLine < scrollbackSize) {
                const auto &sbLine = m_grid->scrollbackLine(globalLine);
                if (col < static_cast<int>(sbLine.size()))
                    cp = &sbLine[col];
            } else {
                int screenRow = globalLine - scrollbackSize;
                if (screenRow >= 0 && screenRow < rows)
                    cp = &m_grid->cellAt(screenRow, col);
            }

            const Cell &c = *cp;
            QColor fg = c.attrs.fg;
            QColor bg = c.attrs.bg;
            if (c.attrs.inverse) std::swap(fg, bg);
            if (c.attrs.dim) fg = fg.darker(150);

            if (bg != defaultBg) {
                p.fillRect(px_x, px_y, m_cellWidth, m_cellHeight, bg);
            }

            if (c.codepoint != ' ' && c.codepoint != 0) {
                QFont drawFont = m_font;
                if (c.attrs.bold) drawFont.setBold(true);
                if (c.attrs.italic) drawFont.setItalic(true);
                if (drawFont != m_font) p.setFont(drawFont);
                p.setPen(fg);
                p.drawText(px_x, px_y + m_fontAscent, QString::fromUcs4(reinterpret_cast<const char32_t *>(&c.codepoint), 1));
                if (drawFont != m_font) p.setFont(m_font);
            }

            if (c.attrs.underline) {
                p.setPen(fg);
                p.drawLine(px_x, px_y + m_cellHeight - 1,
                           px_x + m_cellWidth - 1, px_y + m_cellHeight - 1);
            }
            if (c.attrs.strikethrough) {
                p.setPen(fg);
                int sy = px_y + m_cellHeight / 2;
                p.drawLine(px_x, sy, px_x + m_cellWidth - 1, sy);
            }
        }
    }

    // Cursor (only when viewing current screen)
    if (m_scrollOffset == 0 && m_grid->cursorVisible() && (m_cursorBlinkOn || !m_hasFocus)) {
        int cx = m_padding + m_grid->cursorCol() * m_cellWidth;
        int cy = m_padding + m_grid->cursorRow() * m_cellHeight;

        if (m_hasFocus) {
            p.fillRect(cx, cy, m_cellWidth, m_cellHeight, m_cursorColor);
            const Cell &cc = m_grid->cellAt(m_grid->cursorRow(), m_grid->cursorCol());
            if (cc.codepoint != ' ' && cc.codepoint != 0) {
                p.setPen(m_grid->defaultBg());
                p.drawText(cx, cy + m_fontAscent, QString::fromUcs4(reinterpret_cast<const char32_t *>(&cc.codepoint), 1));
            }
        } else {
            p.setPen(m_cursorColor);
            p.setBrush(Qt::NoBrush);
            p.drawRect(cx, cy, m_cellWidth - 1, m_cellHeight - 1);
        }
    }
}

void TerminalWidget::keyPressEvent(QKeyEvent *event) {
    // Reset scroll on keypress
    m_scrollOffset = 0;

    // Reset cursor blink
    m_cursorBlinkOn = true;
    m_cursorTimer.start();

    QByteArray data;
    int key = event->key();
    Qt::KeyboardModifiers mods = event->modifiers();

    // Ctrl+Shift+V — paste
    if (key == Qt::Key_V && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        const QClipboard *clipboard = QApplication::clipboard();
        const QMimeData *mime = clipboard->mimeData();
        if (mime->hasImage()) {
            QImage img = clipboard->image();
            if (!img.isNull()) {
                emit imagePasted(img);
                return;
            }
        }
        if (mime->hasText()) {
            QString text = clipboard->text();
            data = text.toUtf8();
            m_pty->write(data);
        }
        return;
    }

    // Ctrl+Shift+C — copy (would need selection support)
    if (key == Qt::Key_C && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        // TODO: copy selection
        return;
    }

    // Shift+Enter — literal newline (Ctrl-V + newline for readline)
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && (mods & Qt::ShiftModifier)) {
        data = QByteArray("\x16\n", 2); // Ctrl-V + LF
        m_pty->write(data);
        return;
    }

    // Ctrl+key combinations
    if (mods & Qt::ControlModifier && !(mods & Qt::ShiftModifier)) {
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            // Ctrl+A = 0x01, Ctrl+B = 0x02, ..., Ctrl+Z = 0x1A
            char ch = static_cast<char>(key - Qt::Key_A + 1);
            data.append(ch);
            m_pty->write(data);
            return;
        }
        if (key == Qt::Key_BracketLeft) { data = "\x1B"; m_pty->write(data); return; }
        if (key == Qt::Key_Backslash)   { data = "\x1C"; m_pty->write(data); return; }
        if (key == Qt::Key_BracketRight){ data = "\x1D"; m_pty->write(data); return; }
    }

    // Special keys
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        data = "\r";
        break;
    case Qt::Key_Backspace:
        data = "\x7F";
        break;
    case Qt::Key_Tab:
        data = "\t";
        break;
    case Qt::Key_Escape:
        data = "\x1B";
        break;
    case Qt::Key_Up:
        data = "\x1B[A";
        break;
    case Qt::Key_Down:
        data = "\x1B[B";
        break;
    case Qt::Key_Right:
        data = "\x1B[C";
        break;
    case Qt::Key_Left:
        data = "\x1B[D";
        break;
    case Qt::Key_Home:
        data = "\x1B[H";
        break;
    case Qt::Key_End:
        data = "\x1B[F";
        break;
    case Qt::Key_Insert:
        data = "\x1B[2~";
        break;
    case Qt::Key_Delete:
        data = "\x1B[3~";
        break;
    case Qt::Key_PageUp:
        if (mods & Qt::ShiftModifier) {
            // Shift+PgUp — scroll back
            m_scrollOffset = std::min(m_scrollOffset + m_grid->rows() / 2,
                                       m_grid->scrollbackSize());
            update();
            return;
        }
        data = "\x1B[5~";
        break;
    case Qt::Key_PageDown:
        if (mods & Qt::ShiftModifier) {
            m_scrollOffset = std::max(m_scrollOffset - m_grid->rows() / 2, 0);
            update();
            return;
        }
        data = "\x1B[6~";
        break;
    case Qt::Key_F1:  data = "\x1BOP"; break;
    case Qt::Key_F2:  data = "\x1BOQ"; break;
    case Qt::Key_F3:  data = "\x1BOR"; break;
    case Qt::Key_F4:  data = "\x1BOS"; break;
    case Qt::Key_F5:  data = "\x1B[15~"; break;
    case Qt::Key_F6:  data = "\x1B[17~"; break;
    case Qt::Key_F7:  data = "\x1B[18~"; break;
    case Qt::Key_F8:  data = "\x1B[19~"; break;
    case Qt::Key_F9:  data = "\x1B[20~"; break;
    case Qt::Key_F10: data = "\x1B[21~"; break;
    case Qt::Key_F11: data = "\x1B[23~"; break;
    case Qt::Key_F12: data = "\x1B[24~"; break;
    default:
        // Regular text input
        {
            QString text = event->text();
            if (!text.isEmpty()) {
                data = text.toUtf8();
            }
        }
        break;
    }

    if (!data.isEmpty()) {
        m_pty->write(data);
    }
}

void TerminalWidget::resizeEvent(QResizeEvent *) {
    recalcGridSize();
}

void TerminalWidget::focusInEvent(QFocusEvent *) {
    m_hasFocus = true;
    m_cursorBlinkOn = true;
    m_cursorTimer.start();
    update();
}

void TerminalWidget::focusOutEvent(QFocusEvent *) {
    m_hasFocus = false;
    m_cursorBlinkOn = false;
    m_cursorTimer.stop();
    update();
}

void TerminalWidget::wheelEvent(QWheelEvent *event) {
    int delta = event->angleDelta().y();
    if (delta > 0) {
        // Scroll up (into scrollback)
        m_scrollOffset = std::min(m_scrollOffset + 3, m_grid->scrollbackSize());
    } else if (delta < 0) {
        // Scroll down (toward current)
        m_scrollOffset = std::max(m_scrollOffset - 3, 0);
    }
    update();
}

void TerminalWidget::inputMethodEvent(QInputMethodEvent *event) {
    if (!event->commitString().isEmpty()) {
        QByteArray data = event->commitString().toUtf8();
        m_pty->write(data);
    }
    event->accept();
}

QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (query == Qt::ImCursorRectangle) {
        int x = m_padding + m_grid->cursorCol() * m_cellWidth;
        int y = m_padding + m_grid->cursorRow() * m_cellHeight;
        return QRect(x, y, m_cellWidth, m_cellHeight);
    }
    return QWidget::inputMethodQuery(query);
}

void TerminalWidget::onPtyData(const QByteArray &data) {
    m_parser->feed(data.constData(), data.size());

    // Check for title changes
    QString title = m_grid->windowTitle();
    if (title != m_lastTitle) {
        m_lastTitle = title;
        emit titleChanged(title);
    }

    update();
}

void TerminalWidget::onPtyFinished(int exitCode) {
    emit shellExited(exitCode);
}

void TerminalWidget::blinkCursor() {
    m_cursorBlinkOn = !m_cursorBlinkOn;
    // Only repaint the cursor area
    int cx = m_padding + m_grid->cursorCol() * m_cellWidth;
    int cy = m_padding + m_grid->cursorRow() * m_cellHeight;
    update(cx, cy, m_cellWidth, m_cellHeight);
}

void TerminalWidget::recalcGridSize() {
    int availW = width() - m_padding * 2;
    int availH = height() - m_padding * 2;
    int cols = std::max(availW / m_cellWidth, 10);
    int rows = std::max(availH / m_cellHeight, 3);

    if (cols != m_grid->cols() || rows != m_grid->rows()) {
        m_grid->resize(rows, cols);
        if (m_pty) {
            m_pty->resize(rows, cols);
        }
    }
}

QPoint TerminalWidget::cellToPixel(int row, int col) const {
    return QPoint(m_padding + col * m_cellWidth, m_padding + row * m_cellHeight);
}
