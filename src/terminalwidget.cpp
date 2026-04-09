#include "terminalwidget.h"
#include "glrenderer.h"

#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <QEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QInputMethodEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QHBoxLayout>
#include <QPushButton>
#include <QMenu>
#include <QDesktopServices>
#include <QUrl>
#include <QProcess>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QSurfaceFormat>
#include <QOpenGLFunctions>
#include <QFileDialog>
#include <QTextStream>
#include <QJsonObject>

TerminalWidget::TerminalWidget(QWidget *parent) : QOpenGLWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Request alpha buffer for per-pixel transparency
    // Only request Core Profile if GPU rendering is needed (QPainter works
    // best without it — Core Profile causes font scaling bugs on mismatched DPI)
    QSurfaceFormat fmt = format();
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);


    // Pick a good monospace font
    QStringList families = {"JetBrains Mono", "Fira Code", "Source Code Pro",
                            "DejaVu Sans Mono", "Liberation Mono", "Monospace"};
    m_font = QFont(families);
    m_font.setPointSize(11);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    // Enable font features (ligatures)
    m_font.setStyleStrategy(QFont::PreferDefault);

    updateFontMetrics();

    // Initial grid
    m_grid = std::make_unique<TerminalGrid>(24, 80);

    // Response callback: DA, CPR replies go back to PTY; OSC 52 sets clipboard
    m_grid->setResponseCallback([this](const std::string &response) {
        if (response.size() > 6 && response.compare(1, 6, "OSC52:") == 0) {
            // OSC 52 clipboard set (prefixed with \0OSC52:)
            QString text = QString::fromUtf8(response.data() + 7,
                                              static_cast<int>(response.size()) - 7);
            QApplication::clipboard()->setText(text);
            return;
        }
        if (m_pty)
            m_pty->write(QByteArray(response.data(), static_cast<int>(response.size())));
    });

    m_parser = std::make_unique<VtParser>([this](const VtAction &a) {
        m_grid->processAction(a);
    });

    // Font fallback for emoji/symbols
    QStringList fallbackFamilies = {"Noto Color Emoji", "Noto Emoji", "Symbola",
                                     "Noto Sans CJK SC", "Noto Sans CJK", "WenQuanYi Micro Hei"};
    for (const QString &family : fallbackFamilies) {
        QFont test(family);
        QFontMetrics fm(test);
        if (fm.horizontalAdvance(QChar(0x2603)) > 0) { // snowman test
            m_fallbackFont = test;
            m_fallbackFont.setPointSize(m_font.pointSize());
            m_hasFallbackFont = true;
            break;
        }
    }

    // Cursor blink timer
    m_cursorTimer.setInterval(530);
    connect(&m_cursorTimer, &QTimer::timeout, this, &TerminalWidget::blinkCursor);
    m_cursorTimer.start();

    // Idle notification timer (check every 2 seconds)
    m_idleTimer.setInterval(2000);
    connect(&m_idleTimer, &QTimer::timeout, this, &TerminalWidget::checkIdleNotification);
    m_idleTimer.start();
    m_lastOutputTime.start();

    // Claude Code permission detection (debounced)
    m_claudeDetectTimer.setSingleShot(true);
    m_claudeDetectTimer.setInterval(300);
    connect(&m_claudeDetectTimer, &QTimer::timeout, this, &TerminalWidget::checkForClaudePermissionPrompt);

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
    // Clean up GL resources while context is still valid
    if (m_glRenderer) {
        makeCurrent();
        m_glRenderer->cleanup();
        doneCurrent();
    }
    if (m_logFile)
        m_logFile->close();
}

void TerminalWidget::updateFontMetrics() {
    QFontMetrics fm(m_font);
    m_cellWidth = fm.horizontalAdvance('M');
    m_cellHeight = fm.height();
    m_fontAscent = fm.ascent();

    // Create bold/italic variants
    m_fontBold = m_font;
    m_fontBold.setBold(true);
    m_fontItalic = m_font;
    m_fontItalic.setItalic(true);
    m_fontBoldItalic = m_font;
    m_fontBoldItalic.setBold(true);
    m_fontBoldItalic.setItalic(true);
}

void TerminalWidget::setFontSize(int size) {
    size = qBound(4, size, 48);
    m_font.setPointSize(size);
    if (m_hasFallbackFont) m_fallbackFont.setPointSize(size);
    updateFontMetrics();
    recalcGridSize();
    update();
}

int TerminalWidget::fontSize() const {
    return m_font.pointSize();
}

void TerminalWidget::setMaxScrollback(int lines) {
    m_grid->setMaxScrollback(lines);
}

void TerminalWidget::setSessionLogging(bool enabled) {
    m_loggingEnabled = enabled;
    if (enabled && !m_logFile) {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                      + "/ants-terminal/logs";
        QDir().mkpath(dir);
        QString filename = dir + "/session_"
                           + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")
                           + ".log";
        m_logFile = std::make_unique<QFile>(filename);
        m_logFile->open(QIODevice::WriteOnly | QIODevice::Append);
    } else if (!enabled && m_logFile) {
        m_logFile->close();
        m_logFile.reset();
    }
}

void TerminalWidget::sendToPty(const QByteArray &data) {
    if (m_pty)
        m_pty->write(data);
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
                                       const QColor &cursorColor,
                                       const QColor &accent,
                                       const QColor &border) {
    m_grid->setDefaultFg(fg);
    m_grid->setDefaultBg(bg);
    m_cursorColor = cursorColor;

    // Derive highlight colors from theme
    QColor accentColor = accent.isValid() ? accent : cursorColor;
    m_selectionBg = accentColor;
    m_selectionFg = bg;
    m_urlColor = accentColor;
    m_bracketHighlightBg = border.isValid() ? border : accentColor.darker(150);

    // Search highlights: derive from accent
    m_searchHighlightBg = accentColor.lighter(140);
    m_searchHighlightFg = bg;
    m_searchCurrentBg = accentColor;
    m_searchCurrentFg = bg;

    // Update search bar colors if visible
    if (m_searchVisible) {
        m_searchBar->setStyleSheet(QString(
            "QWidget#searchBar { background-color: %1; border-bottom: 1px solid %2; }"
            "QLineEdit { background: %3; color: %4; border: 1px solid %2; border-radius: 4px; padding: 2px 6px; }"
            "QLabel { color: %5; }"
            "QPushButton { background: transparent; color: %5; border: none; font-size: 12px; }"
            "QPushButton:hover { color: %4; }"
        ).arg(bg.darker(110).name(), border.isValid() ? border.name() : bg.lighter(150).name(),
              bg.name(), fg.name(), fg.darker(130).name()));
    }

    QPalette pal = palette();
    pal.setColor(QPalette::Window, bg);
    pal.setColor(QPalette::Base, bg);
    setPalette(pal);

    update();
}

bool TerminalWidget::event(QEvent *event) {
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
            keyPressEvent(ke);
            return true;
        }
    }
    return QOpenGLWidget::event(event);
}

void TerminalWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setFont(m_font);

    const int rows = m_grid->rows();
    const int cols = m_grid->cols();
    const QColor defaultBg = m_grid->defaultBg();

    // Per-pixel alpha for terminal background transparency (driven by Opacity setting).
    // Must use CompositionMode_Source to REPLACE the FBO content rather than blend
    // with it — QOpenGLWidget reuses its FBO across frames, so SourceOver would
    // accumulate alpha toward 1.0 (opaque) over successive paints.
    QColor bgFill = defaultBg;
    int effectiveAlpha = static_cast<int>(255 * m_windowOpacity);
    if (effectiveAlpha < 255)
        bgFill.setAlpha(effectiveAlpha);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), bgFill);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    int scrollbackSize = m_grid->scrollbackSize();
    int viewStart = scrollbackSize - m_scrollOffset;

    BracketPair bp = (m_scrollOffset == 0) ? findMatchingBracket() : BracketPair{-1,-1,-1,-1};

    for (int vr = 0; vr < rows; ++vr) {
        int globalLine = viewStart + vr;
        int px_y = m_padding + vr * m_cellHeight;

        auto urlSpans = detectUrls(globalLine);

        // Check highlight rules for this line
        struct HighlightSpan { int start; int end; QColor fg; QColor bg; };
        std::vector<HighlightSpan> hlSpans;
        if (!m_highlightRules.empty()) {
            QString lt = lineText(globalLine);
            for (const auto &rule : m_highlightRules) {
                auto it = rule.pattern.globalMatch(lt);
                while (it.hasNext()) {
                    auto m = it.next();
                    hlSpans.push_back({static_cast<int>(m.capturedStart()),
                                       static_cast<int>(m.capturedStart() + m.capturedLength() - 1),
                                       rule.fg, rule.bg});
                }
            }
        }

        // --- Ligature-aware text rendering ---
        // We accumulate runs of same-attribute cells and draw them together
        // to let Qt apply font ligatures (JetBrains Mono, Fira Code, etc.)
        struct TextRun {
            int startCol;
            QString text;
            QColor fg;
            QColor bg;
            bool bold;
            bool italic;
            bool underline;
            bool strikethrough;
            bool isUrl;
        };
        std::vector<TextRun> runs;
        TextRun current{};
        current.startCol = 0;
        bool currentValid = false;

        for (int col = 0; col < cols; ++col) {
            int px_x = m_padding + col * m_cellWidth;

            const Cell &c = cellAtGlobal(globalLine, col);
            QColor fg = c.attrs.fg;
            QColor bg = c.attrs.bg;
            if (c.attrs.inverse) std::swap(fg, bg);
            if (c.attrs.dim) fg = fg.darker(150);

            bool isUrl = false;
            for (const auto &u : urlSpans) {
                if (col >= u.startCol && col <= u.endCol) {
                    isUrl = true;
                    break;
                }
            }

            // Apply highlight rules
            for (const auto &hl : hlSpans) {
                if (col >= hl.start && col <= hl.end) {
                    if (hl.fg.isValid()) fg = hl.fg;
                    if (hl.bg.isValid()) bg = hl.bg;
                    break;
                }
            }

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

            // Paint cell background (wide chars get double-width bg)
            int cellDrawWidth = c.isWideChar ? m_cellWidth * 2 : m_cellWidth;
            if (bg != defaultBg) {
                QColor cellBg = bg;
                if (effectiveAlpha < 255 && !selected && !isCellSearchMatch(globalLine, col))
                    cellBg.setAlpha(effectiveAlpha);
                p.fillRect(px_x, px_y, cellDrawWidth, m_cellHeight, cellBg);
            }

            // Skip continuation cells of wide characters (already drawn)
            if (c.isWideCont) continue;

            // Determine if this cell continues the current run
            bool sameAttrs = currentValid &&
                fg == current.fg && bg == current.bg &&
                c.attrs.bold == current.bold &&
                c.attrs.italic == current.italic &&
                c.attrs.underline == current.underline &&
                c.attrs.strikethrough == current.strikethrough &&
                isUrl == current.isUrl;

            if (c.codepoint != ' ' && c.codepoint != 0) {
                uint32_t baseCp = c.codepoint;
                QString ch = QString::fromUcs4(reinterpret_cast<const char32_t *>(&baseCp), 1);
                // Append combining characters
                if (auto *comb = combiningAt(globalLine, col)) {
                    for (uint32_t combCp : *comb)
                        ch += QString::fromUcs4(reinterpret_cast<const char32_t *>(&combCp), 1);
                }
                if (sameAttrs) {
                    current.text += ch;
                } else {
                    if (currentValid && !current.text.isEmpty()) {
                        runs.push_back(current);
                    }
                    current = TextRun{};
                    current.startCol = col;
                    current.text = ch;
                    current.fg = fg;
                    current.bg = bg;
                    current.bold = c.attrs.bold;
                    current.italic = c.attrs.italic;
                    current.underline = c.attrs.underline;
                    current.strikethrough = c.attrs.strikethrough;
                    current.isUrl = isUrl;
                    currentValid = true;
                }
            } else {
                // Space or null breaks the run
                if (currentValid && !current.text.isEmpty()) {
                    runs.push_back(current);
                    currentValid = false;
                }
            }

            // Underline rendering (supports multiple styles + custom color)
            if (c.attrs.underline || isUrl) {
                QColor ulColor = c.attrs.underlineColor.isValid() ? c.attrs.underlineColor : fg;
                int ulY = px_y + m_cellHeight - 2;
                int ulW = c.isWideChar ? m_cellWidth * 2 : m_cellWidth;
                UnderlineStyle style = isUrl ? UnderlineStyle::Single : c.attrs.underlineStyle;
                if (style == UnderlineStyle::None) style = UnderlineStyle::Single;

                p.setPen(ulColor);
                switch (style) {
                case UnderlineStyle::Single:
                    p.drawLine(px_x, ulY, px_x + ulW - 1, ulY);
                    break;
                case UnderlineStyle::Double:
                    p.drawLine(px_x, ulY, px_x + ulW - 1, ulY);
                    p.drawLine(px_x, ulY - 2, px_x + ulW - 1, ulY - 2);
                    break;
                case UnderlineStyle::Curly: {
                    QPainterPath path;
                    path.moveTo(px_x, ulY);
                    for (int x = 0; x < ulW; x += 4) {
                        int x0 = px_x + x;
                        path.cubicTo(x0 + 1, ulY - 2, x0 + 3, ulY + 2, x0 + 4, ulY);
                    }
                    p.setBrush(Qt::NoBrush);
                    p.drawPath(path);
                    break;
                }
                case UnderlineStyle::Dotted: {
                    QPen dotPen(ulColor, 1, Qt::DotLine);
                    p.setPen(dotPen);
                    p.drawLine(px_x, ulY, px_x + ulW - 1, ulY);
                    p.setPen(ulColor);
                    break;
                }
                case UnderlineStyle::Dashed: {
                    QPen dashPen(ulColor, 1, Qt::DashLine);
                    p.setPen(dashPen);
                    p.drawLine(px_x, ulY, px_x + ulW - 1, ulY);
                    p.setPen(ulColor);
                    break;
                }
                default: break;
                }
            }
            if (c.attrs.strikethrough) {
                p.setPen(fg);
                int sy = px_y + m_cellHeight / 2;
                p.drawLine(px_x, sy, px_x + m_cellWidth - 1, sy);
            }
        }

        // Flush last run
        if (currentValid && !current.text.isEmpty()) {
            runs.push_back(current);
        }

        // Draw all text runs using QTextLayout for proper ligature shaping
        for (const auto &run : runs) {
            const QFont *drawFont = &m_font;
            if (run.bold && run.italic) drawFont = &m_fontBoldItalic;
            else if (run.bold) drawFont = &m_fontBold;
            else if (run.italic) drawFont = &m_fontItalic;

            p.setPen(run.fg);
            int px_x = m_padding + run.startCol * m_cellWidth;

            // Use QTextLayout for HarfBuzz-powered ligature shaping
            QTextLayout layout(run.text, *drawFont);
            layout.beginLayout();
            QTextLine tline = layout.createLine();
            if (tline.isValid()) {
                tline.setLineWidth(run.text.length() * m_cellWidth * 2); // generous width
                tline.setPosition(QPointF(0, 0));
            }
            layout.endLayout();
            layout.draw(&p, QPointF(px_x, px_y));
        }
    }

    // Draw bookmark markers in left gutter
    if (!m_bookmarks.empty()) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_cursorColor);
        for (int bm : m_bookmarks) {
            int vr = bm - viewStart;
            if (vr >= 0 && vr < rows) {
                int py = m_padding + vr * m_cellHeight + m_cellHeight / 2;
                p.drawEllipse(QPoint(2, py), 2, 2);
            }
        }
    }

    // Draw inline images
    for (const auto &img : m_grid->inlineImages()) {
        int px_x = m_padding + img.col * m_cellWidth;
        int imgRow = img.row;
        int screenGlobalLine = scrollbackSize + imgRow;
        int vr = screenGlobalLine - viewStart;
        if (vr < 0 || vr >= rows) continue;
        int px_y = m_padding + vr * m_cellHeight;
        int pw, ph;
        if (img.pixelSized) {
            // Sixel/Kitty: dimensions are pixels, render at actual size
            pw = img.cellWidth;
            ph = img.cellHeight;
        } else {
            // iTerm2/cell-based: dimensions are in cells
            pw = img.cellWidth * m_cellWidth;
            ph = img.cellHeight * m_cellHeight;
        }
        p.drawImage(QRect(px_x, px_y, pw, ph), img.image);
    }

    // Cursor — shape-aware (DECSCUSR: block, underline, bar)
    if (m_scrollOffset == 0 && m_grid->cursorVisible() && (m_cursorBlinkOn || !m_hasFocus)) {
        int cx = m_padding + m_grid->cursorCol() * m_cellWidth;
        int cy = m_padding + m_grid->cursorRow() * m_cellHeight;
        CursorShape shape = m_grid->cursorShape();

        if (m_hasFocus) {
            switch (shape) {
            case CursorShape::BlinkBar:
            case CursorShape::SteadyBar:
                // Thin vertical bar (2px wide)
                p.fillRect(cx, cy, 2, m_cellHeight, m_cursorColor);
                break;
            case CursorShape::BlinkUnderline:
            case CursorShape::SteadyUnderline:
                // Underline cursor (2px tall at baseline)
                p.fillRect(cx, cy + m_cellHeight - 2, m_cellWidth, 2, m_cursorColor);
                break;
            default: // Block cursor (default)
                p.fillRect(cx, cy, m_cellWidth, m_cellHeight, m_cursorColor);
                {
                    int cursorGL = scrollbackSize + m_grid->cursorRow();
                    QString cursorText = cellText(cursorGL, m_grid->cursorCol());
                    if (!cursorText.isEmpty()) {
                        p.setPen(m_grid->defaultBg());
                        p.drawText(cx, cy + m_fontAscent, cursorText);
                    }
                }
                break;
            }
        } else {
            p.setPen(m_cursorColor);
            p.setBrush(Qt::NoBrush);
            p.drawRect(cx, cy, m_cellWidth - 1, m_cellHeight - 1);
        }
    }

    // Autocomplete ghost text (drawn after cursor, in dim text)
    if (m_scrollOffset == 0 && !m_currentSuggestion.isEmpty()) {
        int cursorCol = m_grid->cursorCol();
        int gx = m_padding + cursorCol * m_cellWidth;
        int gy = m_padding + m_grid->cursorRow() * m_cellHeight;

        QColor ghost = m_grid->defaultFg();
        ghost.setAlpha(80);
        p.setPen(ghost);
        p.setFont(m_font);

        // Truncate suggestion to remaining columns
        int remaining = cols - cursorCol;
        QString text = m_currentSuggestion.left(remaining);
        p.drawText(gx, gy + m_fontAscent, text);
    }
}

