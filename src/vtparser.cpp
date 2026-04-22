#include "vtparser.h"

#include <cstddef>

#if defined(__SSE2__)
#  include <emmintrin.h>
#  define ANTS_VTPARSER_HAS_SSE2 1
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
#  include <arm_neon.h>
#  define ANTS_VTPARSER_HAS_NEON 1
#endif

namespace {

// Scan for the first byte in [data, data+len) that the state machine would
// need to treat as special when consumed in Ground state with no pending
// UTF-8 continuation: any C0 control (<0x20), DEL (0x7F), or high-bit byte
// (≥0x80, which starts a multi-byte UTF-8 sequence). A byte in the
// printable-ASCII range [0x20..0x7E] is *safe* — in Ground state it is
// emitted verbatim as a Print action with the byte as its codepoint, and
// the state machine never changes.
//
// Returns the index of the first non-safe byte, or `len` if the entire span
// is safe. The scan is the SIMD hot path for TUI-repaint workloads where a
// single PTY read can be thousands of printable-ASCII bytes punctuated by
// a few escape sequences.
//
// Correctness: this function MUST return the exact same index as the
// byte-at-a-time scalar fallback. The fast-path in feed() relies on that
// equivalence to emit Print actions for the safe run without running the
// full state machine. The feature test at tests/features/vtparser_simd_scan/
// asserts action-stream equivalence against the scalar path.
static std::size_t scanSafeAsciiRun(const std::uint8_t *data, std::size_t len) noexcept {
    std::size_t i = 0;

#if defined(ANTS_VTPARSER_HAS_SSE2)
    // Signed-compare trick: XOR with 0x80 maps unsigned bytes into a signed
    // space where the safe range [0x20..0x7E] becomes [-96..-2]. Anything
    // outside is either < -96 (C0 controls) or > -2 (0x7F + all high-bit
    // bytes). Two signed cmpgt + or + movemask flags any interesting byte
    // in a 16-byte chunk.
    const __m128i kSignFlip = _mm_set1_epi8(static_cast<char>(0x80));
    const __m128i kLowBound = _mm_set1_epi8(static_cast<char>(-96));  // 0x20 - 0x80
    const __m128i kHighBound = _mm_set1_epi8(static_cast<char>(-2));  // 0x7E - 0x80
    while (i + 16 <= len) {
        __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
        __m128i s = _mm_xor_si128(b, kSignFlip);
        __m128i lowMask = _mm_cmpgt_epi8(kLowBound, s);   // s < -96  ↔ original < 0x20
        __m128i highMask = _mm_cmpgt_epi8(s, kHighBound); // s > -2   ↔ original >= 0x7F
        __m128i interesting = _mm_or_si128(lowMask, highMask);
        int mask = _mm_movemask_epi8(interesting);
        if (mask != 0) {
            return i + static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>(mask)));
        }
        i += 16;
    }
#elif defined(ANTS_VTPARSER_HAS_NEON)
    const uint8x16_t kLowBound = vdupq_n_u8(0x20);
    const uint8x16_t kHighBound = vdupq_n_u8(0x7E);
    while (i + 16 <= len) {
        uint8x16_t b = vld1q_u8(data + i);
        uint8x16_t lowMask = vcltq_u8(b, kLowBound);   // b < 0x20
        uint8x16_t highMask = vcgtq_u8(b, kHighBound); // b > 0x7E  (covers 0x7F + 0x80+)
        uint8x16_t interesting = vorrq_u8(lowMask, highMask);
        // Reduce to two 64-bit lanes; a byte is interesting iff its lane is 0xFF.
        uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(interesting), 0);
        uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(interesting), 1);
        if (lo != 0) {
            return i + static_cast<std::size_t>(__builtin_ctzll(lo) / 8);
        }
        if (hi != 0) {
            return i + 8 + static_cast<std::size_t>(__builtin_ctzll(hi) / 8);
        }
        i += 16;
    }
#endif

    // Scalar tail (also serves as the full fallback on architectures without
    // SSE2 or NEON). Compilers typically auto-vectorize this well enough that
    // the scalar-only path is still fast, but we keep the explicit SIMD above
    // because auto-vectorization varies across -O levels and compilers.
    while (i < len) {
        std::uint8_t b = data[i];
        if (b < 0x20 || b >= 0x7F) return i;
        ++i;
    }
    return len;
}

} // namespace

VtParser::VtParser(ActionCallback callback)
    : m_callback(std::move(callback)) {}

