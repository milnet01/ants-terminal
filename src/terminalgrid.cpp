#include "terminalgrid.h"

#include <QByteArray>
#include <QDateTime>
#include <QStandardPaths>
#include <QDir>

#include <algorithm>
#include <cwchar>
#include <cstdio>

#define DBGLOG(fmt, ...) do { if (m_debugLog && m_debugFile) { fprintf(m_debugFile, fmt "\n", ##__VA_ARGS__); fflush(m_debugFile); } } while(0)

void TerminalGrid::setDebugLog(bool enabled) {
    m_debugLog = enabled;
    if (enabled && !m_debugFile) {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                      + "/ants-terminal";
        QDir().mkpath(dir);
        QString path = dir + "/debug_sgr.log";
        m_debugFile = fopen(path.toUtf8().constData(), "w");
        if (m_debugFile)
            fprintf(m_debugFile, "=== Ants Terminal SGR debug log ===\n");
    } else if (!enabled && m_debugFile) {
        fclose(m_debugFile);
        m_debugFile = nullptr;
    }
}

QColor TerminalGrid::s_palette256[256] = {};
bool TerminalGrid::s_paletteInitialized = false;

void TerminalGrid::initPalette() {
    if (s_paletteInitialized) return;
    s_paletteInitialized = true;

    // Standard 16 colors (approximate xterm defaults)
    s_palette256[0]  = QColor(0x00, 0x00, 0x00);
    s_palette256[1]  = QColor(0xCD, 0x00, 0x00);
    s_palette256[2]  = QColor(0x00, 0xCD, 0x00);
    s_palette256[3]  = QColor(0xCD, 0xCD, 0x00);
    s_palette256[4]  = QColor(0x00, 0x00, 0xEE);
    s_palette256[5]  = QColor(0xCD, 0x00, 0xCD);
    s_palette256[6]  = QColor(0x00, 0xCD, 0xCD);
    s_palette256[7]  = QColor(0xE5, 0xE5, 0xE5);
    s_palette256[8]  = QColor(0x7F, 0x7F, 0x7F);
    s_palette256[9]  = QColor(0xFF, 0x00, 0x00);
    s_palette256[10] = QColor(0x00, 0xFF, 0x00);
    s_palette256[11] = QColor(0xFF, 0xFF, 0x00);
    s_palette256[12] = QColor(0x5C, 0x5C, 0xFF);
    s_palette256[13] = QColor(0xFF, 0x00, 0xFF);
    s_palette256[14] = QColor(0x00, 0xFF, 0xFF);
    s_palette256[15] = QColor(0xFF, 0xFF, 0xFF);

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

static std::vector<Cell> makeRow(int cols, const QColor &fg, const QColor &bg) {
    std::vector<Cell> row(cols);
    for (auto &c : row) {
        c.attrs.fg = fg;
        c.attrs.bg = bg;
    }
    return row;
}

TerminalGrid::TerminalGrid(int rows, int cols)
    : m_rows(rows), m_cols(cols)
{
    initPalette();
    m_scrollBottom = m_rows - 1;
    m_currentAttrs.fg = m_defaultFg;
    m_currentAttrs.bg = m_defaultBg;
    m_screenLines.resize(m_rows);
    for (auto &line : m_screenLines) {
        line.cells = makeRow(m_cols, m_defaultFg, m_defaultBg);
    }
    m_screenHyperlinks.resize(m_rows);
    initTabStops();
}

void TerminalGrid::initTabStops() {
    m_tabStops.assign(m_cols, false);
    for (int i = 0; i < m_cols; i += 8)
        m_tabStops[i] = true;
}

void TerminalGrid::setResponseCallback(ResponseCallback cb) {
    m_responseCallback = std::move(cb);
}

const std::vector<HyperlinkSpan> &TerminalGrid::screenHyperlinks(int row) const {
    static const std::vector<HyperlinkSpan> s_empty;
    if (row >= 0 && row < static_cast<int>(m_screenHyperlinks.size()))
        return m_screenHyperlinks[row];
    return s_empty;
}

const std::vector<HyperlinkSpan> &TerminalGrid::scrollbackHyperlinks(int idx) const {
    static const std::vector<HyperlinkSpan> s_empty;
    if (idx >= 0 && idx < static_cast<int>(m_scrollbackHyperlinks.size()))
        return m_scrollbackHyperlinks[idx];
    return s_empty;
}

void TerminalGrid::setMaxScrollback(int lines) {
    m_maxScrollback = std::max(1000, lines);
    while (static_cast<int>(m_scrollback.size()) > m_maxScrollback)
        m_scrollback.pop_front();
    // Keep scrollback hyperlinks in sync
    while (static_cast<int>(m_scrollbackHyperlinks.size()) > m_maxScrollback)
        m_scrollbackHyperlinks.pop_front();
}

void TerminalGrid::setDefaultFg(const QColor &c) {
    QColor oldFg = m_defaultFg;
    m_defaultFg = c;
    for (auto &line : m_screenLines)
        for (auto &c2 : line.cells)
            if (c2.attrs.fg == oldFg)
                c2.attrs.fg = c;
    if (m_currentAttrs.fg == oldFg)
        m_currentAttrs.fg = c;
}

void TerminalGrid::setDefaultBg(const QColor &c) {
    QColor oldBg = m_defaultBg;
    m_defaultBg = c;
    for (auto &line : m_screenLines)
        for (auto &c2 : line.cells)
            if (c2.attrs.bg == oldBg)
                c2.attrs.bg = c;
    if (m_currentAttrs.bg == oldBg)
        m_currentAttrs.bg = c;
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
    case VtAction::DcsEnd:
        handleDcs(action.oscString);
        break;
    case VtAction::ApcEnd:
        handleApc(action.oscString);
        break;
    }
}

void TerminalGrid::handlePrint(uint32_t cp) {
    // Combining character (zero-width) — attach to previous cell
    int w = wcwidth(static_cast<wchar_t>(cp));
    if (w == 0 && cp > 0x20) {
        int targetCol;
        if (m_wrapNext) {
            targetCol = m_cursorCol;  // cursor on the just-written char (delayed wrap)
        } else if (m_cursorCol > 0) {
            targetCol = m_cursorCol - 1;
        } else {
            return;  // no previous cell to attach to
        }
        auto &comb = m_screenLines[m_cursorRow].combining[targetCol];
        if (static_cast<int>(comb.size()) < MAX_COMBINING_PER_CELL)
            comb.push_back(cp);
        return;
    }

    if (m_wrapNext && m_autoWrap) {
        // Mark current line as soft-wrapped before moving to next
        m_screenLines[m_cursorRow].softWrapped = true;
        m_cursorCol = 0;
        newLine();
        m_wrapNext = false;
    }

    if (m_cursorCol >= m_cols) {
        m_cursorCol = m_cols - 1;
    }

    // Determine character width (1 or 2 cells)
    int charWidth = 1;
    if (w == 2) charWidth = 2;
    else if (w < 0) charWidth = 1; // non-printable, treat as 1

    // For double-width chars, check if we need to wrap
    if (charWidth == 2 && m_cursorCol >= m_cols - 1) {
        if (m_autoWrap) {
            m_screenLines[m_cursorRow].softWrapped = true;
            m_cursorCol = 0;
            newLine();
        } else {
            return; // Can't fit, ignore
        }
    }

    auto &c = cell(m_cursorRow, m_cursorCol);
    c.codepoint = cp;
    c.attrs = m_currentAttrs;
    if (m_currentAttrs.underline && m_debugLog)
        DBGLOG("PRINT U+%04X '%c' with underline=1 style=%d row=%d col=%d",
               cp, (cp >= 0x20 && cp < 0x7F) ? (char)cp : '?',
               (int)m_currentAttrs.underlineStyle, m_cursorRow, m_cursorCol);
    c.isWideChar = (charWidth == 2);
    c.isWideCont = false;
    markScreenDirty(m_cursorRow);
    m_screenLines[m_cursorRow].combining.erase(m_cursorCol);

    if (charWidth == 2 && m_cursorCol + 1 < m_cols) {
        // Mark next cell as continuation
        auto &c2 = cell(m_cursorRow, m_cursorCol + 1);
        c2.codepoint = 0;
        c2.attrs = m_currentAttrs;
        c2.isWideChar = false;
        c2.isWideCont = true;
        m_screenLines[m_cursorRow].combining.erase(m_cursorCol + 1);
    }

    m_cursorCol += charWidth;
    if (m_cursorCol >= m_cols) {
        m_cursorCol = m_cols - 1;
        m_wrapNext = true;
    }
}

