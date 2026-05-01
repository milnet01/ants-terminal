#pragma once

#include "vtparser.h"
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <functional>
#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QString>

// Cursor shape for DECSCUSR (CSI Ps SP q)
enum class CursorShape : uint8_t {
    BlinkBlock = 0,
    BlinkBlock1 = 1,
    SteadyBlock = 2,
    BlinkUnderline = 3,
    SteadyUnderline = 4,
    BlinkBar = 5,
    SteadyBar = 6,
};

// Underline style for SGR 4:x
enum class UnderlineStyle : uint8_t {
    None = 0,
    Single = 1,
    Double = 2,
    Curly = 3,    // undercurl
    Dotted = 4,
    Dashed = 5,
};

struct CellAttrs {
    QColor fg;
    QColor bg;
    QColor underlineColor;  // CSI 58;2;r;g;b m — invalid means use fg
    bool bold = false;
    bool italic = false;
    bool underline = false;
    UnderlineStyle underlineStyle = UnderlineStyle::None;
    bool inverse = false;
    bool dim = false;
    bool strikethrough = false;
};

struct Cell {
    uint32_t codepoint = ' ';
    CellAttrs attrs;
    bool isWideChar = false;     // true if this cell is a double-width character
    bool isWideCont = false;     // true if this cell is the continuation of a wide char
};

// A line stored in scrollback or screen, with wrap metadata
struct TermLine {
    std::vector<Cell> cells;
    bool softWrapped = false; // true if line was wrapped at right edge (not \n)
    // Dirty bit — set whenever this line's cells mutate. Consumers (span caches,
    // future partial-repaint paths) read + clear it to skip re-work on clean
    // lines. Scoped to screen lines only; scrollback is treated as immutable
    // once pushed.
    bool dirty = true;
    // Combining characters: col -> list of combining codepoints (zero overhead when empty)
    std::unordered_map<int, std::vector<uint32_t>> combining;
};

// OSC 9;4 progress state (ConEmu / Microsoft Terminal convention)
// Spec: https://learn.microsoft.com/en-us/windows/terminal/tutorials/progress-bar-sequences
enum class ProgressState : uint8_t {
    None = 0,           // state 0: remove progress
    Normal = 1,         // state 1: progress value (blue bar)
    Error = 2,          // state 2: error state (red bar)
    Indeterminate = 3,  // state 3: indeterminate (animated bar)
    Warning = 4,        // state 4: paused / warning (yellow bar)
};

// Inline image placed in the terminal
struct InlineImage {
    QImage image;
    int row;        // screen row where image starts
    int col;        // column where image starts
    int cellWidth;  // width in cells (or pixels if pixelSized)
    int cellHeight; // height in cells (or pixels if pixelSized)
    bool pixelSized = false; // true for Sixel/Kitty (dimensions are pixels, not cells)
};

// OSC 8 hyperlink span stored per-line
struct HyperlinkSpan {
    int startCol;
    int endCol;
    std::string uri;
    std::string id;  // optional link id for grouping
};

// Shell integration (OSC 133) prompt region
struct PromptRegion {
    int startLine;   // global line (scrollback + screen)
    int endLine;
    bool hasOutput = false;
    qint64 commandStartMs = 0;  // epoch ms when command started (OSC 133 B)
    qint64 commandEndMs = 0;    // epoch ms when command ended (OSC 133 D)
    bool folded = false;        // whether output is collapsed
    int exitCode = 0;           // parsed from OSC 133 D (0.7.0). Only meaningful when commandEndMs > 0.
    int commandStartCol = 0;    // cursor column at OSC 133 B fire — lets UI extract command text from the prompt line (0.7.0).
    int outputStartLine = -1;   // global line at OSC 133 C — where command output begins, -1 if unset (0.7.0).
};

// Represents the terminal screen buffer and handles all actions from the parser.
class TerminalGrid {
public:
    TerminalGrid(int rows, int cols);

    void processAction(const VtAction &action);
    void resize(int rows, int cols);