void TerminalWidget::keyPressEvent(QKeyEvent *event) {
    m_scrollOffset = 0;
    m_cursorBlinkOn = true;
    m_cursorTimer.start();

    QByteArray data;
    int key = event->key();
    Qt::KeyboardModifiers mods = event->modifiers();

    // Ctrl+Shift+V -- paste
    if (key == Qt::Key_V && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        const QClipboard *clipboard = QApplication::clipboard();
        const QMimeData *mime = clipboard->mimeData();
        if (mime->hasImage()) {
            QImage img = clipboard->image();
            if (!img.isNull()) {
                // Auto-save image and insert filepath into terminal
                QString dir = m_imagePasteDir;
                if (dir.isEmpty())
                    dir = QDir::homePath() + "/Pictures/ClaudePaste";
                QDir().mkpath(dir);
                QString filename = dir + "/paste_"
                    + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz")
                    + ".png";
                if (img.save(filename)) {
                    // Insert the filepath into the terminal
                    if (m_pty) m_pty->write(filename.toUtf8());
                }
                emit imagePasted(img);
                return;
            }
        }
        if (mime->hasText() && m_pty) {
            QString text = clipboard->text();
            data = text.toUtf8();
            if (m_grid->bracketedPaste()) {
                m_pty->write(QByteArray("\x1B[200~"));
                m_pty->write(data);
                m_pty->write(QByteArray("\x1B[201~"));
            } else {
                m_pty->write(data);
            }
        }
        return;
    }

    // Ctrl+Shift+C -- copy selection
    if (key == Qt::Key_C && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        copySelection();
        return;
    }

    // Ctrl+Shift+F -- search
    if (key == Qt::Key_F && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        showSearchBar();
        return;
    }

    // Ctrl+Shift+Up/Down -- prompt navigation (OSC 133)
    if ((mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        if (key == Qt::Key_Up) {
            navigatePrompt(-1);
            return;
        }
        if (key == Qt::Key_Down) {
            navigatePrompt(1);
            return;
        }
        // Ctrl+Shift+B -- toggle bookmark
        if (key == Qt::Key_B) {
            toggleBookmark();
            return;
        }
        // Ctrl+Shift+J / Ctrl+Shift+K -- navigate bookmarks
        if (key == Qt::Key_J) {
            nextBookmark();
            return;
        }
        if (key == Qt::Key_K) {
            prevBookmark();
            return;
        }
    }


    // Escape -- close search bar
    if (key == Qt::Key_Escape && m_searchVisible) {
        hideSearchBar();
        return;
    }

    // Shift+Enter -- literal newline
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && (mods & Qt::ShiftModifier)) {
        if (m_pty) {
            data = QByteArray("\x16\n", 2);
            m_pty->write(data);
        }
        return;
    }

    // Ctrl+key combinations
    if (mods & Qt::ControlModifier && !(mods & Qt::ShiftModifier)) {
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            char ch = static_cast<char>(key - Qt::Key_A + 1);
            data.append(ch);
            m_pty->write(data);
            return;
        }
        if (key == Qt::Key_BracketLeft) { data = "\x1B"; m_pty->write(data); return; }
        if (key == Qt::Key_Backslash)   { data = "\x1C"; m_pty->write(data); return; }
        if (key == Qt::Key_BracketRight){ data = "\x1D"; m_pty->write(data); return; }
    }

    // Accept autocomplete suggestion with Right arrow (only when suggestion is showing)
    if (key == Qt::Key_Right && !m_currentSuggestion.isEmpty() && m_pty &&
        !(mods & Qt::ControlModifier) && !(mods & Qt::ShiftModifier)) {
        // Check if cursor is at the end of input (no printable char ahead)
        int cursorLine = m_grid->scrollbackSize() + m_grid->cursorRow();
        int cursorCol = m_grid->cursorCol();
        const Cell &nextCell = cellAtGlobal(cursorLine, cursorCol);
        if (nextCell.codepoint == ' ' || nextCell.codepoint == 0) {
            m_pty->write(m_currentSuggestion.toUtf8());
            m_currentSuggestion.clear();
            update();
            return;
        }
    }

    // Clear suggestion on any non-navigation key
    if (!m_currentSuggestion.isEmpty() &&
        key != Qt::Key_Right && key != Qt::Key_Left &&
        key != Qt::Key_Shift && key != Qt::Key_Control && key != Qt::Key_Alt) {
        m_currentSuggestion.clear();
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
    case Qt::Key_Backtab:
        data = "\x1B[Z";
        break;
    case Qt::Key_Escape:
        data = "\x1B";
        break;
    case Qt::Key_Up:
        data = m_grid->applicationCursorKeys() ? "\x1BOA" : "\x1B[A";
        break;
    case Qt::Key_Down:
        data = m_grid->applicationCursorKeys() ? "\x1BOB" : "\x1B[B";
        break;
    case Qt::Key_Right:
        data = m_grid->applicationCursorKeys() ? "\x1BOC" : "\x1B[C";
        break;
    case Qt::Key_Left:
        data = m_grid->applicationCursorKeys() ? "\x1BOD" : "\x1B[D";
        break;
    case Qt::Key_Home:
        if (mods & Qt::ShiftModifier) {
            m_scrollOffset = m_grid->scrollbackSize();
            update();
            return;
        }
        data = m_grid->applicationCursorKeys() ? "\x1BOH" : "\x1B[H";
        break;
    case Qt::Key_End:
        if (mods & Qt::ShiftModifier) {
            m_scrollOffset = 0;
            update();
            return;
        }
        data = m_grid->applicationCursorKeys() ? "\x1BOF" : "\x1B[F";
        break;
    case Qt::Key_Insert:
        data = "\x1B[2~";
        break;
    case Qt::Key_Delete:
        data = "\x1B[3~";
        break;
    case Qt::Key_PageUp:
        if (mods & Qt::ShiftModifier) {
            m_scrollOffset = std::min(m_scrollOffset + m_grid->rows(),
                                       m_grid->scrollbackSize());
            update();
            return;
        }
        data = "\x1B[5~";
        break;
    case Qt::Key_PageDown:
        if (mods & Qt::ShiftModifier) {
            m_scrollOffset = std::max(m_scrollOffset - m_grid->rows(), 0);
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
        {
            QString text = event->text();
            if (!text.isEmpty()) {
                data = text.toUtf8();
            }
        }
        break;
    }

    if (!data.isEmpty() && m_pty) {
        m_pty->write(data);
        // Broadcast to other terminals if callback is set
        if (m_broadcastCallback)
            m_broadcastCallback(this, data);
    }
}

void TerminalWidget::resizeEvent(QResizeEvent *event) {
    // QOpenGLWidget::resizeEvent recreates the internal FBO at the new size.
    // Without this call, the FBO stays at its initial size and gets stretched.
    QOpenGLWidget::resizeEvent(event);
    recalcGridSize();
}

void TerminalWidget::focusInEvent(QFocusEvent *) {
    m_hasFocus = true;
    m_cursorBlinkOn = true;
    m_cursorTimer.start();
    if (m_grid->focusReporting() && m_pty)
        m_pty->write(QByteArray("\x1B[I"));
    update();
}

void TerminalWidget::focusOutEvent(QFocusEvent *) {
    m_hasFocus = false;
    m_cursorBlinkOn = false;
    m_cursorTimer.stop();
    if (m_grid->focusReporting() && m_pty)
        m_pty->write(QByteArray("\x1B[O"));
    update();
}

void TerminalWidget::wheelEvent(QWheelEvent *event) {
    // Mouse wheel reporting to application
    if (mouseReportingActive() && !(event->modifiers() & Qt::ShiftModifier)) {
        QPoint cellPos = pixelToCell(event->position().toPoint());
        int col = cellPos.y() + 1;
        int row = cellPos.x() - m_grid->scrollbackSize() + m_scrollOffset + 1;
        if (row >= 1 && row <= m_grid->rows()) {
            int delta = event->angleDelta().y();
            int button = (delta > 0) ? 64 : 65; // 64=wheel up, 65=wheel down
            if (m_grid->mouseSgrMode()) {
                QString seq = QString("\x1B[<%1;%2;%3M").arg(button).arg(col).arg(row);
                m_pty->write(seq.toUtf8());
            } else {
                char cb = static_cast<char>(button + 32);
                char cx = static_cast<char>(col + 32);
                char cy = static_cast<char>(row + 32);
                QByteArray seq;
                seq.append("\x1B[M");
                seq.append(cb);
                seq.append(cx);
                seq.append(cy);
                m_pty->write(seq);
            }
            return;
        }
    }

    int delta = event->angleDelta().y();
    if (delta > 0) {
        m_scrollOffset = std::min(m_scrollOffset + 3, m_grid->scrollbackSize());
    } else if (delta < 0) {
        m_scrollOffset = std::max(m_scrollOffset - 3, 0);
    }
    update();
}