void TerminalGrid::handleExecute(char ch) {
    m_wrapNext = false;
    switch (ch) {
    case '\n':
    case '\x0B':
    case '\x0C':
        newLine();
        break;
    case '\r':
        carriageReturn();
        break;
    case '\t':
        tab();
        break;
    case '\b':
        if (m_cursorCol > 0) --m_cursorCol;
        break;
    case '\a':
        if (m_bellCallback) m_bellCallback();
        break;
    case '\x0E':
    case '\x0F':
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
    case 'A': moveCursor(-param(0), 0); break;
    case 'B': moveCursor(param(0), 0); break;
    case 'C': moveCursor(0, param(0)); break;
    case 'D': moveCursor(0, -param(0)); break;
    case 'E': m_cursorCol = 0; moveCursor(param(0), 0); break;
    case 'F': m_cursorCol = 0; moveCursor(-param(0), 0); break;
    case 'G': m_cursorCol = std::clamp(param(0, 1) - 1, 0, m_cols - 1); break;
    case 'H':
    case 'f':
        setCursorPos(param(0, 1) - 1, p.size() > 1 ? param(1, 1) - 1 : 0);
        break;
    case 'J': eraseInDisplay(p.empty() ? 0 : p[0]); break;
    case 'K': eraseInLine(p.empty() ? 0 : p[0]); break;
    case 'L': insertLines(param(0)); break;
    case 'M': deleteLines(param(0)); break;
    case 'P': deleteChars(param(0)); break;
    case '@': insertBlanks(param(0)); break;
    case 'S': scrollUp(param(0)); break;
    case 'T': scrollDown(param(0)); break;
    case 'd': m_cursorRow = std::clamp(param(0, 1) - 1, 0, m_rows - 1); break;
    case 'm': handleSGR(p, a.colonSep); break;
    case 'r':
        setScrollRegion(param(0, 1) - 1, p.size() > 1 ? param(1, m_rows) - 1 : m_rows - 1);
        break;
    case 's': saveCursor(); break;
    case 'h':
        if (a.intermediate == "?") {
            for (int v : p) {
                switch (v) {
                case 1: m_applicationCursorKeys = true; break;
                case 6: m_originMode = true; break;
                case 7: m_autoWrap = true; break;
                case 25: m_cursorVisible = true; break;
                case 1049:
                case 47:
                case 1047:
                    if (!m_altScreenActive) {
                        m_altScreenActive = true;
                        markAllScreenDirty();
                        m_altCursorRow = m_cursorRow;
                        m_altCursorCol = m_cursorCol;
                        m_altScrollTop = m_scrollTop;
                        m_altScrollBottom = m_scrollBottom;
                        m_altScreen = m_screenLines;
                        m_altScreenHyperlinks = m_screenHyperlinks;
                        m_altInlineImages = m_inlineImages;
                        m_altPromptRegions = m_promptRegions;
                        // DECSC-equivalent: save SGR + origin + wrap alongside cursor.
                        m_altSavedAttrs = m_currentAttrs;
                        m_altSavedOriginMode = m_originMode;
                        m_altSavedAutoWrap = m_autoWrap;
                        for (auto &line : m_screenLines) {
                            for (auto &c : line.cells) {
                                c.codepoint = ' ';
                                c.attrs = CellAttrs{};
                                c.attrs.fg = m_defaultFg;
                                c.attrs.bg = m_defaultBg;
                            }
                            line.combining.clear();
                        }
                        for (auto &hl : m_screenHyperlinks)
                            hl.clear();
                        m_inlineImages.clear();
                        m_promptRegions.clear();
                        m_cursorRow = 0;
                        m_cursorCol = 0;
                    }
                    break;
                case 1000: m_mouseButtonMode = true; break;
                case 1002: m_mouseMotionMode = true; break;
                case 1003: m_mouseAnyMode = true; break;
                case 1004: m_focusReporting = true; break;
                case 1006: m_mouseSgrMode = true; break;
                case 2004: m_bracketedPaste = true; break;
                case 2026: m_synchronizedOutput = true; break;
                case 2031: m_colorSchemeNotify = true; break;
                }
            }
        }
        break;
    case 'l':
        if (a.intermediate == "?") {
            for (int v : p) {
                switch (v) {
                case 1: m_applicationCursorKeys = false; break;
                case 6: m_originMode = false; break;
                case 7: m_autoWrap = false; break;
                case 25: m_cursorVisible = false; break;
                case 1049:
                case 47:
                case 1047:
                    if (m_altScreenActive) {
                        m_altScreenActive = false;
                        markAllScreenDirty();
                        m_screenLines = m_altScreen;
                        m_screenHyperlinks = m_altScreenHyperlinks;
                        m_inlineImages = m_altInlineImages;
                        m_promptRegions = m_altPromptRegions;
                        m_cursorRow = m_altCursorRow;
                        m_cursorCol = m_altCursorCol;
                        m_scrollTop = m_altScrollTop;
                        m_scrollBottom = m_altScrollBottom;
                        // Restore DECSC-equivalent state saved on entry.
                        m_currentAttrs = m_altSavedAttrs;
                        m_originMode = m_altSavedOriginMode;
                        m_autoWrap = m_altSavedAutoWrap;
                        m_altScreen.clear();
                        m_altScreenHyperlinks.clear();
                        m_altInlineImages.clear();
                        m_altPromptRegions.clear();
                    }
                    break;
                case 1000: m_mouseButtonMode = false; break;
                case 1002: m_mouseMotionMode = false; break;
                case 1003: m_mouseAnyMode = false; break;
                case 1004: m_focusReporting = false; break;
                case 1006: m_mouseSgrMode = false; break;
                case 2004: m_bracketedPaste = false; break;
                case 2026: m_synchronizedOutput = false; break;
                case 2031: m_colorSchemeNotify = false; break;
                }
            }
        }
        break;
    case 'X': {
        int count = param(0);
        for (int i = 0; i < count && m_cursorCol + i < m_cols; ++i) {
            auto &c = cell(m_cursorRow, m_cursorCol + i);
            c.codepoint = ' ';
            c.attrs = m_currentAttrs;
            m_screenLines[m_cursorRow].combining.erase(m_cursorCol + i);
        }
        break;
    }
    case 'c':
        if (a.intermediate.empty()) {
            // DA1: Primary Device Attributes
            int da = p.empty() ? 0 : p[0];
            if (da == 0 && m_responseCallback)
                m_responseCallback("\x1B[?62;22c"); // VT220 + ANSI color
        } else if (a.intermediate == ">") {
            // DA2: Secondary Device Attributes
            int da = p.empty() ? 0 : p[0];
            if (da == 0 && m_responseCallback)
                m_responseCallback("\x1B[>41;0;0c"); // xterm-like
        }
        break;
    case 'q':
        // DECSCUSR — Set Cursor Style (CSI Ps SP q)
        if (a.intermediate == " ") {
            int ps = p.empty() ? 0 : p[0];
            if (ps >= 0 && ps <= 6)
                m_cursorShape = static_cast<CursorShape>(ps);
        }
        break;
    case 'g':
        // TBC — Tab Clear
        if (a.intermediate.empty()) {
            int mode = p.empty() ? 0 : p[0];
            if (mode == 0) {
                // Clear tab stop at current position
                if (m_cursorCol >= 0 && m_cursorCol < static_cast<int>(m_tabStops.size()))
                    m_tabStops[m_cursorCol] = false;
            } else if (mode == 3) {
                // Clear all tab stops
                std::fill(m_tabStops.begin(), m_tabStops.end(), false);
            }
        }
        break;
    case 'n':
        if (a.intermediate.empty()) {
            if (!p.empty() && p[0] == 6 && m_responseCallback) {
                // CPR: Cursor Position Report
                std::string r = "\x1B[" + std::to_string(m_cursorRow + 1)
                              + ";" + std::to_string(m_cursorCol + 1) + "R";
                m_responseCallback(r);
            } else if (!p.empty() && p[0] == 5 && m_responseCallback) {
                // DSR: Device Status Report — terminal OK
                m_responseCallback("\x1B[0n");
            }
        } else if (a.intermediate == "?") {
            // CSI ? 996 n — Color scheme query
            if (!p.empty() && p[0] == 996 && m_responseCallback) {
                // Report current color scheme preference (1=dark, 2=light)
                m_responseCallback("\x1B[?996;1n");
            }
        }
        break;
    case 'u':
        if (a.intermediate.empty()) {
            // DECRC — Restore cursor position
            restoreCursor();
        } else if (a.intermediate == ">") {
            // CSI > flags u — Kitty keyboard: push current flags, set new
            int flags = (!p.empty() && p[0] > 0) ? p[0] : 0;
            if (m_kittyKeyStack.size() < 16) // Cap stack size
                m_kittyKeyStack.push_back(m_kittyKeyFlags);
            m_kittyKeyFlags = flags & 0x1F; // Only bits 0-4 valid
        } else if (a.intermediate == "<") {
            // CSI < number u — Kitty keyboard: pop flags from stack
            int count = (!p.empty() && p[0] > 0) ? p[0] : 1;
            for (int i = 0; i < count && !m_kittyKeyStack.empty(); ++i) {
                m_kittyKeyFlags = m_kittyKeyStack.back();
                m_kittyKeyStack.pop_back();
            }
            if (m_kittyKeyStack.empty() && count > 0)
                m_kittyKeyFlags = 0;
        } else if (a.intermediate == "?") {
            // CSI ? u — Kitty keyboard: query current flags
            if (m_responseCallback) {
                std::string r = "\x1B[?" + std::to_string(m_kittyKeyFlags) + "u";
                m_responseCallback(r);
            }
        } else if (a.intermediate == "=") {
            // CSI = flags ; mode u — Kitty keyboard: set/or/not flags
            int flags = (!p.empty() && p[0] > 0) ? p[0] : 0;
            int mode = (p.size() > 1 && p[1] > 0) ? p[1] : 1;
            flags &= 0x1F;
            switch (mode) {
            case 1: m_kittyKeyFlags = flags; break;           // set
            case 2: m_kittyKeyFlags |= flags; break;          // or
            case 3: m_kittyKeyFlags &= ~flags; break;         // not
            }
        }
        break;
    }
    // SECURITY: CSI 20t/21t (XTWINOPS title reporting) intentionally NOT implemented.
    // Reporting the window title allows escape-sequence injection attacks
    // (CVE-2024-56803, CVE-2003-0063).  Similarly, DECRQSS (DCS $q) is not
    // implemented to prevent echoing attacker-controlled data.
}