    int rows() const { return m_rows; }
    int cols() const { return m_cols; }
    int cursorRow() const { return m_cursorRow; }
    int cursorCol() const { return m_cursorCol; }
    bool cursorVisible() const { return m_cursorVisible; }
    bool bracketedPaste() const { return m_bracketedPaste; }
    CursorShape cursorShape() const { return m_cursorShape; }
    bool cursorBlink() const {
        return m_cursorShape == CursorShape::BlinkBlock ||
               m_cursorShape == CursorShape::BlinkBlock1 ||
               m_cursorShape == CursorShape::BlinkUnderline ||
               m_cursorShape == CursorShape::BlinkBar;
    }
    bool applicationKeypad() const { return m_applicationKeypad; }
    bool applicationCursorKeys() const { return m_applicationCursorKeys; }
    int lastExitCode() const { return m_lastExitCode; }
    int commandOutputStartLine() const { return m_commandOutputStart; }

    const Cell &cellAt(int row, int col) const;
    const QString &windowTitle() const { return m_windowTitle; }

    // Scrollback
    int scrollbackSize() const { return static_cast<int>(m_scrollback.size()); }
    const std::vector<Cell> &scrollbackLine(int index) const { return m_scrollback[index].cells; }
    bool scrollbackLineWrapped(int index) const { return m_scrollback[index].softWrapped; }
    // Monotonic counter of lines ever pushed into scrollback — unlike scrollbackSize()
    // it keeps incrementing after the buffer hits m_maxScrollback (where a push pops a
    // stale line off the front). Scroll-anchor math must diff this, not the size, or
    // the viewport drifts by one content-line per push once the buffer is full.
    uint64_t scrollbackPushed() const { return m_scrollbackPushed; }

    // Configurable scrollback limit
    void setMaxScrollback(int lines);
    int maxScrollback() const { return m_maxScrollback; }

    // Default colors (set by theme) -- recolors existing cells
    void setDefaultFg(const QColor &c);
    void setDefaultBg(const QColor &c);
    QColor defaultFg() const { return m_defaultFg; }
    QColor defaultBg() const { return m_defaultBg; }

    // Inline images
    const std::vector<InlineImage> &inlineImages() const { return m_inlineImages; }

    // Response callback (DA, DSR, CPR replies sent back to PTY)
    using ResponseCallback = std::function<void(const std::string &)>;
    void setResponseCallback(ResponseCallback cb);
    // Send a response directly (used for unsolicited reports like color scheme change)
    void sendResponse(const std::string &data) { if (m_responseCallback) m_responseCallback(data); }

    // Bell callback (invoked on BEL character)
    using BellCallback = std::function<void()>;
    void setBellCallback(BellCallback cb) { m_bellCallback = std::move(cb); }

    // Combining character access per line (bounds-checked)
    const std::unordered_map<int, std::vector<uint32_t>> &screenCombining(int row) const {
        static const std::unordered_map<int, std::vector<uint32_t>> s_empty;
        if (row < 0 || row >= m_rows) return s_empty;
        return m_screenLines[row].combining;
    }
    const std::unordered_map<int, std::vector<uint32_t>> &scrollbackCombining(int idx) const {
        static const std::unordered_map<int, std::vector<uint32_t>> s_empty;
        if (idx < 0 || idx >= static_cast<int>(m_scrollback.size())) return s_empty;
        return m_scrollback[idx].combining;
    }

    // Mouse reporting modes
    bool mouseButtonMode() const { return m_mouseButtonMode; }
    bool mouseMotionMode() const { return m_mouseMotionMode; }
    bool mouseAnyMode() const { return m_mouseAnyMode; }
    bool mouseSgrMode() const { return m_mouseSgrMode; }

    // Focus reporting
    bool focusReporting() const { return m_focusReporting; }

    // Synchronized output
    bool synchronizedOutput() const { return m_synchronizedOutput; }

    // Alt screen active (vim, htop, etc.)
    bool altScreenActive() const { return m_altScreenActive; }

    // Scrollback-insert pause. When true, lines that scroll off the top of
    // the screen are NOT pushed into scrollback — they're just dropped.
    // The TerminalWidget sets this while the user is scrolled up into
    // history so that TUI-redraw traffic (Claude Code, spinners, progress
    // bars that re-emit their content via cursor movement on the main
    // screen) doesn't interleave intermediate frames into scrollback and
    // shift the user's reading position. Scrollback growth resumes when
    // the user scrolls back to the bottom. This is a policy knob, not a
    // mode-toggle: grid semantics are unchanged while paused; we simply
    // choose not to persist lines that are about to fall off.
    void setScrollbackInsertPaused(bool paused) { m_scrollbackInsertPaused = paused; }
    bool scrollbackInsertPaused() const { return m_scrollbackInsertPaused; }

