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
#include <QMouseEvent>
#include <QLabel>
#include <QHBoxLayout>
#include <QPushButton>
#include <QDesktopServices>
#include <QUrl>
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
    setMouseTracking(true);

    // Search bar (hidden by default)
    m_searchBar = new QWidget(this);
    m_searchBar->setObjectName("searchBar");
    auto *searchLayout = new QHBoxLayout(m_searchBar);
    searchLayout->setContentsMargins(8, 4, 8, 4);
    searchLayout->setSpacing(6);

    auto *searchIcon = new QLabel("Find:", m_searchBar);
    searchLayout->addWidget(searchIcon);

    m_searchInput = new QLineEdit(m_searchBar);
    m_searchInput->setPlaceholderText("Search...");
    m_searchInput->setMinimumWidth(200);
    searchLayout->addWidget(m_searchInput);

    m_searchLabel = new QLabel("0/0", m_searchBar);
    searchLayout->addWidget(m_searchLabel);

    auto *prevBtn = new QPushButton("\u25B2", m_searchBar);
    prevBtn->setFixedSize(28, 24);
    prevBtn->setToolTip("Previous (Shift+Enter)");
    searchLayout->addWidget(prevBtn);

    auto *nextBtn = new QPushButton("\u25BC", m_searchBar);
    nextBtn->setFixedSize(28, 24);
    nextBtn->setToolTip("Next (Enter)");
    searchLayout->addWidget(nextBtn);

    auto *closeBtn = new QPushButton("\u2715", m_searchBar);
    closeBtn->setFixedSize(28, 24);
    closeBtn->setToolTip("Close (Escape)");
    searchLayout->addWidget(closeBtn);

    searchLayout->addStretch();
    m_searchBar->hide();

    connect(m_searchInput, &QLineEdit::textChanged, this, [this](const QString &) {
        performSearch();
    });
    connect(m_searchInput, &QLineEdit::returnPressed, this, &TerminalWidget::searchNext);
    connect(nextBtn, &QPushButton::clicked, this, &TerminalWidget::searchNext);
    connect(prevBtn, &QPushButton::clicked, this, &TerminalWidget::searchPrev);
    connect(closeBtn, &QPushButton::clicked, this, &TerminalWidget::hideSearchBar);
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

    // Precompute bracket match for current cursor
    BracketPair bp = (m_scrollOffset == 0) ? findMatchingBracket() : BracketPair{-1,-1,-1,-1};

    for (int vr = 0; vr < rows; ++vr) {
        int globalLine = viewStart + vr;
        int px_y = m_padding + vr * m_cellHeight;

        // Precompute URL spans for this line
        auto urlSpans = detectUrls(globalLine);

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

            // Check if cell is part of a URL
            bool isUrl = false;
            for (const auto &u : urlSpans) {
                if (col >= u.startCol && col <= u.endCol) {
                    isUrl = true;
                    break;
                }
            }

            // Selection highlight (highest priority)
            bool selected = isCellSelected(globalLine, col);
            if (selected) {
                fg = m_selectionFg;
                bg = m_selectionBg;
            } else if (isCellCurrentMatch(globalLine, col)) {
                fg = m_searchCurrentFg;
                bg = m_searchCurrentBg;
            } else if (isCellSearchMatch(globalLine, col)) {
                fg = m_searchHighlightFg;
                bg = m_searchHighlightBg;
            } else if ((globalLine == bp.line1 && col == bp.col1) ||
                       (globalLine == bp.line2 && col == bp.col2)) {
                bg = m_bracketHighlightBg;
            } else if (isUrl) {
                fg = m_urlColor;
            }

            if (bg != defaultBg || selected) {
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

            // Underline for URLs or explicit underline attribute
            if (c.attrs.underline || isUrl) {
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

    // Ctrl+Shift+C — copy selection
    if (key == Qt::Key_C && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        copySelection();
        return;
    }

    // Ctrl+Shift+F — search
    if (key == Qt::Key_F && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        showSearchBar();
        return;
    }

    // Escape — close search bar
    if (key == Qt::Key_Escape && m_searchVisible) {
        hideSearchBar();
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
    int searchBarH = m_searchVisible ? m_searchBar->height() : 0;
    int availH = height() - m_padding * 2 - searchBarH;
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

QPoint TerminalWidget::pixelToCell(const QPoint &pos) const {
    int col = std::clamp((pos.x() - m_padding) / m_cellWidth, 0, m_grid->cols() - 1);
    int vr = std::clamp((pos.y() - m_padding) / m_cellHeight, 0, m_grid->rows() - 1);
    int globalLine = m_grid->scrollbackSize() - m_scrollOffset + vr;
    return QPoint(globalLine, col);
}

void TerminalWidget::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        // Ctrl+Click opens URLs
        if (event->modifiers() & Qt::ControlModifier) {
            QPoint cell = pixelToCell(event->pos());
            QString url = urlAtCell(cell.x(), cell.y());
            if (!url.isEmpty()) {
                QDesktopServices::openUrl(QUrl(url));
                return;
            }
        }
        clearSelection();
        m_selecting = true;
        m_selStart = pixelToCell(event->pos());
        m_selEnd = m_selStart;
        update();
    }
    // Middle-click paste (X11 primary selection)
    if (event->button() == Qt::MiddleButton) {
        QString text = QApplication::clipboard()->text(QClipboard::Selection);
        if (!text.isEmpty()) {
            m_pty->write(text.toUtf8());
        }
    }
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event) {
    if (m_selecting) {
        m_selEnd = pixelToCell(event->pos());
        m_hasSelection = (m_selStart != m_selEnd);
        update();
    }

    // Change cursor to hand when hovering over a URL with Ctrl held
    if (event->modifiers() & Qt::ControlModifier) {
        QPoint cell = pixelToCell(event->pos());
        QString url = urlAtCell(cell.x(), cell.y());
        setCursor(url.isEmpty() ? Qt::IBeamCursor : Qt::PointingHandCursor);
    } else {
        setCursor(Qt::IBeamCursor);
    }
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && m_selecting) {
        m_selecting = false;
        m_selEnd = pixelToCell(event->pos());
        m_hasSelection = (m_selStart != m_selEnd);
        // Copy to X11 primary selection (middle-click paste)
        if (m_hasSelection) {
            QString text = selectedText();
            if (!text.isEmpty()) {
                QApplication::clipboard()->setText(text, QClipboard::Selection);
            }
        }
        update();
    }
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        // Select word under cursor
        QPoint cell = pixelToCell(event->pos());
        int globalLine = cell.x();
        int col = cell.y();
        int scrollbackSize = m_grid->scrollbackSize();
        int rows = m_grid->rows();

        // Get character at position
        auto getChar = [&](int gl, int c) -> uint32_t {
            if (gl >= 0 && gl < scrollbackSize) {
                const auto &line = m_grid->scrollbackLine(gl);
                if (c < static_cast<int>(line.size()))
                    return line[c].codepoint;
            } else {
                int sr = gl - scrollbackSize;
                if (sr >= 0 && sr < rows)
                    return m_grid->cellAt(sr, c).codepoint;
            }
            return ' ';
        };

        auto isWordChar = [](uint32_t cp) {
            return cp != ' ' && cp != 0 && cp != '\t';
        };

        if (!isWordChar(getChar(globalLine, col))) return;

        // Expand left
        int left = col;
        while (left > 0 && isWordChar(getChar(globalLine, left - 1)))
            --left;

        // Expand right
        int right = col;
        while (right < m_grid->cols() - 1 && isWordChar(getChar(globalLine, right + 1)))
            ++right;

        m_selStart = QPoint(globalLine, left);
        m_selEnd = QPoint(globalLine, right);
        m_hasSelection = true;
        m_selecting = false;

        QString text = selectedText();
        if (!text.isEmpty()) {
            QApplication::clipboard()->setText(text, QClipboard::Selection);
        }

        update();
    }
}

