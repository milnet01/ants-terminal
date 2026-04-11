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
#include <QSplitter>
#include <QPlainTextEdit>

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

    // Nerd Font symbol fallback (Powerline, Devicons, etc.)
    QStringList nerdFamilies = {"Symbols Nerd Font Mono", "Symbols Nerd Font",
                                 "Hack Nerd Font Mono", "JetBrainsMono Nerd Font Mono",
                                 "FiraCode Nerd Font Mono"};
    for (const QString &family : nerdFamilies) {
        QFont test(family);
        QFontMetrics fm(test);
        // Test with Powerline branch symbol (U+E0A0)
        if (fm.horizontalAdvance(QChar(0xE0A0)) > 0) {
            m_nerdFallbackFont = test;
            m_nerdFallbackFont.setPointSize(m_font.pointSize());
            m_hasNerdFallback = true;
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

    // Sync output safety timeout — if sync mode stays on for >500ms
    // (e.g. truncated sequence), force a repaint so the screen isn't frozen
    m_syncTimer.setSingleShot(true);
    m_syncTimer.setInterval(500);
    connect(&m_syncTimer, &QTimer::timeout, this, [this]() {
        if (m_syncOutputActive) {
            m_syncOutputActive = false;
            update();
        }
    });

    // Visual bell flash timer
    m_bellFlashTimer.setSingleShot(true);
    m_bellFlashTimer.setInterval(150);
    connect(&m_bellFlashTimer, &QTimer::timeout, this, [this]() {
        m_bellFlashActive = false;
        update();
    });

    // Bell callback from grid
    m_grid->setBellCallback([this]() { triggerVisualBell(); });

    // Desktop notification callback (OSC 9/777)
    m_grid->setNotifyCallback([this](const QString &title, const QString &body) {
        emit desktopNotification(title, body);
    });

    // Smooth scrolling timer (16ms = ~60fps)
    m_smoothScrollTimer.setInterval(16);
    connect(&m_smoothScrollTimer, &QTimer::timeout, this, &TerminalWidget::smoothScrollStep);

    // Selection auto-scroll timer (when dragging past edges)
    m_selectionScrollTimer.setInterval(50);
    connect(&m_selectionScrollTimer, &QTimer::timeout, this, [this]() {
        if (m_selectionScrollDirection != 0 && m_selecting) {
            int delta = m_selectionScrollDirection * 2;
            m_scrollOffset = std::clamp(m_scrollOffset - delta, 0, m_grid->scrollbackSize());
            m_selEnd = pixelToCell(m_lastMousePos);
            m_hasSelection = (m_selStart != m_selEnd);
            updateScrollBar();
            update();
        } else {
            m_selectionScrollTimer.stop();
        }
    });

    // Triple-click timer
    m_lastClickTime.start();

    // Performance overlay timer
    m_perfFrameTimer.start();

    // Scrollbar (child widget overlaid on right edge)
    m_scrollBar = new QScrollBar(Qt::Vertical, this);
    m_scrollBar->setMinimum(0);
    m_scrollBar->setMaximum(0);
    m_scrollBar->setValue(0);
    m_scrollBar->setStyleSheet(
        "QScrollBar:vertical { background: transparent; width: 12px; margin: 0; }"
        "QScrollBar::handle:vertical { background: rgba(255,255,255,80); border-radius: 4px; min-height: 20px; margin: 2px; }"
        "QScrollBar::handle:vertical:hover { background: rgba(255,255,255,120); }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
    );
    connect(m_scrollBar, &QScrollBar::valueChanged, this, [this](int value) {
        // Scrollbar value 0 = top of scrollback, max = bottom (scrollOffset 0)
        int maxScroll = m_grid->scrollbackSize();
        m_scrollOffset = maxScroll - value;
        update();
    });

    // Scroll-to-bottom floating button
    m_scrollToBottomBtn = new QPushButton(this);
    m_scrollToBottomBtn->setText("\u25BC");  // down arrow
    m_scrollToBottomBtn->setFixedSize(32, 32);
    m_scrollToBottomBtn->setToolTip("Scroll to bottom");
    m_scrollToBottomBtn->hide();
    m_scrollToBottomBtn->setStyleSheet(
        "QPushButton { background: rgba(40,40,60,200); color: rgba(200,200,220,220); "
        "border: 1px solid rgba(100,100,130,150); border-radius: 16px; font-size: 14px; }"
        "QPushButton:hover { background: rgba(60,60,90,220); color: white; }"
    );
    connect(m_scrollToBottomBtn, &QPushButton::clicked, this, [this]() {
        m_scrollOffset = 0;
        m_newOutputMarkerLine = -1;
        updateScrollBar();
        updateScrollToBottomButton();
        update();
    });

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
    if (m_glRenderer && context() && context()->isValid()) {
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
    if (m_hasNerdFallback) m_nerdFallbackFont.setPointSize(size);
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
        if (!m_logFile->open(QIODevice::WriteOnly | QIODevice::Append)) {
            qWarning("Failed to open log file: %s", qPrintable(filename));
            m_logFile.reset();
        }
    } else if (!enabled && m_logFile) {
        m_logFile->close();
        m_logFile.reset();
    }
}

void TerminalWidget::sendToPty(const QByteArray &data) {
    if (m_pty)
        m_pty->write(data);
}

bool TerminalWidget::startShell(const QString &workDir) {
    m_pty = new Pty(this);
    connect(m_pty, &Pty::dataReceived, this, &TerminalWidget::onPtyData);
    connect(m_pty, &Pty::finished, this, &TerminalWidget::onPtyFinished);

    recalcGridSize();

    int rows = m_grid->rows();
    int cols = m_grid->cols();
    if (!m_pty->start(QString(), workDir, rows, cols)) {
        return false;
    }

    // Ensure PTY matches grid (no-op if forkpty already used correct size)
    m_pty->resize(rows, cols);
    return true;
}

void TerminalWidget::applyThemeColors(const QColor &fg, const QColor &bg,
                                       const QColor &cursorColor,
                                       const QColor &accent,
                                       const QColor &border) {
    m_grid->setDefaultFg(fg);
    m_grid->setDefaultBg(bg);
    m_cursorColor = cursorColor;

    // Cursor glow color (same as cursor, but only show in dark themes)
    m_cursorGlowColor = cursorColor;
    // Disable glow for light themes (bg luminance > 0.5)
    if (bg.lightnessF() > 0.5)
        m_cursorGlowColor = QColor();

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
        // Scratchpad: Ctrl+Enter to send, Escape to close
        if (m_scratchpad && m_scratchpad->isVisible()) {
            if (ke->key() == Qt::Key_Escape) {
                hideScratchpad();
                return true;
            }
            if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) &&
                (ke->modifiers() & Qt::ControlModifier)) {
                auto *edit = m_scratchpad->findChild<QPlainTextEdit *>();
                if (edit) {
                    QString text = edit->toPlainText();
                    if (!text.isEmpty() && m_pty) {
                        pasteToTerminal(text.toUtf8());
                        m_pty->write("\r");
                    }
                    edit->clear();
                    hideScratchpad();
                }
                return true;
            }
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

    // Background image (rendered behind all content)
    if (!m_backgroundImage.isNull()) {
        p.setOpacity(0.15); // subtle background
        p.drawImage(rect(), m_backgroundImage);
        p.setOpacity(1.0);
    }

    // Badge watermark (large semi-transparent text in background)
    if (!m_badgeText.isEmpty()) {
        QFont badgeFont = m_font;
        badgeFont.setPointSize(m_font.pointSize() * 4);
        badgeFont.setBold(true);
        p.setFont(badgeFont);
        QColor badgeColor = m_grid->defaultFg();
        badgeColor.setAlpha(20);
        p.setPen(badgeColor);
        QRect badgeRect = rect().adjusted(0, 0, -20, -20);
        p.drawText(badgeRect, Qt::AlignBottom | Qt::AlignRight, m_badgeText);
        p.setFont(m_font);
    }

    int scrollbackSize = m_grid->scrollbackSize();
    int viewStart = scrollbackSize - m_scrollOffset;

    BracketPair bp = (m_scrollOffset == 0) ? findMatchingBracket() : BracketPair{-1,-1,-1,-1};

    // Invalidate span caches if content changed since last paint
    if (m_spanCacheDirty)
        invalidateSpanCaches();

    for (int vr = 0; vr < rows; ++vr) {
        int globalLine = viewStart + vr;
        int px_y = m_padding + vr * m_cellHeight;

        // URL spans — use cache to avoid per-frame regex matching
        auto urlCacheIt = m_urlSpanCache.find(globalLine);
        if (urlCacheIt == m_urlSpanCache.end()) {
            auto computed = detectUrls(globalLine);
            urlCacheIt = m_urlSpanCache.emplace(globalLine, std::move(computed)).first;
        }
        const auto &urlSpans = urlCacheIt->second;

        // Highlight spans — use cache to avoid per-frame regex matching
        auto hlCacheIt = m_hlSpanCache.find(globalLine);
        if (hlCacheIt == m_hlSpanCache.end()) {
            std::vector<PaintHighlightSpan> computed;
            if (!m_highlightRules.empty()) {
                QString lt = lineText(globalLine);
                for (const auto &rule : m_highlightRules) {
                    auto it = rule.pattern.globalMatch(lt);
                    while (it.hasNext()) {
                        auto m = it.next();
                        computed.push_back({static_cast<int>(m.capturedStart()),
                                            static_cast<int>(m.capturedStart() + m.capturedLength() - 1),
                                            rule.fg, rule.bg});
                    }
                }
            }
            hlCacheIt = m_hlSpanCache.emplace(globalLine, std::move(computed)).first;
        }
        const auto &hlSpans = hlCacheIt->second;

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
            bool isHoveredUrl = false;
            for (const auto &u : urlSpans) {
                if (col >= u.startCol && col <= u.endCol) {
                    isUrl = true;
                    // Check if this URL span is being hovered
                    if (m_hoverGlobalLine == globalLine &&
                        m_hoverCol >= u.startCol && m_hoverCol <= u.endCol)
                        isHoveredUrl = true;
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
            if (c.attrs.underline || isUrl || isHoveredUrl) {
                QColor ulColor = c.attrs.underlineColor.isValid() ? c.attrs.underlineColor : fg;
                int ulY = px_y + m_cellHeight - 2;
                int ulW = c.isWideChar ? m_cellWidth * 2 : m_cellWidth;
                UnderlineStyle style = isHoveredUrl ? UnderlineStyle::Single
                                     : isUrl ? UnderlineStyle::None  // URLs only underline on hover
                                     : c.attrs.underlineStyle;
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

    // Command timestamps and fold indicators (OSC 133 shell integration)
    {
        const auto &regions = m_grid->promptRegions();
        QFont smallFont = m_font;
        smallFont.setPointSize(std::max(7, m_font.pointSize() - 2));
        QFontMetrics sfm(smallFont);

        for (size_t ri = 0; ri < regions.size(); ++ri) {
            const auto &pr = regions[ri];
            int vr = pr.startLine - viewStart;
            if (vr < 0 || vr >= rows) continue;

            int py = m_padding + vr * m_cellHeight;

            // Command duration and timestamp (right-aligned)
            if (pr.commandStartMs > 0) {
                p.setFont(smallFont);
                QColor dimColor = m_grid->defaultFg();
                dimColor.setAlpha(100);
                p.setPen(dimColor);

                QString info;
                if (pr.commandEndMs > 0) {
                    qint64 durationMs = pr.commandEndMs - pr.commandStartMs;
                    if (durationMs < 1000)
                        info = QString("%1ms").arg(durationMs);
                    else if (durationMs < 60000)
                        info = QString("%1s").arg(durationMs / 1000.0, 0, 'f', 1);
                    else
                        info = QString("%1m%2s").arg(durationMs / 60000).arg((durationMs % 60000) / 1000);
                    info += "  ";
                }

                QDateTime dt = QDateTime::fromMSecsSinceEpoch(pr.commandStartMs);
                info += dt.toString("HH:mm:ss");

                int textW = sfm.horizontalAdvance(info);
                p.drawText(width() - textW - m_padding - 14, py + sfm.ascent(), info);
                p.setFont(m_font);
            }

            // Fold indicator for completed command output
            if (pr.hasOutput && pr.commandEndMs > 0 && ri + 1 < regions.size()) {
                int foldX = m_padding - 2;
                if (foldX < 0) foldX = 2;
                QColor foldColor = m_cursorColor;
                foldColor.setAlpha(120);
                p.setPen(foldColor);
                // Draw a small triangle (right = expanded, down = collapsed)
                if (pr.folded) {
                    // Right-pointing triangle (collapsed)
                    QPolygon tri;
                    tri << QPoint(foldX, py + 2)
                        << QPoint(foldX, py + m_cellHeight - 2)
                        << QPoint(foldX + 6, py + m_cellHeight / 2);
                    p.setBrush(foldColor);
                    p.drawPolygon(tri);
                    p.setBrush(Qt::NoBrush);

                    // Draw fold summary bar
                    int outputStart = pr.endLine + 1;
                    int nextStart = (ri + 1 < regions.size()) ? regions[ri + 1].startLine : outputStart;
                    int foldedLines = nextStart - outputStart;
                    if (foldedLines > 0) {
                        int barY = py + m_cellHeight;
                        QColor barBg = m_grid->defaultBg().lighter(130);
                        barBg.setAlpha(80);
                        p.fillRect(m_padding, barY, width() - m_padding * 2, m_cellHeight, barBg);
                        p.setFont(smallFont);
                        QColor foldText = m_grid->defaultFg();
                        foldText.setAlpha(130);
                        p.setPen(foldText);
                        p.drawText(m_padding + 8, barY + sfm.ascent() + 2,
                                   QString("... %1 lines hidden (click to expand)").arg(foldedLines));
                        p.setFont(m_font);
                    }
                } else {
                    // Down-pointing triangle (expanded)
                    QPolygon tri;
                    tri << QPoint(foldX, py + 3)
                        << QPoint(foldX + 6, py + 3)
                        << QPoint(foldX + 3, py + 3 + 5);
                    p.setBrush(foldColor);
                    p.drawPolygon(tri);
                    p.setBrush(Qt::NoBrush);
                }
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

        // Cursor glow effect (subtle radial glow behind cursor in dark themes)
        if (m_hasFocus && m_cursorBlinkOn) {
            QColor glow = m_cursorGlowColor.isValid() ? m_cursorGlowColor : m_cursorColor;
            glow.setAlpha(30);
            int glowSize = 4;
            p.setPen(Qt::NoPen);
            p.setBrush(glow);
            p.drawRoundedRect(cx - glowSize, cy - glowSize,
                              m_cellWidth + glowSize * 2, m_cellHeight + glowSize * 2,
                              glowSize, glowSize);
        }

        if (m_hasFocus) {
            switch (shape) {
            case CursorShape::BlinkBar:
            case CursorShape::SteadyBar:
                p.fillRect(cx, cy, 2, m_cellHeight, m_cursorColor);
                break;
            case CursorShape::BlinkUnderline:
            case CursorShape::SteadyUnderline:
                p.fillRect(cx, cy + m_cellHeight - 2, m_cellWidth, 2, m_cursorColor);
                break;
            default: // Block cursor
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
            // Unfocused: hollow outline cursor
            QPen outlinePen(m_cursorColor, 1.5);
            p.setPen(outlinePen);
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

    // Dim overlay for unfocused split panes
    if (!m_hasFocus && parentWidget() && qobject_cast<QSplitter *>(parentWidget())) {
        QColor dim(0, 0, 0, 35);
        p.fillRect(rect(), dim);
    }

    // Visual bell flash overlay
    if (m_bellFlashActive) {
        QColor flash = m_grid->defaultFg();
        flash.setAlpha(40);
        p.fillRect(rect(), flash);
    }

    // New output separator line (when scrolled up and new output arrived)
    if (m_newOutputMarkerLine >= 0 && m_scrollOffset > 0) {
        int vr = m_newOutputMarkerLine - viewStart;
        if (vr >= 0 && vr < rows) {
            int markerY = m_padding + vr * m_cellHeight;
            QColor markerColor = m_cursorColor;
            markerColor.setAlpha(120);
            QPen dashPen(markerColor, 1, Qt::DashLine);
            p.setPen(dashPen);
            p.drawLine(m_padding, markerY, width() - m_padding, markerY);
            // Label
            QFont smallFont = m_font;
            smallFont.setPointSize(std::max(7, m_font.pointSize() - 2));
            p.setFont(smallFont);
            p.setPen(markerColor);
            p.drawText(width() - 120, markerY - 2, "new output");
            p.setFont(m_font);
        }
    }

    // URL quick-select overlay labels
    if (m_urlQuickSelectActive && !m_quickSelectLabels.empty()) {
        QFont labelFont = m_font;
        labelFont.setBold(true);
        labelFont.setPointSize(std::max(8, m_font.pointSize() - 1));
        p.setFont(labelFont);
        QFontMetrics lfm(labelFont);

        for (const auto &ql : m_quickSelectLabels) {
            int vr = ql.globalLine - viewStart;
            if (vr < 0 || vr >= rows) continue;
            int lx = m_padding + ql.startCol * m_cellWidth - 2;
            int ly = m_padding + vr * m_cellHeight;

            // Draw label badge
            int labelW = lfm.horizontalAdvance(ql.label) + 8;
            int labelH = lfm.height() + 4;
            QColor badgeBg = m_cursorColor;
            p.setPen(Qt::NoPen);
            p.setBrush(badgeBg);
            p.drawRoundedRect(lx - labelW - 2, ly, labelW, labelH, 3, 3);
            p.setPen(m_grid->defaultBg());
            p.drawText(lx - labelW + 2, ly + lfm.ascent() + 2, ql.label);

            // Highlight the URL span
            QColor hlBg = m_cursorColor;
            hlBg.setAlpha(40);
            int spanW = (ql.endCol - ql.startCol + 1) * m_cellWidth;
            p.fillRect(m_padding + ql.startCol * m_cellWidth, ly, spanW, m_cellHeight, hlBg);
        }
        p.setFont(m_font);

        // Draw input so far at bottom
        if (!m_quickSelectInput.isEmpty()) {
            QColor inputBg = m_grid->defaultBg().darker(120);
            inputBg.setAlpha(220);
            int ibH = m_cellHeight + 8;
            int ibY = height() - ibH - (m_searchVisible ? m_searchBar->height() : 0);
            p.fillRect(0, ibY, width(), ibH, inputBg);
            p.setPen(m_grid->defaultFg());
            p.drawText(m_padding + 4, ibY + m_fontAscent + 4,
                        "Quick Select: " + m_quickSelectInput);
        }
    }

    // Sticky command header (pin command at top when scrolling through its output)
    if (m_scrollOffset > 0) {
        const auto &regions = m_grid->promptRegions();
        // Find the prompt region whose output spans the current viewport top
        for (int ri = static_cast<int>(regions.size()) - 1; ri >= 0; --ri) {
            const auto &pr = regions[ri];
            // The command output starts after pr.endLine; the next region starts at regions[ri+1].startLine
            int outputStart = pr.endLine + 1;
            int outputEnd = (ri + 1 < static_cast<int>(regions.size()))
                            ? regions[ri + 1].startLine - 1
                            : scrollbackSize + rows - 1;
            // Viewport top line
            if (outputStart <= viewStart && outputEnd >= viewStart && pr.hasOutput) {
                // Extract the command text from the prompt region (startLine to endLine)
                QString cmdText;
                for (int gl = pr.startLine; gl <= pr.endLine; ++gl) {
                    cmdText += lineText(gl).trimmed();
                    if (gl < pr.endLine) cmdText += " ";
                }
                cmdText = cmdText.trimmed();
                if (cmdText.isEmpty()) break;

                // Draw sticky header bar at top
                QColor headerBg = m_grid->defaultBg().lighter(120);
                headerBg.setAlpha(230);
                int headerH = m_cellHeight + 4;
                p.fillRect(0, 0, width(), headerH, headerBg);

                // Bottom border
                QColor borderColor = m_cursorColor;
                borderColor.setAlpha(100);
                p.setPen(borderColor);
                p.drawLine(0, headerH, width(), headerH);

                // Command text
                p.setPen(m_grid->defaultFg());
                p.setFont(m_font);
                QString truncated = cmdText;
                int maxChars = (width() - m_padding * 2 - 80) / m_cellWidth;
                if (truncated.length() > maxChars)
                    truncated = truncated.left(maxChars - 1) + QChar(0x2026); // ellipsis
                p.drawText(m_padding + 4, 2 + m_fontAscent, truncated);

                // Duration (right side)
                if (pr.commandStartMs > 0 && pr.commandEndMs > 0) {
                    qint64 dur = pr.commandEndMs - pr.commandStartMs;
                    QString durStr;
                    if (dur < 1000) durStr = QString("%1ms").arg(dur);
                    else if (dur < 60000) durStr = QString("%1s").arg(dur / 1000.0, 0, 'f', 1);
                    else durStr = QString("%1m%2s").arg(dur / 60000).arg((dur % 60000) / 1000);
                    QColor dimColor = m_grid->defaultFg();
                    dimColor.setAlpha(130);
                    p.setPen(dimColor);
                    QFontMetrics fm(m_font);
                    p.drawText(width() - fm.horizontalAdvance(durStr) - m_padding - 4, 2 + m_fontAscent, durStr);
                }
                break;
            }
        }
    }

    // Performance overlay
    if (m_showPerfOverlay) {
        ++m_perfFrameCount;
        qint64 elapsed = m_perfFrameTimer.elapsed();
        if (elapsed >= 1000) {
            m_perfFps = m_perfFrameCount * 1000.0 / elapsed;
            m_perfFrameCount = 0;
            m_perfFrameTimer.restart();
        }

        QStringList lines;
        lines << QString("FPS: %1").arg(m_perfFps, 0, 'f', 1);
        lines << QString("Paint: %1 us").arg(m_perfLastPaintUs);
        lines << QString("Scrollback: %1").arg(m_grid->scrollbackSize());
        lines << QString("Grid: %1x%2").arg(cols).arg(rows);
        lines << QString("Scroll offset: %1").arg(m_scrollOffset);

        // Measure and draw background
        QFont perfFont = m_font;
        perfFont.setPointSize(9);
        p.setFont(perfFont);
        QFontMetrics pfm(perfFont);
        int lineH = pfm.height();
        int maxW = 0;
        for (const auto &l : lines) maxW = qMax(maxW, pfm.horizontalAdvance(l));
        int boxW = maxW + 16;
        int boxH = lines.size() * lineH + 12;
        int bx = width() - boxW - 8;
        int by = 8;

        p.setPen(Qt::NoPen);
        QColor overlay = m_grid->defaultBg();
        overlay.setAlpha(200);
        p.setBrush(overlay);
        p.drawRoundedRect(bx, by, boxW, boxH, 4, 4);

        p.setPen(m_grid->defaultFg());
        for (int i = 0; i < lines.size(); ++i) {
            p.drawText(bx + 8, by + 8 + (i + 1) * lineH - pfm.descent(), lines[i]);
        }
        p.setFont(m_font);
    }
}

void TerminalWidget::keyPressEvent(QKeyEvent *event) {
    // URL quick-select mode input handling
    if (m_urlQuickSelectActive) {
        if (event->key() == Qt::Key_Escape) {
            exitUrlQuickSelect();
            return;
        }
        QString ch = event->text().toLower();
        if (!ch.isEmpty() && ch[0].isLetter()) {
            m_quickSelectInput += ch;
            // Check for matching label
            for (const auto &ql : m_quickSelectLabels) {
                if (ql.label == m_quickSelectInput) {
                    exitUrlQuickSelect();
                    if (ql.isFilePath) {
                        openFileAtPath(ql.url);
                    } else if (ql.url.startsWith("http://") || ql.url.startsWith("https://")) {
                        QDesktopServices::openUrl(QUrl(ql.url));
                    } else {
                        // Non-URL match (SHA, IP, email) — copy to clipboard
                        QApplication::clipboard()->setText(ql.url);
                    }
                    return;
                }
            }
            update();
        }
        return;
    }

    m_scrollOffset = 0;
    m_newOutputMarkerLine = -1;
    updateScrollBar();
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
            pasteToTerminal(clipboard->text().toUtf8());
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

    // Ctrl+Shift+G -- URL quick-select mode
    if (key == Qt::Key_G && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        enterUrlQuickSelect();
        return;
    }

    // Ctrl+Shift+O -- select command output at cursor (semantic selection)
    if (key == Qt::Key_O && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        int globalLine = m_grid->scrollbackSize() + m_grid->cursorRow();
        selectCommandOutput(globalLine);
        return;
    }

    // Ctrl+Shift+Enter -- open scratchpad (multi-line command editor)
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) &&
        (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        showScratchpad();
        return;
    }

    // Ctrl+Shift+. -- toggle command output fold at cursor
    if (key == Qt::Key_Period && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        toggleFoldAtCursor();
        return;
    }

    // Ctrl+Shift+F12 -- toggle performance overlay
    if (key == Qt::Key_F12 && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        m_showPerfOverlay = !m_showPerfOverlay;
        if (m_showPerfOverlay) m_perfFrameTimer.restart();
        update();
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

    // Ctrl+arrow keys — word movement (xterm modifier encoding: CSI 1;5 X)
    if ((mods & Qt::ControlModifier) && !(mods & Qt::ShiftModifier)) {
        if (key == Qt::Key_Left)  { if (m_pty) m_pty->write("\x1B[1;5D"); return; }
        if (key == Qt::Key_Right) { if (m_pty) m_pty->write("\x1B[1;5C"); return; }
        if (key == Qt::Key_Up)    { if (m_pty) m_pty->write("\x1B[1;5A"); return; }
        if (key == Qt::Key_Down)  { if (m_pty) m_pty->write("\x1B[1;5B"); return; }
    }

    // Ctrl+key combinations
    if (mods & Qt::ControlModifier && !(mods & Qt::ShiftModifier)) {
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            char ch = static_cast<char>(key - Qt::Key_A + 1);
            data.append(ch);
            if (m_pty) m_pty->write(data);
            return;
        }
        if (key == Qt::Key_BracketLeft) { data = "\x1B"; if (m_pty) m_pty->write(data); return; }
        if (key == Qt::Key_Backslash)   { data = "\x1C"; if (m_pty) m_pty->write(data); return; }
        if (key == Qt::Key_BracketRight){ data = "\x1D"; if (m_pty) m_pty->write(data); return; }
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

    // Kitty keyboard protocol — encode key if mode is active
    if (m_grid->kittyKeyFlags() > 0) {
        QByteArray kittyData = encodeKittyKey(event);
        if (!kittyData.isEmpty()) {
            if (m_hasSelection) clearSelection();
            if (m_pty) m_pty->write(kittyData);
            if (m_broadcastCallback) m_broadcastCallback(this, kittyData);
            return;
        }
        // Fall through to legacy encoding for keys not handled by Kitty
    }

    // Special keys (legacy encoding)
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
            updateScrollBar();
            update();
            return;
        }
        data = m_grid->applicationCursorKeys() ? "\x1BOH" : "\x1B[H";
        break;
    case Qt::Key_End:
        if (mods & Qt::ShiftModifier) {
            m_scrollOffset = 0;
            updateScrollBar();
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
            updateScrollBar();
            update();
            return;
        }
        data = "\x1B[5~";
        break;
    case Qt::Key_PageDown:
        if (mods & Qt::ShiftModifier) {
            m_scrollOffset = std::max(m_scrollOffset - m_grid->rows(), 0);
            updateScrollBar();
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
        // Clear selection when user types (selection is now stale)
        if (m_hasSelection)
            clearSelection();
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
    m_spanCacheDirty = true;
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

    // Smooth scrolling: accumulate target and animate
    int delta = event->angleDelta().y();
    double lines = delta / 40.0;  // ~3 lines per standard wheel notch (120 units)
    m_smoothScrollTarget += lines;
    int maxScroll = m_grid->scrollbackSize();
    m_smoothScrollTarget = std::clamp(m_smoothScrollTarget,
                                       -static_cast<double>(m_scrollOffset),
                                       static_cast<double>(maxScroll - m_scrollOffset));
    m_smoothScrollCurrent = 0.0;
    if (!m_smoothScrollTimer.isActive())
        m_smoothScrollTimer.start();
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
    m_spanCacheDirty = true;

    // Clear selection when screen content changes (e.g., 'clear' command, ED, etc.)
    // Check for clear-screen sequences: ESC[2J, ESC[3J, ESC[H ESC[2J, or form feed
    if (data.contains("\x1B[2J") || data.contains("\x1B[3J") || data.contains("\x0C")) {
        clearSelection();
    }

    // Scroll anchoring: if the user has scrolled up, keep their viewport stable
    // as new lines push content into scrollback.
    int scrollbackBefore = m_grid->scrollbackSize();

    m_parser->feed(data.constData(), data.size());

    if (m_scrollOffset > 0) {
        int added = m_grid->scrollbackSize() - scrollbackBefore;
        if (added > 0) {
            m_scrollOffset = std::min(m_scrollOffset + added, m_grid->scrollbackSize());
            // Set new output marker when user is scrolled up and new content arrives
            if (m_newOutputMarkerLine < 0)
                m_newOutputMarkerLine = m_grid->scrollbackSize() + m_grid->rows() - 1;
        }
    } else {
        m_newOutputMarkerLine = -1;
    }

    // Session logging
    if (m_loggingEnabled && m_logFile && m_logFile->isOpen()) {
        m_logFile->write(data);
        m_logFile->flush();
    }

    // Asciicast recording
    if (m_recording && m_recordFile && m_recordFile->isOpen()) {
        double elapsed = m_recordTimer.elapsed() / 1000.0;
        // Escape the data for JSON in a single pass
        QString raw = QString::fromUtf8(data);
        QString escaped;
        escaped.reserve(raw.size() + raw.size() / 4);
        for (QChar ch : raw) {
            ushort u = ch.unicode();
            if (u == '\\')      escaped += "\\\\";
            else if (u == '"')  escaped += "\\\"";
            else if (u == '\n') escaped += "\\n";
            else if (u == '\r') escaped += "\\r";
            else if (u == '\t') escaped += "\\t";
            else if (u < 0x20)  escaped += QString("\\u%1").arg(u, 4, 16, QChar('0'));
            else                escaped += ch;
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

    // Synchronized output (DEC mode 2026): defer screen update until the
    // application signals the frame is complete by disabling sync mode.
    // Only repaint when sync is OFF at the end of this data chunk.  The old
    // condition `!syncActive || !wasSync` incorrectly painted partial frames
    // when a sync-on arrived without its matching sync-off in the same read.
    bool wasSync = m_syncOutputActive;
    m_syncOutputActive = m_grid->synchronizedOutput();
    if (!m_syncOutputActive) {
        update();
        m_syncTimer.stop();
    } else if (!wasSync) {
        // Sync just started — arm a safety timeout so the screen still
        // updates if the matching sync-off never arrives (truncated data).
        m_syncTimer.start();
    }

    updateScrollBar();

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

void TerminalWidget::smoothScrollStep() {
    if (std::abs(m_smoothScrollTarget) < 0.01) {
        m_smoothScrollTimer.stop();
        m_smoothScrollTarget = 0.0;
        m_smoothScrollCurrent = 0.0;
        return;
    }
    // Ease toward target: move 30% of remaining distance each frame
    double step = m_smoothScrollTarget * 0.30;
    if (std::abs(step) < 0.5) step = (m_smoothScrollTarget > 0) ? 0.5 : -0.5;
    m_smoothScrollCurrent += step;
    m_smoothScrollTarget -= step;

    int intStep = static_cast<int>(m_smoothScrollCurrent);
    if (intStep != 0) {
        m_scrollOffset = std::clamp(m_scrollOffset + intStep, 0, m_grid->scrollbackSize());
        m_smoothScrollCurrent -= intStep;
        updateScrollBar();
        updateScrollToBottomButton();
        update();
    }
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

void TerminalWidget::pasteToTerminal(const QByteArray &data) {
    if (!m_pty || data.isEmpty()) return;
    if (m_grid->bracketedPaste()) {
        // Strip any embedded \e[201~ that could prematurely end the bracketed paste
        // and allow escape-sequence injection (CVE-2021-28848 class)
        QByteArray sanitized = data;
        sanitized.replace("\x1B[201~", "");
        m_pty->write(QByteArray("\x1B[200~"));
        m_pty->write(sanitized);
        m_pty->write(QByteArray("\x1B[201~"));
    } else {
        m_pty->write(data);
    }
}

QByteArray TerminalWidget::encodeKittyKey(QKeyEvent *event) const {
    int flags = m_grid->kittyKeyFlags();
    if (flags == 0) return {};  // Legacy mode — caller handles

    Qt::KeyboardModifiers mods = event->modifiers();
    int key = event->key();

    // Build modifier value (1 + bitmask): shift=1, alt=2, ctrl=4, super=8
    int modVal = 1;
    if (mods & Qt::ShiftModifier)   modVal += 1;
    if (mods & Qt::AltModifier)     modVal += 2;
    if (mods & Qt::ControlModifier) modVal += 4;
    if (mods & Qt::MetaModifier)    modVal += 8;  // Super/Meta key

    // Map Qt key to Kitty key code
    int keyCode = 0;
    bool useLegacyFormat = false;  // true for keys using CSI number ~ or CSI 1;mod letter
    char legacyFinal = 0;
    int legacyNumber = 0;

    // Functional keys that keep their legacy CSI encoding format
    switch (key) {
    case Qt::Key_Up:       legacyFinal = 'A'; useLegacyFormat = true; break;
    case Qt::Key_Down:     legacyFinal = 'B'; useLegacyFormat = true; break;
    case Qt::Key_Right:    legacyFinal = 'C'; useLegacyFormat = true; break;
    case Qt::Key_Left:     legacyFinal = 'D'; useLegacyFormat = true; break;
    case Qt::Key_Home:     legacyFinal = 'H'; useLegacyFormat = true; break;
    case Qt::Key_End:      legacyFinal = 'F'; useLegacyFormat = true; break;
    case Qt::Key_F1:       legacyFinal = 'P'; useLegacyFormat = true; break;
    case Qt::Key_F2:       legacyFinal = 'Q'; useLegacyFormat = true; break;
    case Qt::Key_F4:       legacyFinal = 'S'; useLegacyFormat = true; break;
    case Qt::Key_F3:       legacyNumber = 13; useLegacyFormat = true; break;
    case Qt::Key_F5:       legacyNumber = 15; useLegacyFormat = true; break;
    case Qt::Key_F6:       legacyNumber = 17; useLegacyFormat = true; break;
    case Qt::Key_F7:       legacyNumber = 18; useLegacyFormat = true; break;
    case Qt::Key_F8:       legacyNumber = 19; useLegacyFormat = true; break;
    case Qt::Key_F9:       legacyNumber = 20; useLegacyFormat = true; break;
    case Qt::Key_F10:      legacyNumber = 21; useLegacyFormat = true; break;
    case Qt::Key_F11:      legacyNumber = 23; useLegacyFormat = true; break;
    case Qt::Key_F12:      legacyNumber = 24; useLegacyFormat = true; break;
    case Qt::Key_Insert:   legacyNumber = 2;  useLegacyFormat = true; break;
    case Qt::Key_Delete:   legacyNumber = 3;  useLegacyFormat = true; break;
    case Qt::Key_PageUp:   legacyNumber = 5;  useLegacyFormat = true; break;
    case Qt::Key_PageDown: legacyNumber = 6;  useLegacyFormat = true; break;
    // Keys using CSI u format
    case Qt::Key_Escape:    keyCode = 27; break;
    case Qt::Key_Return:
    case Qt::Key_Enter:     keyCode = 13; break;
    case Qt::Key_Tab:       keyCode = 9; break;
    case Qt::Key_Backtab:   keyCode = 9; break;  // Shift+Tab
    case Qt::Key_Backspace: keyCode = 127; break;
    default: break;
    }

    if (useLegacyFormat) {
        // Legacy format: CSI 1;modifier letter or CSI number;modifier ~
        if (legacyFinal) {
            if (modVal <= 1)
                return QByteArray("\x1B[") + legacyFinal;
            return QByteArray("\x1B[1;") + QByteArray::number(modVal) + legacyFinal;
        } else {
            if (modVal <= 1)
                return QByteArray("\x1B[") + QByteArray::number(legacyNumber) + "~";
            return QByteArray("\x1B[") + QByteArray::number(legacyNumber)
                   + ";" + QByteArray::number(modVal) + "~";
        }
    }

    // For Enter/Tab/Backspace in disambiguate-only mode (flag 1, not flag 8):
    // send legacy bytes, not CSI u
    if (keyCode > 0 && (keyCode == 13 || keyCode == 9 || keyCode == 127)) {
        if (!(flags & 8)) {
            // Legacy behavior for these keys unless "all keys as escape codes" is set
            return {};  // Let the normal handler deal with it
        }
    }

    // Flag 1 (disambiguate): encode Escape as CSI 27 u
    if (keyCode == 27 && (flags & 1) && !(flags & 8)) {
        if (modVal <= 1)
            return QByteArray("\x1B[27u");
        return QByteArray("\x1B[27;") + QByteArray::number(modVal) + "u";
    }

    // Flag 8 (all keys as escape codes): encode everything via CSI u
    if (flags & 8) {
        if (keyCode == 0) {
            // Text-producing key — use the Unicode codepoint (lowercase)
            QString text = event->text();
            if (text.isEmpty()) return {};
            keyCode = text.at(0).toLower().unicode();
        }
        if (keyCode > 0) {
            QByteArray result = QByteArray("\x1B[") + QByteArray::number(keyCode);
            if (modVal > 1)
                result += ";" + QByteArray::number(modVal);
            result += "u";
            return result;
        }
        return {};
    }

    // Flag 1 (disambiguate): ctrl+key and alt+key use CSI u
    if ((flags & 1) && keyCode == 0) {
        QString text = event->text();
        if (text.isEmpty()) return {};
        int cp = text.at(0).toLower().unicode();

        // Only encode if there are modifiers that would be ambiguous
        if (modVal > 1 && cp >= 0x20) {
            QByteArray result = QByteArray("\x1B[") + QByteArray::number(cp);
            result += ";" + QByteArray::number(modVal);
            result += "u";
            return result;
        }
    }

    return {};  // Fall through to legacy encoding
}

void TerminalWidget::copySelectionRich() {
    if (!m_hasSelection) return;

    // Build both plain text and HTML
    QString plainText = selectedText();
    QString html = QStringLiteral("<pre style='font-family: monospace;'>");

    // m_selStart/m_selEnd: x() = globalLine, y() = col
    int startLine = std::min(m_selStart.x(), m_selEnd.x());
    int endLine = std::max(m_selStart.x(), m_selEnd.x());
    int startCol = (m_selStart.x() <= m_selEnd.x()) ? m_selStart.y() : m_selEnd.y();
    int endCol = (m_selStart.x() <= m_selEnd.x()) ? m_selEnd.y() : m_selStart.y();

    for (int line = startLine; line <= endLine; ++line) {
        int colStart = (line == startLine) ? startCol : 0;
        int colEnd = (line == endLine) ? endCol : m_grid->cols() - 1;

        for (int col = colStart; col <= colEnd; ++col) {
            const Cell &c = cellAtGlobal(line, col);
            if (c.isWideCont) continue;

            QColor fg = c.attrs.fg;
            QColor bg = c.attrs.bg;
            if (c.attrs.inverse) std::swap(fg, bg);

            QString style;
            style += QStringLiteral("color:%1;").arg(fg.name());
            if (bg != m_grid->defaultBg())
                style += QStringLiteral("background:%1;").arg(bg.name());
            if (c.attrs.bold) style += "font-weight:bold;";
            if (c.attrs.italic) style += "font-style:italic;";

            uint32_t cp = c.codepoint;
            if (cp == 0 || cp == ' ') {
                html += ' ';
            } else {
                QString ch = QString::fromUcs4(reinterpret_cast<const char32_t *>(&cp), 1);
                html += QStringLiteral("<span style='%1'>%2</span>")
                            .arg(style, ch.toHtmlEscaped());
            }
        }
        if (line < endLine) html += '\n';
    }
    html += QStringLiteral("</pre>");

    auto *mimeData = new QMimeData();
    mimeData->setText(plainText);
    mimeData->setHtml(html);
    QApplication::clipboard()->setMimeData(mimeData);
}

void TerminalWidget::clickToMoveCursor(int col, int row) {
    if (!m_pty) return;

    // Only works when cursor is in a prompt region (OSC 133)
    int cursorGlobalLine = m_grid->scrollbackSize() + m_grid->cursorRow();
    int cursorCol = m_grid->cursorCol();

    // Check if we're in a prompt editing area (between OSC 133 B and C markers)
    const auto &regions = m_grid->promptRegions();
    bool inPrompt = false;
    for (const auto &pr : regions) {
        if (cursorGlobalLine >= pr.startLine && cursorGlobalLine <= pr.endLine && !pr.hasOutput) {
            inPrompt = true;
            break;
        }
    }
    if (!inPrompt) return;

    // Calculate offset from current cursor position and send arrow keys
    int clickGlobalLine = m_grid->scrollbackSize() + row;
    if (clickGlobalLine != cursorGlobalLine) return;  // Only same line

    int delta = col - cursorCol;
    if (delta == 0) return;

    QByteArray arrows;
    const char *arrow = (delta > 0) ? "\x1B[C" : "\x1B[D";
    int count = std::abs(delta);
    for (int i = 0; i < count && i < 200; ++i)
        arrows += arrow;

    m_pty->write(arrows);
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

void TerminalWidget::forceRecalcSize() {
    recalcGridSize();
}

void TerminalWidget::recalcGridSize() {
    int scrollBarW = m_scrollBar ? m_scrollBar->width() : 0;
    int availW = width() - m_padding * 2 - scrollBarW;
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
    positionScrollBar();
    updateScrollBar();
}

void TerminalWidget::positionScrollBar() {
    if (!m_scrollBar) return;
    int sbW = m_scrollBar->sizeHint().width();
    int searchBarH = m_searchVisible ? m_searchBar->height() : 0;
    m_scrollBar->setGeometry(width() - sbW, searchBarH, sbW, height() - searchBarH);
}

void TerminalWidget::updateScrollBar() {
    if (!m_scrollBar) return;
    int maxScroll = m_grid->scrollbackSize();
    // Block signals to avoid re-entrant valueChanged -> scrollOffset update
    m_scrollBar->blockSignals(true);
    m_scrollBar->setMaximum(maxScroll);
    m_scrollBar->setPageStep(m_grid->rows());
    m_scrollBar->setValue(maxScroll - m_scrollOffset);
    m_scrollBar->blockSignals(false);
    // Show scrollbar only when there's scrollback
    m_scrollBar->setVisible(maxScroll > 0);
    updateScrollToBottomButton();
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
        // Exit URL quick-select mode on any click
        if (m_urlQuickSelectActive) {
            exitUrlQuickSelect();
            return;
        }

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
        // Alt+Click: rectangular selection mode start OR semantic output select
        if (event->modifiers() & Qt::AltModifier) {
            if (event->modifiers() & Qt::ShiftModifier) {
                // Alt+Shift+Click: semantic output selection
                QPoint cell = pixelToCell(event->pos());
                selectCommandOutput(cell.x());
                return;
            }
            // Alt+drag starts rectangular/column selection
            m_rectSelection = true;
            QPoint cell = pixelToCell(event->pos());
            clearSelection();
            m_selecting = true;
            m_selStart = cell;
            m_selEnd = m_selStart;
            update();
            return;
        }

        // Click on fold triangle to toggle fold
        if (event->pos().x() < m_padding + 10) {
            QPoint cell = pixelToCell(event->pos());
            const auto &regions = m_grid->promptRegions();
            for (auto &pr : regions) {
                int vr = pr.startLine - (m_grid->scrollbackSize() - m_scrollOffset);
                if (vr >= 0 && vr < m_grid->rows() && cell.x() == pr.startLine + m_grid->scrollbackSize() - m_scrollOffset) {
                    // Close enough — toggle fold for prompt at this line
                    if (pr.hasOutput && pr.commandEndMs > 0) {
                        const_cast<PromptRegion &>(pr).folded = !pr.folded;
                        update();
                        return;
                    }
                }
            }
        }

        // Triple-click detection: select entire line
        QPoint cell = pixelToCell(event->pos());
        if (m_lastClickTime.elapsed() < 400 && cell.x() == m_lastClickCell.x()) {
            m_clickCount++;
        } else {
            m_clickCount = 1;
        }
        m_lastClickTime.restart();
        m_lastClickCell = cell;

        if (m_clickCount >= 3) {
            // Triple-click: select entire line
            int globalLine = cell.x();
            m_selStart = QPoint(globalLine, 0);
            m_selEnd = QPoint(globalLine, m_grid->cols() - 1);
            m_hasSelection = true;
            m_selecting = false;
            m_clickCount = 0;

            QString text = selectedText();
            if (!text.isEmpty()) {
                QApplication::clipboard()->setText(text, QClipboard::Selection);
                if (m_autoCopyOnSelect)
                    QApplication::clipboard()->setText(text);
            }
            update();
            return;
        }

        m_rectSelection = false;
        clearSelection();
        m_selecting = true;
        m_selStart = cell;
        m_selEnd = m_selStart;
        update();
    }
    if (event->button() == Qt::MiddleButton && m_pty) {
        QString text = QApplication::clipboard()->text(QClipboard::Selection);
        if (!text.isEmpty())
            pasteToTerminal(text.toUtf8());
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
        m_lastMousePos = event->pos();
        m_selEnd = pixelToCell(event->pos());
        m_hasSelection = (m_selStart != m_selEnd);

        // Auto-scroll when dragging past top/bottom edges
        if (event->pos().y() < m_padding) {
            m_selectionScrollDirection = 1; // scroll up (increase offset)
            if (!m_selectionScrollTimer.isActive())
                m_selectionScrollTimer.start();
        } else if (event->pos().y() > height() - m_padding - (m_searchVisible ? m_searchBar->height() : 0)) {
            m_selectionScrollDirection = -1; // scroll down (decrease offset)
            if (!m_selectionScrollTimer.isActive())
                m_selectionScrollTimer.start();
        } else {
            m_selectionScrollDirection = 0;
            m_selectionScrollTimer.stop();
        }

        update();
    }

    // URL hover detection — underline on hover, show hand cursor
    QPoint cell = pixelToCell(event->pos());
    int oldHoverLine = m_hoverGlobalLine;
    int oldHoverCol = m_hoverCol;
    m_hoverGlobalLine = -1;
    m_hoverCol = -1;

    auto spans = detectUrls(cell.x());
    bool onLink = false;
    for (const auto &s : spans) {
        if (cell.y() >= s.startCol && cell.y() <= s.endCol) {
            onLink = true;
            m_hoverGlobalLine = cell.x();
            m_hoverCol = cell.y();
            break;
        }
    }
    setCursor(onLink ? Qt::PointingHandCursor : Qt::IBeamCursor);

    // Repaint if hover state changed
    if (m_hoverGlobalLine != oldHoverLine || m_hoverCol != oldHoverCol)
        update();
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (mouseReportingActive() && !(event->modifiers() & Qt::ShiftModifier)) {
        sendMouseEvent(event, false, true);
        return;
    }

    if (event->button() == Qt::LeftButton && m_selecting) {
        m_selecting = false;
        m_selectionScrollTimer.stop();
        m_selectionScrollDirection = 0;
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
        } else {
            // Simple click (no drag) — click-to-move cursor in prompt
            QPoint cell = pixelToCell(event->pos());
            clickToMoveCursor(cell.y(), cell.x() - m_grid->scrollbackSize());
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

    if (m_rectSelection) {
        // Rectangular/column selection: same column range on every line
        int minCol = std::min(m_selStart.y(), m_selEnd.y());
        int maxCol = std::max(m_selStart.y(), m_selEnd.y());
        return globalLine >= sLine && globalLine <= eLine &&
               col >= minCol && col <= maxCol;
    }

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

    if (m_rectSelection) {
        // Rectangular selection: same column range on every line
        int minCol = std::min(m_selStart.y(), m_selEnd.y());
        int maxCol = std::max(m_selStart.y(), m_selEnd.y());
        for (int gl = s.x(); gl <= e.x(); ++gl) {
            QString line;
            for (int c = minCol; c <= maxCol && c < cols; ++c) {
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

void TerminalWidget::invalidateSpanCaches() const {
    m_urlSpanCache.clear();
    m_hlSpanCache.clear();
    m_spanCacheDirty = false;
}

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

    static QRegularExpression lineColRe(R"(^(.+):(\d+)(?::(\d+))?$)");
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

// --- URL Quick-Select Mode ---

void TerminalWidget::enterUrlQuickSelect() {
    m_urlQuickSelectActive = true;
    m_quickSelectInput.clear();
    m_quickSelectLabels.clear();

    int scrollbackSize = m_grid->scrollbackSize();
    int viewStart = scrollbackSize - m_scrollOffset;
    int viewEnd = viewStart + m_grid->rows();

    // Collect all URLs visible on screen
    int labelIdx = 0;
    QString labels = "asdfghjklqwertyuiopzxcvbnm";

    // Extended patterns: git SHAs, IP addresses, email addresses
    static QRegularExpression rxSha("\\b[0-9a-f]{7,40}\\b");
    static QRegularExpression rxIp("\\b(?:\\d{1,3}\\.){3}\\d{1,3}(?::\\d+)?\\b");
    static QRegularExpression rxEmail("\\b[\\w.+-]+@[\\w.-]+\\.[a-zA-Z]{2,}\\b");

    for (int gl = viewStart; gl < viewEnd && labelIdx < labels.size(); ++gl) {
        // First: URLs and file paths (from existing detection)
        auto spans = detectUrls(gl);
        for (const auto &s : spans) {
            if (labelIdx >= labels.size()) break;
            QuickSelectLabel ql;
            ql.globalLine = gl;
            ql.startCol = s.startCol;
            ql.endCol = s.endCol;
            ql.url = s.url;
            ql.isFilePath = s.isFilePath;
            ql.label = labels.mid(labelIdx, 1);
            m_quickSelectLabels.push_back(ql);
            ++labelIdx;
        }

        // Then: extended patterns (SHAs, IPs, emails)
        if (labelIdx >= labels.size()) break;
        QString lt = lineText(gl);

        auto addMatches = [&](const QRegularExpression &rx, bool isPath) {
            auto it = rx.globalMatch(lt);
            while (it.hasNext() && labelIdx < labels.size()) {
                auto m = it.next();
                int sc = static_cast<int>(m.capturedStart());
                int ec = static_cast<int>(m.capturedStart() + m.capturedLength() - 1);
                // Skip if overlapping with an existing URL span
                bool overlaps = false;
                for (const auto &existing : m_quickSelectLabels) {
                    if (existing.globalLine == gl && sc <= existing.endCol && ec >= existing.startCol) {
                        overlaps = true;
                        break;
                    }
                }
                if (overlaps) continue;

                QuickSelectLabel ql;
                ql.globalLine = gl;
                ql.startCol = sc;
                ql.endCol = ec;
                ql.url = m.captured();
                ql.isFilePath = isPath;
                ql.label = labels.mid(labelIdx, 1);
                m_quickSelectLabels.push_back(ql);
                ++labelIdx;
            }
        };

        addMatches(rxSha, false);
        addMatches(rxIp, false);
        addMatches(rxEmail, false);
    }

    if (m_quickSelectLabels.empty()) {
        m_urlQuickSelectActive = false;
        return;
    }

    update();
}

void TerminalWidget::exitUrlQuickSelect() {
    m_urlQuickSelectActive = false;
    m_quickSelectInput.clear();
    m_quickSelectLabels.clear();
    update();
}

// --- Command Output Folding ---

void TerminalWidget::toggleFoldAtCursor() {
    int globalLine = m_grid->scrollbackSize() + m_grid->cursorRow();
    std::vector<PromptRegion> &regions = m_grid->promptRegions();
    for (size_t i = 0; i < regions.size(); ++i) {
        if (globalLine >= regions[i].startLine && regions[i].hasOutput && regions[i].commandEndMs > 0) {
            regions[i].folded = !regions[i].folded;
            update();
            return;
        }
    }
}

// --- Badge Text ---

void TerminalWidget::setBadgeText(const QString &text) {
    m_badgeText = text;
    update();
}

// --- Scratchpad (Multi-line Command Editor) ---

void TerminalWidget::showScratchpad() {
    if (m_scratchpad) {
        m_scratchpad->show();
        m_scratchpad->findChild<QPlainTextEdit *>()->setFocus();
        return;
    }

    m_scratchpad = new QWidget(this);
    m_scratchpad->setObjectName("scratchpad");

    auto *layout = new QVBoxLayout(m_scratchpad);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(6);

    auto *titleLabel = new QLabel("Scratchpad — compose multi-line command", m_scratchpad);
    titleLabel->setStyleSheet("color: gray; font-size: 11px;");
    layout->addWidget(titleLabel);

    auto *edit = new QPlainTextEdit(m_scratchpad);
    edit->setObjectName("scratchpadEdit");
    edit->setPlaceholderText("Type your command here...\nCtrl+Enter to send, Escape to close");
    edit->setMinimumHeight(100);
    edit->setFont(m_font);
    layout->addWidget(edit);

    auto *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    auto *sendBtn = new QPushButton("Send (Ctrl+Enter)", m_scratchpad);
    auto *closeBtn = new QPushButton("Close (Esc)", m_scratchpad);
    btnLayout->addWidget(sendBtn);
    btnLayout->addWidget(closeBtn);
    layout->addLayout(btnLayout);

    // Style
    m_scratchpad->setStyleSheet(
        "QWidget#scratchpad { background: rgba(30,30,50,230); border: 1px solid rgba(100,100,140,150); border-radius: 8px; }"
        "QPlainTextEdit { background: rgba(20,20,35,200); color: #CDD6F4; border: 1px solid rgba(80,80,110,120); border-radius: 4px; padding: 6px; }"
        "QPushButton { background: rgba(60,60,90,200); color: #CDD6F4; border: 1px solid rgba(100,100,140,150); border-radius: 4px; padding: 4px 12px; }"
        "QPushButton:hover { background: rgba(80,80,120,220); }"
    );

    connect(sendBtn, &QPushButton::clicked, this, [this, edit]() {
        QString text = edit->toPlainText();
        if (!text.isEmpty() && m_pty) {
            pasteToTerminal(text.toUtf8());
            m_pty->write("\r");
        }
        edit->clear();
        hideScratchpad();
    });

    connect(closeBtn, &QPushButton::clicked, this, &TerminalWidget::hideScratchpad);

    // Ctrl+Enter to send, Escape to close
    edit->installEventFilter(this);

    // Position and size
    int padW = 40;
    m_scratchpad->setGeometry(padW, height() / 2 - 100, width() - padW * 2, 200);
    m_scratchpad->show();
    edit->setFocus();
}

void TerminalWidget::hideScratchpad() {
    if (m_scratchpad) {
        m_scratchpad->hide();
        setFocus();
    }
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

void TerminalWidget::setPadding(int px) {
    m_padding = qBound(0, px, 32);
    recalcGridSize();
    update();
}

void TerminalWidget::updateScrollToBottomButton() {
    if (!m_scrollToBottomBtn) return;
    if (m_scrollOffset > 0) {
        int x = width() - 52;
        int y = height() - 52;
        if (m_searchVisible) y -= m_searchBar->height();
        m_scrollToBottomBtn->move(x, y);
        m_scrollToBottomBtn->show();
        m_scrollToBottomBtn->raise();
    } else {
        m_scrollToBottomBtn->hide();
    }
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
    // Notify listeners that the terminal has recent output (debounced)
    emit outputReceived();

    // Claude Code permission prompts have this structure:
    //   <Tool header>          e.g. "Bash command", "Read", "Edit", etc.
    //   <command/path details>
    //   ...
    //   y · yes / n · no
    //   Esc to cancel · Tab to accept · Ctrl+e to explain
    //
    // Strategy: anchor on the distinctive footer, then scan backwards for the tool header.

    int scrollbackSize = m_grid->scrollbackSize();
    int totalLines = scrollbackSize + m_grid->rows();
    int scanStart = std::max(0, totalLines - 30);

    // Step 1: Find the Claude Code prompt footer
    // Two known formats:
    //   a) "Tab to accept" (older/tool prompts)
    //   b) "Do you want to proceed?" (newer permission prompts)
    //   c) "y · yes / n · no" or "y/n" patterns
    int footerLine = -1;
    for (int i = totalLines - 1; i >= std::max(0, totalLines - 12); --i) {
        QString text = lineText(i);
        if (text.contains(QLatin1String("Tab to accept"))
            || text.contains(QLatin1String("Do you want to proceed"))
            || text.contains(QLatin1String("allow access to"))
            || text.contains(QLatin1String("always allow"))) {
            footerLine = i;
            break;
        }
    }
    if (footerLine < 0) {
        // No prompt on screen — reset so we can detect the next one
        m_lastDetectedRule.clear();
        return;
    }

    // Step 2: Scan backwards from footer for the tool type header
    static QRegularExpression bashRe(R"(Bash\s+command|Bash\()");
    static QRegularExpression otherToolRe(
        R"(\b(Read|Edit|Write|Glob|Grep|WebFetch|WebSearch|Agent|NotebookEdit|Bash|TaskCreate|TaskUpdate|Skill|ToolSearch)\b)");
    // Also match "allow access to <path>" patterns from newer prompts
    static QRegularExpression allowAccessRe(R"(allow\s+access\s+to\s+(.+?)(?:\s+from|\s*$))");

    QString toolType;
    int headerLine = -1;
    QString extractedPath;

    for (int i = footerLine; i >= scanStart; --i) {
        QString text = lineText(i).trimmed();
        if (text.isEmpty()) continue;

        // Check for "allow access to <path>" pattern
        auto am = allowAccessRe.match(text);
        if (am.hasMatch()) {
            extractedPath = am.captured(1).trimmed();
        }

        if (bashRe.match(text).hasMatch()) {
            toolType = QStringLiteral("Bash");
            headerLine = i;
            break;
        }
        // Match other tool names on short-ish lines (likely headers, not content)
        if (text.length() < 60) {
            auto tm = otherToolRe.match(text);
            if (tm.hasMatch()) {
                toolType = tm.captured(1);
                headerLine = i;
                break;
            }
        }
    }
    if (toolType.isEmpty()) {
        // If we found an "allow access" path but no tool type, infer from path
        if (!extractedPath.isEmpty()) {
            toolType = QStringLiteral("Read"); // default for file access
        } else {
            return;
        }
    }

    // Step 3: Extract command/path from lines between header and footer
    QString toolArg;
    if (headerLine >= 0) {
        for (int j = headerLine + 1; j < footerLine; ++j) {
            QString next = lineText(j).trimmed();
            if (next.isEmpty()) continue;
            // Stop at prompt UI elements
            if (next.startsWith(QLatin1String("y ")) || next.startsWith(QLatin1String("n "))
                || next == QLatin1String("y") || next == QLatin1String("n")
                || next.contains(QLatin1String("Do you want"))
                || next.contains(QLatin1String("Esc to cancel")))
                break;
            toolArg = next;
            break;
        }
    }

    // Step 4: Build the allowlist rule
    QString rule;
    if (!extractedPath.isEmpty()) {
        // Use the more specific path from "allow access to" prompts
        rule = toolType + QStringLiteral("(") + extractedPath + QStringLiteral(")");
    } else if (toolType == QLatin1String("Bash") && !toolArg.isEmpty()) {
        rule = QStringLiteral("Bash(") + toolArg + QStringLiteral(")");
    } else if (!toolArg.isEmpty()) {
        rule = toolType + QStringLiteral("(") + toolArg + QStringLiteral(")");
    } else {
        rule = toolType;
    }

    if (rule != m_lastDetectedRule) {
        m_lastDetectedRule = rule;
        emit claudePermissionDetected(rule);
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
        if (!text.isEmpty() && m_pty)
            pasteToTerminal(text.toUtf8());
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
    m_spanCacheDirty = true;
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

// --- Visual Bell ---

void TerminalWidget::triggerVisualBell() {
    if (!m_visualBellEnabled) return;
    m_bellFlashActive = true;
    m_bellFlashTimer.start();
    update();
}

// --- Per-style Font Families ---

void TerminalWidget::setBoldFontFamily(const QString &family) {
    if (family.isEmpty()) return;
    m_fontBold = QFont(family, m_font.pointSize());
    m_fontBold.setBold(true);
    m_fontBold.setStyleHint(QFont::Monospace);
    m_fontBold.setFixedPitch(true);
    update();
}

void TerminalWidget::setItalicFontFamily(const QString &family) {
    if (family.isEmpty()) return;
    m_fontItalic = QFont(family, m_font.pointSize());
    m_fontItalic.setItalic(true);
    m_fontItalic.setStyleHint(QFont::Monospace);
    m_fontItalic.setFixedPitch(true);
    update();
}

void TerminalWidget::setBoldItalicFontFamily(const QString &family) {
    if (family.isEmpty()) return;
    m_fontBoldItalic = QFont(family, m_font.pointSize());
    m_fontBoldItalic.setBold(true);
    m_fontBoldItalic.setItalic(true);
    m_fontBoldItalic.setStyleHint(QFont::Monospace);
    m_fontBoldItalic.setFixedPitch(true);
    update();
}

// --- Background Image ---

void TerminalWidget::setBackgroundImage(const QString &path) {
    m_backgroundImagePath = path;
    if (path.isEmpty()) {
        m_backgroundImage = QImage();
    } else {
        m_backgroundImage = QImage(path);
        if (!m_backgroundImage.isNull())
            m_backgroundImage = m_backgroundImage.scaled(size(), Qt::KeepAspectRatioByExpanding,
                                                          Qt::SmoothTransformation);
    }
    update();
}

// --- Semantic Output Selection ---

void TerminalWidget::selectCommandOutput(int globalLine) {
    const auto &regions = m_grid->promptRegions();
    if (regions.empty()) return;

    // Find the prompt region that contains this line
    for (size_t i = 0; i < regions.size(); ++i) {
        int outputStart = regions[i].endLine; // output starts after prompt ends
        int outputEnd = (i + 1 < regions.size()) ? regions[i + 1].startLine
                                                  : m_grid->scrollbackSize() + m_grid->rows();

        if (globalLine >= outputStart && globalLine < outputEnd) {
            // Select this entire output region
            m_hasSelection = true;
            m_selStart = QPoint(outputStart, 0);
            m_selEnd = QPoint(outputEnd - 1, m_grid->cols() - 1);
            update();

            // Auto-copy to clipboard
            copySelection();
            return;
        }
    }
}
