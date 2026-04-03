#include "terminalgrid.h"
#include <algorithm>
#include <cstring>

QColor TerminalGrid::s_palette256[256] = {};
bool TerminalGrid::s_paletteInitialized = false;

void TerminalGrid::initPalette() {
    if (s_paletteInitialized) return;
    s_paletteInitialized = true;

    // Standard 16 colors (approximate xterm defaults)
    s_palette256[0]  = QColor(0x00, 0x00, 0x00); // black
    s_palette256[1]  = QColor(0xCD, 0x00, 0x00); // red
    s_palette256[2]  = QColor(0x00, 0xCD, 0x00); // green
    s_palette256[3]  = QColor(0xCD, 0xCD, 0x00); // yellow
    s_palette256[4]  = QColor(0x00, 0x00, 0xEE); // blue
    s_palette256[5]  = QColor(0xCD, 0x00, 0xCD); // magenta
    s_palette256[6]  = QColor(0x00, 0xCD, 0xCD); // cyan
    s_palette256[7]  = QColor(0xE5, 0xE5, 0xE5); // white
    s_palette256[8]  = QColor(0x7F, 0x7F, 0x7F); // bright black
    s_palette256[9]  = QColor(0xFF, 0x00, 0x00); // bright red
    s_palette256[10] = QColor(0x00, 0xFF, 0x00); // bright green
    s_palette256[11] = QColor(0xFF, 0xFF, 0x00); // bright yellow
    s_palette256[12] = QColor(0x5C, 0x5C, 0xFF); // bright blue
    s_palette256[13] = QColor(0xFF, 0x00, 0xFF); // bright magenta
    s_palette256[14] = QColor(0x00, 0xFF, 0xFF); // bright cyan
    s_palette256[15] = QColor(0xFF, 0xFF, 0xFF); // bright white

    // 216-color cube (6x6x6)
    for (int i = 0; i < 216; ++i) {
        int r = (i / 36) % 6;
        int g = (i / 6) % 6;
        int b = i % 6;
        s_palette256[16 + i] = QColor(
            r ? 55 + r * 40 : 0,
            g ? 55 + g * 40 : 0,
            b ? 55 + b * 40 : 0
        );
    }

    // 24 grayscale
    for (int i = 0; i < 24; ++i) {
        int v = 8 + i * 10;
        s_palette256[232 + i] = QColor(v, v, v);
    }
}

TerminalGrid::TerminalGrid(int rows, int cols)
    : m_rows(rows), m_cols(cols)
{
    initPalette();
    m_scrollBottom = m_rows - 1;
    m_currentAttrs.fg = m_defaultFg;
    m_currentAttrs.bg = m_defaultBg;
    m_screen.resize(m_rows, std::vector<Cell>(m_cols));
    for (auto &row : m_screen)
        for (auto &c : row) {
            c.attrs.fg = m_defaultFg;
            c.attrs.bg = m_defaultBg;
        }
}

void TerminalGrid::processAction(const VtAction &action) {
    switch (action.type) {
    case VtAction::Print:
        handlePrint(action.codepoint);
        break;
    case VtAction::Execute:
        handleExecute(action.controlChar);
        break;
    case VtAction::CsiDispatch:
        handleCsi(action);
        break;
    case VtAction::EscDispatch:
        handleEsc(action);
        break;
    case VtAction::OscEnd:
        handleOsc(action.oscString);
        break;
    }
}

void TerminalGrid::handlePrint(uint32_t cp) {
    if (m_wrapNext && m_autoWrap) {
        m_cursorCol = 0;
        newLine();
        m_wrapNext = false;
    }

    if (m_cursorCol >= m_cols) {
        m_cursorCol = m_cols - 1;
    }

    auto &c = cell(m_cursorRow, m_cursorCol);
    c.codepoint = cp;
    c.attrs = m_currentAttrs;

    if (m_cursorCol < m_cols - 1) {
        ++m_cursorCol;
    } else {
        // At the right edge — set delayed wrap flag
        m_wrapNext = true;
    }
}

void TerminalGrid::handleExecute(char ch) {
    m_wrapNext = false;
    switch (ch) {
    case '\n':  // LF
    case '\x0B': // VT
    case '\x0C': // FF
        newLine();
        break;
    case '\r':  // CR
        carriageReturn();
        break;
    case '\t':  // HT
        tab();
        break;
    case '\b':  // BS
        if (m_cursorCol > 0) --m_cursorCol;
        break;
    case '\a':  // BEL
        // Could emit a signal for visual/audio bell
        break;
    case '\x0E': // SO (shift out)
    case '\x0F': // SI (shift in)
        // Character set switching — ignore for now
        break;
    }
}

