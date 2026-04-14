#pragma once

#include "terminalgrid.h"
#include "vtparser.h"
#include "ptyhandler.h"

#include <QOpenGLWidget>
#include <QFont>
#include <QTimer>
#include <QImage>
#include <QLineEdit>
#include <QRegularExpression>
#include <QFile>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QScrollBar>
#include <unordered_map>
#include <memory>

class GlRenderer;

class QLabel;
class QHBoxLayout;
class QPushButton;
class QPlainTextEdit;

class TerminalWidget : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget *parent = nullptr);
    ~TerminalWidget() override;

    bool startShell(const QString &workDir = QString());
    void applyThemeColors(const QColor &fg, const QColor &bg,
                          const QColor &cursorColor,
                          const QColor &accent = QColor(),
                          const QColor &border = QColor());
    int scrollOffset() const { return m_scrollOffset; }
    QString shellTitle() const { return m_lastTitle; }

    // Live font size change
    void setFontSize(int size);
    int fontSize() const;

    // Session logging
    void setSessionLogging(bool enabled);
    bool sessionLogging() const { return m_loggingEnabled; }

    // Auto-copy on select
    void setAutoCopyOnSelect(bool enabled) { m_autoCopyOnSelect = enabled; }
    bool autoCopyOnSelect() const { return m_autoCopyOnSelect; }

    // Scrollback limit
    void setMaxScrollback(int lines);

    // Editor command for opening file paths
    void setEditorCommand(const QString &cmd) { m_editorCommand = cmd; }

    // Image paste directory (auto-save + insert path)
    void setImagePasteDir(const QString &dir) { m_imagePasteDir = dir; }
    QString imagePasteDir() const { return m_imagePasteDir; }

    // Send raw data to the PTY (for actions like clear-line)
    void sendToPty(const QByteArray &data);

    // Terminal recording
    void startRecording(const QString &path);
    void stopRecording();
    bool isRecording() const { return m_recording; }

    // Bookmarks
    void toggleBookmark();
    void nextBookmark();
    void prevBookmark();
    const std::vector<int> &bookmarks() const { return m_bookmarks; }

    // Extract recent terminal output as plain text (for AI context)
    QString recentOutput(int lines = 50) const;

    // Write a command string to the PTY (for SSH manager, plugins)
    void writeCommand(const QString &cmd);

    // GPU rendering toggle
    void setGpuRendering(bool enabled);
    bool gpuRendering() const { return m_gpuRendering; }

    // Per-pixel background alpha (0-255)
    void setBackgroundAlpha(int alpha);
    int backgroundAlpha() const { return m_backgroundAlpha; }

    // Window-level opacity (0.0-1.0), applied to background via per-pixel alpha
    void setWindowOpacityLevel(double opacity);
    double windowOpacityLevel() const { return m_windowOpacity; }

    // Access to grid for session save/restore
    TerminalGrid *grid() { return m_grid.get(); }

    // Force grid size recalculation (call after session restore)
    void forceRecalcSize();

    // Get the shell's current working directory (via /proc/PID/cwd)
    QString shellCwd() const;

    // Get the shell's PID (for process tree inspection)
    pid_t shellPid() const;

    // Get the foreground process name (what's running in the shell)
    QString foregroundProcess() const;

    // Scrollback export
    QString exportAsText() const;
    QString exportAsHtml() const;

    // Highlight rules (regex patterns with colors)
    struct HighlightRule {
        QRegularExpression pattern;
        QColor fg;
        QColor bg;
    };
    void setHighlightRules(const QJsonArray &rules);

    // Trigger rules (regex -> action)
    struct TriggerRule {
        QRegularExpression pattern;
        QString actionType; // "notify", "sound", "command"
        QString actionValue;
    };
    void setTriggerRules(const QJsonArray &rules);

    // Autocomplete
    void setHistoryFile(const QString &path);

    // Font family
    void setFontFamily(const QString &family);
    QString fontFamily() const { return m_font.family(); }

    // Per-style font families
    void setBoldFontFamily(const QString &family);
    void setItalicFontFamily(const QString &family);
    void setBoldItalicFontFamily(const QString &family);

    // Visual bell
    void setVisualBell(bool enabled) { m_visualBellEnabled = enabled; }
    bool visualBell() const { return m_visualBellEnabled; }

    // Configurable padding
    void setPadding(int px);
    int padding() const { return m_padding; }

    // URL / hints quick-select mode (extended: URLs, paths, SHAs, IPs, emails)
    void enterUrlQuickSelect();
    void exitUrlQuickSelect();

    // Command output folding (OSC 133 regions)
    void toggleFoldAtCursor();

    // Rectangular (column/block) selection
    void setRectSelection(bool enabled) { m_rectSelection = enabled; }
    bool rectSelection() const { return m_rectSelection; }

    // Scratchpad (multi-line command editor)
    void showScratchpad();
    void hideScratchpad();

    // Badge text (watermark in terminal background)
    void setBadgeText(const QString &text);

    // Background image
    void setBackgroundImage(const QString &path);

    // Performance overlay
    void setShowPerformanceOverlay(bool show) { m_showPerfOverlay = show; update(); }
    bool showPerformanceOverlay() const { return m_showPerfOverlay; }

    // Semantic output selection (select command output block)
    void selectCommandOutput(int globalLine);

    // Rich clipboard — copy with text/html preserving terminal colors
    void copySelectionRich();

    // Broadcast callback: called when key data should be sent to other terminals too
    using BroadcastCallback = std::function<void(TerminalWidget *source, const QByteArray &data)>;
    void setBroadcastCallback(BroadcastCallback cb) { m_broadcastCallback = std::move(cb); }

    // Last command exit code (via OSC 133 D marker)
    int lastExitCode() const;
    // Last command output text (between OSC 133 C and D markers)
    QString lastCommandOutput() const;

    // Prompt navigation (OSC 133 shell integration). -1 = previous, +1 = next.
    // Public so MainWindow can expose via menu / command palette entries.
    void navigatePrompt(int direction);