void TerminalWidget::inputMethodEvent(QInputMethodEvent *event) {
    if (!event->commitString().isEmpty() && m_pty) {
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
    return QOpenGLWidget::inputMethodQuery(query);
}

void TerminalWidget::onPtyData(const QByteArray &data) {
    m_parser->feed(data.constData(), data.size());

    // Session logging
    if (m_loggingEnabled && m_logFile && m_logFile->isOpen()) {
        m_logFile->write(data);
        m_logFile->flush();
    }

    // Asciicast recording
    if (m_recording && m_recordFile && m_recordFile->isOpen()) {
        double elapsed = m_recordTimer.elapsed() / 1000.0;
        // Escape the data for JSON
        QString escaped = QString::fromUtf8(data);
        escaped.replace('\\', "\\\\");
        escaped.replace('"', "\\\"");
        escaped.replace('\n', "\\n");
        escaped.replace('\r', "\\r");
        escaped.replace('\t', "\\t");
        // Escape control characters
        for (int i = 0; i < escaped.size(); ++i) {
            QChar ch = escaped[i];
            if (ch.unicode() < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') {
                QString esc = QString("\\u%1").arg(static_cast<int>(ch.unicode()), 4, 16, QChar('0'));
                escaped.replace(i, 1, esc);
                i += esc.size() - 1;
            }
        }
        QString line = QString("[%1, \"o\", \"%2\"]\n")
            .arg(elapsed, 0, 'f', 6)
            .arg(escaped);
        m_recordFile->write(line.toUtf8());
    }

    // Track output for idle notification
    m_lastOutputTime.restart();
    if (!m_hadRecentOutput)
        m_commandStartTime.restart();
    m_hadRecentOutput = true;
    m_notifiedIdle = false;

    QString title = m_grid->windowTitle();
    if (title != m_lastTitle) {
        m_lastTitle = title;
        emit titleChanged(title);
    }

    // Synchronized output: defer screen update until mode is turned off
    bool wasSync = m_syncOutputActive;
    m_syncOutputActive = m_grid->synchronizedOutput();
    if (!m_syncOutputActive || !wasSync)
        update();

    // Restart Claude Code detection debounce timer
    m_claudeDetectTimer.start();

    // Check for failed command (OSC 133 exit code)
    int exitCode = m_grid->lastExitCode();
    if (exitCode != 0 && exitCode != m_lastTrackedExitCode) {
        m_lastTrackedExitCode = exitCode;
        emit commandFailed(exitCode, lastCommandOutput());
    } else if (exitCode == 0) {
        m_lastTrackedExitCode = 0;
    }

    // Check triggers
    checkTriggers(data);

    // Update autocomplete suggestion
    updateSuggestion();
}

void TerminalWidget::onPtyFinished(int exitCode) {
    emit shellExited(exitCode);
}

void TerminalWidget::blinkCursor() {
    m_cursorBlinkOn = !m_cursorBlinkOn;
    int cx = m_padding + m_grid->cursorCol() * m_cellWidth;
    int cy = m_padding + m_grid->cursorRow() * m_cellHeight;
    update(cx, cy, m_cellWidth, m_cellHeight);
}

void TerminalWidget::checkIdleNotification() {
    if (m_notifiedIdle || !m_hadRecentOutput) return;

    // If no output for 3 seconds after a burst, and window is not focused
    if (m_lastOutputTime.elapsed() > 3000) {
        m_hadRecentOutput = false;
        if (!m_hasFocus && !m_notifiedIdle) {
            m_notifiedIdle = true;

            // Try to get command info from OSC 133 prompt regions
            QString body = "A command has finished in Ants Terminal";
            qint64 elapsed = m_commandStartTime.isValid()
                ? m_commandStartTime.elapsed() / 1000 : 0;
            if (elapsed > 0) {
                int mins = static_cast<int>(elapsed / 60);
                int secs = static_cast<int>(elapsed % 60);
                if (mins > 0)
                    body = QString("Finished after %1m %2s").arg(mins).arg(secs);
                else
                    body = QString("Finished after %1s").arg(secs);
            }

            QProcess::startDetached("notify-send", {
                "-a", "Ants Terminal",
                "-i", "utilities-terminal",
                "Command Complete",
                body
            });
        }
    }
}

void TerminalWidget::navigatePrompt(int direction) {
    const auto &regions = m_grid->promptRegions();
    if (regions.empty()) return;

    int scrollbackSize = m_grid->scrollbackSize();
    // Current view top in global line coordinates
    int viewTop = scrollbackSize - m_scrollOffset;

    if (direction < 0) {
        // Find the prompt region whose startLine is above current viewTop
        for (int i = static_cast<int>(regions.size()) - 1; i >= 0; --i) {
            if (regions[i].startLine < viewTop - 1) {
                m_scrollOffset = scrollbackSize - regions[i].startLine;
                m_scrollOffset = std::clamp(m_scrollOffset, 0, scrollbackSize);
                update();
                return;
            }
        }
        // Already at topmost prompt — scroll to start
        m_scrollOffset = scrollbackSize;
        update();
    } else {
        // Find the prompt region whose startLine is below current viewTop
        for (const auto &region : regions) {
            if (region.startLine > viewTop + 1) {
                m_scrollOffset = scrollbackSize - region.startLine;
                m_scrollOffset = std::clamp(m_scrollOffset, 0, scrollbackSize);
                update();
                return;
            }
        }
        // Already at bottommost prompt — scroll to end
        m_scrollOffset = 0;
        update();
    }
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

QPoint TerminalWidget::pixelToCell(const QPoint &pos) const {
    int col = std::clamp((pos.x() - m_padding) / m_cellWidth, 0, m_grid->cols() - 1);
    int vr = std::clamp((pos.y() - m_padding) / m_cellHeight, 0, m_grid->rows() - 1);
    int globalLine = m_grid->scrollbackSize() - m_scrollOffset + vr;
    return QPoint(globalLine, col);
}

const Cell &TerminalWidget::cellAtGlobal(int globalLine, int col) const {
    static const Cell s_blankCell{' ', {}};
    int scrollbackSize = m_grid->scrollbackSize();
    if (globalLine >= 0 && globalLine < scrollbackSize) {
        const auto &line = m_grid->scrollbackLine(globalLine);
        if (col >= 0 && col < static_cast<int>(line.size()))
            return line[col];
    } else {
        int sr = globalLine - scrollbackSize;
        if (sr >= 0 && sr < m_grid->rows() && col >= 0 && col < m_grid->cols())
            return m_grid->cellAt(sr, col);
    }
    return s_blankCell;
}

const std::vector<uint32_t> *TerminalWidget::combiningAt(int globalLine, int col) const {
    int scrollbackSize = m_grid->scrollbackSize();
    const std::unordered_map<int, std::vector<uint32_t>> *map = nullptr;
    if (globalLine >= 0 && globalLine < scrollbackSize) {
        map = &m_grid->scrollbackCombining(globalLine);
    } else {
        int sr = globalLine - scrollbackSize;
        if (sr >= 0 && sr < m_grid->rows())
            map = &m_grid->screenCombining(sr);
    }
    if (map) {
        auto it = map->find(col);
        if (it != map->end())
            return &it->second;
    }
    return nullptr;
}

QString TerminalWidget::cellText(int globalLine, int col) const {
    const Cell &c = cellAtGlobal(globalLine, col);
    if (c.codepoint == ' ' || c.codepoint == 0) return {};
    uint32_t cp = c.codepoint;
    QString text = QString::fromUcs4(reinterpret_cast<const char32_t *>(&cp), 1);
    if (auto *comb = combiningAt(globalLine, col)) {
        for (uint32_t combCp : *comb)
            text += QString::fromUcs4(reinterpret_cast<const char32_t *>(&combCp), 1);
    }
    return text;
}

bool TerminalWidget::mouseReportingActive() const {
    return m_grid->mouseButtonMode() || m_grid->mouseMotionMode() || m_grid->mouseAnyMode();
}

void TerminalWidget::sendMouseEvent(QMouseEvent *event, bool press, bool release) {
    if (!m_pty) return;
    QPoint cell = pixelToCell(event->pos());
    int col = cell.y() + 1; // 1-based
    int row = cell.x() - m_grid->scrollbackSize() + m_scrollOffset + 1; // 1-based screen row
    if (row < 1 || row > m_grid->rows()) return;

    int button = 0;
    if (event->button() == Qt::LeftButton || (event->buttons() & Qt::LeftButton))
        button = 0;
    else if (event->button() == Qt::MiddleButton || (event->buttons() & Qt::MiddleButton))
        button = 1;
    else if (event->button() == Qt::RightButton || (event->buttons() & Qt::RightButton))
        button = 2;
    else if (!press && !release)
        button = 35; // motion with no button

    // Add modifier bits
    int mods = 0;
    if (event->modifiers() & Qt::ShiftModifier) mods |= 4;
    if (event->modifiers() & Qt::AltModifier) mods |= 8;
    if (event->modifiers() & Qt::ControlModifier) mods |= 16;

    if (m_grid->mouseSgrMode()) {
        // SGR mode: CSI < button;col;row M/m
        char suffix = release ? 'm' : 'M';
        int btn = button + mods;
        if (!press && !release) btn = button + 32 + mods; // motion
        QString seq = QString("\x1B[<%1;%2;%3%4").arg(btn).arg(col).arg(row).arg(suffix);
        m_pty->write(seq.toUtf8());
    } else {
        // X10/normal mode: CSI M Cb Cx Cy (all + 32)
        if (release) return; // X10 doesn't report release
        char cb = static_cast<char>(button + mods + 32);
        char cx = static_cast<char>(col + 32);
        char cy = static_cast<char>(row + 32);
        QByteArray seq;
        seq.append("\x1B[M");
        seq.append(cb);
        seq.append(cx);
        seq.append(cy);
        m_pty->write(seq);
    }
}

void TerminalWidget::mousePressEvent(QMouseEvent *event) {
    // Mouse reporting to application (unless Shift held for override)
    if (mouseReportingActive() && !(event->modifiers() & Qt::ShiftModifier)) {
        sendMouseEvent(event, true, false);
        return;
    }

    if (event->button() == Qt::LeftButton) {
        // Ctrl+Click opens URLs and file paths
        if (event->modifiers() & Qt::ControlModifier) {
            QPoint cell = pixelToCell(event->pos());
            auto spans = detectUrls(cell.x());
            for (const auto &s : spans) {
                if (cell.y() >= s.startCol && cell.y() <= s.endCol) {
                    if (s.isFilePath) {
                        openFileAtPath(s.url);
                    } else {
                        QDesktopServices::openUrl(QUrl(s.url));
                    }
                    return;
                }
            }
        }
        clearSelection();
        m_selecting = true;
        m_selStart = pixelToCell(event->pos());
        m_selEnd = m_selStart;
        update();
    }
    if (event->button() == Qt::MiddleButton && m_pty) {
        QString text = QApplication::clipboard()->text(QClipboard::Selection);
        if (!text.isEmpty()) {
            QByteArray data = text.toUtf8();
            if (m_grid->bracketedPaste()) {
                m_pty->write(QByteArray("\x1B[200~"));
                m_pty->write(data);
                m_pty->write(QByteArray("\x1B[201~"));
            } else {
                m_pty->write(data);
            }
        }
    }
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event) {
    // Mouse motion reporting
    if (mouseReportingActive() && !(event->modifiers() & Qt::ShiftModifier)) {
        bool anyButton = event->buttons() != Qt::NoButton;
        if (m_grid->mouseAnyMode() || (m_grid->mouseMotionMode() && anyButton)) {
            QPoint cell = pixelToCell(event->pos());
            if (cell != m_lastMouseCell) {
                m_lastMouseCell = cell;
                sendMouseEvent(event, false, false);
            }
        }
        return;
    }

    if (m_selecting) {
        m_selEnd = pixelToCell(event->pos());
        m_hasSelection = (m_selStart != m_selEnd);
        update();
    }

    // Change cursor for URLs/file paths with Ctrl held
    if (event->modifiers() & Qt::ControlModifier) {
        QPoint cell = pixelToCell(event->pos());
        auto spans = detectUrls(cell.x());
        bool onLink = false;
        for (const auto &s : spans) {
            if (cell.y() >= s.startCol && cell.y() <= s.endCol) {
                onLink = true;
                break;
            }
        }
        setCursor(onLink ? Qt::PointingHandCursor : Qt::IBeamCursor);
    } else {
        setCursor(Qt::IBeamCursor);
    }
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (mouseReportingActive() && !(event->modifiers() & Qt::ShiftModifier)) {
        sendMouseEvent(event, false, true);
        return;
    }

    if (event->button() == Qt::LeftButton && m_selecting) {
        m_selecting = false;
        m_selEnd = pixelToCell(event->pos());
        m_hasSelection = (m_selStart != m_selEnd);
        if (m_hasSelection) {
            QString text = selectedText();
            if (!text.isEmpty()) {
                // Always copy to X11 primary selection
                QApplication::clipboard()->setText(text, QClipboard::Selection);
                // Auto-copy to clipboard too if enabled
                if (m_autoCopyOnSelect) {
                    QApplication::clipboard()->setText(text);
                }
            }
        }
        update();
    }
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        QPoint cell = pixelToCell(event->pos());
        int globalLine = cell.x();
        int col = cell.y();

        auto getChar = [&](int gl, int c) -> uint32_t {
            return cellAtGlobal(gl, c).codepoint;
        };

        auto isWordChar = [](uint32_t cp) {
            return cp != ' ' && cp != 0 && cp != '\t';
        };

        if (!isWordChar(getChar(globalLine, col))) return;

        int left = col;
        while (left > 0 && isWordChar(getChar(globalLine, left - 1)))
            --left;
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
            if (m_autoCopyOnSelect) {
                QApplication::clipboard()->setText(text);
            }
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

    QPoint s = m_selStart, e = m_selEnd;
    if (s.x() > e.x() || (s.x() == e.x() && s.y() > e.y()))
        std::swap(s, e);

    int sLine = s.x(), sCol = s.y();
    int eLine = e.x(), eCol = e.y();

    if (globalLine < sLine || globalLine > eLine) return false;
    if (sLine == eLine) return col >= sCol && col <= eCol;
    if (globalLine == sLine) return col >= sCol;
    if (globalLine == eLine) return col <= eCol;
    return true;
}

QString TerminalWidget::selectedText() const {
    if (!m_hasSelection) return {};

    QPoint s = m_selStart, e = m_selEnd;
    if (s.x() > e.x() || (s.x() == e.x() && s.y() > e.y()))
        std::swap(s, e);

    int cols = m_grid->cols();
    QString result;

    for (int gl = s.x(); gl <= e.x(); ++gl) {
        int startCol = (gl == s.x()) ? s.y() : 0;
        int endCol = (gl == e.x()) ? e.y() : cols - 1;

        QString line;
        for (int c = startCol; c <= endCol; ++c) {
            uint32_t cp = cellAtGlobal(gl, c).codepoint;
            if (cp == 0) cp = ' ';
            line += QString::fromUcs4(reinterpret_cast<const char32_t *>(&cp), 1);
            if (auto *comb = combiningAt(gl, c)) {
                for (uint32_t combCp : *comb)
                    line += QString::fromUcs4(reinterpret_cast<const char32_t *>(&combCp), 1);
            }
        }

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

// --- URL and File Path Detection ---

QString TerminalWidget::lineText(int globalLine) const {
    int cols = m_grid->cols();
    QString text;
    text.reserve(cols * 2);

    auto appendCp = [&](uint32_t cp) {
        if (cp < 0x10000) {
            text += QChar(static_cast<char16_t>(cp));
        } else {
            cp -= 0x10000;
            text += QChar(static_cast<char16_t>(0xD800 + (cp >> 10)));
            text += QChar(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        }
    };

    for (int c = 0; c < cols; ++c) {
        uint32_t cp = cellAtGlobal(globalLine, c).codepoint;
        if (cp == 0) cp = ' ';
        appendCp(cp);
        if (auto *comb = combiningAt(globalLine, c)) {
            for (uint32_t combCp : *comb)
                appendCp(combCp);
        }
    }
    return text;
}

std::vector<TerminalWidget::UrlSpan> TerminalWidget::detectUrls(int globalLine) const {
    // URL pattern
    static QRegularExpression urlRe(
        R"((https?://|ftp://|file://)[^\s<>"'\)\]}{,;]+)",
        QRegularExpression::CaseInsensitiveOption
    );

    // File path with optional line:col (compiler/linter output)
    // Matches: /abs/path.ext:line:col, ./rel/path.ext:line, src/file.cpp:42:10, file.rs:17
    static QRegularExpression filePathRe(
        R"RE((?:^|[\s("'])((\.{0,2}/)?[a-zA-Z0-9_\-./]+\.[a-zA-Z0-9_]+(?::(\d+)(?::(\d+))?)?)(?=[\s)"',;:\]}>]|$))RE"
    );

    std::vector<UrlSpan> spans;

    // OSC 8 explicit hyperlinks first (highest priority)
    int scrollbackSize = m_grid->scrollbackSize();
    const std::vector<HyperlinkSpan> *hlSpans = nullptr;
    if (globalLine >= 0 && globalLine < scrollbackSize) {
        hlSpans = &m_grid->scrollbackHyperlinks(globalLine);
    } else {
        int sr = globalLine - scrollbackSize;
        if (sr >= 0 && sr < m_grid->rows())
            hlSpans = &m_grid->screenHyperlinks(sr);
    }
    if (hlSpans) {
        for (const auto &hl : *hlSpans) {
            spans.push_back({hl.startCol, hl.endCol,
                             QString::fromStdString(hl.uri), false, true});
        }
    }

    QString text = lineText(globalLine);

    // Detect URLs
    auto it = urlRe.globalMatch(text);
    while (it.hasNext()) {
        auto match = it.next();
        QString url = match.captured();
        int endCol = match.capturedEnd() - 1;
        while (endCol > match.capturedStart() &&
               (url.endsWith('.') || url.endsWith(',') || url.endsWith(':'))) {
            url.chop(1);
            --endCol;
        }
        spans.push_back({static_cast<int>(match.capturedStart()), endCol, url, false, false});
    }

    // Detect file paths with line numbers (e.g., src/main.cpp:42)
    auto it2 = filePathRe.globalMatch(text);
    while (it2.hasNext()) {
        auto match = it2.next();
        QString path = match.captured(1);
        int startCol = match.capturedStart(1);
        int endCol = match.capturedEnd(1) - 1;

        // Verify it looks like a real file path (has a file extension)
        // Skip if it overlaps with an existing URL span
        bool overlaps = false;
        for (const auto &s : spans) {
            if (startCol <= s.endCol && endCol >= s.startCol) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps) {
            // Accept paths with '/' or paths with :line (compiler output)
            bool hasLineNum = match.capturedStart(3) >= 0;
            if (path.contains('/') || hasLineNum) {
                spans.push_back({startCol, endCol, path, true, false});
            }
        }
    }

    return spans;
}