void TerminalGrid::handleCsi(const VtAction &a) {
    m_wrapNext = false;
    const auto &p = a.params;
    auto param = [&](int idx, int def = 1) -> int {
        return (idx < static_cast<int>(p.size()) && p[idx] > 0) ? p[idx] : def;
    };

    switch (a.finalChar) {
    case 'A': // CUU — cursor up
        moveCursor(-param(0), 0);
        break;
    case 'B': // CUD — cursor down
        moveCursor(param(0), 0);
        break;
    case 'C': // CUF — cursor forward
        moveCursor(0, param(0));
        break;
    case 'D': // CUB — cursor backward
        moveCursor(0, -param(0));
        break;
    case 'E': // CNL — cursor next line
        m_cursorCol = 0;
        moveCursor(param(0), 0);
        break;
    case 'F': // CPL — cursor previous line
        m_cursorCol = 0;
        moveCursor(-param(0), 0);
        break;
    case 'G': // CHA — cursor horizontal absolute
        m_cursorCol = std::clamp(param(0, 1) - 1, 0, m_cols - 1);
        break;
    case 'H': // CUP — cursor position
    case 'f': // HVP
        setCursorPos(param(0, 1) - 1, p.size() > 1 ? param(1, 1) - 1 : 0);
        break;
    case 'J': // ED — erase in display
        eraseInDisplay(p.empty() ? 0 : p[0]);
        break;
    case 'K': // EL — erase in line
        eraseInLine(p.empty() ? 0 : p[0]);
        break;
    case 'L': // IL — insert lines
        insertLines(param(0));
        break;
    case 'M': // DL — delete lines
        deleteLines(param(0));
        break;
    case 'P': // DCH — delete characters
        deleteChars(param(0));
        break;
    case '@': // ICH — insert blanks
        insertBlanks(param(0));
        break;
    case 'S': // SU — scroll up
        scrollUp(param(0));
        break;
    case 'T': // SD — scroll down
        scrollDown(param(0));
        break;
    case 'd': // VPA — line position absolute
        m_cursorRow = std::clamp(param(0, 1) - 1, 0, m_rows - 1);
        break;
    case 'm': // SGR — select graphic rendition
        handleSGR(p);
        break;
    case 'r': // DECSTBM — set scroll region
        setScrollRegion(param(0, 1) - 1, p.size() > 1 ? param(1, m_rows) - 1 : m_rows - 1);
        break;
    case 's': // Save cursor
        saveCursor();
        break;
    case 'u': // Restore cursor
        restoreCursor();
        break;
    case 'h': // Set mode
        if (a.intermediate == "?") {
            for (int v : p) {
                switch (v) {
                case 1: break; // DECCKM — app cursor keys
                case 6: m_originMode = true; break;
                case 7: m_autoWrap = true; break;
                case 25: m_cursorVisible = true; break;
                case 1049: // Alt screen buffer + save cursor
                case 47:
                case 1047:
                    if (!m_altScreenActive) {
                        m_altScreenActive = true;
                        m_altCursorRow = m_cursorRow;
                        m_altCursorCol = m_cursorCol;
                        m_altScreen = m_screen;
                        // Clear screen for alt buffer
                        for (auto &row : m_screen)
                            for (auto &c : row) {
                                c.codepoint = ' ';
                                c.attrs.fg = m_defaultFg;
                                c.attrs.bg = m_defaultBg;
                                c.attrs.bold = false;
                                c.attrs.italic = false;
                                c.attrs.underline = false;
                                c.attrs.inverse = false;
                                c.attrs.dim = false;
                                c.attrs.strikethrough = false;
                            }
                        m_cursorRow = 0;
                        m_cursorCol = 0;
                    }
                    break;
                case 2004: break; // Bracketed paste — acknowledge
                }
            }
        }
        break;
    case 'l': // Reset mode
        if (a.intermediate == "?") {
            for (int v : p) {
                switch (v) {
                case 1: break; // DECCKM
                case 6: m_originMode = false; break;
                case 7: m_autoWrap = false; break;
                case 25: m_cursorVisible = false; break;
                case 1049:
                case 47:
                case 1047:
                    if (m_altScreenActive) {
                        m_altScreenActive = false;
                        m_screen = m_altScreen;
                        m_cursorRow = m_altCursorRow;
                        m_cursorCol = m_altCursorCol;
                    }
                    break;
                case 2004: break;
                }
            }
        }
        break;
    case 'X': // ECH — erase characters
    {
        int count = param(0);
        for (int i = 0; i < count && m_cursorCol + i < m_cols; ++i) {
            auto &c = cell(m_cursorRow, m_cursorCol + i);
            c.codepoint = ' ';
            c.attrs = m_currentAttrs;
        }
        break;
    }
    case 'n': // DSR — device status report
        // Cursor position report is handled externally
        break;
    }
}

