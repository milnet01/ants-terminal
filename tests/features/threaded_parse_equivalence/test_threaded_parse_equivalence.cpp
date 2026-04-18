// Threaded parse equivalence — see spec.md for the contract.
//
// Pins the invariant that VtParser, fed any byte sequence, produces an
// action stream identical regardless of how the stream is chunked. The
// worker path in 0.7.0 chunks by 16 KB or 4096-action flush thresholds;
// if any of those thresholds breaks the parser's state, the chunked
// path will diverge from the single-shot reference here.

#include "vtparser.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

struct Fixture {
    const char *name;
    std::string bytes;
};

static std::vector<VtAction> parseAll(const std::string &data,
                                       const std::vector<size_t> &chunks) {
    std::vector<VtAction> out;
    VtParser parser([&out](const VtAction &a) { out.push_back(a); });
    size_t offset = 0;
    for (size_t n : chunks) {
        if (n == 0) continue;
        size_t take = std::min(n, data.size() - offset);
        parser.feed(data.data() + offset, static_cast<int>(take));
        offset += take;
        if (offset >= data.size()) break;
    }
    // Flush any remainder (in case sum(chunks) < data.size())
    if (offset < data.size()) {
        parser.feed(data.data() + offset,
                    static_cast<int>(data.size() - offset));
    }
    return out;
}

static std::vector<VtAction> parseWhole(const std::string &data) {
    return parseAll(data, { data.size() });
}

static std::vector<VtAction> parseByteByByte(const std::string &data) {
    std::vector<size_t> chunks(data.size(), 1u);
    return parseAll(data, chunks);
}

static std::vector<VtAction> parseByFixedChunks(const std::string &data,
                                                 size_t chunkSize) {
    std::vector<size_t> chunks;
    for (size_t i = 0; i < data.size(); i += chunkSize) chunks.push_back(chunkSize);
    return parseAll(data, chunks);
}

static std::vector<VtAction> parseByPseudoRandomChunks(const std::string &data,
                                                        uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> dist(1, 97);  // prime-ish cap
    std::vector<size_t> chunks;
    size_t remaining = data.size();
    while (remaining > 0) {
        size_t n = std::min<size_t>(dist(rng), remaining);
        chunks.push_back(n);
        remaining -= n;
    }
    return parseAll(data, chunks);
}

// Parse in chunks matching the VtStream worker's flush heuristic:
// 16 KB per chunk (kFlushBytes). Fixtures are small so in practice this
// yields a single chunk for most — good, it asserts the whole-shot path
// itself matches, which is the tightest possible reference.
static std::vector<VtAction> parseByWorkerFlush(const std::string &data) {
    return parseByFixedChunks(data, 16 * 1024);
}

static bool actionsEqual(const VtAction &a, const VtAction &b, std::string &why) {
    if (a.type != b.type)          { why = "type"; return false; }
    if (a.codepoint != b.codepoint){ why = "codepoint"; return false; }
    if (a.controlChar != b.controlChar) { why = "controlChar"; return false; }
    if (a.finalChar != b.finalChar){ why = "finalChar"; return false; }
    if (a.params != b.params)      { why = "params"; return false; }
    if (a.colonSep != b.colonSep)  { why = "colonSep"; return false; }
    if (a.intermediate != b.intermediate) { why = "intermediate"; return false; }
    if (a.oscString != b.oscString){ why = "oscString"; return false; }
    return true;
}

static const char *typeName(VtAction::Type t) {
    switch (t) {
        case VtAction::Print:       return "Print";
        case VtAction::Execute:     return "Execute";
        case VtAction::CsiDispatch: return "CsiDispatch";
        case VtAction::EscDispatch: return "EscDispatch";
        case VtAction::OscEnd:      return "OscEnd";
        case VtAction::DcsEnd:      return "DcsEnd";
        case VtAction::ApcEnd:      return "ApcEnd";
    }
    return "???";
}

static void dumpAction(const VtAction &a, const char *tag) {
    std::fprintf(stderr, "  %s: type=%s cp=0x%x cc=0x%02x fc=0x%02x "
                        "params={",
                 tag, typeName(a.type), a.codepoint,
                 static_cast<unsigned char>(a.controlChar),
                 static_cast<unsigned char>(a.finalChar));
    for (size_t i = 0; i < a.params.size(); ++i)
        std::fprintf(stderr, "%s%d", i ? "," : "", a.params[i]);
    std::fprintf(stderr, "} intermediate=\"%s\" osc_len=%zu\n",
                 a.intermediate.c_str(), a.oscString.size());
}

static bool compareAndReport(const char *fixture,
                              const char *label,
                              const std::vector<VtAction> &ref,
                              const std::vector<VtAction> &cand) {
    if (ref.size() != cand.size()) {
        std::fprintf(stderr,
                     "FAIL [%s / %s]: action count %zu != %zu\n",
                     fixture, label, ref.size(), cand.size());
        return false;
    }
    for (size_t i = 0; i < ref.size(); ++i) {
        std::string why;
        if (!actionsEqual(ref[i], cand[i], why)) {
            std::fprintf(stderr,
                         "FAIL [%s / %s]: action[%zu] mismatch on %s\n",
                         fixture, label, i, why.c_str());
            dumpAction(ref[i], "ref ");
            dumpAction(cand[i], "cand");
            return false;
        }
    }
    return true;
}