void TerminalWidget::openFileAtPath(const QString &path) {
    // Parse path:line:col format
    QString filePath = path;
    int line = -1;
    int col = -1;

    QRegularExpression lineColRe(R"(^(.+):(\d+)(?::(\d+))?$)");
    auto match = lineColRe.match(path);
    if (match.hasMatch()) {
        filePath = match.captured(1);
        line = match.captured(2).toInt();
        if (match.lastCapturedIndex() >= 3 && !match.captured(3).isEmpty())
            col = match.captured(3).toInt();
    }

    // Resolve relative paths using shell's actual CWD
    QFileInfo fi(filePath);
    if (!fi.isAbsolute()) {
        QStringList tryPaths;
        // Get shell's CWD from /proc if possible
        if (m_pty) {
            int pid = m_pty->childPid();
            if (pid > 0) {
                QString cwdLink = QString("/proc/%1/cwd").arg(pid);
                QFileInfo cwdInfo(cwdLink);
                if (cwdInfo.exists())
                    tryPaths.append(cwdInfo.symLinkTarget() + "/" + filePath);
            }
        }
        tryPaths.append(QDir::currentPath() + "/" + filePath);
        tryPaths.append(QDir::homePath() + "/" + filePath);
        for (const QString &p : tryPaths) {
            if (QFileInfo::exists(p)) {
                filePath = p;
                break;
            }
        }
    }

    // Determine editor command
    QString editor = m_editorCommand;
    if (editor.isEmpty()) {
        // Auto-detect: try VS Code, Kate, then xdg-open
        if (QStandardPaths::findExecutable("code").size())
            editor = "code";
        else if (QStandardPaths::findExecutable("kate").size())
            editor = "kate";
        else
            editor = "xdg-open";
    }

    QStringList args;
    if (editor == "code" && line > 0) {
        args << "--goto" << QString("%1:%2%3").arg(filePath).arg(line).arg(col > 0 ? QString(":%1").arg(col) : "");
    } else if (editor == "kate" && line > 0) {
        args << "-l" << QString::number(line) << filePath;
    } else {
        args << filePath;
    }

    QProcess::startDetached(editor, args);
}