    // Kitty keyboard protocol
    int kittyKeyFlags() const { return m_kittyKeyFlags; }

    // Color palette update notifications (CSI ? 2031)
    bool colorSchemeNotify() const { return m_colorSchemeNotify; }

    // Desktop notification callback (OSC 9/777)
    using NotifyCallback = std::function<void(const QString &title, const QString &body)>;
    void setNotifyCallback(NotifyCallback cb) { m_notifyCallback = std::move(cb); }

    // Per-row completion callback (0.6.13). Fires from newLine() BEFORE the
    // actual cursor/scroll motion, so the callback sees the screen row that
    // the cursor was leaving (and any mutation it applies via
    // applyRowAttrs / addRowHyperlink lands on the right row before that
    // row may be pushed to scrollback). Used by TerminalWidget to run the
    // grid-mutation trigger rules (highlight_line, highlight_text,
    // make_hyperlink) on the just-completed line. Passes the screen row
    // index; the widget computes the global line as needed.
    using LineCompletionCallback = std::function<void(int screenRow)>;
    void setLineCompletionCallback(LineCompletionCallback cb) {
        m_lineCompletionCallback = std::move(cb);
    }

    // Grid mutation helpers for trigger actions (0.6.13).
    // Override fg/bg attrs for cols [startCol..endCol] (inclusive) on screen
    // `row`. A default-constructed (invalid) QColor means "leave unchanged"
    // — so callers can set fg only, bg only, or both.
    void applyRowAttrs(int row, int startCol, int endCol,
                       const QColor &fg, const QColor &bg);
    // Push an OSC 8-equivalent hyperlink span onto screen `row`.
    void addRowHyperlink(int row, int startCol, int endCol, const QString &uri);

    // Progress reporting callback (OSC 9;4 / ConEmu)
    using ProgressCallback = std::function<void(ProgressState state, int percent)>;
    void setProgressCallback(ProgressCallback cb) { m_progressCallback = std::move(cb); }
    ProgressState progressState() const { return m_progressState; }
    int progressValue() const { return m_progressValue; }

    // Command-finished callback (OSC 133 D). Fires once per shell-emitted
    // command end with the parsed exit code and wall-clock duration in ms
    // (computed from the matching B marker; 0 if no B was seen).
    using CommandFinishedCallback = std::function<void(int exitCode, qint64 durationMs)>;
    void setCommandFinishedCallback(CommandFinishedCallback cb) { m_commandFinishedCallback = std::move(cb); }

    // User-var callback (iTerm2 OSC 1337;SetUserVar=NAME=<base64-value>).
    // Decoded value is forwarded as UTF-8 — non-UTF-8 payloads decode best-
    // effort. Fires once per `SetUserVar=` directive; duplicate sets fire
    // again so the consumer can react to value changes.
    using UserVarCallback = std::function<void(const QString &name, const QString &value)>;
    void setUserVarCallback(UserVarCallback cb) { m_userVarCallback = std::move(cb); }

    // OSC 133 HMAC forgery callback (0.7.0 shell-integration HMAC item).
    // Fires the first time per cooldown window that an OSC 133 marker arrives
    // with a missing or invalid `ahmac=` param while the verifier is active
    // (see m_osc133Key). Lets the UI surface a status-bar warning so the user
    // notices a process running inside the terminal trying to forge prompt
    // markers. `count` is the running total since process start (monotonic).
    using Osc133ForgeryCallback = std::function<void(int count)>;
    void setOsc133ForgeryCallback(Osc133ForgeryCallback cb) { m_osc133ForgeryCallback = std::move(cb); }
    int osc133ForgeryCount() const { return m_osc133ForgeryCount; }
    bool osc133HmacEnforced() const { return !m_osc133Key.isEmpty(); }
    // Test-only entry point: override the key without re-reading the env.
    // Production code never calls this — the env var is the only knob.
    void setOsc133KeyForTest(const QByteArray &key) { m_osc133Key = key; }