bool TerminalWidget::hasSelection() const {
    return m_hasSelection;
}

void TerminalWidget::clearSelection() {
    m_hasSelection = false;
    m_selStart = QPoint(-1, -1);
    m_selEnd = QPoint(-1, -1);
}

bool TerminalWidget::isCellSelected(int globalLine, int col) const {
    if (!m_hasSelection) return false;

    // Normalize so start <= end
    QPoint s = m_selStart, e = m_selEnd;
    if (s.x() > e.x() || (s.x() == e.x() && s.y() > e.y()))
        std::swap(s, e);

    int sLine = s.x(), sCol = s.y();
    int eLine = e.x(), eCol = e.y();

    if (globalLine < sLine || globalLine > eLine) return false;
    if (sLine == eLine) return col >= sCol && col <= eCol;
    if (globalLine == sLine) return col >= sCol;
    if (globalLine == eLine) return col <= eCol;
    return true;  // middle line — fully selected
}

QString TerminalWidget::selectedText() const {
    if (!m_hasSelection) return {};

    QPoint s = m_selStart, e = m_selEnd;
    if (s.x() > e.x() || (s.x() == e.x() && s.y() > e.y()))
        std::swap(s, e);

    int scrollbackSize = m_grid->scrollbackSize();
    int rows = m_grid->rows();
    int cols = m_grid->cols();
    QString result;

    for (int gl = s.x(); gl <= e.x(); ++gl) {
        int startCol = (gl == s.x()) ? s.y() : 0;
        int endCol = (gl == e.x()) ? e.y() : cols - 1;

        QString line;
        for (int c = startCol; c <= endCol; ++c) {
            uint32_t cp = ' ';
            if (gl >= 0 && gl < scrollbackSize) {
                const auto &sbLine = m_grid->scrollbackLine(gl);
                if (c < static_cast<int>(sbLine.size()))
                    cp = sbLine[c].codepoint;
            } else {
                int sr = gl - scrollbackSize;
                if (sr >= 0 && sr < rows)
                    cp = m_grid->cellAt(sr, c).codepoint;
            }
            if (cp == 0) cp = ' ';
            line += QString::fromUcs4(reinterpret_cast<const char32_t *>(&cp), 1);
        }

        // Trim trailing spaces on each line
        while (line.endsWith(' ')) line.chop(1);
        result += line;
        if (gl < e.x()) result += '\n';
    }

    return result;
}

