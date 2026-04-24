// Feature test: VT-parser SIMD fast-path preserves action-stream equivalence.
// See spec.md for the contract. Fails non-zero with diagnostic output if the
// SIMD scan ever diverges from the scalar path — including divergence only
// visible at specific byte-boundary alignments between safe-ASCII runs and
// the first interesting byte.

#include "vtparser.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

// Collect every VtAction the parser emits into a flat vector so two feeds
// can be compared byte-for-byte.
//
// Since 0.7.17, the parser's SIMD fast path emits printable-ASCII runs as a
// single `Print` action carrying `printRun` + `printRunLen`. The byte-by-
// byte feed strategy (one-byte feeds) goes through the scalar path and
// emits one `Print` per byte with `codepoint` set. To keep the equivalence
// contract intact across both strategies we canonicalize runs back into
// per-byte `Print` actions at sink time — the *semantic* action stream is
// unchanged, only the encoding differs.
struct Sink {
    std::vector<VtAction> actions;
    void operator()(const VtAction &a) {
        if (a.type == VtAction::Print && a.printRun != nullptr) {
            for (int i = 0; i < a.printRunLen; ++i) {
                VtAction b;
                b.type = VtAction::Print;
                b.codepoint = static_cast<uint8_t>(a.printRun[i]);
                actions.push_back(b);
            }
            return;
        }
        actions.push_back(a);
    }
};

// Deep equality. VtAction's default operator== doesn't exist, so we spell
// out every field. Missing a field here would give us a false-pass — keep
// in sync with vtparser.h.
bool actionsEqual(const VtAction &a, const VtAction &b) {
    return a.type == b.type
        && a.codepoint == b.codepoint
        && a.controlChar == b.controlChar
        && a.finalChar == b.finalChar
        && a.params == b.params
        && a.colonSep == b.colonSep
        && a.intermediate == b.intermediate
        && a.oscString == b.oscString;
}

const char *actionTypeName(VtAction::Type t) {
    switch (t) {
    case VtAction::Print:       return "Print";
    case VtAction::Execute:     return "Execute";
    case VtAction::CsiDispatch: return "CsiDispatch";
    case VtAction::EscDispatch: return "EscDispatch";
    case VtAction::OscEnd:      return "OscEnd";
    case VtAction::DcsEnd:      return "DcsEnd";
    case VtAction::ApcEnd:      return "ApcEnd";
    }
    return "?";
}

void dumpAction(const VtAction &a, const char *tag) {
    std::fprintf(stderr, "  %s: type=%s cp=0x%x ctrl=0x%x final='%c'(%d) "
                         "params=%zu inter='%s' osc_size=%zu\n",
        tag, actionTypeName(a.type),
        a.codepoint, static_cast<unsigned>(static_cast<uint8_t>(a.controlChar)),
        (a.finalChar >= 0x20 && a.finalChar <= 0x7E) ? a.finalChar : '?',
        static_cast<int>(a.finalChar),
        a.params.size(), a.intermediate.c_str(), a.oscString.size());
}

bool streamsEqual(const std::vector<VtAction> &expected,
                  const std::vector<VtAction> &actual,
                  const std::string &caseName,
                  const std::string &strategy) {
    if (expected.size() != actual.size()) {
        std::fprintf(stderr,
            "FAIL [%s / %s]: action count mismatch "
            "(expected %zu, got %zu)\n",
            caseName.c_str(), strategy.c_str(),
            expected.size(), actual.size());
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (!actionsEqual(expected[i], actual[i])) {
            std::fprintf(stderr,
                "FAIL [%s / %s]: action %zu diverges\n",
                caseName.c_str(), strategy.c_str(), i);
            dumpAction(expected[i], "  expected");
            dumpAction(actual[i],   "  actual  ");
            return false;
        }
    }
    return true;
}

// Run input through a parser, one chunk at a time; return emitted actions.
std::vector<VtAction> parse(const std::vector<std::uint8_t> &input,
                            const std::vector<std::size_t> &chunkSizes) {
    Sink sink;
    VtParser p([&](const VtAction &a) { sink(a); });
    std::size_t off = 0;
    for (std::size_t cs : chunkSizes) {
        if (off + cs > input.size()) cs = input.size() - off;
        if (cs == 0) break;
        p.feed(reinterpret_cast<const char *>(input.data() + off),
               static_cast<int>(cs));
        off += cs;
    }
    return sink.actions;
}