signals:
    void titleChanged(const QString &title);
    void shellExited(int code);
    void imagePasted(const QImage &image);
    void claudePermissionDetected(const QString &rule);
    void triggerFired(const QString &pattern, const QString &actionType, const QString &actionValue);
    void commandFailed(int exitCode, const QString &output);
    void outputReceived();  // debounced notification of PTY output
    void desktopNotification(const QString &title, const QString &body);
    void progressChanged(int state, int percent);  // OSC 9;4: state = ProgressState enum, percent = 0-100

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private slots:
    void onPtyData(const QByteArray &data);
    void onPtyFinished(int exitCode);
    void blinkCursor();
    void checkIdleNotification();

private:
    void recalcGridSize();
    void updateFontMetrics();
    QPoint pixelToCell(const QPoint &pos) const;
    QString selectedText() const;
    void copySelection();
    void clearSelection();
    bool hasSelection() const;
    bool isCellSelected(int globalLine, int col) const;

    // Cell access helper (consolidates scrollback/screen lookup)
    const Cell &cellAtGlobal(int globalLine, int col) const;

    // Combining character access (returns null if none)
    const std::vector<uint32_t> *combiningAt(int globalLine, int col) const;

    // Build full cell text (base + combining chars)
    QString cellText(int globalLine, int col) const;

    // Mouse reporting
    void sendMouseEvent(QMouseEvent *event, bool press, bool release = false);
    bool mouseReportingActive() const;

    // URL and file path detection (includes OSC 8 hyperlinks)
    struct UrlSpan { int startCol; int endCol; QString url; bool isFilePath; bool isOsc8; };
    std::vector<UrlSpan> detectUrls(int globalLine) const;
    QString lineText(int globalLine) const;
    void openFileAtPath(const QString &path);
    // Opens a hyperlink, with OSC 8 homograph-attack warning when the visible
    // label encodes a hostname that doesn't match the URL host.
    void openHyperlink(const UrlSpan &span, int globalLine);

    // Search
    struct SearchMatch { int globalLine; int col; int length; };
    void showSearchBar();
    void hideSearchBar();
    void performSearch();
    void scrollToMatch();
    void searchNext();
    void searchPrev();
    bool isCellSearchMatch(int globalLine, int col) const;
    bool isCellCurrentMatch(int globalLine, int col) const;

    // Bracketed paste helper — sanitizes text and wraps with mode markers
    void pasteToTerminal(const QByteArray &data);
    // Returns true if the paste should proceed (user confirmed or no prompt needed)
    bool confirmDangerousPaste(const QByteArray &data);

    // Enable/disable multi-line paste confirmation (bound from config)
    bool m_confirmMultilinePaste = true;
