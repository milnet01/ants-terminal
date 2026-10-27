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
    // Signed boundary constants for the [0x20..0x7E] safe-ASCII range after
    // XOR-with-0x80: 0x20-0x80 = -96, 0x7E-0x80 = -2.
    constexpr char kSafeMinSigned = -96;
    constexpr char kSafeMaxSigned = -2;
    const __m128i kSignFlip = _mm_set1_epi8(static_cast<char>(0x80));
    const __m128i kLowBound = _mm_set1_epi8(kSafeMinSigned);
    const __m128i kHighBound = _mm_set1_epi8(kSafeMaxSigned);
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
            if (run > 0) {
                // Emit the whole safe-ASCII run as a single Print action
                // carrying a pointer+length into the caller's buffer. Saves
                // `run - 1` VtAction constructions (and the attendant
                // vector<int> / vector<bool> / string zero-init per action)
                // plus `run - 1` std::function dispatches.
                VtAction a;
                a.type = VtAction::Print;
                a.printRun = data + i;
                a.printRunLen = static_cast<int>(run);
                m_callback(a);
                i += static_cast<int>(run);
                if (i >= length) break;
            }
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
                processChar(valid ? m_utf8Accum : 0xFFFD);
            }
            return;
        }
        // Invalid continuation — reset and process this byte
        m_utf8Remaining = 0;
        processChar(0xFFFD);
    }

    if (byte <= 0x9F) {
        // ASCII, or an 8-bit C1 control (0x80-0x9F). UTF-8 encodes C1 as a 2-byte
        // sequence (0xC2 0x80..0xC2 0x9F), so raw 0x80-0x9F here means
        // either a legacy 8-bit-C1-aware producer or a malformed UTF-8
        // stream. ECMA-48 § 5.3 and the Williams VT500 state machine
        // treat C1 bytes as first-class controls; route them through
        // processChar() rather than substituting U+FFFD, so 0x9C (ST)
        // reaches the string states' terminator branches.
        processChar(byte);
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
        processChar(0xFFFD);
    }
}

namespace {
// No real parameter comes near this, and the clamp keeps `* 10 + digit` from
// ever overflowing an int.
constexpr int kMaxParamValue = 16384;
// ANTS-1827 — more parameters than this is a DoS shape, not a sequence.
constexpr size_t kMaxParams = 32;
// Room for a large inline image; caps what a hostile stream can make the
// parser hold.
constexpr size_t kMaxStringBytes = size_t{10} * 1024 * 1024;
} // namespace

// Called on an accumulator just moved into an End action. The standard does
// not promise a moved-from string gives up its buffer, so this makes sure a
// 10 MiB image payload does not stay resident for the parser's lifetime.
static void releaseIfLarge(std::string &s) {
    constexpr size_t kShrinkThreshold = 64 * 1024;   // 64 KB
    if (s.capacity() > kShrinkThreshold) {
        std::string().swap(s);
    } else {
        s.clear();
    }
}

bool VtParser::appendUtf8(std::string &out, uint32_t ch, size_t maxBytes) {
    // Compute encoded length first so we can reject atomically without a
    // partial write that would overshoot maxBytes by up to 3 bytes.
    size_t need;
    if (ch < 0x80)        need = 1;
    else if (ch < 0x800)  need = 2;
    else if (ch < 0x10000) need = 3;
    else                  need = 4;

    if (out.size() + need > maxBytes) return false;

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
    return true;
}

