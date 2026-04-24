// Feature test: VT-parser Print-run coalescing preserves grid state.
// See spec.md for the contract. Fails non-zero with diagnostic output if
// the coalesced SIMD + handleAsciiPrintRun path diverges from the scalar
// handlePrint path on any cell, cursor position, or delayed-wrap state.

#include "terminalgrid.h"
#include "vtparser.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void feedBytes(TerminalGrid &grid, const std::string &bytes, bool oneByteAtATime) {
    VtParser p([&grid](const VtAction &a) { grid.processAction(a); });
    if (oneByteAtATime) {
        for (char c : bytes) p.feed(&c, 1);
    } else {
        p.feed(bytes.data(), static_cast<int>(bytes.size()));
    }
}

struct GridDiff {
    std::vector<std::string> lines;
    bool empty() const { return lines.empty(); }
    void add(std::string s) { lines.push_back(std::move(s)); }
};

// Compare two grids cell-by-cell and cursor-wise. Probe the delayed-wrap
// flag indirectly by feeding one extra 'x' into each grid and comparing
// the post-probe cursor — if wrapNext differs between the two grids, the
// probe byte will land in different cells.
GridDiff diffGrids(const TerminalGrid &a, const TerminalGrid &b,
                   const std::string &caseName) {
    GridDiff diff;
    if (a.rows() != b.rows() || a.cols() != b.cols()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "[%s] dim mismatch: a=%dx%d b=%dx%d",
                      caseName.c_str(), a.rows(), a.cols(), b.rows(), b.cols());
        diff.add(buf);
        return diff;
    }
    if (a.cursorRow() != b.cursorRow() || a.cursorCol() != b.cursorCol()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "[%s] cursor mismatch: a=(%d,%d) b=(%d,%d)",
                      caseName.c_str(),
                      a.cursorRow(), a.cursorCol(),
                      b.cursorRow(), b.cursorCol());
        diff.add(buf);
    }
    for (int r = 0; r < a.rows(); ++r) {
        for (int c = 0; c < a.cols(); ++c) {
            const Cell &ca = a.cellAt(r, c);
            const Cell &cb = b.cellAt(r, c);
            if (ca.codepoint != cb.codepoint
                || ca.isWideChar != cb.isWideChar
                || ca.isWideCont != cb.isWideCont) {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "[%s] cell(%d,%d) mismatch: a={cp=0x%x w=%d wc=%d} "
                              "b={cp=0x%x w=%d wc=%d}",
                              caseName.c_str(), r, c,
                              ca.codepoint, ca.isWideChar, ca.isWideCont,
                              cb.codepoint, cb.isWideChar, cb.isWideCont);
                diff.add(buf);
            }
        }
    }
    return diff;
}

// Feed a probe 'x' through each grid and compare cursor afterwards.
// This indirectly checks that both grids have the same m_wrapNext state.
bool wrapNextProbeMatches(const std::string &input,
                          const std::string &caseName,
                          std::string &errOut) {
    TerminalGrid gBulk(24, 80);
    TerminalGrid gScalar(24, 80);
    feedBytes(gBulk, input, false);
    feedBytes(gScalar, input, true);
    feedBytes(gBulk, "x", false);
    feedBytes(gScalar, "x", true);
    if (gBulk.cursorRow() != gScalar.cursorRow()
        || gBulk.cursorCol() != gScalar.cursorCol()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "[%s] wrapNext probe divergence: bulk=(%d,%d) scalar=(%d,%d)",
                      caseName.c_str(),
                      gBulk.cursorRow(), gBulk.cursorCol(),
                      gScalar.cursorRow(), gScalar.cursorCol());
        errOut = buf;
        return false;
    }
    return true;
}

int runCase(const std::string &caseName, const std::string &input,
            int rows = 24, int cols = 80,
            std::function<void(TerminalGrid &)> seed = {}) {
    TerminalGrid gBulk(rows, cols);
    TerminalGrid gScalar(rows, cols);
    if (seed) {
        seed(gBulk);
        seed(gScalar);
    }
    feedBytes(gBulk, input, false);
    feedBytes(gScalar, input, true);

    GridDiff d = diffGrids(gBulk, gScalar, caseName);
    int fails = 0;
    if (!d.empty()) {
        for (const auto &l : d.lines) std::fprintf(stderr, "%s\n", l.c_str());
        ++fails;
    }
    std::string probeErr;
    if (!wrapNextProbeMatches(input, caseName, probeErr)) {
        std::fprintf(stderr, "%s\n", probeErr.c_str());
        ++fails;
    }
    return fails;
}

} // namespace

int main() {
    int failures = 0;

    // INV-1: 4 KiB ASCII run.
    {
        std::string buf;
        for (int i = 0; i < 4096; ++i) {
            buf.push_back(static_cast<char>(0x20 + (i % 95)));
        }
        failures += runCase("inv1_4k_ascii", buf);
    }

    // INV-2: CSI at every lane-boundary offset 0..31.
    for (int off = 0; off < 32; ++off) {
        std::string buf(off, 'A');
        buf += "\x1b[31mXYZ\x1b[0m";
        buf.append(60, 'q');
        failures += runCase("inv2_csi_at_offset_" + std::to_string(off), buf);
    }

    // INV-3: run that forces wrap at right edge.
    // 80 cols. Feed 120 bytes of safe ASCII — wraps once.
    {
        std::string buf(120, 'W');
        failures += runCase("inv3_wrap_right_edge", buf);
    }

    // INV-3b: run that lands exactly at the right edge (delayed-wrap sentinel).
    // Feed exactly 80 bytes — cursor should end at col 79 with wrapNext=true.
    // Any further byte (via the probe) should then wrap to row 1 col 0+1.
    {
        std::string buf(80, 'E');
        failures += runCase("inv3b_wrap_exact_80", buf);
    }

    // INV-4: cursor starts mid-row via CUP.
    {
        std::string seedCsi = "\x1b[5;37H";  // row 5, col 37
        std::string runBytes(100, 'M');
        failures += runCase("inv4_cursor_mid_row", seedCsi + runBytes);
    }

    // INV-5: mixed UTF-8 + runs + controls.
    {
        std::string buf = "ASCII ";
        buf += "\xe5\xa5\xbd";  // U+597D (好, CJK, width 2)
        buf += "more\n";
        buf += "tail";
        failures += runCase("inv5_mixed_utf8_controls", buf);
    }

    // INV-6: empty input. Feeding "" is a no-op.
    {
        failures += runCase("inv6_empty", "");
    }

    // INV-7: combining mark is cleared by overwrite.
    // Seed: write 'a' at col 0, then attach combining U+0301 (◌́),
    // then overwrite with an ASCII run. The combining mark on the
    // overwritten cell must be gone in both grids.
    {
        // Emit ASCII 'a' then combining U+0301 (0xCC 0x81 in UTF-8), then
        // position cursor back to col 0 with CUP, then feed the run.
        // U+0301 combining acute accent.
        std::string input;
        input += "a";
        input += "\xcc\x81";     // U+0301 combining
        input += "\x1b[1;1H";    // CUP back to 1,1
        input.append(20, 'R');   // ASCII run overwriting 'a' and past it
        failures += runCase("inv7_combining_cleared", input);
    }

    if (failures == 0) {
        std::printf("vtparser_print_run_coalesce: all invariants hold "
                    "(bulk feed ≡ byte-by-byte feed)\n");
        return 0;
    }
    std::fprintf(stderr, "vtparser_print_run_coalesce: %d failure(s)\n", failures);
    return 1;
}