void TerminalGrid::handleEsc(const VtAction &a) {
    m_wrapNext = false;
    if (a.intermediate.empty()) {
        switch (a.finalChar) {
        case 'M': reverseIndex(); break;
        case 'D': newLine(); break;
        case 'E': carriageReturn(); newLine(); break;
        case '7': saveCursor(); break;
        case '8': restoreCursor(); break;
        case 'H': // HTS — Horizontal Tab Set
            if (m_cursorCol >= 0 && m_cursorCol < static_cast<int>(m_tabStops.size()))
                m_tabStops[m_cursorCol] = true;
            break;
        case '=': // DECKPAM — Application Keypad Mode
            m_applicationKeypad = true;
            break;
        case '>': // DECKPNM — Normal Keypad Mode
            m_applicationKeypad = false;
            break;
        case 'c': {
            auto cb = std::move(m_responseCallback);
            auto bellCb = std::move(m_bellCallback);
            *this = TerminalGrid(m_rows, m_cols);
            m_responseCallback = std::move(cb);
            m_bellCallback = std::move(bellCb);
            break;
        }
        }
    }
}

void TerminalGrid::handleOsc(const std::string &payload) {
    if (payload.empty()) return;

    // Find the OSC number (everything before the first ';')
    size_t semi = payload.find(';');
    std::string oscNum = (semi != std::string::npos) ? payload.substr(0, semi) : payload;

    // OSC 0 or OSC 2 — window title
    if ((oscNum == "0" || oscNum == "2") && semi != std::string::npos) {
        QString title = QString::fromUtf8(payload.c_str() + semi + 1);
        QString sanitized;
        sanitized.reserve(title.size());
        for (const QChar &ch : title) {
            if (ch.unicode() >= 0x20 && ch.unicode() != 0x7F)
                sanitized += ch;
        }
        m_windowTitle = sanitized;
    }
    // OSC 8 — Hyperlinks: OSC 8 ; params ; uri ST (open) or OSC 8 ; ; ST (close)
    else if (oscNum == "8" && semi != std::string::npos) {
        std::string rest = payload.substr(semi + 1);
        size_t semi2 = rest.find(';');
        if (semi2 != std::string::npos) {
            std::string params = rest.substr(0, semi2);
            std::string uri = rest.substr(semi2 + 1);

            if (uri.empty()) {
                // Close hyperlink
                if (m_hyperlinkActive) {
                    HyperlinkSpan span;
                    span.startCol = m_hyperlinkStartCol;
                    span.endCol = m_cursorCol > 0 ? m_cursorCol - 1 : 0;
                    span.uri = m_hyperlinkUri;
                    span.id = m_hyperlinkId;
                    if (m_hyperlinkStartRow < m_rows)
                        m_screenHyperlinks[m_hyperlinkStartRow].push_back(std::move(span));
                    m_hyperlinkActive = false;
                    m_hyperlinkUri.clear();
                    m_hyperlinkId.clear();
                }
            } else {
                // Validate URI scheme — STANDARDS.md §Security and RULES.md
                // rule 7 mandate only http/https/ftp/file/mailto. A hostile
                // program could otherwise emit `javascript:` / `data:` /
                // `vnc:` and get Ctrl+click activation via QDesktopServices.
                auto colonPos = uri.find(':');
                bool validScheme = false;
                if (colonPos != std::string::npos && colonPos > 0) {
                    std::string scheme(uri.begin(), uri.begin() + colonPos);
                    for (auto &c : scheme) {
                        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    }
                    validScheme = (scheme == "http"   || scheme == "https" ||
                                   scheme == "ftp"    || scheme == "file"  ||
                                   scheme == "mailto");
                }
                if (!validScheme) {
                    // Drop the OSC 8 entirely; the following text remains
                    // un-hyperlinked but still prints normally.
                    m_hyperlinkActive = false;
                    m_hyperlinkUri.clear();
                    m_hyperlinkId.clear();
                } else {
                    // Open hyperlink
                    if (m_hyperlinkActive) {
                        // Close previous first
                        HyperlinkSpan span;
                        span.startCol = m_hyperlinkStartCol;
                        span.endCol = m_cursorCol > 0 ? m_cursorCol - 1 : 0;
                        span.uri = m_hyperlinkUri;
                        span.id = m_hyperlinkId;
                        if (m_hyperlinkStartRow < m_rows)
                            m_screenHyperlinks[m_hyperlinkStartRow].push_back(std::move(span));
                    }
                    m_hyperlinkActive = true;
                    m_hyperlinkUri = uri;
                    m_hyperlinkStartCol = m_cursorCol;
                    m_hyperlinkStartRow = m_cursorRow;
                    // Parse id from params (id=value)
                    m_hyperlinkId.clear();
                    auto idpos = params.find("id=");
                    if (idpos != std::string::npos) {
                        auto idend = params.find(':', idpos);
                        m_hyperlinkId = params.substr(idpos + 3, idend != std::string::npos ? idend - idpos - 3 : std::string::npos);
                    }
                }
            }
        }
    }
    // OSC 52 — Clipboard access: OSC 52 ; selection ; base64-data ST
    else if (oscNum == "52" && semi != std::string::npos) {
        std::string rest = payload.substr(semi + 1);
        size_t semi2 = rest.find(';');
        if (semi2 != std::string::npos) {
            // std::string selection = rest.substr(0, semi2); // c, p, s, etc.
            std::string b64 = rest.substr(semi2 + 1);
            if (b64 == "?") {
                // Query — not supported for security (terminal exfil vector)
            } else {
                // Cap clipboard writes at 1MB decoded (base64 expands ~4/3, so
                // ~1.33MB encoded). Larger pastes are almost certainly abuse;
                // real user copy operations are well below this.
                constexpr int kMaxClipboardBytes = 1 * 1024 * 1024;
                if (b64.size() > static_cast<size_t>(kMaxClipboardBytes) * 4 / 3 + 16) {
                    // Silently drop oversized payload
                    return;
                }
                QByteArray decoded = QByteArray::fromBase64(
                    QByteArray::fromRawData(b64.data(), static_cast<int>(b64.size())));
                if (decoded.size() > kMaxClipboardBytes) {
                    return;
                }
                if (!decoded.isEmpty()) {
                    // Per-terminal rolling 60s write quota (independent of the
                    // per-string cap). Protects against drip-feed exfiltration
                    // and against runaway programs. Silently drop once the
                    // window's budget is exhausted.
                    qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (now - m_osc52WindowStartMs >= 60'000) {
                        m_osc52WindowStartMs = now;
                        m_osc52WriteCount = 0;
                        m_osc52WriteBytes = 0;
                    }
                    if (m_osc52WriteCount >= OSC52_MAX_WRITES_PER_MIN) return;
                    if (m_osc52WriteBytes + decoded.size() > OSC52_MAX_BYTES_PER_MIN) return;
                    m_osc52WriteCount += 1;
                    m_osc52WriteBytes += decoded.size();

                    // Emit clipboard set via response callback with special prefix
                    if (m_responseCallback) {
                        std::string clipData(1, '\0');
                        clipData += "OSC52:";
                        clipData += std::string(decoded.constData(), decoded.size());
                        m_responseCallback(clipData);
                    }
                }
            }
        }
    }
    // OSC 133 — Shell integration: OSC 133 ; A/B/C/D ST
    else if (oscNum == "133" && semi != std::string::npos && semi + 1 < payload.size()) {
        char marker = payload[semi + 1];
        int globalLine = static_cast<int>(m_scrollback.size()) + m_cursorRow;
        switch (marker) {
        case 'A': // Prompt start
            m_shellIntegState = 'A';
            m_promptRegions.push_back({globalLine, globalLine, false, 0, 0, false});
            break;
        case 'B': // Command start (end of prompt)
            m_shellIntegState = 'B';
            if (!m_promptRegions.empty()) {
                m_promptRegions.back().endLine = globalLine;
                m_promptRegions.back().commandStartMs = QDateTime::currentMSecsSinceEpoch();
                // Record the cursor column at B fire — this is where the PS1
                // ends and the user's command text begins on the prompt line.
                // Block UI uses this to extract just the command (strip PS1).
                m_promptRegions.back().commandStartCol = m_cursorCol;
            }
            break;
        case 'C': // Command output start
            m_shellIntegState = 'C';
            if (!m_promptRegions.empty()) {
                m_promptRegions.back().hasOutput = true;
                m_promptRegions.back().outputStartLine = globalLine;
            }
            m_commandOutputStart = globalLine;
            break;
        case 'D': { // Command finished
            m_shellIntegState = 0;
            qint64 endMs = QDateTime::currentMSecsSinceEpoch();
            qint64 durationMs = 0;
            if (!m_promptRegions.empty()) {
                m_promptRegions.back().commandEndMs = endMs;
                qint64 startMs = m_promptRegions.back().commandStartMs;
                if (startMs > 0) durationMs = endMs - startMs;
            }
            // Parse exit code: OSC 133 ; D ; exitcode ST
            std::string rest = payload.substr(semi + 1);
            size_t semi2 = rest.find(';');
            if (semi2 != std::string::npos) {
                std::string code = rest.substr(semi2 + 1);
                try {
                    m_lastExitCode = std::stoi(code);
                } catch (...) {
                    DBGLOG("OSC 133 D exit-code parse failed: '%s'", code.c_str());
                    m_lastExitCode = 0;
                }
            } else {
                m_lastExitCode = 0;
            }
            // Record the exit code on the closing region (0.7.0 block UI reads
            // region.exitCode directly so it can paint per-block pass/fail
            // indicators without relying on the grid-global most-recent value).
            if (!m_promptRegions.empty())
                m_promptRegions.back().exitCode = m_lastExitCode;
            // Notify consumers (TerminalWidget → MainWindow → PluginManager
            // command_finished event). Fires after exit code is parsed so the
            // payload is complete.
            if (m_commandFinishedCallback)
                m_commandFinishedCallback(m_lastExitCode, durationMs);
            break;
        }
        }
        // Cap stored prompt regions
        while (static_cast<int>(m_promptRegions.size()) > MAX_PROMPT_REGIONS)
            m_promptRegions.erase(m_promptRegions.begin());
    }
    // OSC 1337 — iTerm2 multi-purpose channel.
    //   ESC ] 1337 ; File=<params>:<base64>      → inline image
    //   ESC ] 1337 ; SetUserVar=<NAME>=<base64>  → user-var (WezTerm/iTerm2)
    // Disambiguate by the byte after "1337;": 'F' (File=...) vs 'S' (SetUserVar=...).
    // Cheap prefix peek avoids running the image-decode path on user-var traffic
    // and vice versa.
    else if (payload.compare(0, 5, "1337;") == 0) {
        if (payload.compare(5, 11, "SetUserVar=") == 0 && m_userVarCallback) {
            // Format: SetUserVar=NAME=<base64-value>
            std::string rest = payload.substr(16);  // strip "1337;SetUserVar="
            size_t eq = rest.find('=');
            if (eq != std::string::npos) {
                std::string name = rest.substr(0, eq);
                std::string b64  = rest.substr(eq + 1);
                // Reject empty / overlong names. iTerm2 caps NAME to identifier-
                // ish; we match that pragmatically (≤128 chars, non-empty).
                if (!name.empty() && name.size() <= 128) {
                    QByteArray decoded = QByteArray::fromBase64(
                        QByteArray::fromRawData(b64.data(), static_cast<int>(b64.size())));
                    // Cap decoded value at 4 KiB — this is a status payload, not
                    // a transport. Anything larger is hostile or buggy.
                    constexpr int kMaxUserVarBytes = 4096;
                    if (decoded.size() > kMaxUserVarBytes)
                        decoded.truncate(kMaxUserVarBytes);
                    m_userVarCallback(QString::fromUtf8(name.c_str(), static_cast<int>(name.size())),
                                       QString::fromUtf8(decoded));
                }
            }
        } else {
            handleOscImage(payload);
        }
    }
    // OSC 9;4 — ConEmu / Microsoft Terminal progress reporting
    // Spec: https://learn.microsoft.com/en-us/windows/terminal/tutorials/progress-bar-sequences
    // Payload: "9;4;<state>;<percent>"  (percent optional for state 0/3)
    else if (oscNum == "9" && semi != std::string::npos &&
             payload.size() >= semi + 3 &&
             payload[semi + 1] == '4' && payload[semi + 2] == ';') {
        std::string rest = payload.substr(semi + 3);
        size_t semi2 = rest.find(';');
        int stateNum = 0;
        int percent = 0;
        try {
            stateNum = std::stoi(semi2 == std::string::npos ? rest : rest.substr(0, semi2));
            if (semi2 != std::string::npos) {
                percent = std::stoi(rest.substr(semi2 + 1));
            }
        } catch (...) {
            DBGLOG("OSC 9;4 progress parse failed: '%s'", rest.c_str());
            return;  // malformed — ignore
        }
        if (stateNum < 0 || stateNum > 4) return;
        percent = std::clamp(percent, 0, 100);
        m_progressState = static_cast<ProgressState>(stateNum);
        m_progressValue = (stateNum == 0 || stateNum == 3) ? 0 : percent;
        if (m_progressCallback) m_progressCallback(m_progressState, m_progressValue);
    }
    // OSC 9 — Desktop notification (body only, iTerm2/Ghostty style)
    //
    // The VT parser caps OSC payloads at 10MB (vtparser.cpp:282) to cover
    // inline-image escape sequences. Notifications are short by nature; a
    // multi-MB body here is either a mistake or an attempt to spam the
    // freedesktop notification daemon. Clamp aggressively before forwarding.
    else if (oscNum == "9" && semi != std::string::npos && m_notifyCallback) {
        constexpr int kMaxNotifyBody = 1024;
        QString body = QString::fromUtf8(payload.c_str() + semi + 1).left(kMaxNotifyBody);
        m_notifyCallback(QString(), body);
    }
    // OSC 777 — Desktop notification (notify;title;body, foot/Ghostty/WezTerm style)
    else if (oscNum == "777" && semi != std::string::npos && m_notifyCallback) {
        constexpr int kMaxNotifyTitle = 256;
        constexpr int kMaxNotifyBody = 1024;
        std::string rest = payload.substr(semi + 1);
        // Format: notify;title;body
        if (rest.compare(0, 7, "notify;") == 0) {
            rest = rest.substr(7);
            size_t semi2 = rest.find(';');
            if (semi2 != std::string::npos) {
                QString title = QString::fromUtf8(rest.c_str(), static_cast<int>(semi2)).left(kMaxNotifyTitle);
                QString body = QString::fromUtf8(rest.c_str() + semi2 + 1).left(kMaxNotifyBody);
                m_notifyCallback(title, body);
            } else {
                m_notifyCallback(QString(), QString::fromUtf8(rest.c_str()).left(kMaxNotifyBody));
            }
        }
    }
}