void TerminalWidget::copySelection() {
    if (!m_hasSelection) return;
    QString text = selectedText();
    if (!text.isEmpty()) {
        QApplication::clipboard()->setText(text);
    }
}

// --- URL Detection ---

QString TerminalWidget::lineText(int globalLine) const {
    int scrollbackSize = m_grid->scrollbackSize();
    int rows = m_grid->rows();
    int cols = m_grid->cols();
    QString text;
    text.reserve(cols);

    for (int c = 0; c < cols; ++c) {
        uint32_t cp = ' ';
        if (globalLine >= 0 && globalLine < scrollbackSize) {
            const auto &sbLine = m_grid->scrollbackLine(globalLine);
            if (c < static_cast<int>(sbLine.size()))
                cp = sbLine[c].codepoint;
        } else {
            int sr = globalLine - scrollbackSize;
            if (sr >= 0 && sr < rows)
                cp = m_grid->cellAt(sr, c).codepoint;
        }
        if (cp == 0) cp = ' ';
        text += QChar(cp < 0x10000 ? static_cast<char16_t>(cp) : ' ');
    }
    return text;
}

std::vector<TerminalWidget::UrlSpan> TerminalWidget::detectUrls(int globalLine) const {
    static QRegularExpression urlRe(
        R"((https?://|ftp://|file://)[^\s<>"'\)\]}{,;]+)",
        QRegularExpression::CaseInsensitiveOption
    );

    std::vector<UrlSpan> spans;
    QString text = lineText(globalLine);
    auto it = urlRe.globalMatch(text);
    while (it.hasNext()) {
        auto match = it.next();
        // Trim trailing punctuation that's likely not part of the URL
        QString url = match.captured();
        int endCol = match.capturedEnd() - 1;
        while (endCol > match.capturedStart() &&
               (url.endsWith('.') || url.endsWith(',') || url.endsWith(':'))) {
            url.chop(1);
            --endCol;
        }
        spans.push_back({static_cast<int>(match.capturedStart()), endCol, url});
    }
    return spans;
}

