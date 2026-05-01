#pragma once

#include "terminalgrid.h"
#include "vtstream.h"

#include <QWidget>
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
#include <sys/types.h>

class QLabel;
class QHBoxLayout;
class QPushButton;
class QPlainTextEdit;
class QThread;

class TerminalWidget : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget *parent = nullptr);
    ~TerminalWidget() override;

    // Start the PTY-backed shell.
    //   workDir: cwd for the child (empty → $HOME fallback path).
    //   shell:   user-chosen shell override (empty → $SHELL → pw_shell →
    //            /bin/bash in that order, see ptyhandler.cpp::Pty::start).
    //            Populated by MainWindow from Config::shellCommand() so
    //            the Settings → General shell picker actually takes
    //            effect instead of being silently dropped on the floor.
    bool startShell(const QString &workDir = QString(),
                    const QString &shell = QString());
    void applyThemeColors(const QColor &fg, const QColor &bg,
                          const QColor &cursorColor,
                          const QColor &accent = QColor(),
                          const QColor &border = QColor());
    int scrollOffset() const { return m_scrollOffset; }
    const QString &shellTitle() const { return m_lastTitle; }

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
    const QString &imagePasteDir() const { return m_imagePasteDir; }

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

    // Opacity (0.0-1.0), driven by `opacity` config key. Despite the
    // method name and the m_windowOpacity member, this is per-pixel
    // alpha for the terminal-area fillRect only — the window chrome
    // (menubar, tabs, status bar, title bar) is painted by the parent
    // widget tree and stays fully opaque. Users at 0.9-0.95 get a
    // slightly translucent terminal area with solid chrome.
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

    // Trigger rules (regex -> action). 0.6.9 adds:
    //   - actions: "bell", "inject", "run_script" (Lua function/action id)
    //   - instant: fire on every PTY chunk vs. only on completed lines
    //     (today checkTriggers runs on every chunk; instant is honored as the
    //     current behavior, while non-instant batches into completed-line
    //     evaluation — matches iTerm2's "Instant" flag semantics).
    struct TriggerRule {
        QRegularExpression pattern;
        // Dispatch types (emit via checkTriggers → triggerFired signal):
        //   "notify" | "sound" | "command" | "bell" | "inject" | "run_script"
        // Grid-mutation types (run via onLineCompleted, mutate grid cells):
        //   "highlight_line" | "highlight_text" | "make_hyperlink"  (0.6.13)
        QString actionType;
        QString actionValue;
        bool instant = false;
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

    // Command-mark gutter (0.6.41): paint a narrow column of tick marks
    // just left of the scrollbar, one per OSC 133 PromptRegion, colored
    // by exit status (green/red/gray). No-op when no prompt regions
    // exist (users without shell integration see no visual change).
    void setShowCommandMarks(bool enabled) { m_showCommandMarks = enabled; update(); }
    bool showCommandMarks() const { return m_showCommandMarks; }

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

    // Command-block helpers (0.7.0). Block = one PromptRegion. index is
    // into m_grid->promptRegions(); validity is the caller's problem (-1
    // for "no block at that line"). The context menu is the primary caller.
    //   promptRegionIndexAtLine: finds the region whose range contains
    //     globalLine (prompt line or its output). Returns -1 if none.
    //   commandTextAt: extracts just the command (strips PS1 using the B
    //     marker column). Multi-line commands are joined with newlines.
    //   outputTextAt: extracts output lines between C and next A (or D).
    //   rerunCommandAt: writes the command text + '\r' to the PTY.
    //   toggleFoldAt: flips region.folded and triggers a repaint.
    //   exportBlockAsCast: writes a standalone asciicast v2 file with this
    //     block's output as a single event chunk.
    int promptRegionIndexAtLine(int globalLine) const;
    QString commandTextAt(int index) const;
    QString outputTextAt(int index) const;
    void rerunCommandAt(int index);
    void toggleFoldAt(int index);
    bool exportBlockAsCast(int index, const QString &path) const;

    // "Last completed command" top-level actions (0.6.40). These walk
    // promptRegions() backwards for the most recent region with
    // commandEndMs > 0 (skipping the tail region if a command is still
    // running), then delegate to outputTextAt / rerunCommandAt.
    // Return value is the number of characters copied / the index re-run
    // (>=0 on success), or -1 when no completed command exists. Callers
    // use the return to decide whether to toast "copied N chars" or
    // "no completed command" in the status bar.
    //
    // Why not reuse lastCommandOutput(): that helper caps at 100 lines
    // (it feeds commandFailed triggers where huge stderr noise is bad).
    // For a user-initiated "copy last output" action, capping silently
    // would lose data — outputTextAt is unbounded.
    int copyLastCommandOutput();
    int rerunLastCommand();

signals:
    void titleChanged(const QString &title);
    void shellExited(int code);
    void imagePasted(const QImage &image);
    void claudePermissionDetected(const QString &rule);
    // Fires when a previously-detected Claude Code permission prompt is no
    // longer on screen (user responded, or prompt scrolled off). Used by
    // mainwindow to retract the status-bar allowlist button. This is
    // stricter than `outputReceived` — the latter fires on every repaint
    // tick while the prompt is still up, which would retract the button
    // prematurely during Claude Code's continuous TUI animation.
    void claudePermissionCleared();
    void triggerFired(const QString &pattern, const QString &actionType, const QString &actionValue);
    void commandFailed(int exitCode, const QString &output);
    void outputReceived();  // debounced notification of PTY output
    void desktopNotification(const QString &title, const QString &body);
    void progressChanged(int state, int percent);  // OSC 9;4: state = ProgressState enum, percent = 0-100
    // 0.6.9 — trigger system bundle:
    //   commandFinished: emitted on OSC 133 D (after exit code parsed).
    //   userVarChanged:  emitted on OSC 1337;SetUserVar=NAME=base64(value).
    //   triggerRunScript: emitted by trigger system when an action_type is
    //     "run_script" — actionValue is the action id, payload is the
    //     matched substring. Routed by MainWindow into a plugin event.
    void commandFinished(int exitCode, qint64 durationMs);
    void userVarChanged(const QString &name, const QString &value);
    void triggerRunScript(const QString &actionId, const QString &matched);
    // 0.7.0 — fired the first time per cooldown window the OSC 133 HMAC
    // verifier rejects a marker. `count` is the running total since
    // process start. Only fires when the verifier is active (env var set).
    void osc133ForgeryDetected(int count);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
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
    void onPtyFinished(int exitCode);
    // Handler for VtStream::batchReady; applies the pre-parsed action
    // stream to m_grid, consumes the raw-byte side-data (selection-clear
    // hint, logging, asciicast), then acknowledges the batch so the
    // worker can refill the queue.
    void onVtBatch(VtBatchPtr batch);
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

    // Bracketed paste helper — sanitizes text and wraps with mode markers.
    // If the payload is "risky" (multi-line, sudo, pipe-to-shell, control
    // chars) and the confirm-multiline-paste setting is on, this shows a
    // non-modal QDialog and defers the actual paste to its Accept callback.
    // Otherwise pastes immediately.
    void pasteToTerminal(const QByteArray &data);
private:
    // Unconditional paste — pasteToTerminal() funnels into this after the
    // (possibly-async) confirmation step.
    void performPaste(const QByteArray &data);
    // Classify a payload as risky and, if so, return a reason list for the
    // confirmation dialog headline. Empty list = safe to paste without prompt.
    QStringList pasteRiskReasons(const QByteArray &data) const;

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

    std::unique_ptr<TerminalGrid> m_grid;

    // VtStream owns the Pty + VtParser on m_parseThread. GUI thread
    // talks to it only via queued invokeMethod / batchReady signal.
    QThread *m_parseThread = nullptr;
    VtStream *m_vtStream = nullptr;

    // Cross-thread PTY-write entry point — Qt::QueuedConnection invoke
    // so the PTY master FD is only touched on the worker thread. All
    // keystroke / response-callback / paste call sites route through
    // here.
    void ptyWrite(const QByteArray &data);

    // "Is a PTY available to write to?" — true once startShell() has
    // succeeded. Call sites must guard on this rather than m_vtStream
    // directly so future refactors don't silently break them.
    bool hasPty() const { return m_vtStream != nullptr; }

    // Child-PID accessor. The Pty lives on the worker thread; reading
    // its childPid() is safe because the PID is written once during
    // forkpty() (synchronised by startShell's BlockingQueuedConnection)
    // and never changes afterwards.
    pid_t ptyChildPid() const;

    QFont m_font;
    QFont m_fontBold;
    QFont m_fontItalic;
    QFont m_fontBoldItalic;
    int m_cellWidth = 0;
    int m_cellHeight = 0;
    int m_fontAscent = 0;
    // Cached on font change so the per-cell underline path in paintEvent
    // doesn't rebuild QFontMetrics for every drawn cell — the 0.7.7 perf
    // pass measured this as a hot allocator on TUI apps that underline
    // every cell (Claude Code's SGR 4 pattern).
    int m_fontUnderlinePos = 1;
    int m_fontLineWidth = 1;
    int m_padding = 4;

    // Cursor blink
    QTimer m_cursorTimer;
    bool m_cursorBlinkOn = true;
    bool m_hasFocus = true;

    // Scrollback viewing
    int m_scrollOffset = 0;

    // Frozen-view snapshot (0.6.33, user spec 2026-04-18). When the
    // user scrolls up (m_scrollOffset transitions 0 → >0), the current
    // screen rows are captured into m_frozenScreenRows + the
    // combining-char side-map into m_frozenScreenCombining. While the
    // snapshot is populated, cellAtGlobal / combiningAt read screen
    // rows from the snapshot instead of the live grid — so incoming
    // PTY output cannot change what the user sees. When
    // m_scrollOffset returns to 0 (or resize / alt-screen / RIS
    // events invalidate the snapshot), the snapshot is cleared and
    // live reads resume. Shape mirrors TerminalGrid's screen-row
    // storage so the intercept is a trivial index lookup.
    std::vector<std::vector<Cell>> m_frozenScreenRows;
    std::vector<std::unordered_map<int, std::vector<uint32_t>>>
        m_frozenScreenCombining;
    void captureScreenSnapshot();
    void clearScreenSnapshot();

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

    // Opacity factor for the terminal-area fillRect paint (0.0-1.0).
    // Driven by the `opacity` config key through setWindowOpacityLevel.
    // Name is historical — the value affects only the terminal's own
    // fillRect, not Qt's whole-window setWindowOpacity (the window
    // chrome stays solid).
    double m_windowOpacity = 1.0;

    // Claude Code permission detection
    QTimer m_claudeDetectTimer;
    QString m_lastDetectedRule;
    // Debounce counter for the "footer went off screen" retraction path.
    // Claude Code v2.1+ repaints its TUI several times per second while
    // a prompt is live; individual repaints can transiently push the
    // footer out of the 12-line lookback window, making one scan miss
    // even though the prompt is still on screen. Requiring N
    // consecutive misses before retracting the "Add to allowlist"
    // button eliminates the flicker without adding noticeable latency
    // to a real dismiss (N * 300 ms poll ≈ 900 ms — the button persists
    // ~1s past the approve/decline, well below the user's frustration
    // threshold). 0 means "footer visible last scan."
    int m_claudePermissionMissedCount = 0;
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
    // ANTS-1134 — track the monotonic scrollback-push count so the
    // span caches can drop stale entries whose globalLine key now
    // refers to a different historical line. Without this, URL +
    // highlight spans could paint at the wrong column ranges on
    // scrolled-back lines after high-throughput output.
    mutable uint64_t m_lastScrollbackPushed = 0;
    void invalidateSpanCaches() const;

    // Trigger rules
    std::vector<TriggerRule> m_triggerRules;
    void checkTriggers(const QByteArray &data);
    // 0.6.13 — grid-mutation trigger dispatch. Called by the TerminalGrid
    // line-completion callback once per newline, passing the screen row
    // that the cursor was leaving. Runs `highlight_line`, `highlight_text`,
    // and `make_hyperlink` rules against the finalized line text and applies
    // attr / hyperlink mutations directly to the grid row.
    void onGridLineCompleted(int screenRow);

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

    // Command-mark gutter (0.6.41). See the setter's docblock for rationale.
    bool m_showCommandMarks = true;

    // Badge text (large watermark in background)
    QString m_badgeText;

    // Scratchpad overlay
    QWidget *m_scratchpad = nullptr;

    // Nerd Font symbol fallback
    QFont m_nerdFallbackFont;
    bool m_hasNerdFallback = false;
};