void TerminalGrid::handleOscImage(const std::string &payload) {
    // Format: 1337;File=[params]:BASE64DATA
    // Params: name=..., size=..., width=..., height=..., inline=1
    auto colonPos = payload.find(':', 5);
    if (colonPos == std::string::npos) return;

    std::string params = payload.substr(5, colonPos - 5);
    std::string b64data = payload.substr(colonPos + 1);

    // Check inline=1
    if (params.find("inline=1") == std::string::npos) return;

    QByteArray decoded = QByteArray::fromBase64(QByteArray::fromRawData(b64data.data(), b64data.size()));
    QImage img;
    if (!img.loadFromData(decoded)) return;
    if (img.width() > MAX_IMAGE_DIM || img.height() > MAX_IMAGE_DIM) return;

    // Parse optional width/height in cells
    // Default: scale image to fit reasonable cell dimensions
    int cellW = std::min(img.width() / 8, m_cols - m_cursorCol);
    int cellH = std::min(img.height() / 16, m_rows / 2);
    if (cellW < 1) cellW = 1;
    if (cellH < 1) cellH = 1;

    // Check for explicit width=Nc (cells) in params
    auto parseParam = [&](const std::string &key) -> int {
        auto pos = params.find(key + "=");
        if (pos == std::string::npos) return -1;
        pos += key.size() + 1;
        auto end = params.find(';', pos);
        std::string val = (end != std::string::npos) ? params.substr(pos, end - pos) : params.substr(pos);
        try { return std::stoi(val); }
        catch (...) { DBGLOG("OSC 1337 param '%s' int-parse failed: '%s'", key.c_str(), val.c_str()); return -1; }
    };

    int pw = parseParam("width");
    int ph = parseParam("height");
    if (pw > 0) cellW = std::min(pw, m_cols);
    if (ph > 0) cellH = std::min(ph, m_rows);

    InlineImage iimg;
    iimg.image = std::move(img);
    iimg.row = m_cursorRow;
    iimg.col = m_cursorCol;
    iimg.cellWidth = cellW;
    iimg.cellHeight = cellH;
    m_inlineImages.push_back(std::move(iimg));

    // Cap inline images to prevent memory exhaustion
    while (static_cast<int>(m_inlineImages.size()) > MAX_INLINE_IMAGES)
        m_inlineImages.erase(m_inlineImages.begin());

    // Advance cursor past image
    m_cursorRow += cellH;
    if (m_cursorRow >= m_rows) m_cursorRow = m_rows - 1;
    m_cursorCol = 0;
}