void VtParser::feed(const char *data, int length) {
    int i = 0;
    while (i < length) {
        // Fast path: Ground state with no pending UTF-8 continuation is the
        // dominant case during TUI repaints. Skip the per-byte state machine
        // for runs of printable ASCII by scanning with SIMD, then emit the
        // runs as Print actions directly. The scalar fallback below handles
        // every other byte one at a time.
        if (m_state == Ground && m_utf8Remaining == 0) {
            std::size_t run = scanSafeAsciiRun(
                reinterpret_cast<const std::uint8_t *>(data + i),
                static_cast<std::size_t>(length - i));
            for (std::size_t k = 0; k < run; ++k) {
                VtAction a;
                a.type = VtAction::Print;
                a.codepoint = static_cast<std::uint8_t>(data[i + static_cast<int>(k)]);
                m_callback(a);
            }
            i += static_cast<int>(run);
            if (i >= length) break;
        }
        feedByte(static_cast<std::uint8_t>(data[i]));
        ++i;
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

// After a large OSC/DCS/APC sequence dispatches, std::string::clear() keeps
// the underlying capacity. An attacker who streamed a ~10 MB inline image
// would otherwise leave three 10 MB buffers resident for the lifetime of
// the parser (30 MB permanent overhead per terminal). Swap against an empty
// string to actually release memory when the buffer grew past the
// "one legitimate notification fits in this much" threshold.
static void releaseIfLarge(std::string &s) {
    constexpr size_t kShrinkThreshold = 64 * 1024;   // 64 KB
    if (s.capacity() > kShrinkThreshold) {
        std::string().swap(s);
    } else {
        s.clear();
    }
}

void VtParser::appendUtf8(std::string &out, uint32_t ch, size_t maxBytes) {
    // Compute encoded length first so we can reject atomically without a
    // partial write that would overshoot maxBytes by up to 3 bytes.
    size_t need;
    if (ch < 0x80)        need = 1;
    else if (ch < 0x800)  need = 2;
    else if (ch < 0x10000) need = 3;
    else                  need = 4;

    if (out.size() + need > maxBytes) return;

    if (ch < 0x80) {
        out += static_cast<char>(ch);
    } else if (ch < 0x800) {
        out += static_cast<char>(0xC0 | (ch >> 6));
        out += static_cast<char>(0x80 | (ch & 0x3F));
    } else if (ch < 0x10000) {
        out += static_cast<char>(0xE0 | (ch >> 12));
        out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (ch & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (ch >> 18));
        out += static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (ch & 0x3F));
    }
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
            if (m_intermediate.size() < 8) // Cap intermediate bytes (real sequences use 1-2)
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
            if (m_intermediate.size() < 8) // Cap intermediate bytes
                m_intermediate += static_cast<char>(ch);
        } else if (ch >= 0x40 && ch <= 0x7E) {
            VtAction a;
            a.type = VtAction::CsiDispatch;
            a.finalChar = static_cast<char>(ch);
            a.params = m_params;
            a.intermediate = m_intermediate;
            a.colonSep = m_colonSep;
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
            a.oscString = std::move(m_oscString);
            m_callback(a);
            releaseIfLarge(m_oscString);
            transition(Ground);
        } else if (ch == 0x1B) {
            // Might be ESC \ (ST)
            // Peek: we handle this by checking next char
            // For simplicity, end the OSC here
            VtAction a;
            a.type = VtAction::OscEnd;
            a.oscString = std::move(m_oscString);
            m_callback(a);
            releaseIfLarge(m_oscString);
            transition(Escape);
        } else {
            appendUtf8(m_oscString, ch, 10 * 1024 * 1024); // 10MB cap for inline images
        }
        break;

    case DcsString:
        if (ch == 0x9C || ch == 0x07) {
            VtAction a;
            a.type = VtAction::DcsEnd;
            a.oscString = std::move(m_dcsString); // Reuse oscString field for payload
            m_callback(a);
            releaseIfLarge(m_dcsString);
            transition(Ground);
        } else if (ch == 0x1B) {
            VtAction a;
            a.type = VtAction::DcsEnd;
            a.oscString = std::move(m_dcsString);
            m_callback(a);
            releaseIfLarge(m_dcsString);
            transition(Escape);
        } else {
            appendUtf8(m_dcsString, ch, 10 * 1024 * 1024); // 10MB cap
        }
        break;

    case ApcString:
        if (ch == 0x9C || ch == 0x07) {
            VtAction a;
            a.type = VtAction::ApcEnd;
            a.oscString = std::move(m_apcString); // Reuse oscString field for payload
            m_callback(a);
            releaseIfLarge(m_apcString);
            transition(Ground);
        } else if (ch == 0x1B) {
            VtAction a;
            a.type = VtAction::ApcEnd;
            a.oscString = std::move(m_apcString);
            m_callback(a);
            releaseIfLarge(m_apcString);
            transition(Escape);
        } else {
            appendUtf8(m_apcString, ch, 10 * 1024 * 1024); // 10MB cap
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