void VtParser::processChar(uint32_t ch) {
    // C0 controls are handled in almost every state
    if (ch < 0x20 || ch == 0x7F) {
        switch (ch) {
        case 0x1B: // ESC
            // The string states handle ESC themselves: they dispatch
            // their End action, and the *Esc peek state consumes the next
            // byte so a hostile payload cannot end in a live ESC sequence
            // such as RIS. See tests/features/vt_osc_esc_discard/spec.md.
            if (m_state == OscString || m_state == DcsString ||
                m_state == ApcString || m_state == IgnoreString) {
                break;  // exit the C0 switch — per-state switch handles it
            }
            transition(Escape);
            return;
        case 0x18: // CAN
        case 0x1A: // SUB
            // An aborted string never reaches endString, so its accumulator
            // (up to kMaxStringBytes) is released here instead of staying
            // resident for the parser's lifetime.
            if (m_state == OscString) releaseIfLarge(m_oscString);
            else if (m_state == DcsString) releaseIfLarge(m_dcsString);
            else if (m_state == ApcString) releaseIfLarge(m_apcString);
            transition(Ground);
            return;
        default:
            break;
        }
    }

    switch (m_state) {
    case Ground:
        if (ch < 0x20) {
            emitExecute(ch);
        } else if (ch == 0x7F) {
            // DEL — ignore
        } else if (ch >= 0x80 && ch <= 0x9F) {
            // 8-bit C1 controls (ECMA-48 § 5.3). Each is the one-byte form of
            // ESC followed by (byte - 0x40): 0x9B is CSI (ESC [), 0x9D is OSC
            // (ESC ]), 0x90 DCS, 0x9F APC, 0x98 SOS, 0x9E PM.
            if (ch == 0x9B)
                transition(CsiEntry);
            else if (!reArmStringIntroducer(ch - 0x40))
                emitExecute(ch);  // IND, NEL, HTS, RI, SS2, SS3, …
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
        } else if (ch >= 0x20 && ch <= 0x2F) {
            m_intermediate.clear();
            m_intermediate += static_cast<char>(ch);
            transition(EscapeIntermediate);
        } else if (ch >= 0x30 && ch <= 0x7E) {
            // A final byte, unless it opens a string (] P _ X ^).
            if (!reArmStringIntroducer(ch))
                dispatchEsc(ch);
        } else if (ch < 0x20) {
            emitExecute(ch);
        } else {
            // ANTS-1998 — catch-all for the bytes none of the branches above
            // match: 0x7F (DEL) and decoded codepoints > 0x7E. Without this the
            // parser stayed in Escape and mis-consumed the *next* byte as the
            // ESC final. Aborting to Ground matches EscapeIntermediate's
            // existing treatment of the same bytes (its trailing else).
            transition(Ground);
        }
        break;

    case EscapeIntermediate:
        if (ch >= 0x20 && ch <= 0x2F) {
            if (m_intermediate.size() < 8) // Cap intermediate bytes (real sequences use 1-2)
                m_intermediate += static_cast<char>(ch);
        } else if (ch >= 0x30 && ch <= 0x7E) {
            dispatchEsc(ch);
        } else if (ch < 0x20) {
            // ECMA-48 / Williams parser table: a C0 control inside
            // `esc_intermediate` Executes and stays in esc_intermediate
            // (the ESC sequence is NOT aborted) — same rule as the
            // CsiIntermediate branch.
            emitExecute(ch);
        } else {
            transition(Ground);
        }
        break;

    case CsiEntry:
    case CsiParam:
    case CsiIntermediate:
    case CsiIgnore:
        processCsi(ch);
        break;

    case OscString:
    case OscStringEsc:
    case DcsString:
    case DcsStringEsc:
    case ApcString:
    case ApcStringEsc:
    case IgnoreString:
    case IgnoreStringEsc:
        processString(ch);
        break;
    }
}