// --- Search ---

void TerminalWidget::showSearchBar() {
    m_searchVisible = true;
    m_searchBar->setGeometry(0, 0, width(), 36);
    QColor bg = m_grid->defaultBg();
    QColor fg = m_grid->defaultFg();
    QColor barBg = bg.darker(110);
    QColor borderColor = m_bracketHighlightBg;
    m_searchBar->setStyleSheet(QString(
        "QWidget#searchBar { background-color: %1; border-bottom: 1px solid %2; }"
        "QLineEdit { background: %3; color: %4; border: 1px solid %2; border-radius: 4px; padding: 2px 6px; }"
        "QLabel { color: %5; }"
        "QPushButton { background: transparent; color: %5; border: none; font-size: 12px; }"
        "QPushButton:hover { color: %4; }"
    ).arg(barBg.name(), borderColor.name(), bg.name(), fg.name(), fg.darker(130).name()));
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
    // Binary search to find matches on this line (matches are sorted by globalLine)
    for (size_t i = 0; i < m_searchMatches.size(); ++i) {
        const auto &m = m_searchMatches[i];
        if (m.globalLine < globalLine) continue;
        if (m.globalLine > globalLine) break;
        if (col >= m.col && col < m.col + m.length)
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
    return cellAtGlobal(globalLine, col).codepoint;
}

TerminalWidget::BracketPair TerminalWidget::findMatchingBracket() const {
    int cursorRow = m_grid->cursorRow();
    int cursorCol = m_grid->cursorCol();

    // Return cached result if cursor hasn't moved
    if (cursorRow == m_cachedBracketCursorRow && cursorCol == m_cachedBracketCursorCol)
        return m_cachedBracket;
    m_cachedBracketCursorRow = cursorRow;
    m_cachedBracketCursorCol = cursorCol;

    int scrollbackSize = m_grid->scrollbackSize();
    int cursorGL = scrollbackSize + cursorRow;

    uint32_t ch = getCellCodepoint(cursorGL, cursorCol);

    char32_t openBrackets[]  = {'(', '[', '{'};
    char32_t closeBrackets[] = {')', ']', '}'};

    auto cacheAndReturn = [this](BracketPair bp) {
        m_cachedBracket = bp;
        return bp;
    };

    for (int i = 0; i < 3; ++i) {
        if (ch == openBrackets[i]) {
            int depth = 1;
            int gl = cursorGL, col = cursorCol + 1;
            int totalLines = scrollbackSize + m_grid->rows();
            int cols = m_grid->cols();
            while (gl < totalLines && depth > 0) {
                if (col >= cols) { col = 0; ++gl; continue; }
                uint32_t c = getCellCodepoint(gl, col);
                if (c == openBrackets[i]) ++depth;
                else if (c == closeBrackets[i]) --depth;
                if (depth == 0) return cacheAndReturn({cursorGL, cursorCol, gl, col});
                ++col;
            }
            return cacheAndReturn({-1, -1, -1, -1});
        }
    }

    for (int i = 0; i < 3; ++i) {
        if (ch == closeBrackets[i]) {
            int depth = 1;
            int gl = cursorGL, col = cursorCol - 1;
            int cols = m_grid->cols();
            while (gl >= 0 && depth > 0) {
                if (col < 0) { --gl; col = cols - 1; continue; }
                uint32_t c = getCellCodepoint(gl, col);
                if (c == closeBrackets[i]) ++depth;
                else if (c == openBrackets[i]) --depth;
                if (depth == 0) return cacheAndReturn({gl, col, cursorGL, cursorCol});
                --col;
            }
            return cacheAndReturn({-1, -1, -1, -1});
        }
    }

    return cacheAndReturn({-1, -1, -1, -1});
}

// --- Terminal Recording (asciicast v2) ---

void TerminalWidget::startRecording(const QString &path) {
    if (m_recording) return;
    m_recordFile = std::make_unique<QFile>(path);
    if (!m_recordFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_recordFile.reset();
        return;
    }

    // Write asciicast v2 header
    QString header = QString(R"({"version": 2, "width": %1, "height": %2, "timestamp": %3, "env": {"TERM": "xterm-256color"}})")
        .arg(m_grid->cols())
        .arg(m_grid->rows())
        .arg(QDateTime::currentSecsSinceEpoch());
    m_recordFile->write(header.toUtf8() + "\n");

    m_recordTimer.start();
    m_recording = true;
}

void TerminalWidget::stopRecording() {
    if (!m_recording) return;
    m_recording = false;
    if (m_recordFile) {
        m_recordFile->close();
        m_recordFile.reset();
    }
}

// --- Bookmarks ---

void TerminalWidget::toggleBookmark() {
    int globalLine = m_grid->scrollbackSize() + m_grid->cursorRow();
    auto it = std::find(m_bookmarks.begin(), m_bookmarks.end(), globalLine);
    if (it != m_bookmarks.end()) {
        m_bookmarks.erase(it);
    } else {
        m_bookmarks.push_back(globalLine);
        std::sort(m_bookmarks.begin(), m_bookmarks.end());
    }
    update();
}

void TerminalWidget::nextBookmark() {
    if (m_bookmarks.empty()) return;
    int scrollbackSize = m_grid->scrollbackSize();
    int viewTop = scrollbackSize - m_scrollOffset;
    for (int bm : m_bookmarks) {
        if (bm > viewTop + 1) {
            m_scrollOffset = scrollbackSize - bm;
            m_scrollOffset = std::clamp(m_scrollOffset, 0, scrollbackSize);
            update();
            return;
        }
    }
    // Wrap to first bookmark
    m_scrollOffset = scrollbackSize - m_bookmarks.front();
    m_scrollOffset = std::clamp(m_scrollOffset, 0, scrollbackSize);
    update();
}

void TerminalWidget::prevBookmark() {
    if (m_bookmarks.empty()) return;
    int scrollbackSize = m_grid->scrollbackSize();
    int viewTop = scrollbackSize - m_scrollOffset;
    for (int i = static_cast<int>(m_bookmarks.size()) - 1; i >= 0; --i) {
        if (m_bookmarks[i] < viewTop - 1) {
            m_scrollOffset = scrollbackSize - m_bookmarks[i];
            m_scrollOffset = std::clamp(m_scrollOffset, 0, scrollbackSize);
            update();
            return;
        }
    }
    // Wrap to last bookmark
    m_scrollOffset = scrollbackSize - m_bookmarks.back();
    m_scrollOffset = std::clamp(m_scrollOffset, 0, scrollbackSize);
    update();
}

// --- OpenGL ---

void TerminalWidget::initializeGL() {
    // Clear to transparent so per-pixel alpha works when WA_TranslucentBackground is set.
    // Without this, QOpenGLWidget clears its internal FBO to opaque black before
    // QPainter draws, making background transparency impossible.
    if (auto *f = QOpenGLContext::currentContext()->functions())
        f->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    if (m_gpuRendering) {
        m_glRenderer = std::make_unique<GlRenderer>();
        if (!m_glRenderer->initialize()) {
            m_gpuRendering = false;
            m_glRenderer.reset();
        } else {
            m_glRenderer->setFont(m_font, m_fontBold, m_fontItalic, m_fontBoldItalic);
            m_glRenderer->setCellSize(m_cellWidth, m_cellHeight, m_fontAscent);
        }
    }
}

void TerminalWidget::resizeGL(int w, int h) {
    if (m_glRenderer) {
        m_glRenderer->setViewportSize(w, h);
    }
    recalcGridSize();
}

void TerminalWidget::setGpuRendering(bool enabled) {
    m_gpuRendering = enabled;
    if (enabled && !m_glRenderer && isValid()) {
        // GPU path needs Core Profile for GLSL 3.3 shaders
        QSurfaceFormat fmt = format();
        fmt.setVersion(3, 3);
        fmt.setProfile(QSurfaceFormat::CoreProfile);
        setFormat(fmt);
        // Initialize GL renderer now (context must be valid)
        makeCurrent();
        initializeGL();
        doneCurrent();
    } else if (!enabled && m_glRenderer) {
        makeCurrent();
        m_glRenderer->cleanup();
        doneCurrent();
        m_glRenderer.reset();
    }
    update();
}

void TerminalWidget::setBackgroundAlpha(int alpha) {
    m_backgroundAlpha = qBound(0, alpha, 255);
    update();
}

void TerminalWidget::setWindowOpacityLevel(double opacity) {
    m_windowOpacity = qBound(0.0, opacity, 1.0);
    update();
}

// --- Recent output for AI context ---

QString TerminalWidget::recentOutput(int lines) const {
    QStringList result;
    int scrollbackSize = m_grid->scrollbackSize();
    int totalLines = scrollbackSize + m_grid->rows();

    int start = std::max(0, totalLines - lines);
    for (int i = start; i < totalLines; ++i) {
        result.append(lineText(i));
    }
    return result.join('\n');
}

// --- Shell CWD ---

QString TerminalWidget::shellCwd() const {
    if (!m_pty) return {};
    int pid = m_pty->childPid();
    if (pid <= 0) return {};
    QFileInfo cwdInfo(QString("/proc/%1/cwd").arg(pid));
    return cwdInfo.exists() ? cwdInfo.symLinkTarget() : QString();
}

pid_t TerminalWidget::shellPid() const {
    return m_pty ? m_pty->childPid() : -1;
}

// --- Claude Code permission detection ---

void TerminalWidget::checkForClaudePermissionPrompt() {
    // Scan last 5 lines for Claude Code permission prompt pattern
    int scrollbackSize = m_grid->scrollbackSize();
    int totalLines = scrollbackSize + m_grid->rows();
    int start = std::max(0, totalLines - 5);

    // Pattern: "Allow ToolName" or "Allow ToolName(args)"
    static QRegularExpression promptRe(
        R"((?:Allow|allow)\s+(\w+(?:\([^)]*\))?)\s*\?)"
    );

    for (int i = start; i < totalLines; ++i) {
        QString text = lineText(i);
        auto match = promptRe.match(text);
        if (match.hasMatch()) {
            QString rule = match.captured(1);
            if (rule != m_lastDetectedRule) {
                m_lastDetectedRule = rule;
                emit claudePermissionDetected(rule);
            }
            return;
        }
    }
}

// --- Write command to PTY ---

void TerminalWidget::writeCommand(const QString &cmd) {
    if (m_pty && !cmd.isEmpty()) {
        m_pty->write(cmd.toUtf8());
        m_pty->write(QByteArray("\n", 1));
    }
}

// --- Right-click context menu ---

void TerminalWidget::contextMenuEvent(QContextMenuEvent *event) {
    QMenu menu(this);

    QAction *copyAction = menu.addAction("Copy");
    copyAction->setShortcut(QKeySequence("Ctrl+Shift+C"));
    copyAction->setEnabled(m_hasSelection);
    connect(copyAction, &QAction::triggered, this, &TerminalWidget::copySelection);

    QAction *pasteAction = menu.addAction("Paste");
    pasteAction->setShortcut(QKeySequence("Ctrl+Shift+V"));
    connect(pasteAction, &QAction::triggered, this, [this]() {
        QString text = QApplication::clipboard()->text();
        if (!text.isEmpty() && m_pty) {
            QByteArray data = text.toUtf8();
            if (m_grid->bracketedPaste()) {
                m_pty->write(QByteArray("\x1B[200~"));
                m_pty->write(data);
                m_pty->write(QByteArray("\x1B[201~"));
            } else {
                m_pty->write(data);
            }
        }
    });

    QAction *selectAllAction = menu.addAction("Select All");
    connect(selectAllAction, &QAction::triggered, this, [this]() {
        m_selStart = QPoint(0, 0);
        int totalLines = m_grid->scrollbackSize() + m_grid->rows();
        m_selEnd = QPoint(totalLines - 1, m_grid->cols() - 1);
        m_hasSelection = true;
        m_selecting = false;
        update();
    });

    menu.addSeparator();

    QAction *clearAction = menu.addAction("Clear Scrollback");
    connect(clearAction, &QAction::triggered, this, [this]() {
        if (m_pty) m_pty->write(QByteArray("\x1B[3J\x1B[H\x1B[2J", 11));
    });

    menu.addSeparator();

    // Search Web (if there's a selection)
    if (m_hasSelection) {
        QString sel = selectedText().trimmed();
        if (!sel.isEmpty() && sel.length() < 200) {
            QAction *searchAction = menu.addAction("Search Web");
            connect(searchAction, &QAction::triggered, this, [sel]() {
                QDesktopServices::openUrl(
                    QUrl("https://www.google.com/search?q=" + QUrl::toPercentEncoding(sel)));
            });
        }
    }

    // Open URL (if cursor is on a link)
    QPoint cell = pixelToCell(event->pos());
    auto spans = detectUrls(cell.x());
    for (const auto &s : spans) {
        if (cell.y() >= s.startCol && cell.y() <= s.endCol) {
            QAction *openUrl = menu.addAction("Open Link");
            connect(openUrl, &QAction::triggered, this, [s, this]() {
                if (s.isFilePath) openFileAtPath(s.url);
                else QDesktopServices::openUrl(QUrl(s.url));
            });
            QAction *copyUrl = menu.addAction("Copy Link");
            connect(copyUrl, &QAction::triggered, this, [s]() {
                QApplication::clipboard()->setText(s.url);
            });
            break;
        }
    }

    menu.addSeparator();

    QAction *exportText = menu.addAction("Export Scrollback as Text...");
    connect(exportText, &QAction::triggered, this, [this]() {
        QString path = QFileDialog::getSaveFileName(this, "Export Scrollback", QString(), "Text Files (*.txt)");
        if (!path.isEmpty()) {
            QFile file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream stream(&file);
                stream << exportAsText();
            }
        }
    });

    QAction *exportHtml = menu.addAction("Export Scrollback as HTML...");
    connect(exportHtml, &QAction::triggered, this, [this]() {
        QString path = QFileDialog::getSaveFileName(this, "Export Scrollback", QString(), "HTML Files (*.html)");
        if (!path.isEmpty()) {
            QFile file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream stream(&file);
                stream << exportAsHtml();
            }
        }
    });

    menu.exec(event->globalPos());
}