    // OSC 8 hyperlinks (per screen line)
    const std::vector<HyperlinkSpan> &screenHyperlinks(int row) const;
    const std::vector<HyperlinkSpan> &scrollbackHyperlinks(int idx) const;

    // Test-only: current size of the Kitty APC chunk-accumulation buffer.
    // Locked by tests/features/osc8_apc_memory_caps to confirm
    // MAX_KITTY_CHUNK_BYTES drops stale frames on cap breach.
    size_t kittyChunkBufferSizeForTest() const { return m_kittyChunkBuffer.size(); }

    // OSC 8 URI: real URLs are <2 KiB (browsers/servers cap similarly).
    // A hostile program could otherwise emit a ~10 MB URI (bounded only
    // by the VT parser's per-OSC cap) per open-close pair, then copy it
    // into every row-span and into scrollback — N rows × ~10 MB = tens
    // of GB. Reject the open entirely when the URI exceeds this cap;
    // following text prints unlinked. Aligns with the scheme-allowlist
    // drop path.
    static constexpr size_t MAX_OSC8_URI_BYTES = 2048;
    // Kitty APC chunked-transmission buffer: a single image chunked
    // across frames with `m=1` accumulates into m_kittyChunkBuffer until
    // `m=0`. Without a cap an attacker can keep sending `m=1` chunks
    // forever and exhaust RAM before the image budget (which runs after
    // the final chunk) gets a say. 32 MiB fits any legitimate chunked
    // upload (Kitty transmits in ~4 KiB frames; a 24 MiB decoded image
    // = ~32 MiB base64-encoded). Past the cap we drop the buffer and
    // reset chunk state so a subsequent `m=0` is ignored.
    static constexpr size_t MAX_KITTY_CHUNK_BYTES = 32ULL * 1024ULL * 1024ULL;

    // Shell integration (OSC 133)
    const std::vector<PromptRegion> &promptRegions() const { return m_promptRegions; }
    std::vector<PromptRegion> &promptRegions() { return m_promptRegions; }

    // Session restore: direct access for SessionManager (respects max scrollback)
    void pushScrollbackLine(TermLine &&line) {
        m_scrollback.push_back(std::move(line));
        while (static_cast<int>(m_scrollback.size()) > m_maxScrollback)
            m_scrollback.pop_front();
    }
    TermLine &screenLine(int row) { return m_screenLines[row]; }
    void setCursorPosition(int row, int col) { m_cursorRow = row; m_cursorCol = col; }
    void setTitle(const QString &title) { m_windowTitle = title; }

    // Clear screen content (keeps scrollback) — used after session restore
    void clearScreenContent();

    // Per-line dirty tracking. Callers (span caches, partial-paint paths) can
    // query which screen lines have been mutated since the last render, then
    // clear the flags to acknowledge. Scrollback lines are treated as
    // immutable once pushed and are not tracked.
    bool screenLineDirty(int row) const {
        return (row >= 0 && row < m_rows) ? m_screenLines[row].dirty : false;
    }
    void clearScreenLineDirty(int row) {
        if (row >= 0 && row < m_rows) m_screenLines[row].dirty = false;
    }
    void clearAllScreenDirty() {
        for (auto &line : m_screenLines) line.dirty = false;
    }
    void markScreenDirty(int row) {
        if (row >= 0 && row < m_rows) m_screenLines[row].dirty = true;
    }
    void markAllScreenDirty() {
        for (auto &line : m_screenLines) line.dirty = true;
    }

    // Debug logging — writes SGR/underline state to a log file
    void setDebugLog(bool enabled);
    bool debugLog() const { return m_debugLog; }

private:
    void handlePrint(uint32_t cp);
    // Fast path for the VtParser SIMD coalesced Print run. Every byte in
    // [data, data+len) is in [0x20..0x7E] (the parser's scanSafeAsciiRun
    // contract), so width=1, no combining, no UTF-8 decode. Avoids the
    // wcwidth() call and cell() clamping overhead per byte; splits the
    // run into per-row spans so markScreenDirty / combining.erase fire
    // once per row instead of once per byte.
    void handleAsciiPrintRun(const char *data, int len);
    // Zero the stranded mate when a write at [startCol, endCol) splits a
    // wide-char pair at either edge. Must be called BEFORE the write so we
    // can still tell the neighbor's pre-write state. See
    // tests/features/wide_char_overwrite_mate for the contract.
    void breakWidePairsAround(int row, int startCol, int endCol);
    void handleExecute(char ch);
    void handleCsi(const VtAction &a);
    void handleEsc(const VtAction &a);
    void handleOsc(const std::string &payload);
    void handleDcs(const std::string &payload);  // Sixel graphics
    void handleApc(const std::string &payload);  // Kitty graphics