void VtParser::processCsi(uint32_t ch) {
    switch (m_state) {
    case CsiEntry:
        // transition(CsiEntry) has already reset what a CSI collects.
        m_state = CsiParam;
        if (ch >= 0x3C && ch <= 0x3F)
            m_intermediate += static_cast<char>(ch);  // private prefix: ? > = <
        else
            processCsi(ch);  // a parameter, intermediate or final byte
        break;

    case CsiParam:
        if (ch >= '0' && ch <= '9') {
            if (m_currentParam < 0) m_currentParam = 0;
            m_currentParam = m_currentParam * 10 + (ch - '0');
            if (m_currentParam > kMaxParamValue) m_currentParam = kMaxParamValue;
        } else if (ch == ';') {
            pushParam(m_currentParam < 0 ? 0 : m_currentParam);
            m_currentParam = -1;
            m_nextIsSubParam = false;
        } else if (ch >= 0x20 && ch <= 0x2F) {
            // Intermediate byte
            if (m_currentParam >= 0) {
                pushParam(m_currentParam);
                m_currentParam = -1;
                m_nextIsSubParam = false;
            }
            m_intermediate += static_cast<char>(ch);
            m_state = CsiIntermediate;
        } else if (ch >= 0x40 && ch <= 0x7E) {
            // Final byte — dispatch
            if (m_currentParam >= 0) pushParam(m_currentParam);
            m_nextIsSubParam = false;
            dispatchCsi(ch);
        } else if (ch == ':') {
            // Sub-parameter separator (used in SGR colon syntax like 4:3 for curly underline)
            pushParam(m_currentParam < 0 ? 0 : m_currentParam);
            m_currentParam = -1;
            m_nextIsSubParam = true;  // Mark next param as a colon sub-parameter
        } else if (ch < 0x20) {
            emitExecute(ch);
        } else if (ch == 0x7F) {
            // ANTS-5075 — DEL is ignored in every CSI state (ECMA-48 /
            // Williams parser table).
        } else if (ch >= 0x3C && ch <= 0x3F) {
            // ANTS-5075 — a private-marker byte after parameters makes the
            // sequence malformed: swallow it up to its final byte.
            m_state = CsiIgnore;
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
            dispatchCsi(ch);
        } else if (ch < 0x20) {
            // ECMA-48 / Williams parser table: a C0 control inside
            // `csi_intermediate` Executes and stays in csi_intermediate
            // (the CSI is NOT aborted), so `CSI ! <BEL> p` is still DECSTR.
            emitExecute(ch);
        } else if (ch == 0x7F) {
            // ANTS-5075 — DEL ignored, as in CsiParam.
        } else if (ch >= 0x30 && ch <= 0x3F) {
            // ANTS-5075 — a parameter byte after an intermediate is malformed.
            m_state = CsiIgnore;
        } else {
            transition(Ground);
        }
        break;

    case CsiIgnore:
        // ANTS-5075 — csi_ignore: C0 controls still execute, 0x20-0x3F and
        // DEL are consumed, a final byte ends the sequence with no dispatch.
        if (ch < 0x20)
            emitExecute(ch);
        else if (ch >= 0x40 && ch != 0x7F)
            transition(Ground);
        break;

    default:
        break;
    }
}