// --- Scrollback export ---

QString TerminalWidget::exportAsText() const {
    QStringList lines;
    int scrollbackSize = m_grid->scrollbackSize();
    int totalLines = scrollbackSize + m_grid->rows();
    for (int i = 0; i < totalLines; ++i) {
        QString line = lineText(i);
        while (line.endsWith(' ')) line.chop(1);
        lines.append(line);
    }
    // Trim trailing empty lines
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines.join('\n');
}

QString TerminalWidget::exportAsHtml() const {
    QString html;
    html += "<!DOCTYPE html>\n<html><head><meta charset='utf-8'>\n";
    html += "<title>Ants Terminal Export</title>\n";
    html += "<style>\n";
    html += "body { background: " + m_grid->defaultBg().name() + "; ";
    html += "color: " + m_grid->defaultFg().name() + "; ";
    html += "font-family: 'JetBrains Mono', 'Fira Code', monospace; ";
    html += "font-size: " + QString::number(m_font.pointSize()) + "pt; ";
    html += "white-space: pre; }\n";
    html += "span.bold { font-weight: bold; }\n";
    html += "span.italic { font-style: italic; }\n";
    html += "span.underline { text-decoration: underline; }\n";
    html += "span.strikethrough { text-decoration: line-through; }\n";
    html += "</style>\n</head><body>\n";

    int scrollbackSize = m_grid->scrollbackSize();
    int totalLines = scrollbackSize + m_grid->rows();

    for (int gl = 0; gl < totalLines; ++gl) {
        int cols = m_grid->cols();
        QColor lastFg, lastBg;
        bool inSpan = false;

        for (int c = 0; c < cols; ++c) {
            const Cell &cell = cellAtGlobal(gl, c);
            if (cell.isWideCont) continue;

            QColor fg = cell.attrs.fg;
            QColor bg = cell.attrs.bg;
            if (cell.attrs.inverse) std::swap(fg, bg);

            // Open new span if colors changed
            if (fg != lastFg || bg != lastBg || c == 0) {
                if (inSpan) html += "</span>";
                QString style = "color:" + fg.name() + ";";
                if (bg != m_grid->defaultBg())
                    style += "background:" + bg.name() + ";";
                QStringList classes;
                if (cell.attrs.bold) classes << "bold";
                if (cell.attrs.italic) classes << "italic";
                if (cell.attrs.underline) classes << "underline";
                if (cell.attrs.strikethrough) classes << "strikethrough";
                html += "<span";
                if (!classes.isEmpty())
                    html += " class='" + classes.join(' ') + "'";
                html += " style='" + style + "'>";
                inSpan = true;
                lastFg = fg;
                lastBg = bg;
            }

            uint32_t cp = cell.codepoint;
            if (cp == 0) cp = ' ';
            if (cp == '<') html += "&lt;";
            else if (cp == '>') html += "&gt;";
            else if (cp == '&') html += "&amp;";
            else html += QString::fromUcs4(reinterpret_cast<const char32_t *>(&cp), 1);
        }
        if (inSpan) html += "</span>";
        html += "\n";
    }

    html += "</body></html>\n";
    return html;
}