void TerminalGrid::handleEsc(const VtAction &a) {
    m_wrapNext = false;
    if (a.intermediate.empty()) {
        switch (a.finalChar) {
        case 'M': // RI — reverse index
            reverseIndex();
            break;
        case 'D': // IND — index (move down)
            newLine();
            break;
        case 'E': // NEL — next line
            carriageReturn();
            newLine();
            break;
        case '7': // DECSC — save cursor
            saveCursor();
            break;
        case '8': // DECRC — restore cursor
            restoreCursor();
            break;
        case 'c': // RIS — full reset
            *this = TerminalGrid(m_rows, m_cols);
            break;
        }
    } else if (a.intermediate == "(") {
        // Designate G0 charset — ignore
    } else if (a.intermediate == ")") {
        // Designate G1 charset — ignore
    }
}

void TerminalGrid::handleOsc(const std::string &payload) {
    // OSC 0;title ST — set window title
    // OSC 2;title ST — set window title
    if (payload.size() >= 2 && (payload[0] == '0' || payload[0] == '2') && payload[1] == ';') {
        m_windowTitle = QString::fromUtf8(payload.c_str() + 2);
    }
}

void TerminalGrid::handleSGR(const std::vector<int> &params) {
    if (params.empty()) {
        // Reset
        m_currentAttrs = CellAttrs{};
        m_currentAttrs.fg = m_defaultFg;
        m_currentAttrs.bg = m_defaultBg;
        return;
    }

    for (size_t i = 0; i < params.size(); ++i) {
        int code = params[i];
        switch (code) {
        case 0: // Reset
            m_currentAttrs = CellAttrs{};
            m_currentAttrs.fg = m_defaultFg;
            m_currentAttrs.bg = m_defaultBg;
            break;
        case 1: m_currentAttrs.bold = true; break;
        case 2: m_currentAttrs.dim = true; break;
        case 3: m_currentAttrs.italic = true; break;
        case 4: m_currentAttrs.underline = true; break;
        case 7: m_currentAttrs.inverse = true; break;
        case 9: m_currentAttrs.strikethrough = true; break;
        case 21: m_currentAttrs.bold = false; break;
        case 22: m_currentAttrs.bold = false; m_currentAttrs.dim = false; break;
        case 23: m_currentAttrs.italic = false; break;
        case 24: m_currentAttrs.underline = false; break;
        case 27: m_currentAttrs.inverse = false; break;
        case 29: m_currentAttrs.strikethrough = false; break;

        // Standard foreground colors
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            m_currentAttrs.fg = s_palette256[code - 30];
            break;
        case 38: // Extended foreground
            if (i + 1 < params.size()) {
                if (params[i + 1] == 5) { // 256-color
                    m_currentAttrs.fg = parse256Color(params, i);
                } else if (params[i + 1] == 2) { // RGB
                    m_currentAttrs.fg = parseRGBColor(params, i);
                }
            }
            break;
        case 39: // Default foreground
            m_currentAttrs.fg = m_defaultFg;
            break;

        // Standard background colors
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            m_currentAttrs.bg = s_palette256[code - 40];
            break;
        case 48: // Extended background
            if (i + 1 < params.size()) {
                if (params[i + 1] == 5) {
                    m_currentAttrs.bg = parse256Color(params, i);
                } else if (params[i + 1] == 2) {
                    m_currentAttrs.bg = parseRGBColor(params, i);
                }
            }
            break;
        case 49: // Default background
            m_currentAttrs.bg = m_defaultBg;
            break;

        // Bright foreground
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            m_currentAttrs.fg = s_palette256[code - 90 + 8];
            break;

        // Bright background
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            m_currentAttrs.bg = s_palette256[code - 100 + 8];
            break;
        }
    }
}