void TerminalGrid::handleSGR(const std::vector<int> &params, const std::vector<bool> &colonSep) {
    if (params.empty()) {
        m_currentAttrs = CellAttrs{};
        m_currentAttrs.fg = m_defaultFg;
        m_currentAttrs.bg = m_defaultBg;
        return;
    }

    // Helper: is params[i] a colon sub-parameter of the previous param?
    auto isSubParam = [&](size_t idx) -> bool {
        return idx < colonSep.size() && colonSep[idx];
    };

    for (size_t i = 0; i < params.size(); ++i) {
        int code = params[i];
        switch (code) {
        case 0:
            if (m_currentAttrs.underline)
                DBGLOG("SGR 0 (reset) -> underline OFF (was on) row=%d col=%d", m_cursorRow, m_cursorCol);
            m_currentAttrs = CellAttrs{};
            m_currentAttrs.fg = m_defaultFg;
            m_currentAttrs.bg = m_defaultBg;
            break;
        case 1: m_currentAttrs.bold = true; break;
        case 2: m_currentAttrs.dim = true; break;
        case 3: m_currentAttrs.italic = true; break;
        case 4:
            // SGR 4 — plain underline, or 4:x colon sub-param for underline style.
            // Only consume next param as sub-param if it was colon-separated.
            if (i + 1 < params.size() && isSubParam(i + 1) &&
                params[i + 1] >= 0 && params[i + 1] <= 5) {
                int style = params[++i];
                m_currentAttrs.underlineStyle = static_cast<UnderlineStyle>(style);
                m_currentAttrs.underline = (style != 0);
                DBGLOG("SGR 4:%d -> underline=%d style=%d row=%d col=%d",
                       style, m_currentAttrs.underline, style, m_cursorRow, m_cursorCol);
            } else {
                m_currentAttrs.underline = true;
                m_currentAttrs.underlineStyle = UnderlineStyle::Single;
                DBGLOG("SGR 4 (plain) -> underline=1 row=%d col=%d", m_cursorRow, m_cursorCol);
            }
            break;
        case 7: m_currentAttrs.inverse = true; break;
        case 9: m_currentAttrs.strikethrough = true; break;
        case 21:
            // SGR 21 — double underline (also used as bold off)
            m_currentAttrs.underline = true;
            m_currentAttrs.underlineStyle = UnderlineStyle::Double;
            break;
        case 22: m_currentAttrs.bold = false; m_currentAttrs.dim = false; break;
        case 23: m_currentAttrs.italic = false; break;
        case 24:
            DBGLOG("SGR 24 -> underline OFF row=%d col=%d", m_cursorRow, m_cursorCol);
            m_currentAttrs.underline = false;
            m_currentAttrs.underlineStyle = UnderlineStyle::None;
            break;
        case 27: m_currentAttrs.inverse = false; break;
        case 29: m_currentAttrs.strikethrough = false; break;

        // Underline color: CSI 58;2;r;g;b m or CSI 58;5;idx m
        case 58:
            if (i + 1 < params.size()) {
                if (params[i + 1] == 2 && i + 4 < params.size()) {
                    int r = std::clamp(params[i + 2], 0, 255);
                    int g = std::clamp(params[i + 3], 0, 255);
                    int b = std::clamp(params[i + 4], 0, 255);
                    m_currentAttrs.underlineColor = QColor(r, g, b);
                    i += 4;
                } else if (params[i + 1] == 5 && i + 2 < params.size()) {
                    int idx = params[i + 2];
                    if (idx >= 0 && idx < 256)
                        m_currentAttrs.underlineColor = s_palette256[idx];
                    i += 2;
                }
            }
            break;
        case 59:
            m_currentAttrs.underlineColor = QColor(); // reset to invalid (use fg)
            break;

        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            m_currentAttrs.fg = s_palette256[code - 30];
            break;
        case 38:
            if (i + 1 < params.size()) {
                if (params[i + 1] == 5) m_currentAttrs.fg = parse256Color(params, i);
                else if (params[i + 1] == 2) m_currentAttrs.fg = parseRGBColor(params, i);
            }
            break;
        case 39: m_currentAttrs.fg = m_defaultFg; break;

        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            m_currentAttrs.bg = s_palette256[code - 40];
            break;
        case 48:
            if (i + 1 < params.size()) {
                if (params[i + 1] == 5) m_currentAttrs.bg = parse256Color(params, i);
                else if (params[i + 1] == 2) m_currentAttrs.bg = parseRGBColor(params, i);
            }
            break;
        case 49: m_currentAttrs.bg = m_defaultBg; break;

        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            m_currentAttrs.fg = s_palette256[code - 90 + 8];
            break;

        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            m_currentAttrs.bg = s_palette256[code - 100 + 8];
            break;
        }
    }
}

QColor TerminalGrid::parse256Color(const std::vector<int> &params, size_t &i) {
    if (i + 2 < params.size()) {
        int idx = params[i + 2];
        i += 2;
        if (idx >= 0 && idx < 256) return s_palette256[idx];
    }
    return m_defaultFg;
}

QColor TerminalGrid::parseRGBColor(const std::vector<int> &params, size_t &i) {
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
    case 0:
        eraseInLine(0);
        for (int r = m_cursorRow + 1; r < m_rows; ++r) clearRow(r);
        break;
    case 1:
        for (int r = 0; r < m_cursorRow; ++r) clearRow(r);
        eraseInLine(1);
        break;
    case 2:
    case 3:
        for (int r = 0; r < m_rows; ++r) clearRow(r);
        if (mode == 3) m_scrollback.clear();
        break;
    }
}

void TerminalGrid::eraseInLine(int mode) {
    switch (mode) {
    case 0: clearRow(m_cursorRow, m_cursorCol, m_cols); break;
    case 1: clearRow(m_cursorRow, 0, m_cursorCol + 1); break;
    case 2: clearRow(m_cursorRow); break;
    }
}

void TerminalGrid::insertLines(int count) {
    int bottom = m_scrollBottom;
    if (m_cursorRow < m_scrollTop || m_cursorRow > bottom) return;
    markAllScreenDirty();
    count = std::min(count, bottom - m_cursorRow + 1);
    for (int i = 0; i < count; ++i) {
        m_screenLines.erase(m_screenLines.begin() + bottom);
        TermLine tl;
        tl.cells = makeRow(m_cols, m_defaultFg, m_defaultBg);
        m_screenLines.insert(m_screenLines.begin() + m_cursorRow, std::move(tl));
    }
}

void TerminalGrid::deleteLines(int count) {
    int bottom = m_scrollBottom;
    if (m_cursorRow < m_scrollTop || m_cursorRow > bottom) return;
    markAllScreenDirty();
    count = std::min(count, bottom - m_cursorRow + 1);
    for (int i = 0; i < count; ++i) {
        m_screenLines.erase(m_screenLines.begin() + m_cursorRow);
        TermLine tl;
        tl.cells = makeRow(m_cols, m_defaultFg, m_defaultBg);
        m_screenLines.insert(m_screenLines.begin() + bottom, std::move(tl));
    }
}

