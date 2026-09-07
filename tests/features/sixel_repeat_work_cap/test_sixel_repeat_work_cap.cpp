// Feature-conformance test for spec.md — ANTS-4456 (vt-core lane).
//
// The Sixel repeat introducer `!` is clamped per group but nothing
// bounds the SUM of repeat counts across a payload. `$` rewinds to
// column zero without advancing the band, so a repeat/`$` alternation
// keeps every write in bounds and scales pixel writes with payload
// length instead of image area — a hang on untrusted output.
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

std::string sixel(const std::string &body) {
    return "\x1BPq" + body + "\x1B\\";
}

void failAt(const char *file, int line, const char *label, const char *detail){
    std::fprintf(stderr, "FAIL [%s]: %s\n", label, detail);
    ADD_FAILURE_AT(file, line) << label << ": " << detail;
}
#define fail(label, detail) failAt(__FILE__, __LINE__, (label), (detail))

}  // namespace

TEST(SixelRepeatWorkCap, Main) {
    std::setlocale(LC_CTYPE, "");

    // --- INV-1: repeat/`$` bomb is refused ---
    // Each group paints a full 4096-wide band with every sixel bit set,
    // then `$` rewinds to column zero so the next group repaints it.
    // The image stays 4096x6, so both the dimension cap and the image
    // budget pass; only a whole-payload write budget can catch this.
    //
    // The budget for this shape is imgWidth * 1 band * 256 column
    // steps; each group charges 4096, so this overshoots it well
    // over threefold while still
    // returning quickly against the pre-fix decoder — the full-length
    // form of this payload is what hangs, and is deliberately not used.
    {
        Harness h;
        const int before = static_cast<int>(h.grid.inlineImages().size());
        std::string body;
        for (int g = 0; g < 400; ++g) body += "!4096~$";
        h.feed(sixel(body));
        const int after = static_cast<int>(h.grid.inlineImages().size());
        if (after != before)
            fail("INV-1",
                 "repeat/$ payload exceeding the work budget still "
                 "produced an inline image — no whole-payload cap fired");
    }

    // --- INV-2: an ordinary small Sixel still renders ---
    {
        Harness h;
        const int before = static_cast<int>(h.grid.inlineImages().size());
        h.feed(sixel("#0;2;100;100;100~~~~~~~~"));
        const int after = static_cast<int>(h.grid.inlineImages().size());
        if (after <= before)
            fail("INV-2",
                 "an ordinary small Sixel did not render — the budget is "
                 "too tight");
    }

    // --- INV-3: legitimate colour passes still render ---
    // Sixel rescans a band once per colour; each pass writes only the
    // pixels of its own colour, so the total stays near the image area.
    // The budget must not read this as the attack.
    {
        Harness h;
        const int before = static_cast<int>(h.grid.inlineImages().size());
        std::string body;
        for (int c = 0; c < 16; ++c) {
            body += "#" + std::to_string(c);
            body += "!64~$";          // full-width pass for this colour
        }
        h.feed(sixel(body));
        const int after = static_cast<int>(h.grid.inlineImages().size());
        if (after <= before)
            fail("INV-3",
                 "a multi-colour Sixel with one pass per colour did not "
                 "render — the budget mistakes colour passes for a bomb");
    }

    // --- INV-4: the out-of-bounds variant of the same bomb ---
    // A raster header pins imgHeight small; `-` then advances the band
    // past it, so every write is out of bounds and no setPixelColor
    // fires — yet the repeat loop still spins once per repeat count.
    // The budget must charge the repeat GROUP, not the surviving write,
    // or this shape walks straight through it.
    {
        Harness h;
        const int before = static_cast<int>(h.grid.inlineImages().size());
        std::string body = "\"1;1;4096;6";
        for (int g = 0; g < 400; ++g) body += "!4096~-";
        h.feed(sixel(body));
        const int after = static_cast<int>(h.grid.inlineImages().size());
        if (after != before)
            fail("INV-4",
                 "out-of-bounds repeat bomb still produced an inline image "
                 "— the budget is charged on the write, not the group");
    }
}
