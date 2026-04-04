#pragma once

#include "vtparser.h"
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <functional>
#include <QColor>
#include <QImage>
#include <QString>

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
    // Combining characters: col -> list of combining codepoints (zero overhead when empty)
    std::unordered_map<int, std::vector<uint32_t>> combining;
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

    const Cell &cellAt(int row, int col) const;
    QString windowTitle() const { return m_windowTitle; }

    // Scrollback
    int scrollbackSize() const { return static_cast<int>(m_scrollback.size()); }
    const std::vector<Cell> &scrollbackLine(int index) const { return m_scrollback[index].cells; }
    bool scrollbackLineWrapped(int index) const { return m_scrollback[index].softWrapped; }

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

    // Combining character access per line
    const std::unordered_map<int, std::vector<uint32_t>> &screenCombining(int row) const {
        return m_screenLines[row].combining;
    }
    const std::unordered_map<int, std::vector<uint32_t>> &scrollbackCombining(int idx) const {
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

    // OSC 8 hyperlinks (per screen line)
    const std::vector<HyperlinkSpan> &screenHyperlinks(int row) const;
    const std::vector<HyperlinkSpan> &scrollbackHyperlinks(int idx) const;

    // Shell integration (OSC 133)
    const std::vector<PromptRegion> &promptRegions() const { return m_promptRegions; }

private:
    void handlePrint(uint32_t cp);
    void handleExecute(char ch);
    void handleCsi(const VtAction &a);
    void handleEsc(const VtAction &a);
    void handleOsc(const std::string &payload);
    void handleDcs(const std::string &payload);  // Sixel graphics
    void handleApc(const std::string &payload);  // Kitty graphics

    // CSI helpers
    void handleSGR(const std::vector<int> &params);
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
    QColor parseRGBColor(const std::vector<int> &params, size_t &i);

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

    QColor m_defaultFg{0xCD, 0xD6, 0xF4};  // Light text
    QColor m_defaultBg{0x1E, 0x1E, 0x2E};  // Dark bg

    // Scroll region (1-based inclusive in VT, stored as 0-based)
    int m_scrollTop = 0;
    int m_scrollBottom = 0; // 0 means "use m_rows - 1"

    // Saved cursor
    int m_savedRow = 0, m_savedCol = 0;
    CellAttrs m_savedAttrs;

    // Modes
    bool m_originMode = false;
    bool m_autoWrap = true;
    bool m_wrapNext = false;  // delayed wrap
    bool m_bracketedPaste = false;  // CSI ?2004h/l

    // Mouse reporting modes
    bool m_mouseButtonMode = false;   // ?1000 — report button press/release
    bool m_mouseMotionMode = false;   // ?1002 — report button+motion
    bool m_mouseAnyMode = false;      // ?1003 — report all motion
    bool m_mouseSgrMode = false;      // ?1006 — SGR encoding

    // Focus reporting
    bool m_focusReporting = false;    // ?1004

    // Synchronized output
    bool m_synchronizedOutput = false; // ?2026

    // Response callback
    ResponseCallback m_responseCallback;

    // Alt screen buffer
    bool m_altScreenActive = false;
    std::vector<TermLine> m_altScreen;
    int m_altCursorRow = 0, m_altCursorCol = 0;

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

    // Kitty graphics image cache (id -> QImage)
    std::unordered_map<uint32_t, QImage> m_kittyImages;
    std::string m_kittyChunkBuffer; // For multi-chunk transmissions
    uint32_t m_kittyChunkId = 0;

    // 256-color palette
    static QColor s_palette256[256];
    static bool s_paletteInitialized;
    static void initPalette();
};