// Convenience: feed the whole buffer in one call.
std::vector<VtAction> parseWhole(const std::vector<std::uint8_t> &input) {
    return parse(input, {input.size()});
}

// Feed one byte at a time — effectively disables the SIMD fast path
// because each feed() call starts with at most 1 byte of safe ASCII.
std::vector<VtAction> parseByByte(const std::vector<std::uint8_t> &input) {
    std::vector<std::size_t> chunks(input.size(), 1);
    return parse(input, chunks);
}

// Feed in pseudo-random 1..17-byte chunks to exercise boundary alignment
// between SIMD 16-byte lanes and the scalar fallback.
std::vector<VtAction> parseChunked(const std::vector<std::uint8_t> &input,
                                   std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<std::size_t> dist(1, 17);
    std::vector<std::size_t> chunks;
    std::size_t remaining = input.size();
    while (remaining > 0) {
        std::size_t c = std::min(dist(rng), remaining);
        chunks.push_back(c);
        remaining -= c;
    }
    return parse(input, chunks);
}

struct Case {
    std::string name;
    std::vector<std::uint8_t> input;
};

Case makePlainAscii() {
    std::vector<std::uint8_t> buf;
    // 4 KiB of printable ASCII — well beyond SSE2's 16-byte lane and
    // exercises the hot loop many times.
    std::mt19937 rng(0xA17CAFE);
    std::uniform_int_distribution<int> dist(0x20, 0x7E);
    for (int i = 0; i < 4096; ++i) buf.push_back(static_cast<std::uint8_t>(dist(rng)));
    return {"plain_ascii_4k", std::move(buf)};
}

Case makeAsciiWithCsiAtEveryOffset(std::size_t offset) {
    // Start with safe bytes, plant a CSI sequence at `offset`, resume safe
    // bytes. Verifies the SIMD scan lands on the first interesting byte
    // at every offset mod 16 (the SSE2 lane width).
    std::vector<std::uint8_t> buf;
    for (std::size_t i = 0; i < offset; ++i) buf.push_back('A');
    // ESC [ 3 1 m A B C ESC [ 0 m
    const char *seq = "\x1b[31mABC\x1b[0m";
    for (const char *p = seq; *p; ++p) buf.push_back(static_cast<std::uint8_t>(*p));
    for (std::size_t i = 0; i < 48; ++i) buf.push_back('Z');
    return {"csi_at_offset_" + std::to_string(offset), std::move(buf)};
}

Case makeUtf8Mixed() {
    // Printable ASCII, then a multibyte UTF-8 codepoint, then more ASCII.
    // 好 = U+597D = 0xE5 0xA5 0xBD.  é = U+00E9 = 0xC3 0xA9.
    std::vector<std::uint8_t> buf;
    for (int i = 0; i < 30; ++i) buf.push_back('a' + (i % 26));
    const std::uint8_t utf8[] = {0xE5, 0xA5, 0xBD, 'x', 'y', 'z',
                                 0xC3, 0xA9, '!', '!'};
    for (auto c : utf8) buf.push_back(c);
    for (int i = 0; i < 30; ++i) buf.push_back('A' + (i % 26));
    return {"utf8_mixed", std::move(buf)};
}

Case makeControlChars() {
    // Embed BEL, BS, HT, LF, CR, DEL into an ASCII stream. Each should
    // hit the scalar path, and the SIMD scan must stop exactly at each.
    std::vector<std::uint8_t> buf;
    for (int i = 0; i < 20; ++i) buf.push_back('x');
    buf.push_back(0x07);  // BEL
    for (int i = 0; i < 5; ++i) buf.push_back('y');
    buf.push_back(0x08);  // BS
    buf.push_back(0x09);  // HT
    buf.push_back(0x0A);  // LF
    buf.push_back(0x0D);  // CR
    for (int i = 0; i < 17; ++i) buf.push_back('z');
    buf.push_back(0x7F);  // DEL — Ground ignores but scan must stop
    for (int i = 0; i < 17; ++i) buf.push_back('w');
    return {"control_chars", std::move(buf)};
}