static std::vector<Fixture> buildFixtures() {
    std::vector<Fixture> f;

    // 1. Plain ASCII.
    f.push_back({"plain_ascii", "hello world\nline two\n"});

    // 2. Mixed ANSI.
    f.push_back({"mixed_ansi",
                 "\x1B[31mred\x1B[0m \x1B[1;32;4mbold-green-ul\x1B[0m\n"
                 "\x1B[2J\x1B[H\x1B[10;20Hcursor\x1B[?25l"});

    // 3. DCS / Sixel-ish.
    f.push_back({"dcs_sixel",
                 "\x1BPq#0;2;0;0;0#1;2;100;100;100"
                 "#0!10~$-#1!10~\x1B\\"});

    // 4. APC / Kitty-ish.
    f.push_back({"apc_kitty",
                 "\x1B_Gf=32,s=1,v=1,m=1;iVBORw0KGgo=\x1B\\"});

    // 5. Long OSC 52 payload (~4 KB).
    {
        std::string osc = "\x1B]52;c;";
        osc.reserve(4200);
        for (int i = 0; i < 4000; ++i)
            osc.push_back(static_cast<char>('A' + (i % 26)));
        osc.append("\x07");
        f.push_back({"osc52_long", osc});
    }

    // 6. UTF-8 multi-byte. A 4-byte sequence (U+1F600 = 😀) split anywhere
    //    inside its continuation bytes must survive chunking — the byte-by-
    //    byte test covers that by construction.
    f.push_back({"utf8_multibyte",
                 "hello \xE2\x98\x83 \xF0\x9F\x98\x80 \xF0\x9F\x8C\x88\n"
                 "ascii after\n"});

    // 7. Sync-output DEC 2026 begin/end.
    f.push_back({"sync_output",
                 "\x1B[?2026h\x1B[2J\x1B[HhelloSyncBody\x1B[?2026l"});

    // 8. OSC 133 markers.
    f.push_back({"osc133_markers",
                 "\x1B]133;A\x07\x1B]133;B\x07"
                 "$ true\r\n\x1B]133;C\x07"
                 "\x1B]133;D;0\x07"});

    // 9. CSI with 32 parameters.
    {
        std::string csi = "\x1B[";
        for (int i = 0; i < 32; ++i) {
            if (i) csi.push_back(';');
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", i * 3 + 1);
            csi.append(buf);
        }
        csi.append("m");
        f.push_back({"csi_32_params", csi});
    }

    // 10. Boundary fixture: 3-byte UTF-8 ✓ repeated 20000 times, ensuring
    //     some continuation-byte pair straddles any fixed chunk boundary
    //     and the 16 KB worker flush cadence.
    {
        std::string s;
        s.reserve(3 * 20000);
        for (int i = 0; i < 20000; ++i) s.append("\xE2\x9C\x93");
        f.push_back({"utf8_bulk", s});
    }

    // 11. Long Print runs spanning the SIMD fast-path boundary (already
    //     tested in vtparser_simd_scan but included here to catch any
    //     interaction between worker chunking and SIMD scan).
    {
        std::string s;
        s.reserve(50000);
        for (int i = 0; i < 50000; ++i)
            s.push_back(static_cast<char>('a' + (i % 26)));
        f.push_back({"long_print", s});
    }

    return f;
}

}  // namespace

int main() {
    const auto fixtures = buildFixtures();
    int failures = 0;

    for (const auto &fx : fixtures) {
        const auto ref = parseWhole(fx.bytes);

        // Byte-by-byte: the tightest chunking.
        if (!compareAndReport(fx.name, "byte-by-byte", ref,
                              parseByteByByte(fx.bytes))) ++failures;

        // Worker flush-sized chunks (what 0.7.0's VtStream will use).
        if (!compareAndReport(fx.name, "worker-flush", ref,
                              parseByWorkerFlush(fx.bytes))) ++failures;

        // Fixed 7-byte chunks — primes-ish boundary that straddles
        // most escape-sequence internal states.
        if (!compareAndReport(fx.name, "chunk-7", ref,
                              parseByFixedChunks(fx.bytes, 7))) ++failures;

        // Fixed 3-byte chunks — guaranteed to split UTF-8 4-byte codepoints.
        if (!compareAndReport(fx.name, "chunk-3", ref,
                              parseByFixedChunks(fx.bytes, 3))) ++failures;

        // Pseudo-random chunking with fixed seed for reproducibility.
        if (!compareAndReport(fx.name, "rand-seed-42", ref,
                              parseByPseudoRandomChunks(fx.bytes, 42))) ++failures;
        if (!compareAndReport(fx.name, "rand-seed-12345", ref,
                              parseByPseudoRandomChunks(fx.bytes, 12345))) ++failures;
    }

    if (failures) {
        std::fprintf(stderr, "\n%d assertion(s) failed\n", failures);
        return 1;
    }
    std::printf("threaded_parse_equivalence: %zu fixtures passed\n",
                fixtures.size());
    return 0;
}