public:
    void setConfirmMultilinePaste(bool enabled) { m_confirmMultilinePaste = enabled; }
    bool confirmMultilinePaste() const { return m_confirmMultilinePaste; }
private:

    // Kitty keyboard protocol encoding
    QByteArray encodeKittyKey(QKeyEvent *event) const;

    // Click-to-move cursor in shell prompt
    void clickToMoveCursor(int col, int row);

    // Bracket matching
    struct BracketPair { int line1; int col1; int line2; int col2; };
    BracketPair findMatchingBracket() const;
    uint32_t getCellCodepoint(int globalLine, int col) const;

    Pty *m_pty = nullptr;  // Owned by Qt parent-child (this)
    std::unique_ptr<TerminalGrid> m_grid;
    std::unique_ptr<VtParser> m_parser;

    QFont m_font;
    QFont m_fontBold;
    QFont m_fontItalic;
    QFont m_fontBoldItalic;
    int m_cellWidth = 0;
    int m_cellHeight = 0;
    int m_fontAscent = 0;
    int m_padding = 4;

    // Cursor blink
    QTimer m_cursorTimer;
    bool m_cursorBlinkOn = true;
    bool m_hasFocus = true;

    // Scrollback viewing
    int m_scrollOffset = 0;

    // Theme cursor color
    QColor m_cursorColor{0x89, 0xB4, 0xFA};

    // Track last title to avoid redundant signals
    QString m_lastTitle;

    // Selection (in global line coordinates)
    bool m_selecting = false;
    bool m_hasSelection = false;
    QPoint m_selStart{-1, -1};  // (globalLine, col)
    QPoint m_selEnd{-1, -1};

    // Selection highlight color
    QColor m_selectionBg{0x89, 0xB4, 0xFA};
    QColor m_selectionFg{0x1E, 0x1E, 0x2E};

    // Auto-copy on select
    bool m_autoCopyOnSelect = true;

    // URL detection
    QColor m_urlColor{0x89, 0xB4, 0xFA};

    // Search bar
    QWidget *m_searchBar = nullptr;
    QLineEdit *m_searchInput = nullptr;
    QLabel *m_searchLabel = nullptr;
    QPushButton *m_searchRegexBtn = nullptr;
    QPushButton *m_searchCaseBtn = nullptr;
    QPushButton *m_searchWordBtn = nullptr;
    bool m_searchVisible = false;
    bool m_searchRegexMode = false;
    bool m_searchCaseSensitive = false;
    bool m_searchWholeWord = false;
    bool m_searchPatternInvalid = false;
    QString m_searchText;
    std::vector<SearchMatch> m_searchMatches;
    int m_currentMatchIdx = -1;
    QColor m_searchHighlightBg{0xF9, 0xE2, 0xAF};
    QColor m_searchHighlightFg{0x1E, 0x1E, 0x2E};
    QColor m_searchCurrentBg{0xFA, 0xB3, 0x87};
    QColor m_searchCurrentFg{0x1E, 0x1E, 0x2E};

    // Bracket matching (cached)
    mutable BracketPair m_cachedBracket{-1, -1, -1, -1};
    mutable int m_cachedBracketCursorRow = -1;
    mutable int m_cachedBracketCursorCol = -1;
    QColor m_bracketHighlightBg{0x58, 0x5B, 0x70};

    // Session logging
    bool m_loggingEnabled = false;
    std::unique_ptr<QFile> m_logFile;

    // Idle notification (detect when long-running command finishes)
    QTimer m_idleTimer;
    QElapsedTimer m_lastOutputTime;
    QElapsedTimer m_commandStartTime;
    bool m_hadRecentOutput = false;
    bool m_notifiedIdle = true; // start as true so we don't notify on first prompt

    // Editor command for file path clicking
    QString m_editorCommand;

    // Image paste auto-save directory
    QString m_imagePasteDir;

    // Synchronized output buffering
    bool m_syncOutputActive = false;
    QTimer m_syncTimer;  // safety timeout for stuck sync mode
    QByteArray m_syncBuffer;

    // Font fallback for missing glyphs (emoji, CJK)
    QFont m_fallbackFont;
    bool m_hasFallbackFont = false;

    // Last mouse cell for SGR reporting
    QPoint m_lastMouseCell{-1, -1};

    // Terminal recording (asciicast v2)
    bool m_recording = false;
    std::unique_ptr<QFile> m_recordFile;
    QElapsedTimer m_recordTimer;

    // Line bookmarks
    std::vector<int> m_bookmarks; // global line numbers

    // GPU rendering
    bool m_gpuRendering = false;
    std::unique_ptr<GlRenderer> m_glRenderer;

    // Per-pixel background alpha (0=transparent, 255=opaque)
    int m_backgroundAlpha = 255;
    double m_windowOpacity = 1.0;

    // Claude Code permission detection
    QTimer m_claudeDetectTimer;
    QString m_lastDetectedRule;
    void checkForClaudePermissionPrompt();

    // Broadcast callback
    BroadcastCallback m_broadcastCallback;

    // Highlight rules
    std::vector<HighlightRule> m_highlightRules;

    // Paint-loop span caches (avoid per-frame regex matching)
    struct PaintHighlightSpan { int start; int end; QColor fg; QColor bg; };
    mutable std::unordered_map<int, std::vector<UrlSpan>> m_urlSpanCache;
    mutable std::unordered_map<int, std::vector<PaintHighlightSpan>> m_hlSpanCache;
    mutable bool m_spanCacheDirty = true;
    void invalidateSpanCaches() const;

    // Trigger rules
    std::vector<TriggerRule> m_triggerRules;
    void checkTriggers(const QByteArray &data);

    // Exit code tracking for error detection
    int m_lastTrackedExitCode = 0;

    // Autocomplete / history suggestions
    QStringList m_historyEntries;
    QString m_currentSuggestion;
    bool m_historyLoaded = false;
    void loadHistory();
    void updateSuggestion();

    // Visual bell
    bool m_visualBellEnabled = true;
    bool m_bellFlashActive = false;
    QTimer m_bellFlashTimer;
    void triggerVisualBell();

    // Background image
    QImage m_backgroundImage;
    QString m_backgroundImagePath;

    // Performance overlay
    bool m_showPerfOverlay = false;
    QElapsedTimer m_perfFrameTimer;
    int m_perfFrameCount = 0;
    double m_perfFps = 0.0;
    qint64 m_perfLastPaintUs = 0;

    // Scrollbar
    QScrollBar *m_scrollBar = nullptr;
    void updateScrollBar();
    void positionScrollBar();

    // Smooth scrolling
    double m_smoothScrollTarget = 0.0;
    double m_smoothScrollCurrent = 0.0;
    QTimer m_smoothScrollTimer;
    void smoothScrollStep();

    // Selection auto-scroll (drag past edge)
    QTimer m_selectionScrollTimer;
    int m_selectionScrollDirection = 0; // -1=up, 0=none, 1=down
    QPoint m_lastMousePos;

    // Triple-click
    QElapsedTimer m_lastClickTime;
    QPoint m_lastClickCell{-1, -1};
    int m_clickCount = 0;

    // URL hover
    int m_hoverGlobalLine = -1;
    int m_hoverCol = -1;

    // Scroll-to-bottom button
    QPushButton *m_scrollToBottomBtn = nullptr;
    void updateScrollToBottomButton();

    // New output marker (line number when user scrolled up and new output arrived)
    int m_newOutputMarkerLine = -1;

    // URL quick-select mode
    bool m_urlQuickSelectActive = false;
    struct QuickSelectLabel { int globalLine; int startCol; int endCol; QString url; QString label; bool isFilePath; };
    std::vector<QuickSelectLabel> m_quickSelectLabels;
    QString m_quickSelectInput;

    // Cursor glow
    QColor m_cursorGlowColor;

    // Rectangular (column/block) selection
    bool m_rectSelection = false;

    // Badge text (large watermark in background)
    QString m_badgeText;

    // Scratchpad overlay
    QWidget *m_scratchpad = nullptr;

    // Nerd Font symbol fallback
    QFont m_nerdFallbackFont;
    bool m_hasNerdFallback = false;
};