Case makeOscSequence() {
    // OSC 8 hyperlink + window title. OSC payload is not safe-ASCII-
    // scanned (we're in OscString state, not Ground), so the contract
    // here is simply that emitted actions match between feed strategies.
    std::vector<std::uint8_t> buf;
    for (int i = 0; i < 40; ++i) buf.push_back('p');
    const char *osc = "\x1b]8;;https://example.com\x1b\\"
                      "link\x1b]8;;\x1b\\"
                      "\x1b]0;window title\x07"
                      "done";
    for (const char *p = osc; *p; ++p) buf.push_back(static_cast<std::uint8_t>(*p));
    return {"osc_sequence", std::move(buf)};
}

Case makeLongAsciiThenEsc() {
    // A full SSE2 lane of safe bytes, then ESC exactly at a lane
    // boundary. Regression guard for "SIMD scan ate the escape byte".
    std::vector<std::uint8_t> buf;
    for (int i = 0; i < 16; ++i) buf.push_back('K');
    buf.push_back(0x1B);  // ESC at offset 16 — start of second lane
    buf.push_back('[');
    buf.push_back('H');
    for (int i = 0; i < 32; ++i) buf.push_back('L');
    return {"long_ascii_then_esc_at_lane_boundary", std::move(buf)};
}

Case makeMixed() {
    // Stress case: 8 KiB of pseudo-random content pulled from a wide
    // byte distribution — ASCII-heavy with ~5% C0, ~10% high-bit, and
    // deliberately planted escape sequences.
    std::mt19937 rng(0xDEADBEEF);
    std::uniform_int_distribution<int> kind(0, 99);
    std::uniform_int_distribution<int> ascii(0x20, 0x7E);
    std::uniform_int_distribution<int> c0(0x00, 0x1F);
    std::uniform_int_distribution<int> hi(0x80, 0xFF);
    std::vector<std::uint8_t> buf;
    for (int i = 0; i < 8192; ++i) {
        int k = kind(rng);
        if (k < 5)       buf.push_back(static_cast<std::uint8_t>(c0(rng)));
        else if (k < 15) buf.push_back(static_cast<std::uint8_t>(hi(rng)));
        else             buf.push_back(static_cast<std::uint8_t>(ascii(rng)));
    }
    // Plant three complete CSI sequences at distinct offsets.
    auto plant = [&](std::size_t off, const char *seq) {
        for (std::size_t j = 0; seq[j] && off + j < buf.size(); ++j) {
            buf[off + j] = static_cast<std::uint8_t>(seq[j]);
        }
    };
    plant(100, "\x1b[2J");
    plant(2000, "\x1b[1;1H");
    plant(5000, "\x1b[38;2;255;128;0mX");
    return {"mixed_8k", std::move(buf)};
}

std::vector<Case> buildCorpus() {
    std::vector<Case> cases;
    cases.push_back(makePlainAscii());
    cases.push_back(makeUtf8Mixed());
    cases.push_back(makeControlChars());
    cases.push_back(makeOscSequence());
    cases.push_back(makeLongAsciiThenEsc());
    cases.push_back(makeMixed());
    // Boundary-alignment sweep: interesting byte at every offset 0..31.
    for (std::size_t off = 0; off < 32; ++off) {
        cases.push_back(makeAsciiWithCsiAtEveryOffset(off));
    }
    return cases;
}

} // namespace

int main() {
    int failures = 0;
    auto cases = buildCorpus();

    for (const auto &c : cases) {
        // Reference action stream: byte-by-byte feed. This hits only the
        // scalar path of feed(), so it's the pre-SIMD parser's behavior.
        const auto reference = parseByByte(c.input);

        // Strategy 1: single whole-buffer feed. Exercises the SIMD path
        // most aggressively (longest contiguous scans).
        const auto whole = parseWhole(c.input);
        if (!streamsEqual(reference, whole, c.name, "whole")) ++failures;

        // Strategy 2: pseudo-random chunking across multiple seeds.
        for (std::uint64_t seed : {0x1ULL, 0x2ULL, 0xFEEDFACEULL}) {
            const auto chunked = parseChunked(c.input, seed);
            if (!streamsEqual(reference, chunked, c.name,
                              "chunked/seed=" + std::to_string(seed))) {
                ++failures;
            }
        }
    }

    if (failures == 0) {
        std::printf("vtparser_simd_scan: all %zu cases pass across 4 feed strategies\n",
                    cases.size());
        return 0;
    }
    std::fprintf(stderr, "vtparser_simd_scan: %d divergence(s)\n", failures);
    return 1;
}