void VtParser::processString(uint32_t ch) {
    // BEL or ST ends a string and returns to Ground. ESC ends it too, whatever
    // follows (xterm semantics): the 7-bit ST is ESC + backslash, and after
    // any other byte the string still ends. The *Esc peek state then consumes
    // that byte, never dispatching it, so a string ending in `ESC c` cannot
    // trigger RIS.
    switch (m_state) {
    case OscString:
        if (ch == 0x07 || ch == 0x9C)
            endString(VtAction::OscEnd, m_oscString, Ground);
        else if (ch == 0x1B)
            endString(VtAction::OscEnd, m_oscString, OscStringEsc);
        else if (!appendUtf8(m_oscString, ch, kMaxStringBytes))
            m_stringTruncated = true;
        break;

    case DcsString:
        if (ch == 0x07 || ch == 0x9C)
            endString(VtAction::DcsEnd, m_dcsString, Ground);
        else if (ch == 0x1B)
            endString(VtAction::DcsEnd, m_dcsString, DcsStringEsc);
        else if (!appendUtf8(m_dcsString, ch, kMaxStringBytes))
            m_stringTruncated = true;
        break;

    case ApcString:
        if (ch == 0x07 || ch == 0x9C)
            endString(VtAction::ApcEnd, m_apcString, Ground);
        else if (ch == 0x1B)
            endString(VtAction::ApcEnd, m_apcString, ApcStringEsc);
        else if (!appendUtf8(m_apcString, ch, kMaxStringBytes))
            m_stringTruncated = true;
        break;

    case IgnoreString:
        // SOS, PM — the body is consumed and dropped.
        if (ch == 0x07 || ch == 0x9C)
            transition(Ground);
        else if (ch == 0x1B)
            transition(IgnoreStringEsc);
        break;

    case OscStringEsc:
    case DcsStringEsc:
    case ApcStringEsc:
    case IgnoreStringEsc:
        // The byte after a string-ending ESC. ANTS-1777: a string introducer
        // (] P _ X ^) re-arms a fresh string, so back-to-back OSC, Sixel and
        // Kitty sequences survive. Every other byte — the dangerous ESC
        // finals `c`=RIS, `D`, `7` …, and the CSI introducer `[` — is
        // discarded to Ground without dispatch. See docs/specs/ANTS-1777.md
        // and tests/features/vt_osc_esc_discard/spec.md.
        if (!reArmStringIntroducer(ch))
            transition(Ground);
        break;

    default:
        break;
    }
}

void VtParser::emitExecute(uint32_t ch) {
    VtAction a;
    a.type = VtAction::Execute;
    a.controlChar = static_cast<char>(ch);
    m_callback(a);
}

void VtParser::dispatchEsc(uint32_t ch) {
    VtAction a;
    a.type = VtAction::EscDispatch;
    a.finalChar = static_cast<char>(ch);
    a.intermediate = m_intermediate;
    m_callback(a);
    transition(Ground);
}

void VtParser::dispatchCsi(uint32_t ch) {
    VtAction a;
    a.type = VtAction::CsiDispatch;
    a.finalChar = static_cast<char>(ch);
    a.params = m_params;
    a.colonSep = m_colonSep;
    a.intermediate = m_intermediate;
    a.paramsTruncated = m_paramsTruncated;
    m_callback(a);
    transition(Ground);
}

void VtParser::pushParam(int value) {
    if (m_params.size() < kMaxParams) {
        m_params.push_back(value);
        m_colonSep.push_back(m_nextIsSubParam);
    } else {
        m_paramsTruncated = true;
    }
}

void VtParser::endString(VtAction::Type type, std::string &buf, State next) {
    VtAction a;
    a.type = type;
    a.payload = std::move(buf);
    a.truncated = m_stringTruncated;
    m_callback(a);
    releaseIfLarge(buf);
    transition(next);
}

bool VtParser::reArmStringIntroducer(uint32_t ch) {
    // Any byte not listed here is NOT a string introducer, so `c`=RIS,
    // `D`=IND, the CSI introducer `[`, intermediates and C0 never re-arm
    // from a peek state. ANTS-1777.
    switch (ch) {
    case ']': m_oscString.clear(); transition(OscString); return true;
    case 'P': m_dcsString.clear(); transition(DcsString); return true;
    case '_': m_apcString.clear(); transition(ApcString); return true;
    case 'X':   // SOS
    case '^':   // PM
        transition(IgnoreString); return true;
    default:
        return false;
    }
}

void VtParser::transition(State newState) {
    m_state = newState;
    if (newState == CsiEntry) {
        m_params.clear();
        m_colonSep.clear();
        m_currentParam = -1;
        m_nextIsSubParam = false;
        m_paramsTruncated = false;  // ANTS-1827 — fresh CSI, clear carry-over
        m_intermediate.clear();
    } else if (newState == Escape) {
        m_intermediate.clear();
    } else if (newState == OscString || newState == DcsString ||
               newState == ApcString) {
        // Fresh string sequence — clear the carried-over truncation flag so it
        // reflects only this payload. ANTS-1663.
        m_stringTruncated = false;
    }
}