void TerminalGrid::deleteChars(int count) {
    markScreenDirty(m_cursorRow);
    auto &row = screenRow(m_cursorRow);
    count = std::min(count, m_cols - m_cursorCol);
    row.erase(row.begin() + m_cursorCol, row.begin() + m_cursorCol + count);
    while (static_cast<int>(row.size()) < m_cols) {
        Cell c;
        c.codepoint = ' ';
        c.attrs.fg = m_defaultFg;
        c.attrs.bg = m_currentAttrs.bg;
        row.push_back(c);
    }
    // Shift combining chars
    auto &comb = m_screenLines[m_cursorRow].combining;
    std::unordered_map<int, std::vector<uint32_t>> newComb;
    for (auto &[col, chars] : comb) {
        if (col < m_cursorCol)
            newComb[col] = std::move(chars);
        else if (col >= m_cursorCol + count)
            newComb[col - count] = std::move(chars);
    }
    comb = std::move(newComb);
}

void TerminalGrid::insertBlanks(int count) {
    markScreenDirty(m_cursorRow);
    auto &row = screenRow(m_cursorRow);
    count = std::min(count, m_cols - m_cursorCol);
    Cell blank;
    blank.codepoint = ' ';
    blank.attrs.fg = m_defaultFg;
    blank.attrs.bg = m_currentAttrs.bg;
    row.insert(row.begin() + m_cursorCol, count, blank);
    row.resize(m_cols);
    // Shift combining chars
    auto &comb = m_screenLines[m_cursorRow].combining;
    std::unordered_map<int, std::vector<uint32_t>> newComb;
    for (auto &[col, chars] : comb) {
        if (col < m_cursorCol)
            newComb[col] = std::move(chars);
        else if (col + count < m_cols)
            newComb[col + count] = std::move(chars);
    }
    comb = std::move(newComb);
}

void TerminalGrid::scrollUp(int count) {
    markAllScreenDirty();
    for (int i = 0; i < count; ++i) {
        if (m_scrollTop == 0 && !m_altScreenActive) {
            m_scrollback.push_back(std::move(m_screenLines[m_scrollTop]));
            ++m_scrollbackPushed;
            if (static_cast<int>(m_scrollback.size()) > m_maxScrollback)
                m_scrollback.pop_front();
            // Move hyperlinks to scrollback
            if (m_scrollTop < static_cast<int>(m_screenHyperlinks.size())) {
                m_scrollbackHyperlinks.push_back(std::move(m_screenHyperlinks[m_scrollTop]));
                while (static_cast<int>(m_scrollbackHyperlinks.size()) > m_maxScrollback)
                    m_scrollbackHyperlinks.pop_front();
            }
        }
        m_screenLines.erase(m_screenLines.begin() + m_scrollTop);
        TermLine tl;
        tl.cells = makeRow(m_cols, m_defaultFg, m_defaultBg);
        m_screenLines.insert(m_screenLines.begin() + m_scrollBottom, std::move(tl));
        // Shift hyperlink rows
        if (m_scrollTop < static_cast<int>(m_screenHyperlinks.size()) &&
            m_scrollBottom <= static_cast<int>(m_screenHyperlinks.size())) {
            m_screenHyperlinks.erase(m_screenHyperlinks.begin() + m_scrollTop);
            m_screenHyperlinks.insert(m_screenHyperlinks.begin() +
                std::min(m_scrollBottom, static_cast<int>(m_screenHyperlinks.size())),
                std::vector<HyperlinkSpan>{});
        }
    }
}