QColor TerminalGrid::parse256Color(const std::vector<int> &params, size_t &i) {
    // 38;5;N or 48;5;N
    if (i + 2 < params.size()) {
        int idx = params[i + 2];
        i += 2;
        if (idx >= 0 && idx < 256) {
            return s_palette256[idx];
        }
    }
    return m_defaultFg;
}

QColor TerminalGrid::parseRGBColor(const std::vector<int> &params, size_t &i) {
    // 38;2;R;G;B or 48;2;R;G;B
    if (i + 4 < params.size()) {
        int r = std::clamp(params[i + 2], 0, 255);
        int g = std::clamp(params[i + 3], 0, 255);
        int b = std::clamp(params[i + 4], 0, 255);
        i += 4;
        return QColor(r, g, b);
    }
    return m_defaultFg;
}

void TerminalGrid::eraseInDisplay(int mode) {
    switch (mode) {
    case 0: // Erase from cursor to end
        eraseInLine(0);
        for (int r = m_cursorRow + 1; r < m_rows; ++r)
            clearRow(r);
        break;
    case 1: // Erase from start to cursor
        for (int r = 0; r < m_cursorRow; ++r)
            clearRow(r);
        eraseInLine(1);
        break;
    case 2: // Erase entire display
    case 3: // Erase display + scrollback
        for (int r = 0; r < m_rows; ++r)
            clearRow(r);
        if (mode == 3)
            m_scrollback.clear();
        break;
    }
}

void TerminalGrid::eraseInLine(int mode) {
    switch (mode) {
    case 0: // Erase from cursor to end of line
        clearRow(m_cursorRow, m_cursorCol, m_cols);
        break;
    case 1: // Erase from start to cursor
        clearRow(m_cursorRow, 0, m_cursorCol + 1);
        break;
    case 2: // Erase entire line
        clearRow(m_cursorRow);
        break;
    }
}

void TerminalGrid::insertLines(int count) {
    int bottom = m_scrollBottom;
    if (m_cursorRow < m_scrollTop || m_cursorRow > bottom) return;
    count = std::min(count, bottom - m_cursorRow + 1);
    for (int i = 0; i < count; ++i) {
        m_screen.erase(m_screen.begin() + bottom);
        m_screen.insert(m_screen.begin() + m_cursorRow, std::vector<Cell>(m_cols));
        clearRow(m_cursorRow);
    }
}

void TerminalGrid::deleteLines(int count) {
    int bottom = m_scrollBottom;
    if (m_cursorRow < m_scrollTop || m_cursorRow > bottom) return;
    count = std::min(count, bottom - m_cursorRow + 1);
    for (int i = 0; i < count; ++i) {
        m_screen.erase(m_screen.begin() + m_cursorRow);
        m_screen.insert(m_screen.begin() + bottom, std::vector<Cell>(m_cols));
        clearRow(bottom);
    }
}

void TerminalGrid::deleteChars(int count) {
    auto &row = m_screen[m_cursorRow];
    count = std::min(count, m_cols - m_cursorCol);
    row.erase(row.begin() + m_cursorCol, row.begin() + m_cursorCol + count);
    row.resize(m_cols);
    // Fill end with blanks
    for (int i = m_cols - count; i < m_cols; ++i) {
        row[i].codepoint = ' ';
        row[i].attrs.fg = m_defaultFg;
        row[i].attrs.bg = m_defaultBg;
    }
}

void TerminalGrid::insertBlanks(int count) {
    auto &row = m_screen[m_cursorRow];
    count = std::min(count, m_cols - m_cursorCol);
    row.insert(row.begin() + m_cursorCol, count, Cell{});
    for (int i = m_cursorCol; i < m_cursorCol + count; ++i) {
        row[i].codepoint = ' ';
        row[i].attrs.fg = m_defaultFg;
        row[i].attrs.bg = m_defaultBg;
    }
    row.resize(m_cols);
}

void TerminalGrid::scrollUp(int count) {
    for (int i = 0; i < count; ++i) {
        // Push top line to scrollback
        if (m_scrollTop == 0 && !m_altScreenActive) {
            m_scrollback.push_back(std::move(m_screen[m_scrollTop]));
            if (static_cast<int>(m_scrollback.size()) > MAX_SCROLLBACK)
                m_scrollback.pop_front();
        }
        m_screen.erase(m_screen.begin() + m_scrollTop);
        m_screen.insert(m_screen.begin() + m_scrollBottom, std::vector<Cell>(m_cols));
        clearRow(m_scrollBottom);
    }
}