QString TerminalWidget::urlAtCell(int globalLine, int col) const {
    auto spans = detectUrls(globalLine);
    for (const auto &s : spans) {
        if (col >= s.startCol && col <= s.endCol)
            return s.url;
    }
    return {};
}

// --- Search ---

void TerminalWidget::showSearchBar() {
    m_searchVisible = true;
    m_searchBar->setGeometry(0, 0, width(), 36);
    m_searchBar->setStyleSheet(
        "QWidget#searchBar { background-color: rgba(49,50,68,240); border-bottom: 1px solid #585b70; }"
        "QLineEdit { background: #1e1e2e; color: #cdd6f4; border: 1px solid #585b70; border-radius: 4px; padding: 2px 6px; }"
        "QLabel { color: #a6adc8; }"
        "QPushButton { background: transparent; color: #a6adc8; border: none; font-size: 12px; }"
        "QPushButton:hover { color: #cdd6f4; }"
    );
    m_searchBar->show();
    m_searchInput->setFocus();
    m_searchInput->selectAll();
    recalcGridSize();
    update();
}

void TerminalWidget::hideSearchBar() {
    m_searchVisible = false;
    m_searchBar->hide();
    m_searchMatches.clear();
    m_currentMatchIdx = -1;
    m_searchText.clear();
    setFocus();
    recalcGridSize();
    update();
}

void TerminalWidget::performSearch() {
    m_searchText = m_searchInput->text();
    m_searchMatches.clear();
    m_currentMatchIdx = -1;

    if (m_searchText.isEmpty()) {
        m_searchLabel->setText("0/0");
        update();
        return;
    }

    int scrollbackSize = m_grid->scrollbackSize();
    int totalLines = scrollbackSize + m_grid->rows();

    for (int gl = 0; gl < totalLines; ++gl) {
        QString text = lineText(gl);
        int pos = 0;
        while ((pos = text.indexOf(m_searchText, pos, Qt::CaseInsensitive)) >= 0) {
            m_searchMatches.push_back({gl, pos, static_cast<int>(m_searchText.length())});
            pos += m_searchText.length();
        }
    }

    if (!m_searchMatches.empty()) {
        // Jump to first match near current view
        int viewCenter = scrollbackSize - m_scrollOffset + m_grid->rows() / 2;
        m_currentMatchIdx = 0;
        for (int i = 0; i < static_cast<int>(m_searchMatches.size()); ++i) {
            if (m_searchMatches[i].globalLine >= viewCenter) {
                m_currentMatchIdx = i;
                break;
            }
        }
        scrollToMatch();
    }

    m_searchLabel->setText(QString("%1/%2")
        .arg(m_searchMatches.empty() ? 0 : m_currentMatchIdx + 1)
        .arg(m_searchMatches.size()));
    update();
}

void TerminalWidget::scrollToMatch() {
    if (m_currentMatchIdx < 0) return;
    const auto &m = m_searchMatches[m_currentMatchIdx];
    int scrollbackSize = m_grid->scrollbackSize();
    int rows = m_grid->rows();
    // Calculate scroll offset to bring match into view
    int viewStart = scrollbackSize - m_scrollOffset;
    int viewEnd = viewStart + rows - 1;
    if (m.globalLine < viewStart || m.globalLine > viewEnd) {
        m_scrollOffset = std::clamp(scrollbackSize - m.globalLine + rows / 2,
                                     0, scrollbackSize);
    }
}

void TerminalWidget::searchNext() {
    if (m_searchMatches.empty()) return;
    m_currentMatchIdx = (m_currentMatchIdx + 1) % static_cast<int>(m_searchMatches.size());
    scrollToMatch();
    m_searchLabel->setText(QString("%1/%2").arg(m_currentMatchIdx + 1).arg(m_searchMatches.size()));
    update();
}