    // CSI helpers
    void handleSGR(const std::vector<int> &params, const std::vector<bool> &colonSep = {});
    void eraseInDisplay(int mode);
    void eraseInLine(int mode);
    void insertLines(int count);
    void deleteLines(int count);
    void deleteChars(int count);
    void insertBlanks(int count);
    void scrollUp(int count);
    void scrollDown(int count);
    void setCursorPos(int row, int col);
    void moveCursor(int dRow, int dCol);
    void setScrollRegion(int top, int bottom);
    void saveCursor();
    void restoreCursor();

    // SGR color parsing helpers
    QColor parse256Color(const std::vector<int> &params, size_t &i);
    // 0.7.55 (2026-04-27 indie-review) — colonSep added so we can
    // detect the ITU/ECMA-48 colon-RGB form `38:2::r:g:b`. The empty
    // colorspace slot becomes a colon-separated zero param, shifting
    // R/G/B by 1 vs the legacy semicolon form `38;2;r;g;b`.
    QColor parseRGBColor(const std::vector<int> &params,
                         const std::vector<bool> &colonSep, size_t &i);

    void newLine();
    void carriageReturn();
    void tab();
    void reverseIndex();
    Cell &cell(int row, int col);
    void clearRow(int row, int startCol = 0, int endCol = -1);

    // OSC 1337 (iTerm2 inline images)
    void handleOscImage(const std::string &payload);

    int m_rows, m_cols;
    int m_cursorRow = 0, m_cursorCol = 0;
    bool m_cursorVisible = true;
    CellAttrs m_currentAttrs;

    // Screen lines with wrap metadata
    std::vector<TermLine> m_screenLines;
    // Convenience accessor for cell grid (delegates to m_screenLines)
    std::vector<Cell> &screenRow(int row) { return m_screenLines[row].cells; }
    const std::vector<Cell> &screenRow(int row) const { return m_screenLines[row].cells; }

    std::deque<TermLine> m_scrollback;
    int m_maxScrollback = 50000;
    // Monotonic — total lines ever pushed (for scroll-anchor drift correction when capped)
    uint64_t m_scrollbackPushed = 0;

    // Free-list of m_cols-sized cell buffers salvaged from discarded rows
    // (scrollback eviction, insertLines/deleteLines erase, scrollDown erase).
    // Recycled into new bottom/top rows on scrollUp/scrollDown/insert/delete,
    // avoiding the per-scroll vector<Cell>(m_cols) heap allocation that
    // dominates the newline_stream hot path. Capped to kFreePoolCap entries
    // so memory stays bounded when scrolls outpace consumption.
    std::vector<std::vector<Cell>> m_freeCellBuffers;
    static constexpr int kFreePoolCap = 4;
    // Pulls a blanked m_cols-wide cells row from the pool, or allocates fresh.
    std::vector<Cell> takeBlankedCellsRow();
    // Returns a cells buffer to the pool if it's the right size and the pool
    // isn't full. Otherwise discards (letting the vector destruct naturally).
    void returnCellsRow(std::vector<Cell> &&cells);
    // Background-Color Erase (BCE): the colour newly-exposed/cleared cells
    // pick up. xterm-compatible — current SGR bg if set, default otherwise.
    // Used by clearRow / insertLines / deleteLines / scrollUp / scrollDown /
    // deleteChars / insertBlanks so `\e[44mCSI L` leaves a blue gap, not
    // default-bg (vim/less/tmux depend on this).
    QColor eraseBg() const {
        return m_currentAttrs.bg.isValid() ? m_currentAttrs.bg : m_defaultBg;
    }

    QColor m_defaultFg{0xCD, 0xD6, 0xF4};  // Light text
    QColor m_defaultBg{0x1E, 0x1E, 0x2E};  // Dark bg

    // Scroll region (1-based inclusive in VT, stored as 0-based)
    int m_scrollTop = 0;
    int m_scrollBottom = 0; // 0 means "use m_rows - 1"