// --- Foreground process detection ---

QString TerminalWidget::foregroundProcess() const {
    if (!m_pty) return {};
    int shellPid = m_pty->childPid();
    if (shellPid <= 0) return {};

    // Read /proc/PID/stat to get the foreground process group
    QFile statFile(QString("/proc/%1/stat").arg(shellPid));
    if (!statFile.open(QIODevice::ReadOnly)) return {};
    QString stat = QString::fromUtf8(statFile.readAll());

    // Find the foreground process group (field 8, 1-indexed)
    // Format: pid (comm) state ppid pgrp session tpgid ...
    int closeParenIdx = stat.lastIndexOf(')');
    if (closeParenIdx < 0) return {};
    QStringList fields = stat.mid(closeParenIdx + 2).split(' ');
    if (fields.size() < 5) return {};
    int tpgid = fields[4].toInt(); // terminal foreground process group

    if (tpgid <= 0 || tpgid == shellPid) return {};

    // Read the foreground process name from /proc/tpgid/comm
    QFile commFile(QString("/proc/%1/comm").arg(tpgid));
    if (!commFile.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(commFile.readAll()).trimmed();
}

// --- Highlight rules ---

void TerminalWidget::setHighlightRules(const QJsonArray &rules) {
    m_highlightRules.clear();
    for (const QJsonValue &v : rules) {
        QJsonObject obj = v.toObject();
        if (!obj["enabled"].toBool(true)) continue;
        QString pattern = obj["pattern"].toString();
        if (pattern.isEmpty()) continue;
        HighlightRule rule;
        rule.pattern = QRegularExpression(pattern);
        if (!rule.pattern.isValid()) continue;
        QString fg = obj["fg"].toString();
        if (!fg.isEmpty()) rule.fg = QColor(fg);
        QString bg = obj["bg"].toString();
        if (!bg.isEmpty()) rule.bg = QColor(bg);
        m_highlightRules.push_back(rule);
    }
    update();
}

// --- Trigger rules ---

void TerminalWidget::setTriggerRules(const QJsonArray &rules) {
    m_triggerRules.clear();
    for (const QJsonValue &v : rules) {
        QJsonObject obj = v.toObject();
        if (!obj["enabled"].toBool(true)) continue;
        QString pattern = obj["pattern"].toString();
        if (pattern.isEmpty()) continue;
        TriggerRule rule;
        rule.pattern = QRegularExpression(pattern);
        if (!rule.pattern.isValid()) continue;
        rule.actionType = obj["action_type"].toString("notify");
        rule.actionValue = obj["action_value"].toString();
        m_triggerRules.push_back(rule);
    }
}

void TerminalWidget::checkTriggers(const QByteArray &data) {
    if (m_triggerRules.empty()) return;
    QString text = QString::fromUtf8(data);
    for (const auto &rule : m_triggerRules) {
        if (rule.pattern.match(text).hasMatch()) {
            emit triggerFired(rule.pattern.pattern(), rule.actionType, rule.actionValue);
        }
    }
}

// --- History / Autocomplete ---

void TerminalWidget::setHistoryFile(const QString &path) {
    Q_UNUSED(path);
    m_historyLoaded = false;
    loadHistory();
}

void TerminalWidget::loadHistory() {
    if (m_historyLoaded) return;
    m_historyEntries.clear();

    // Try zsh history first, then bash
    QStringList histPaths = {
        QDir::homePath() + "/.zsh_history",
        QDir::homePath() + "/.bash_history",
        QDir::homePath() + "/.local/share/fish/fish_history"
    };

    for (const QString &path : histPaths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;

        QByteArray content = file.readAll();
        QStringList lines = QString::fromUtf8(content).split('\n', Qt::SkipEmptyParts);

        for (const QString &line : lines) {
            QString cmd = line.trimmed();
            // zsh extended history format: ": timestamp:0;command"
            if (cmd.startsWith(": ")) {
                int semi = cmd.indexOf(';');
                if (semi >= 0) cmd = cmd.mid(semi + 1);
                else continue;
            }
            // fish history format: "- cmd: command"
            if (cmd.startsWith("- cmd: ")) {
                cmd = cmd.mid(7);
            }
            if (!cmd.isEmpty() && !m_historyEntries.contains(cmd))
                m_historyEntries.prepend(cmd); // Most recent first
        }
        if (!m_historyEntries.isEmpty()) break; // Use the first available history
    }
    m_historyLoaded = true;
}

void TerminalWidget::updateSuggestion() {
    if (!m_historyLoaded) loadHistory();
    m_currentSuggestion.clear();

    // Don't suggest in alt screen (vim, htop, etc.)
    if (m_grid->altScreenActive()) return;

    // Get current input line (text after last prompt)
    int scrollbackSize = m_grid->scrollbackSize();
    int cursorLine = scrollbackSize + m_grid->cursorRow();
    QString currentLine = lineText(cursorLine).trimmed();

    if (currentLine.length() < 2) return;

    // Find first matching history entry
    for (const QString &entry : m_historyEntries) {
        if (entry.startsWith(currentLine) && entry != currentLine) {
            m_currentSuggestion = entry.mid(currentLine.length());
            break;
        }
    }
}

// --- Font family ---

void TerminalWidget::setFontFamily(const QString &family) {
    if (family.isEmpty()) return;
    m_font.setFamily(family);
    m_fontBold.setFamily(family);
    m_fontItalic.setFamily(family);
    m_fontBoldItalic.setFamily(family);
    updateFontMetrics();
    recalcGridSize();
    update();
}

// --- Exit code & command output ---

int TerminalWidget::lastExitCode() const {
    return m_grid->lastExitCode();
}

QString TerminalWidget::lastCommandOutput() const {
    int startLine = m_grid->commandOutputStartLine();
    if (startLine < 0) return {};

    int scrollbackSize = m_grid->scrollbackSize();
    int endLine = scrollbackSize + m_grid->cursorRow();
    // Cap output to last 100 lines to avoid huge strings
    if (endLine - startLine > 100) startLine = endLine - 100;

    QString output;
    for (int gl = startLine; gl < endLine; ++gl) {
        output += lineText(gl);
        output += '\n';
    }
    return output.trimmed();
}
