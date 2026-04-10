#include "vtparser.h"

VtParser::VtParser(ActionCallback callback)
    : m_callback(std::move(callback)) {}

void VtParser::feed(const char *data, int length) {
    for (int i = 0; i < length; ++i) {
        feedByte(static_cast<uint8_t>(data[i]));
    }
}

void VtParser::feedByte(uint8_t byte) {
    if (m_utf8Remaining > 0) {
        if ((byte & 0xC0) == 0x80) {
            m_utf8Accum = (m_utf8Accum << 6) | (byte & 0x3F);
            --m_utf8Remaining;
            if (m_utf8Remaining == 0) {
                // Reject overlong encodings and surrogates
                bool valid = true;
                if (m_utf8Accum < 0x80) valid = false;                    // overlong 2-byte
                else if (m_utf8Accum < 0x800 && m_utf8ExpectedLen >= 3) valid = false;  // overlong 3-byte
                else if (m_utf8Accum < 0x10000 && m_utf8ExpectedLen >= 4) valid = false; // overlong 4-byte
                else if (m_utf8Accum >= 0xD800 && m_utf8Accum <= 0xDFFF) valid = false;  // surrogate
                else if (m_utf8Accum > 0x10FFFF) valid = false;           // out of range
                flushCodepoint(valid ? m_utf8Accum : 0xFFFD);
            }
            return;
        }
        // Invalid continuation — reset and process this byte
        m_utf8Remaining = 0;
        flushCodepoint(0xFFFD);
    }

    if (byte < 0x80) {
        flushCodepoint(byte);
    } else if ((byte & 0xE0) == 0xC0) {
        m_utf8Accum = byte & 0x1F;
        m_utf8Remaining = 1;
        m_utf8ExpectedLen = 2;
    } else if ((byte & 0xF0) == 0xE0) {
        m_utf8Accum = byte & 0x0F;
        m_utf8Remaining = 2;
        m_utf8ExpectedLen = 3;
    } else if ((byte & 0xF8) == 0xF0) {
        m_utf8Accum = byte & 0x07;
        m_utf8Remaining = 3;
        m_utf8ExpectedLen = 4;
    } else {
        flushCodepoint(0xFFFD);
    }
}

void VtParser::flushCodepoint(uint32_t cp) {
    processChar(cp);
}

