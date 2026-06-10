// Feature-conformance test for spec.md — ANTS-1366. Asserts that
// a Sixel payload with an over-cap raster header is rejected and
// that the early-reject path leaves the inline-image vector empty
// (no QImage allocation happened).
//
// Exit 0 = all invariants hold. Non-zero = regression.

#include "terminalgrid.h"
#include "vtparser.h"

#include <clocale>
#include <cstdio>
#include <gtest/gtest.h>
#include <string>

namespace {

constexpr int kRows = 24;
constexpr int kCols = 80;

struct Harness {
    TerminalGrid grid{kRows, kCols};
    VtParser parser{[this](const VtAction &a) { grid.processAction(a); }};
    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
};

// Wrap a Sixel body in DCS … ST framing. The 'q' introducer is part
// of the framing; the body follows after.
std::string sixel(const std::string &body) {
    return "\x1BPq" + body + "\x1B\\";
}

// ANTS-1589 — report failures at the CALL site, not this helper's line.
// `failAt` takes file/line params; the `fail(...)` macro below injects the
// caller's __FILE__/__LINE__ so ctest output points at the real assertion.
void failAt(const char *file, int line, const char *label, const char *detail){
    std::fprintf(stderr, "FAIL [%s]: %s\n", label, detail);
    ADD_FAILURE_AT(file, line) << label << ": " << detail;
}
#define fail(label, detail) failAt(__FILE__, __LINE__, (label), (detail))

}  // namespace

TEST(SixelRasterHeaderPrebudget, Main) {
    std::setlocale(LC_CTYPE, "");

    // --- INV-1: over-cap Ph rejected ---
    // Raster: Pan=1; Pad=1; Ph=9999; Pv=1 (over MAX_IMAGE_DIM=4096)
    {
        Harness h;
        const int before = static_cast<int>(h.grid.inlineImages().size());
        h.feed(sixel("\"1;1;9999;1?"));  // tiny body, big declared dim
        const int after = static_cast<int>(h.grid.inlineImages().size());
        if (after != before)
            return fail("INV-1 Ph reject",
                        "over-cap Ph in raster header did not prevent "
                        "an image from being allocated");
    }

    // --- INV-1 (Pv): over-cap Pv rejected ---
    {
        Harness h;
        const int before = static_cast<int>(h.grid.inlineImages().size());
        h.feed(sixel("\"1;1;1;9999?"));
        const int after = static_cast<int>(h.grid.inlineImages().size());
        if (after != before)
            return fail("INV-1 Pv reject",
                        "over-cap Pv in raster header did not prevent "
                        "an image from being allocated");
    }

    // --- INV-3: small valid Sixel renders ---
    // Raster: Pan=1; Pad=1; Ph=4; Pv=6 with one row of six bits set
    // for each of 4 columns. Bytes '~' = 0x7E = 0b111111 (all six bits).
    {
        Harness h;
        const int before = static_cast<int>(h.grid.inlineImages().size());
        h.feed(sixel("\"1;1;4;6~~~~"));
        const int after = static_cast<int>(h.grid.inlineImages().size());
        if (after <= before)
            return fail("INV-3 valid renders",
                        "small valid Sixel did not produce an image — "
                        "pre-budget might be too aggressive");
    }

    std::printf("sixel_raster_header_prebudget: 3 invariants held\n");
}
