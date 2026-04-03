#pragma once

#include "vtparser.h"
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <QColor>
#include <QString>

struct CellAttrs {
    QColor fg;
    QColor bg;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    bool dim = false;
    bool strikethrough = false;
};

struct Cell {
    uint32_t codepoint = ' ';
    CellAttrs attrs;
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

    const Cell &cellAt(int row, int col) const;
    QString windowTitle() const { return m_windowTitle; }

    // Scrollback
    int scrollbackSize() const { return static_cast<int>(m_scrollback.size()); }
    const std::vector<Cell> &scrollbackLine(int index) const { return m_scrollback[index]; }

    // Default colors (set by theme) — recolors existing cells
    void setDefaultFg(const QColor &c);
    void setDefaultBg(const QColor &c);
    QColor defaultFg() const { return m_defaultFg; }
    QColor defaultBg() const { return m_defaultBg; }

private:
    void handlePrint(uint32_t cp);
    void handleExecute(char ch);
    void handleCsi(const VtAction &a);
    void handleEsc(const VtAction &a);
    void handleOsc(const std::string &payload);

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

    int m_rows, m_cols;
    int m_cursorRow = 0, m_cursorCol = 0;
    bool m_cursorVisible = true;
    CellAttrs m_currentAttrs;
    std::vector<std::vector<Cell>> m_screen;
    std::deque<std::vector<Cell>> m_scrollback;
    static constexpr int MAX_SCROLLBACK = 10000;

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

    // Alt screen buffer
    bool m_altScreenActive = false;
    std::vector<std::vector<Cell>> m_altScreen;
    int m_altCursorRow = 0, m_altCursorCol = 0;

    QString m_windowTitle;

    // 256-color palette
    static QColor s_palette256[256];
    static bool s_paletteInitialized;
    static void initPalette();
};