    // Saved cursor (DECSC / CSI s). Per VT420 spec, DECSC saves not just
    // the cursor position + SGR but also origin mode (DECOM) and auto-wrap
    // (DECAWM); DECRC restores all of them. Required for nested
    // save/restore patterns used by tmux/screen — without it, a program
    // that flips DECOM, scrolls, and DECRCs ends up with the wrong
    // coordinate space active. See tests/features/origin_mode_correctness.
    int m_savedRow = 0, m_savedCol = 0;
    CellAttrs m_savedAttrs;
    bool m_savedOriginMode = false;
    bool m_savedAutoWrap = true;

    // Modes
    bool m_originMode = false;
    bool m_autoWrap = true;
    bool m_wrapNext = false;  // delayed wrap
    bool m_bracketedPaste = false;  // CSI ?2004h/l

    // Cursor shape (DECSCUSR)
    CursorShape m_cursorShape = CursorShape::BlinkBlock;

    // Application keypad mode (DECKPAM/DECKPNM)
    bool m_applicationKeypad = false;
    // Application cursor keys (DECCKM, ?1)
    bool m_applicationCursorKeys = false;

    // Tab stops (customizable via HTS/TBC)
    std::vector<bool> m_tabStops;
    void initTabStops();

    // Last command exit code (OSC 133 D)
    int m_lastExitCode = 0;
    int m_commandOutputStart = -1; // global line where command output starts

    // Mouse reporting modes
    bool m_mouseButtonMode = false;   // ?1000 — report button press/release
    bool m_mouseMotionMode = false;   // ?1002 — report button+motion
    bool m_mouseAnyMode = false;      // ?1003 — report all motion
    bool m_mouseSgrMode = false;      // ?1006 — SGR encoding

    // Focus reporting
    bool m_focusReporting = false;    // ?1004

    // Synchronized output
    bool m_synchronizedOutput = false; // ?2026

    // Color palette update notifications
    bool m_colorSchemeNotify = false; // ?2031

    // Kitty keyboard protocol
    int m_kittyKeyFlags = 0;              // Current enhancement flags
    std::vector<int> m_kittyKeyStack;     // Push/pop stack of flag levels

    // Desktop notification callback (OSC 9/777)
    NotifyCallback m_notifyCallback;

    // Line-completion callback (0.6.13, trigger system grid-mutation actions)
    LineCompletionCallback m_lineCompletionCallback;

    // Progress reporting (OSC 9;4)
    ProgressCallback m_progressCallback;
    ProgressState m_progressState = ProgressState::None;
    int m_progressValue = 0;  // 0-100

    // Command-finished + user-var callbacks (0.6.9 trigger system bundle).
    // Both are owned-by-grid functions invoked synchronously from the OSC
    // dispatch path; consumers (TerminalWidget) translate to Qt signals.
    CommandFinishedCallback m_commandFinishedCallback;
    UserVarCallback m_userVarCallback;

    // OSC 52 per-terminal write quota — independent of the 1 MB per-string cap.
    // 32 writes/min + 1 MB/min. Counters reset when the minute window advances.
    qint64 m_osc52WindowStartMs = 0;
    int m_osc52WriteCount = 0;
    qint64 m_osc52WriteBytes = 0;
    static constexpr int OSC52_MAX_WRITES_PER_MIN = 32;
    static constexpr qint64 OSC52_MAX_BYTES_PER_MIN = 1 * 1024 * 1024;

    // Response callback
    ResponseCallback m_responseCallback;

    // Bell callback
    BellCallback m_bellCallback;

    // Pause scrollback insertion while the user is scrolled up (see
    // setScrollbackInsertPaused). False = normal terminal behaviour.
    bool m_scrollbackInsertPaused = false;