void TerminalWidget::searchPrev() {
    if (m_searchMatches.empty()) return;
    m_currentMatchIdx = (m_currentMatchIdx - 1 + static_cast<int>(m_searchMatches.size()))
                        % static_cast<int>(m_searchMatches.size());
    scrollToMatch();
    m_searchLabel->setText(QString("%1/%2").arg(m_currentMatchIdx + 1).arg(m_searchMatches.size()));
    update();
}

bool TerminalWidget::isCellSearchMatch(int globalLine, int col) const {
    for (const auto &m : m_searchMatches) {
        if (m.globalLine == globalLine && col >= m.col && col < m.col + m.length)
            return true;
    }
    return false;
}

bool TerminalWidget::isCellCurrentMatch(int globalLine, int col) const {
    if (m_currentMatchIdx < 0 || m_currentMatchIdx >= static_cast<int>(m_searchMatches.size()))
        return false;
    const auto &m = m_searchMatches[m_currentMatchIdx];
    return m.globalLine == globalLine && col >= m.col && col < m.col + m.length;
}

// --- Bracket Matching ---

uint32_t TerminalWidget::getCellCodepoint(int globalLine, int col) const {
    int scrollbackSize = m_grid->scrollbackSize();
    int rows = m_grid->rows();
    if (globalLine >= 0 && globalLine < scrollbackSize) {
        const auto &line = m_grid->scrollbackLine(globalLine);
        if (col < static_cast<int>(line.size()))
            return line[col].codepoint;
    } else {
        int sr = globalLine - scrollbackSize;
        if (sr >= 0 && sr < rows && col >= 0 && col < m_grid->cols())
            return m_grid->cellAt(sr, col).codepoint;
    }
    return 0;
}

TerminalWidget::BracketPair TerminalWidget::findMatchingBracket() const {
    int scrollbackSize = m_grid->scrollbackSize();
    int cursorGL = scrollbackSize + m_grid->cursorRow();
    int cursorCol = m_grid->cursorCol();

    uint32_t ch = getCellCodepoint(cursorGL, cursorCol);

    char32_t openBrackets[]  = {'(', '[', '{'};
    char32_t closeBrackets[] = {')', ']', '}'};

    // Check if cursor is on an opening bracket
    for (int i = 0; i < 3; ++i) {
        if (ch == openBrackets[i]) {
            // Search forward for matching close bracket
            int depth = 1;
            int gl = cursorGL, col = cursorCol + 1;
            int totalLines = scrollbackSize + m_grid->rows();
            int cols = m_grid->cols();
            while (gl < totalLines && depth > 0) {
                if (col >= cols) { col = 0; ++gl; continue; }
                uint32_t c = getCellCodepoint(gl, col);
                if (c == openBrackets[i]) ++depth;
                else if (c == closeBrackets[i]) --depth;
                if (depth == 0) return {cursorGL, cursorCol, gl, col};
                ++col;
            }
            return {-1, -1, -1, -1};
        }
    }

    // Check if cursor is on a closing bracket
    for (int i = 0; i < 3; ++i) {
        if (ch == closeBrackets[i]) {
            // Search backward for matching open bracket
            int depth = 1;
            int gl = cursorGL, col = cursorCol - 1;
            int cols = m_grid->cols();
            while (gl >= 0 && depth > 0) {
                if (col < 0) { --gl; col = cols - 1; continue; }
                uint32_t c = getCellCodepoint(gl, col);
                if (c == closeBrackets[i]) ++depth;
                else if (c == openBrackets[i]) --depth;
                if (depth == 0) return {gl, col, cursorGL, cursorCol};
                --col;
            }
            return {-1, -1, -1, -1};
        }
    }

    return {-1, -1, -1, -1};
}

bool TerminalWidget::isCellBracketHighlight(int globalLine, int col) const {
    return (globalLine == m_bracketMatch.line1 && col == m_bracketMatch.col1) ||
           (globalLine == m_bracketMatch.line2 && col == m_bracketMatch.col2);
}