void TerminalGrid::scrollDown(int count) {
    markAllScreenDirty();
    for (int i = 0; i < count; ++i) {
        m_screenLines.erase(m_screenLines.begin() + m_scrollBottom);
        TermLine tl;
        tl.cells = makeRow(m_cols, m_defaultFg, m_defaultBg);
        m_screenLines.insert(m_screenLines.begin() + m_scrollTop, std::move(tl));
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
    m_cursorRow = std::clamp(m_savedRow, 0, m_rows - 1);
    m_cursorCol = std::clamp(m_savedCol, 0, m_cols - 1);
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
    // Advance to next tab stop (or end of line)
    for (int c = m_cursorCol + 1; c < m_cols; ++c) {
        if (c < static_cast<int>(m_tabStops.size()) && m_tabStops[c]) {
            m_cursorCol = c;
            return;
        }
    }
    m_cursorCol = m_cols - 1;
}

void TerminalGrid::reverseIndex() {
    if (m_cursorRow == m_scrollTop) {
        scrollDown(1);
    } else if (m_cursorRow > 0) {
        --m_cursorRow;
    }
}

Cell &TerminalGrid::cell(int row, int col) {
    return m_screenLines[std::clamp(row, 0, m_rows - 1)].cells[std::clamp(col, 0, m_cols - 1)];
}

const Cell &TerminalGrid::cellAt(int row, int col) const {
    return m_screenLines[std::clamp(row, 0, m_rows - 1)].cells[std::clamp(col, 0, m_cols - 1)];
}

void TerminalGrid::clearRow(int row, int startCol, int endCol) {
    if (endCol < 0) endCol = m_cols;
    auto &cells = screenRow(row);
    for (int c = startCol; c < endCol && c < m_cols; ++c) {
        cells[c].codepoint = ' ';
        cells[c].attrs = CellAttrs{};
        cells[c].attrs.fg = m_defaultFg;
        cells[c].attrs.bg = m_currentAttrs.bg.isValid() ? m_currentAttrs.bg : m_defaultBg;
        cells[c].isWideChar = false;
        cells[c].isWideCont = false;
    }
    m_screenLines[row].softWrapped = false;
    markScreenDirty(row);
    // Clear combining characters
    if (startCol == 0 && endCol >= m_cols) {
        m_screenLines[row].combining.clear();
    } else {
        for (int c = startCol; c < endCol && c < m_cols; ++c)
            m_screenLines[row].combining.erase(c);
    }
}

void TerminalGrid::clearScreenContent() {
    for (int r = 0; r < m_rows; ++r)
        clearRow(r);
    m_cursorRow = 0;
    m_cursorCol = 0;
    m_wrapNext = false;
}

void TerminalGrid::resize(int rows, int cols) {
    if (rows == m_rows && cols == m_cols) return;

    // A "logical line" is a join of all cells from one or more soft-wrapped
    // lines, plus a parallel combining map keyed by position-in-the-join.
    struct LogicalLine {
        std::vector<Cell> cells;
        std::unordered_map<int, std::vector<uint32_t>> combining;
    };

    auto joinLogical = [](auto &lines) {
        std::vector<LogicalLine> out;
        LogicalLine cur;
        for (auto &sl : lines) {
            auto &cells = sl.cells;
            int len = static_cast<int>(cells.size());
            while (len > 0 && cells[len - 1].codepoint == ' ') --len;
            int before = static_cast<int>(cur.cells.size());
            cur.cells.insert(cur.cells.end(), cells.begin(), cells.begin() + len);
            // Merge combining entries, shifted by the logical-line offset.
            // Drop any entry whose source col fell inside the trimmed trailing
            // spaces (its base cell is gone).
            for (auto &kv : sl.combining) {
                if (kv.first < len)
                    cur.combining[before + kv.first] = kv.second;
            }
            if (!sl.softWrapped) {
                out.push_back(std::move(cur));
                cur = {};
            }
        }
        if (!cur.cells.empty() || !cur.combining.empty()) out.push_back(std::move(cur));
        return out;
    };

    auto rewrap = [cols, this](LogicalLine &logical, std::vector<TermLine> &out) {
        if (logical.cells.empty()) {
            TermLine tl;
            tl.cells = makeRow(cols, m_defaultFg, m_defaultBg);
            out.push_back(std::move(tl));
            return;
        }
        int pos = 0;
        int total = static_cast<int>(logical.cells.size());
        while (pos < total) {
            int chunk = std::min(cols, total - pos);
            TermLine tl;
            tl.cells = makeRow(cols, m_defaultFg, m_defaultBg);
            for (int i = 0; i < chunk; ++i) tl.cells[i] = logical.cells[pos + i];
            // Redistribute combining entries that fall in [pos, pos+chunk).
            for (auto &kv : logical.combining) {
                if (kv.first >= pos && kv.first < pos + chunk)
                    tl.combining[kv.first - pos] = kv.second;
            }
            pos += chunk;
            tl.softWrapped = (pos < total);
            out.push_back(std::move(tl));
        }
    };

    // --- Reflow scrollback when width changes ---
    if (cols != m_cols && !m_scrollback.empty()) {
        auto logicalLines = joinLogical(m_scrollback);
        m_scrollback.clear();
        std::vector<TermLine> scratch;
        for (auto &logical : logicalLines) {
            rewrap(logical, scratch);
        }
        for (auto &tl : scratch) m_scrollback.push_back(std::move(tl));

        // Trim to limit
        while (static_cast<int>(m_scrollback.size()) > m_maxScrollback)
            m_scrollback.pop_front();
    }

    // Reflow screen lines when width changes
    if (cols != m_cols && !m_altScreenActive) {
        auto logicalScreen = joinLogical(m_screenLines);
        std::vector<TermLine> reflowed;
        for (auto &logical : logicalScreen) {
            rewrap(logical, reflowed);
        }

        // If we have more reflowed lines than fit on screen, push excess to scrollback
        while (static_cast<int>(reflowed.size()) > rows) {
            m_scrollback.push_back(std::move(reflowed.front()));
            reflowed.erase(reflowed.begin());
        }

        // Build final screen
        std::vector<TermLine> newScreen(rows);
        for (auto &line : newScreen)
            line.cells = makeRow(cols, m_defaultFg, m_defaultBg);
        for (int r = 0; r < static_cast<int>(reflowed.size()) && r < rows; ++r)
            newScreen[r] = std::move(reflowed[r]);

        m_screenLines = std::move(newScreen);

        // Reposition cursor on the last content line
        int lastContent = 0;
        for (int r = rows - 1; r >= 0; --r) {
            bool hasContent = false;
            for (int c = 0; c < cols; ++c) {
                if (m_screenLines[r].cells[c].codepoint != ' ') {
                    hasContent = true;
                    break;
                }
            }
            if (hasContent) { lastContent = r; break; }
        }
        m_cursorRow = std::min(m_cursorRow, lastContent + 1);
        m_cursorRow = std::clamp(m_cursorRow, 0, rows - 1);
        m_cursorCol = std::clamp(m_cursorCol, 0, cols - 1);
    } else {
        // No reflow needed (same width or alt screen) — simple copy
        std::vector<TermLine> newScreen(rows);
        for (auto &line : newScreen)
            line.cells = makeRow(cols, m_defaultFg, m_defaultBg);
        int copyRows = std::min(rows, m_rows);
        int copyCols = std::min(cols, m_cols);
        for (int r = 0; r < copyRows; ++r)
            for (int c = 0; c < copyCols; ++c)
                newScreen[r].cells[c] = m_screenLines[r].cells[c];
        m_screenLines = std::move(newScreen);
    }

    // Also resize alt screen buffer if it exists
    if (!m_altScreen.empty()) {
        std::vector<TermLine> newAlt(rows);
        for (auto &line : newAlt)
            line.cells = makeRow(cols, m_defaultFg, m_defaultBg);
        int altCopyRows = std::min(rows, static_cast<int>(m_altScreen.size()));
        int altCopyCols = std::min(cols, m_cols);
        for (int r = 0; r < altCopyRows; ++r)
            for (int c = 0; c < altCopyCols && c < static_cast<int>(m_altScreen[r].cells.size()); ++c)
                newAlt[r].cells[c] = m_altScreen[r].cells[c];
        m_altScreen = std::move(newAlt);
        m_altCursorRow = std::clamp(m_altCursorRow, 0, rows - 1);
        m_altCursorCol = std::clamp(m_altCursorCol, 0, cols - 1);
    }

    m_rows = rows;
    m_cols = cols;
    m_scrollBottom = m_rows - 1;
    m_scrollTop = 0;
    m_cursorRow = std::clamp(m_cursorRow, 0, m_rows - 1);
    m_cursorCol = std::clamp(m_cursorCol, 0, m_cols - 1);
    m_screenHyperlinks.resize(m_rows);

    // Reinitialize tab stops for new width
    initTabStops();

    // Layout changed — force span caches + full repaint.
    markAllScreenDirty();
}

// --- Sixel Graphics (DCS q ... ST) ---

void TerminalGrid::handleDcs(const std::string &payload) {
    if (payload.empty()) return;

    // Find the 'q' that starts Sixel data
    size_t qpos = payload.find('q');
    if (qpos == std::string::npos) return;

    // Parse raster attributes if present
    size_t dataStart = qpos + 1;

    // Sixel color palette (up to 256 entries)
    QColor palette[256];
    for (int i = 0; i < 16; ++i) palette[i] = s_palette256[i];
    for (int i = 16; i < 256; ++i) palette[i] = QColor(0, 0, 0);
    int currentColor = 0;

    // First pass: determine image dimensions
    int imgWidth = 0, imgHeight = 0;
    {
        int x = 0, y = 0, maxX = 0;
        for (size_t i = dataStart; i < payload.size(); ++i) {
            char ch = payload[i];
            if (ch >= '?' && ch <= '~') {
                ++x;
                if (x > maxX) maxX = x;
            } else if (ch == '$') {
                x = 0;
            } else if (ch == '-') {
                x = 0;
                y += 6;
            } else if (ch == '!') {
                // Repeat: !<count><char>
                int count = 0;
                ++i;
                while (i < payload.size() && payload[i] >= '0' && payload[i] <= '9') {
                    count = std::min(count * 10 + (payload[i] - '0'), MAX_IMAGE_DIM);
                    ++i;
                }
                if (i < payload.size() && payload[i] >= '?' && payload[i] <= '~') {
                    x += count;
                    if (x > maxX) maxX = x;
                }
            } else if (ch == '#') {
                // Skip color command
                ++i;
                while (i < payload.size() && ((payload[i] >= '0' && payload[i] <= '9') || payload[i] == ';'))
                    ++i;
                --i;
            } else if (ch == '"') {
                // Raster attributes: "Pan;Pad;Ph;Pv
                ++i;
                int rasterParams[4] = {0, 0, 0, 0};
                int rp = 0;
                while (i < payload.size() && rp < 4) {
                    if (payload[i] >= '0' && payload[i] <= '9') {
                        rasterParams[rp] = std::min(rasterParams[rp] * 10 + (payload[i] - '0'), MAX_IMAGE_DIM);
                    } else if (payload[i] == ';') {
                        ++rp;
                    } else {
                        break;
                    }
                    ++i;
                }
                --i;
                if (rasterParams[2] > 0) maxX = rasterParams[2];
                if (rasterParams[3] > 0) { y = 0; imgHeight = rasterParams[3]; }
            }
        }
        imgWidth = maxX;
        if (imgHeight == 0) imgHeight = y + 6;
    }

    if (imgWidth <= 0 || imgHeight <= 0 || imgWidth > MAX_IMAGE_DIM || imgHeight > MAX_IMAGE_DIM) return;

    QImage image(imgWidth, imgHeight, QImage::Format_ARGB32);
    image.fill(Qt::transparent);

    // Second pass: render pixels
    int x = 0, y = 0;
    for (size_t i = dataStart; i < payload.size(); ++i) {
        char ch = payload[i];
        if (ch >= '?' && ch <= '~') {
            int sixel = ch - '?';
            QColor col = palette[currentColor];
            for (int bit = 0; bit < 6; ++bit) {
                if (sixel & (1 << bit)) {
                    int py = y + bit;
                    if (x < imgWidth && py < imgHeight)
                        image.setPixelColor(x, py, col);
                }
            }
            ++x;
        } else if (ch == '$') {
            x = 0;
        } else if (ch == '-') {
            x = 0;
            y += 6;
        } else if (ch == '!') {
            // Repeat
            int count = 0;
            ++i;
            while (i < payload.size() && payload[i] >= '0' && payload[i] <= '9') {
                count = std::min(count * 10 + (payload[i] - '0'), MAX_IMAGE_DIM);
                ++i;
            }
            if (i < payload.size() && payload[i] >= '?' && payload[i] <= '~') {
                int sixel = payload[i] - '?';
                QColor col = palette[currentColor];
                for (int r = 0; r < count; ++r) {
                    for (int bit = 0; bit < 6; ++bit) {
                        if (sixel & (1 << bit)) {
                            int py = y + bit;
                            if (x < imgWidth && py < imgHeight)
                                image.setPixelColor(x, py, col);
                        }
                    }
                    ++x;
                }
            }
        } else if (ch == '#') {
            // Color command: #idx or #idx;2;r;g;b
            ++i;
            int idx = 0;
            while (i < payload.size() && payload[i] >= '0' && payload[i] <= '9') {
                idx = std::min(idx * 10 + (payload[i] - '0'), 255);
                ++i;
            }
            if (i < payload.size() && payload[i] == ';') {
                // Color definition
                ++i;
                int colorParams[5] = {0, 0, 0, 0, 0};
                int cp = 0;
                while (i < payload.size() && cp < 5) {
                    if (payload[i] >= '0' && payload[i] <= '9') {
                        colorParams[cp] = std::min(colorParams[cp] * 10 + (payload[i] - '0'), 100);
                    } else if (payload[i] == ';') {
                        ++cp;
                    } else {
                        break;
                    }
                    ++i;
                }
                --i;
                if (colorParams[0] == 2) {
                    // RGB (0-100 percentage)
                    int r = colorParams[1] * 255 / 100;
                    int g = colorParams[2] * 255 / 100;
                    int b = colorParams[3] * 255 / 100;
                    palette[idx] = QColor(std::clamp(r, 0, 255),
                                          std::clamp(g, 0, 255),
                                          std::clamp(b, 0, 255));
                }
            }
            currentColor = idx;
        }
    }

    // Place image at cursor position
    if (static_cast<int>(m_inlineImages.size()) >= MAX_INLINE_IMAGES)
        m_inlineImages.erase(m_inlineImages.begin());

    InlineImage img;
    img.image = image;
    img.row = m_cursorRow;
    img.col = m_cursorCol;
    img.cellWidth = imgWidth;
    img.cellHeight = imgHeight;
    img.pixelSized = true;
    m_inlineImages.push_back(img);
}

// --- Kitty Graphics Protocol (APC G ... ST) ---

// Safe integer parsing helpers (returns defaultVal on failure). Used in
// the Kitty graphics APC-parameter loop which fires per chunk of a paste
// and would flood any log attached here — kept intentionally silent.
static int safeStoi(const std::string &s, int defaultVal = 0) {
    try { return std::stoi(s); }
    catch (...) { return defaultVal; }
}
static uint32_t safeStoul(const std::string &s, uint32_t defaultVal = 0) {
    try { return static_cast<uint32_t>(std::stoul(s)); }
    catch (...) { return defaultVal; }
}

void TerminalGrid::handleApc(const std::string &payload) {
    if (payload.empty() || payload[0] != 'G') return;

    // Parse key=value pairs (comma-separated) before the semicolon
    size_t semicolon = payload.find(';');
    std::string params_str = (semicolon != std::string::npos)
        ? payload.substr(1, semicolon - 1) : payload.substr(1);
    std::string data_str = (semicolon != std::string::npos)
        ? payload.substr(semicolon + 1) : "";

    // Parse parameters
    std::unordered_map<char, std::string> params;
    size_t pos = 0;
    while (pos < params_str.size()) {
        char key = params_str[pos];
        if (pos + 1 >= params_str.size() || params_str[pos + 1] != '=') break;
        pos += 2;
        std::string value;
        while (pos < params_str.size() && params_str[pos] != ',') {
            value += params_str[pos];
            ++pos;
        }
        params[key] = value;
        if (pos < params_str.size() && params_str[pos] == ',') ++pos;
    }

    // Extract common parameters
    char action = 'T'; // Default: transmit + display
    if (params.count('a')) action = params['a'][0];

    int format = 32; // Default: RGBA
    if (params.count('f')) format = safeStoi(params['f'], 32);

    uint32_t imageId = 0;
    if (params.count('i')) imageId = safeStoul(params['i']);

    int more = 0; // Chunking: 1 = more data coming
    if (params.count('m')) more = safeStoi(params['m']);

    // Handle chunked transmission
    if (more == 1) {
        if (m_kittyChunkBuffer.empty()) m_kittyChunkId = imageId;
        m_kittyChunkBuffer += data_str;
        return; // Wait for more chunks
    }

    // Final chunk (or single transmission)
    std::string fullData = m_kittyChunkBuffer + data_str;
    m_kittyChunkBuffer.clear();
    if (imageId == 0 && m_kittyChunkId != 0) imageId = m_kittyChunkId;
    m_kittyChunkId = 0;

    // Handle actions
    if (action == 'd') {
        // Delete
        char deleteType = 'a';
        if (params.count('d')) deleteType = params['d'][0];
        if (deleteType == 'a') {
            m_kittyImages.clear();
            m_kittyImageOrder.clear();
            m_inlineImages.clear();
        } else if (deleteType == 'i' && imageId > 0) {
            if (m_kittyImages.erase(imageId) > 0) {
                auto it = std::find(m_kittyImageOrder.begin(), m_kittyImageOrder.end(), imageId);
                if (it != m_kittyImageOrder.end()) m_kittyImageOrder.erase(it);
            }
        }
        // Send response
        if (m_responseCallback) {
            std::string resp = "\x1B_Gi=" + std::to_string(imageId) + ";OK\x1B\\";
            m_responseCallback(resp);
        }
        return;
    }

    if (action == 'q') {
        // Query — respond with OK
        if (m_responseCallback) {
            std::string resp = "\x1B_Gi=" + std::to_string(imageId) + ";OK\x1B\\";
            m_responseCallback(resp);
        }
        return;
    }

    // Decode image data (cap at 10MB decoded to prevent DoS)
    QImage image;
    if (!fullData.empty() && fullData.size() <= 15 * 1024 * 1024) { // ~10MB decoded
        QByteArray base64Data = QByteArray::fromRawData(fullData.data(),
                                                         static_cast<int>(fullData.size()));
        QByteArray decoded = QByteArray::fromBase64(base64Data);

        if (format == 100) {
            // PNG format
            image.loadFromData(decoded, "PNG");
            if (image.width() > MAX_IMAGE_DIM || image.height() > MAX_IMAGE_DIM)
                image = QImage(); // Reject oversized
        } else if (format == 32 || format == 24) {
            // Raw pixel data
            int pixelW = 0, pixelH = 0;
            if (params.count('s')) pixelW = safeStoi(params['s']);
            if (params.count('v')) pixelH = safeStoi(params['v']);
            if (pixelW > 0 && pixelH > 0 && pixelW <= MAX_IMAGE_DIM && pixelH <= MAX_IMAGE_DIM) {
                int bytesPerPixel = (format == 32) ? 4 : 3;
                // Use int64 intermediates — multiplication stays safe even if
                // MAX_IMAGE_DIM is raised later (4096*4096*4 = 67MB fits int32
                // today, but this is a defense against future changes).
                int64_t required = static_cast<int64_t>(pixelW) * pixelH * bytesPerPixel;
                if (static_cast<int64_t>(decoded.size()) >= required) {
                    QImage::Format fmt = (format == 32)
                        ? QImage::Format_RGBA8888 : QImage::Format_RGB888;
                    image = QImage(reinterpret_cast<const uchar *>(decoded.constData()),
                                   pixelW, pixelH, fmt).copy(); // .copy() to own the data
                }
            }
        }
    }

    if (image.isNull() && action != 'p') {
        // Failed to decode — send error response
        if (m_responseCallback) {
            std::string resp = "\x1B_Gi=" + std::to_string(imageId) + ";ENODATA\x1B\\";
            m_responseCallback(resp);
        }
        return;
    }

    // Store image if it has an ID. Evict the oldest single entry on overflow
    // rather than clearing the whole cache (avoids thrashing warm images).
    if (imageId > 0 && !image.isNull()) {
        auto existing = m_kittyImages.find(imageId);
        if (existing != m_kittyImages.end()) {
            // Replacing: drop old position in the FIFO so it's re-appended as newest.
            auto it = std::find(m_kittyImageOrder.begin(), m_kittyImageOrder.end(), imageId);
            if (it != m_kittyImageOrder.end()) m_kittyImageOrder.erase(it);
        }
        while (static_cast<int>(m_kittyImages.size()) >= MAX_KITTY_CACHE
               && !m_kittyImageOrder.empty()) {
            uint32_t oldest = m_kittyImageOrder.front();
            m_kittyImageOrder.pop_front();
            m_kittyImages.erase(oldest);
        }
        m_kittyImages[imageId] = image;
        m_kittyImageOrder.push_back(imageId);
    }

    // Display if action is T (transmit+display) or p (display stored)
    if (action == 'T' || action == 'p') {
        QImage displayImg = image;
        if (action == 'p' && imageId > 0 && m_kittyImages.count(imageId)) {
            displayImg = m_kittyImages[imageId];
        }
        if (!displayImg.isNull()) {
            if (static_cast<int>(m_inlineImages.size()) >= MAX_INLINE_IMAGES)
                m_inlineImages.erase(m_inlineImages.begin());

            InlineImage img;
            img.image = displayImg;
            img.row = m_cursorRow;
            img.col = m_cursorCol;
            img.cellWidth = displayImg.width();
            img.cellHeight = displayImg.height();
            img.pixelSized = true;

            // Use cols/rows if specified (cell units override pixel sizing)
            if (params.count('c')) {
                img.cellWidth = safeStoi(params['c']);
                img.pixelSized = false;
            }
            if (params.count('r')) {
                img.cellHeight = safeStoi(params['r']);
                if (!params.count('c')) img.pixelSized = false;
            }

            m_inlineImages.push_back(img);
        }
    }

    // Send OK response
    if (m_responseCallback) {
        std::string resp = "\x1B_Gi=" + std::to_string(imageId) + ";OK\x1B\\";
        m_responseCallback(resp);
    }
}