    // 0.6.22 — CSI 2J "redraw window" suppression. Main-screen full-display
    // erase (CSI 2J) almost always precedes a TUI full-screen re-paint
    // (Claude Code v2.1+ is the motivating case — it clears+repaints its
    // entire transcript view on *every* internal state update, not just
    // /compact). Without this suppression, each scroll-off line produced
    // by the re-paint is pushed into scrollback, producing N× duplicates
    // after N repaints.
    //
    // Policy: when eraseInDisplay(2) fires on the main screen, open a
    // time window. While the window is open, scrollUp() drops the
    // push-to-scrollback step instead of appending. Each scrollUp during
    // the window *extends* it (kCsiClearRedrawWindowMs from last scrollUp),
    // so the window survives the entire repaint burst regardless of its
    // length, but closes promptly when the app goes quiet. Alt-screen is
    // unaffected (already bypasses scrollback).
    //
    // 0.6.26 — Ink-repaint handling. Claude Code (Ink/React TUI) v2.1+
    // emits `CSI 2J + CSI 3J + CSI H + fullStaticOutput + output` from
    // renderInteractiveFrame when the rendered frame overflows the
    // viewport (see ink/build/ink.js:700 and ansi-escapes/base.js:124
    // clearTerminal = eraseScreen + "\x1B[3J" + "\x1B[H"). Naive
    // handling of that sequence destroys the user's scrollback (mode 3
    // clear) AND still leaves subsequent duplicates if the suppression
    // window closes mid-repaint. The `m_recentMode2Timer` tracks whether
    // CSI 2J just fired; if CSI 3J arrives inside that window it's
    // treated as Ink's overflow-repaint marker — scrollback clear is
    // *skipped* (preserving history) and the suppression window is
    // re-armed (preventing duplicates as the replay streams in).
    // Standalone CSI 3J (e.g. `printf '\e[3J'` at a shell prompt, no
    // recent CSI 2J) still clears scrollback as the user requested.
    bool m_csiClearRedrawActive = false;
    QElapsedTimer m_csiClearRedrawTimer;
    QElapsedTimer m_recentMode2Timer;
    static constexpr qint64 kCsiClearRedrawWindowMs = 250;
    static constexpr qint64 kMode2FollowupWindowMs = 50;

    // Alt screen buffer
    bool m_altScreenActive = false;
    std::vector<TermLine> m_altScreen;
    int m_altCursorRow = 0, m_altCursorCol = 0;
    int m_altScrollTop = 0, m_altScrollBottom = 0;
    std::vector<std::vector<HyperlinkSpan>> m_altScreenHyperlinks;
    std::vector<InlineImage> m_altInlineImages;
    std::vector<PromptRegion> m_altPromptRegions;
    // DECSC state checkpointed on 1049 entry, restored on 1049 exit.
    // 47 / 1047 do NOT save or restore the cursor / SGR / origin / wrap —
    // per xterm ctlseqs, only 1049 carries DECSC semantics. ANTS-1130
    // (0.7.65) split the three modes; m_altScreenMode tracks which mode
    // entered alt-screen so exit can do the right thing.
    CellAttrs m_altSavedAttrs;
    bool m_altSavedOriginMode = false;
    bool m_altSavedAutoWrap = true;
    // ANTS-1130 — mode that entered alt-screen (47, 1047, 1049, or 0
    // when not on alt screen). Consulted on exit to decide whether to
    // restore DECSC. xterm semantics: only 1049 restores cursor; 47
    // and 1047 leave the cursor / SGR / origin / wrap unchanged on exit.
    int m_altScreenMode = 0;

    QString m_windowTitle;

    // Inline images
    std::vector<InlineImage> m_inlineImages;

    // OSC 8 hyperlinks per screen/scrollback line
    std::vector<std::vector<HyperlinkSpan>> m_screenHyperlinks;
    std::deque<std::vector<HyperlinkSpan>> m_scrollbackHyperlinks;
    // Active hyperlink state (between open and close OSC 8)
    bool m_hyperlinkActive = false;
    std::string m_hyperlinkUri;
    std::string m_hyperlinkId;
    int m_hyperlinkStartCol = 0;
    int m_hyperlinkStartRow = 0;

    // Shell integration (OSC 133)
    std::vector<PromptRegion> m_promptRegions;
    int m_shellIntegState = 0; // 0=none, 'A'=prompt start, 'B'=command start, 'C'=output start

