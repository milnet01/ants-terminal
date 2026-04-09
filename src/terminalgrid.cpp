#include "terminalgrid.h"

#include <algorithm>
#include <cwchar>
#include <QByteArray>

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
}

void TerminalGrid::setDefaultFg(const QColor &c) {
    QColor oldFg = m_defaultFg;
    m_defaultFg = c;
    for (auto &line : m_screenLines)
        for (auto &cell : line.cells)
            if (cell.attrs.fg == oldFg)
                cell.attrs.fg = c;
    if (m_currentAttrs.fg == oldFg)
        m_currentAttrs.fg = c;
}

void TerminalGrid::setDefaultBg(const QColor &c) {
    QColor oldBg = m_defaultBg;
    m_defaultBg = c;
    for (auto &line : m_screenLines)
        for (auto &cell : line.cells)
            if (cell.attrs.bg == oldBg)
                cell.attrs.bg = c;
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
    c.isWideChar = (charWidth == 2);
    c.isWideCont = false;
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
    case 'm': handleSGR(p); break;
    case 'r':
        setScrollRegion(param(0, 1) - 1, p.size() > 1 ? param(1, m_rows) - 1 : m_rows - 1);
        break;
    case 's': saveCursor(); break;
    case 'u': restoreCursor(); break;
    case 'h':
        if (a.intermediate == "?") {
            for (int v : p) {
                switch (v) {
                case 1: break;
                case 6: m_originMode = true; break;
                case 7: m_autoWrap = true; break;
                case 25: m_cursorVisible = true; break;
                case 1049:
                case 47:
                case 1047:
                    if (!m_altScreenActive) {
                        m_altScreenActive = true;
                        m_altCursorRow = m_cursorRow;
                        m_altCursorCol = m_cursorCol;
                        m_altScreen = m_screenLines;
                        for (auto &line : m_screenLines) {
                            for (auto &c : line.cells) {
                                c.codepoint = ' ';
                                c.attrs = CellAttrs{};
                                c.attrs.fg = m_defaultFg;
                                c.attrs.bg = m_defaultBg;
                            }
                            line.combining.clear();
                        }
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
                }
            }
        }
        break;
    case 'l':
        if (a.intermediate == "?") {
            for (int v : p) {
                switch (v) {
                case 1: break;
                case 6: m_originMode = false; break;
                case 7: m_autoWrap = false; break;
                case 25: m_cursorVisible = false; break;
                case 1049:
                case 47:
                case 1047:
                    if (m_altScreenActive) {
                        m_altScreenActive = false;
                        m_screenLines = m_altScreen;
                        m_cursorRow = m_altCursorRow;
                        m_cursorCol = m_altCursorCol;
                    }
                    break;
                case 1000: m_mouseButtonMode = false; break;
                case 1002: m_mouseMotionMode = false; break;
                case 1003: m_mouseAnyMode = false; break;
                case 1004: m_focusReporting = false; break;
                case 1006: m_mouseSgrMode = false; break;
                case 2004: m_bracketedPaste = false; break;
                case 2026: m_synchronizedOutput = false; break;
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
        }
        break;
    }
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
        case 'c': {
            auto cb = std::move(m_responseCallback);
            *this = TerminalGrid(m_rows, m_cols);
            m_responseCallback = std::move(cb);
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
    // OSC 52 — Clipboard access: OSC 52 ; selection ; base64-data ST
    else if (oscNum == "52" && semi != std::string::npos) {
        std::string rest = payload.substr(semi + 1);
        size_t semi2 = rest.find(';');
        if (semi2 != std::string::npos) {
            // std::string selection = rest.substr(0, semi2); // c, p, s, etc.
            std::string b64 = rest.substr(semi2 + 1);
            if (b64 == "?") {
                // Query — not supported for security
            } else {
                QByteArray decoded = QByteArray::fromBase64(
                    QByteArray::fromRawData(b64.data(), static_cast<int>(b64.size())));
                if (!decoded.isEmpty()) {
                    // Emit clipboard set via response callback with special prefix
                    if (m_responseCallback) {
                        std::string clipData = "\x00OSC52:" + std::string(decoded.constData(), decoded.size());
                        m_responseCallback(clipData);
                    }
                }
            }
        }
    }
    // OSC 133 — Shell integration: OSC 133 ; A/B/C/D ST
    else if (oscNum == "133" && semi != std::string::npos) {
        char marker = payload[semi + 1];
        int globalLine = static_cast<int>(m_scrollback.size()) + m_cursorRow;
        switch (marker) {
        case 'A': // Prompt start
            m_shellIntegState = 'A';
            m_promptRegions.push_back({globalLine, globalLine, false});
            break;
        case 'B': // Command start (end of prompt)
            m_shellIntegState = 'B';
            if (!m_promptRegions.empty())
                m_promptRegions.back().endLine = globalLine;
            break;
        case 'C': // Command output start
            m_shellIntegState = 'C';
            if (!m_promptRegions.empty())
                m_promptRegions.back().hasOutput = true;
            break;
        case 'D': // Command finished
            m_shellIntegState = 0;
            break;
        }
        // Cap stored prompt regions
        while (static_cast<int>(m_promptRegions.size()) > MAX_PROMPT_REGIONS)
            m_promptRegions.erase(m_promptRegions.begin());
    }
    // OSC 1337 — iTerm2 inline images
    else if (payload.compare(0, 5, "1337;") == 0) {
        handleOscImage(payload);
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
        try { return std::stoi(val); } catch (...) { return -1; }
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

void TerminalGrid::handleSGR(const std::vector<int> &params) {
    if (params.empty()) {
        m_currentAttrs = CellAttrs{};
        m_currentAttrs.fg = m_defaultFg;
        m_currentAttrs.bg = m_defaultBg;
        return;
    }

    for (size_t i = 0; i < params.size(); ++i) {
        int code = params[i];
        switch (code) {
        case 0:
            m_currentAttrs = CellAttrs{};
            m_currentAttrs.fg = m_defaultFg;
            m_currentAttrs.bg = m_defaultBg;
            break;
        case 1: m_currentAttrs.bold = true; break;
        case 2: m_currentAttrs.dim = true; break;
        case 3: m_currentAttrs.italic = true; break;
        case 4:
            // SGR 4 — could be plain underline or 4:x colon-separated subparam
            // Colon params arrive as consecutive params via our parser
            // Check if next param looks like a subparam (0-5)
            if (i + 1 < params.size() && params[i + 1] >= 0 && params[i + 1] <= 5) {
                int style = params[++i];
                m_currentAttrs.underlineStyle = static_cast<UnderlineStyle>(style);
                m_currentAttrs.underline = (style != 0);
            } else {
                m_currentAttrs.underline = true;
                m_currentAttrs.underlineStyle = UnderlineStyle::Single;
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
    count = std::min(count, bottom - m_cursorRow + 1);
    for (int i = 0; i < count; ++i) {
        m_screenLines.erase(m_screenLines.begin() + m_cursorRow);
        TermLine tl;
        tl.cells = makeRow(m_cols, m_defaultFg, m_defaultBg);
        m_screenLines.insert(m_screenLines.begin() + bottom, std::move(tl));
    }
}

void TerminalGrid::deleteChars(int count) {
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
    for (int i = 0; i < count; ++i) {
        if (m_scrollTop == 0 && !m_altScreenActive) {
            m_scrollback.push_back(std::move(m_screenLines[m_scrollTop]));
            if (static_cast<int>(m_scrollback.size()) > m_maxScrollback)
                m_scrollback.pop_front();
            // Move hyperlinks to scrollback
            if (!m_screenHyperlinks.empty()) {
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
        if (m_scrollTop < static_cast<int>(m_screenHyperlinks.size())) {
            m_screenHyperlinks.erase(m_screenHyperlinks.begin() + m_scrollTop);
            m_screenHyperlinks.insert(m_screenHyperlinks.begin() + m_scrollBottom,
                                       std::vector<HyperlinkSpan>{});
        }
    }
}

void TerminalGrid::scrollDown(int count) {
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
        cells[c].attrs.bg = m_defaultBg;
    }
    m_screenLines[row].softWrapped = false;
    // Clear combining characters
    if (startCol == 0 && endCol >= m_cols) {
        m_screenLines[row].combining.clear();
    } else {
        for (int c = startCol; c < endCol && c < m_cols; ++c)
            m_screenLines[row].combining.erase(c);
    }
}

void TerminalGrid::resize(int rows, int cols) {
    if (rows == m_rows && cols == m_cols) return;

    // --- Reflow scrollback when width changes ---
    if (cols != m_cols && !m_scrollback.empty()) {
        // 1. Join soft-wrapped scrollback lines into logical lines
        std::vector<std::vector<Cell>> logicalLines;
        std::vector<Cell> current;
        for (auto &sl : m_scrollback) {
            // Trim trailing spaces
            auto &cells = sl.cells;
            int len = static_cast<int>(cells.size());
            while (len > 0 && cells[len - 1].codepoint == ' ') --len;
            current.insert(current.end(), cells.begin(), cells.begin() + len);
            if (!sl.softWrapped) {
                logicalLines.push_back(std::move(current));
                current.clear();
            }
        }
        if (!current.empty()) {
            logicalLines.push_back(std::move(current));
        }

        // 2. Re-wrap to new width
        m_scrollback.clear();
        for (auto &logical : logicalLines) {
            if (logical.empty()) {
                TermLine tl;
                tl.cells.resize(cols);
                for (auto &c : tl.cells) { c.attrs.fg = m_defaultFg; c.attrs.bg = m_defaultBg; }
                m_scrollback.push_back(std::move(tl));
                continue;
            }
            int pos = 0;
            int total = static_cast<int>(logical.size());
            while (pos < total) {
                int chunk = std::min(cols, total - pos);
                TermLine tl;
                tl.cells.resize(cols);
                for (int i = 0; i < chunk; ++i) tl.cells[i] = logical[pos + i];
                for (int i = chunk; i < cols; ++i) {
                    tl.cells[i].attrs.fg = m_defaultFg;
                    tl.cells[i].attrs.bg = m_defaultBg;
                }
                pos += chunk;
                tl.softWrapped = (pos < total); // still more to wrap
                m_scrollback.push_back(std::move(tl));
            }
        }

        // Trim to limit
        while (static_cast<int>(m_scrollback.size()) > m_maxScrollback)
            m_scrollback.pop_front();
    }

    // Reflow screen lines when width changes
    if (cols != m_cols && !m_altScreenActive) {
        // Join soft-wrapped screen lines into logical lines
        std::vector<std::vector<Cell>> logicalScreen;
        std::vector<Cell> cur;
        for (auto &sl : m_screenLines) {
            auto &cells = sl.cells;
            int len = static_cast<int>(cells.size());
            while (len > 0 && cells[len - 1].codepoint == ' ') --len;
            cur.insert(cur.end(), cells.begin(), cells.begin() + len);
            if (!sl.softWrapped) {
                logicalScreen.push_back(std::move(cur));
                cur.clear();
            }
        }
        if (!cur.empty()) logicalScreen.push_back(std::move(cur));

        // Re-wrap into new width
        std::vector<TermLine> reflowed;
        for (auto &logical : logicalScreen) {
            if (logical.empty()) {
                TermLine tl;
                tl.cells = makeRow(cols, m_defaultFg, m_defaultBg);
                reflowed.push_back(std::move(tl));
                continue;
            }
            int pos = 0;
            int total = static_cast<int>(logical.size());
            while (pos < total) {
                int chunk = std::min(cols, total - pos);
                TermLine tl;
                tl.cells = makeRow(cols, m_defaultFg, m_defaultBg);
                for (int i = 0; i < chunk; ++i) tl.cells[i] = logical[pos + i];
                pos += chunk;
                tl.softWrapped = (pos < total);
                reflowed.push_back(std::move(tl));
            }
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

// Safe integer parsing helper (returns defaultVal on failure)
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
            m_inlineImages.clear();
        } else if (deleteType == 'i' && imageId > 0) {
            m_kittyImages.erase(imageId);
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
        } else if (format == 32 || format == 24) {
            // Raw pixel data
            int pixelW = 0, pixelH = 0;
            if (params.count('s')) pixelW = safeStoi(params['s']);
            if (params.count('v')) pixelH = safeStoi(params['v']);
            if (pixelW > 0 && pixelH > 0 && pixelW <= MAX_IMAGE_DIM && pixelH <= MAX_IMAGE_DIM) {
                int bytesPerPixel = (format == 32) ? 4 : 3;
                if (decoded.size() >= pixelW * pixelH * bytesPerPixel) {
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

    // Store image if it has an ID
    if (imageId > 0 && !image.isNull()) {
        if (static_cast<int>(m_kittyImages.size()) >= MAX_KITTY_CACHE) m_kittyImages.clear();
        m_kittyImages[imageId] = image;
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