void TerminalGrid::scrollDown(int count) {
    for (int i = 0; i < count; ++i) {
        m_screen.erase(m_screen.begin() + m_scrollBottom);
        m_screen.insert(m_screen.begin() + m_scrollTop, std::vector<Cell>(m_cols));
        clearRow(m_scrollTop);
    }
}

void TerminalGrid::setCursorPos(int row, int col) {
    m_cursorRow = std::clamp(row, 0, m_rows - 1);
    m_cursorCol = std::clamp(col, 0, m_cols - 1);
}

void TerminalGrid::moveCursor(int dRow, int dCol) {
    setCursorPos(m_cursorRow + dRow, m_cursorCol + dCol);
}

void TerminalGrid::setScrollRegion(int top, int bottom) {
    top = std::clamp(top, 0, m_rows - 1);
    bottom = std::clamp(bottom, 0, m_rows - 1);
    if (top < bottom) {
        m_scrollTop = top;
        m_scrollBottom = bottom;
    }
    setCursorPos(0, 0);
}

void TerminalGrid::saveCursor() {
    m_savedRow = m_cursorRow;
    m_savedCol = m_cursorCol;
    m_savedAttrs = m_currentAttrs;
}

void TerminalGrid::restoreCursor() {
    m_cursorRow = m_savedRow;
    m_cursorCol = m_savedCol;
    m_currentAttrs = m_savedAttrs;
}

void TerminalGrid::newLine() {
    if (m_cursorRow == m_scrollBottom) {
        scrollUp(1);
    } else if (m_cursorRow < m_rows - 1) {
        ++m_cursorRow;
    }
}

void TerminalGrid::carriageReturn() {
    m_cursorCol = 0;
}

void TerminalGrid::tab() {
    // Move to next tab stop (every 8 columns)
    m_cursorCol = std::min(((m_cursorCol / 8) + 1) * 8, m_cols - 1);
}

void TerminalGrid::reverseIndex() {
    if (m_cursorRow == m_scrollTop) {
        scrollDown(1);
    } else if (m_cursorRow > 0) {
        --m_cursorRow;
    }
}

Cell &TerminalGrid::cell(int row, int col) {
    return m_screen[std::clamp(row, 0, m_rows - 1)][std::clamp(col, 0, m_cols - 1)];
}

const Cell &TerminalGrid::cellAt(int row, int col) const {
    return m_screen[std::clamp(row, 0, m_rows - 1)][std::clamp(col, 0, m_cols - 1)];
}

void TerminalGrid::clearRow(int row, int startCol, int endCol) {
    if (endCol < 0) endCol = m_cols;
    for (int c = startCol; c < endCol && c < m_cols; ++c) {
        m_screen[row][c].codepoint = ' ';
        m_screen[row][c].attrs.fg = m_defaultFg;
        m_screen[row][c].attrs.bg = m_defaultBg;
        m_screen[row][c].attrs.bold = false;
        m_screen[row][c].attrs.italic = false;
        m_screen[row][c].attrs.underline = false;
        m_screen[row][c].attrs.inverse = false;
        m_screen[row][c].attrs.dim = false;
        m_screen[row][c].attrs.strikethrough = false;
    }
}

void TerminalGrid::resize(int rows, int cols) {
    if (rows == m_rows && cols == m_cols) return;

    // Build new screen
    std::vector<std::vector<Cell>> newScreen(rows, std::vector<Cell>(cols));
    for (auto &row : newScreen)
        for (auto &c : row) {
            c.attrs.fg = m_defaultFg;
            c.attrs.bg = m_defaultBg;
        }

    // Copy what fits
    int copyRows = std::min(rows, m_rows);
    int copyCols = std::min(cols, m_cols);
    for (int r = 0; r < copyRows; ++r)
        for (int c = 0; c < copyCols; ++c)
            newScreen[r][c] = m_screen[r][c];

    m_screen = std::move(newScreen);
    m_rows = rows;
    m_cols = cols;
    m_scrollBottom = m_rows - 1;
    m_scrollTop = 0;
    m_cursorRow = std::clamp(m_cursorRow, 0, m_rows - 1);
    m_cursorCol = std::clamp(m_cursorCol, 0, m_cols - 1);
}