    // OSC 133 HMAC verifier state (0.7.0 shell-integration HMAC item).
    // m_osc133Key is read once from $ANTS_OSC133_KEY at construction. When
    // empty, verification is disabled and OSC 133 markers are processed as
    // before (backward-compatible behaviour for users without the shell
    // hook installed). When non-empty, every OSC 133 marker must carry a
    // matching `ahmac=` param computed as
    //   HMAC-SHA256(key, "<marker>|<promptId>[|<exitCode>]")
    // — markers without it (or with a mismatched HMAC) are dropped and
    // m_osc133ForgeryCount increments. The forgery callback fires at most
    // once per OSC133_FORGERY_COOLDOWN_MS so a tight forgery loop can't
    // spam the status-bar.
    QByteArray m_osc133Key;
    int m_osc133ForgeryCount = 0;
    qint64 m_osc133LastForgeryNotifyMs = 0;
    Osc133ForgeryCallback m_osc133ForgeryCallback;
    static constexpr qint64 OSC133_FORGERY_COOLDOWN_MS = 5000;
    // Helper: returns true iff the OSC 133 payload is acceptable. When the
    // verifier is disabled (no key) always returns true. Fed the marker
    // letter, the parsed payload params, and the optional `extra` field
    // (exit code for D markers, empty for A/B/C). Constant-time compare.
    bool verifyOsc133Hmac(char marker,
                          const std::string &promptId,
                          const std::string &extra,
                          const std::string &providedHmacHex) const;

    // Kitty graphics image cache (id -> QImage); FIFO eviction via m_kittyImageOrder
    std::unordered_map<uint32_t, QImage> m_kittyImages;
    std::deque<uint32_t> m_kittyImageOrder; // insertion order, oldest at front
    std::string m_kittyChunkBuffer; // For multi-chunk transmissions
    uint32_t m_kittyChunkId = 0;

    // Image-bomb defense (0.6.11). Tracks total decoded image bytes held by
    // this terminal across both the inline-display vector (m_inlineImages)
    // and the Kitty cache (m_kittyImages). canFit() is consulted before a
    // decode allocates a QImage; on overflow the decoder rejects with an
    // inline error so the user sees the cap was hit (instead of OOM-ing the
    // process). 256 MB matches the per-terminal upper bound called out in
    // ROADMAP.md §0.7.0 → "Image-bomb defenses". Same QImage stored in
    // multiple containers is counted multiple times — Qt COW means the
    // physical footprint is usually smaller, so the budget is conservative
    // (we may reject earlier than strictly necessary; we never reject too
    // late). MAX_IMAGE_DIM (4096) remains the per-image dimension cap.
    struct ImageBudget {
        static constexpr size_t MAX_BYTES = 256ULL * 1024ULL * 1024ULL;
        size_t used = 0;
        bool canFit(size_t bytes) const { return bytes <= MAX_BYTES - used; }
        void add(size_t bytes) { used += bytes; }
        void release(size_t bytes) { used = (bytes >= used) ? 0 : used - bytes; }
        void reset() { used = 0; }
    };
    ImageBudget m_imageBudget;
    // Helper: estimate decoded byte cost of a QImage. QImage::sizeInBytes()
    // is the canonical Qt API but only available since 5.10; we always have
    // it on Qt6 — kept as a wrapper so the call site reads intentionally.
    static size_t imageByteCost(const QImage &img) {
        return img.isNull() ? 0 : static_cast<size_t>(img.sizeInBytes());
    }
    // Print a short ASCII error message at the cursor in the warning color.
    // Used by the image-bomb decoders to signal rejection without surfacing
    // a desktop notification (which would be more disruptive than the
    // failed image).
    void writeInlineError(const QString &text);
    // Recompute the image-budget counter from scratch. Used on alt-screen
    // enter/exit where m_inlineImages is swapped/cleared in bulk and
    // tracking each individual move would be brittle. O(N) where N ≤
    // MAX_INLINE_IMAGES + MAX_KITTY_CACHE so cost is bounded.
    void recomputeImageBudget();

    // Debug logging
    bool m_debugLog = false;
    FILE *m_debugFile = nullptr;

    // Buffer/resource limits
    static constexpr int MAX_INLINE_IMAGES = 100;
    static constexpr int MAX_KITTY_CACHE = 200;
    static constexpr int MAX_IMAGE_DIM = 4096;
    static constexpr int MAX_COMBINING_PER_CELL = 8;
    static constexpr int MAX_PROMPT_REGIONS = 1000;

    // 256-color palette
    static QColor s_palette256[256];
    static bool s_paletteInitialized;
    static void initPalette();
};
