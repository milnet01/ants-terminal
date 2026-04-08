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
#include <memory>

class GlRenderer;

class QLabel;
class QHBoxLayout;

class TerminalWidget : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget *parent = nullptr);
    ~TerminalWidget() override;

    bool startShell();
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

    // Access to grid for session save/restore
    TerminalGrid *grid() { return m_grid.get(); }

    // Get the shell's current working directory (via /proc/PID/cwd)
    QString shellCwd() const;

signals:
    void titleChanged(const QString &title);
    void shellExited(int code);
    void imagePasted(const QImage &image);
    void claudePermissionDetected(const QString &rule);

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

    // Prompt navigation (OSC 133 shell integration)
    void navigatePrompt(int direction); // -1 = prev, +1 = next

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
    bool m_searchVisible = false;
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
    QByteArray m_syncBuffer;

    // Font fallback for missing glyphs (emoji, CJK)
    QFont m_fallbackFont;
    bool m_hasFallbackFont = false;

    // Last mouse button state for SGR reporting
    int m_lastMouseButton = -1;
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

    // Claude Code permission detection
    QTimer m_claudeDetectTimer;
    QString m_lastDetectedRule;
    void checkForClaudePermissionPrompt();
};