void VtParser::processChar(uint32_t ch) {
    // C0 controls are handled in almost every state
    if (ch < 0x20 || ch == 0x7F) {
        switch (ch) {
        case 0x1B: // ESC
            transition(Escape);
            return;
        case 0x18: // CAN
        case 0x1A: // SUB
            transition(Ground);
            return;
        default:
            break;
        }
    }

    switch (m_state) {
    case Ground:
        if (ch < 0x20) {
            // C0 control
            VtAction a;
            a.type = VtAction::Execute;
            a.controlChar = static_cast<char>(ch);
            m_callback(a);
        } else if (ch == 0x7F) {
            // DEL — ignore
        } else {
            // Printable character
            VtAction a;
            a.type = VtAction::Print;
            a.codepoint = ch;
            m_callback(a);
        }
        break;

    case Escape:
        if (ch == '[') {
            transition(CsiEntry);
        } else if (ch == ']') {
            m_oscString.clear();
            transition(OscString);
        } else if (ch >= 0x20 && ch <= 0x2F) {
            m_intermediate.clear();
            m_intermediate += static_cast<char>(ch);
            transition(EscapeIntermediate);
        } else if (ch == 'P') {
            // DCS — Sixel graphics
            m_dcsString.clear();
            transition(DcsString);
        } else if (ch == '_') {
            // APC — Kitty graphics
            m_apcString.clear();
            transition(ApcString);
        } else if (ch == 'X' || ch == '^') {
            // SOS, PM — consume and ignore
            transition(IgnoreString);
        } else if (ch >= 0x30 && ch <= 0x7E) {
            // ESC final byte
            VtAction a;
            a.type = VtAction::EscDispatch;
            a.finalChar = static_cast<char>(ch);
            a.intermediate = m_intermediate;
            m_callback(a);
            transition(Ground);
        } else if (ch < 0x20) {
            // C0 in escape
            VtAction a;
            a.type = VtAction::Execute;
            a.controlChar = static_cast<char>(ch);
            m_callback(a);
        }
        break;

    case EscapeIntermediate:
        if (ch >= 0x20 && ch <= 0x2F) {
            m_intermediate += static_cast<char>(ch);
        } else if (ch >= 0x30 && ch <= 0x7E) {
            VtAction a;
            a.type = VtAction::EscDispatch;
            a.finalChar = static_cast<char>(ch);
            a.intermediate = m_intermediate;
            m_callback(a);
            transition(Ground);
        } else {
            transition(Ground);
        }
        break;

    case CsiEntry:
        m_params.clear();
        m_currentParam = -1;
        m_intermediate.clear();
        if (ch >= 0x3C && ch <= 0x3F) {
            // Private parameter prefix: ?, >, =, <
            m_intermediate += static_cast<char>(ch);
            m_state = CsiParam;
        } else {
            // Fall through to CsiParam processing
            m_state = CsiParam;
            processChar(ch);
        }
        break;

    case CsiParam:
        if (ch >= '0' && ch <= '9') {
            if (m_currentParam < 0) m_currentParam = 0;
            m_currentParam = m_currentParam * 10 + (ch - '0');
            // Clamp to prevent overflow
            if (m_currentParam > 16384) m_currentParam = 16384;
        } else if (ch == ';') {
            if (m_params.size() < 32) { // Cap at 32 params to prevent DoS
                m_params.push_back(m_currentParam < 0 ? 0 : m_currentParam);
                m_colonSep.push_back(m_nextIsSubParam);
            }
            m_currentParam = -1;
            m_nextIsSubParam = false;
        } else if (ch >= 0x20 && ch <= 0x2F) {
            // Intermediate byte
            if (m_currentParam >= 0) {
                m_params.push_back(m_currentParam);
                m_colonSep.push_back(m_nextIsSubParam);
                m_currentParam = -1;
                m_nextIsSubParam = false;
            }
            m_intermediate += static_cast<char>(ch);
            m_state = CsiIntermediate;
        } else if (ch >= 0x40 && ch <= 0x7E) {
            // Final byte — dispatch
            if (m_currentParam >= 0) {
                m_params.push_back(m_currentParam);
                m_colonSep.push_back(m_nextIsSubParam);
            }
            m_nextIsSubParam = false;
            VtAction a;
            a.type = VtAction::CsiDispatch;
            a.finalChar = static_cast<char>(ch);
            a.params = m_params;
            a.colonSep = m_colonSep;
            a.intermediate = m_intermediate;
            m_callback(a);
            transition(Ground);
        } else if (ch == ':') {
            // Sub-parameter separator (used in SGR colon syntax like 4:3 for curly underline)
            if (m_params.size() < 32) {
                m_params.push_back(m_currentParam < 0 ? 0 : m_currentParam);
                m_colonSep.push_back(m_nextIsSubParam);
            }
            m_currentParam = -1;
            m_nextIsSubParam = true;  // Mark next param as a colon sub-parameter
        } else if (ch < 0x20) {
            VtAction a;
            a.type = VtAction::Execute;
            a.controlChar = static_cast<char>(ch);
            m_callback(a);
        } else {
            // Unexpected — abort
            transition(Ground);
        }
        break;

    case CsiIntermediate:
        if (ch >= 0x20 && ch <= 0x2F) {
            m_intermediate += static_cast<char>(ch);
        } else if (ch >= 0x40 && ch <= 0x7E) {
            VtAction a;
            a.type = VtAction::CsiDispatch;
            a.finalChar = static_cast<char>(ch);
            a.params = m_params;
            a.intermediate = m_intermediate;
            m_callback(a);
            transition(Ground);
        } else {
            transition(Ground);
        }
        break;

    case OscString:
        if (ch == 0x07 || ch == 0x9C) {
            // BEL or ST terminates OSC
            VtAction a;
            a.type = VtAction::OscEnd;
            a.oscString = m_oscString;
            m_callback(a);
            transition(Ground);
        } else if (ch == 0x1B) {
            // Might be ESC \ (ST)
            // Peek: we handle this by checking next char
            // For simplicity, end the OSC here
            VtAction a;
            a.type = VtAction::OscEnd;
            a.oscString = m_oscString;
            m_callback(a);
            transition(Escape);
        } else {
            if (m_oscString.size() < 10 * 1024 * 1024) { // 10MB for inline images
                // Encode back to UTF-8
                if (ch < 0x80) {
                    m_oscString += static_cast<char>(ch);
                } else if (ch < 0x800) {
                    m_oscString += static_cast<char>(0xC0 | (ch >> 6));
                    m_oscString += static_cast<char>(0x80 | (ch & 0x3F));
                } else if (ch < 0x10000) {
                    m_oscString += static_cast<char>(0xE0 | (ch >> 12));
                    m_oscString += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                    m_oscString += static_cast<char>(0x80 | (ch & 0x3F));
                } else {
                    m_oscString += static_cast<char>(0xF0 | (ch >> 18));
                    m_oscString += static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
                    m_oscString += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                    m_oscString += static_cast<char>(0x80 | (ch & 0x3F));
                }
            }
        }
        break;

    case DcsString:
        if (ch == 0x9C || ch == 0x07) {
            VtAction a;
            a.type = VtAction::DcsEnd;
            a.oscString = m_dcsString; // Reuse oscString field for payload
            m_callback(a);
            transition(Ground);
        } else if (ch == 0x1B) {
            VtAction a;
            a.type = VtAction::DcsEnd;
            a.oscString = m_dcsString;
            m_callback(a);
            transition(Escape);
        } else {
            if (m_dcsString.size() < 10 * 1024 * 1024) // 10MB cap
                m_dcsString += static_cast<char>(ch);
        }
        break;

    case ApcString:
        if (ch == 0x9C || ch == 0x07) {
            VtAction a;
            a.type = VtAction::ApcEnd;
            a.oscString = m_apcString; // Reuse oscString field for payload
            m_callback(a);
            transition(Ground);
        } else if (ch == 0x1B) {
            VtAction a;
            a.type = VtAction::ApcEnd;
            a.oscString = m_apcString;
            m_callback(a);
            transition(Escape);
        } else {
            if (m_apcString.size() < 10 * 1024 * 1024) // 10MB cap
                m_apcString += static_cast<char>(ch);
        }
        break;

    case IgnoreString:
        if (ch == 0x9C || ch == 0x07) {
            transition(Ground);
        } else if (ch == 0x1B) {
            transition(Escape);
        }
        break;
    }
}

void VtParser::transition(State newState) {
    m_state = newState;
    if (newState == CsiEntry) {
        m_params.clear();
        m_colonSep.clear();
        m_currentParam = -1;
        m_nextIsSubParam = false;
        m_intermediate.clear();
    } else if (newState == Escape) {
        m_intermediate.clear();
    }
}

