// Feature test: OSC 8 URI cap + Kitty APC chunk-buffer cap.
// See spec.md. Drives TerminalGrid through VtParser with crafted OSC 8
// and APC envelopes; asserts the caps drop oversized payloads and keep
// per-row hyperlink state / chunk-buffer state clean.

#include "terminalgrid.h"
#include "vtparser.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void fail(const char *label, const char *detail = "") {
    std::fprintf(stderr, "FAIL %-44s %s%s\n",
                 label,
                 (detail && *detail) ? "— " : "",
                 detail ? detail : "");
    ++g_failures;
}

void feed(VtParser &parser, const std::string &bytes) {
    parser.feed(bytes.data(), static_cast<int>(bytes.size()));
}

VtParser makeParser(TerminalGrid &grid) {
    return VtParser([&grid](const VtAction &a) { grid.processAction(a); });
}

// Wrap a payload in an OSC 8 open envelope terminated by BEL (xterm
// convention). Note: our VT parser's ST (`\x1b\\`) handling aborts the
// accumulated payload — see vtparser.cpp:219 early-return on 0x1B
// shadowing the ch==0x1B case inside OscString. BEL is the portable
// terminator across every OSC handler in the codebase.
std::string osc8Open(const std::string &uri, const std::string &params = "") {
    std::string s;
    s += "\x1b]8;";
    s += params;
    s += ";";
    s += uri;
    s += '\x07';
    return s;
}

const std::string kOsc8Close = "\x1b]8;;\x07";

// Wrap a payload in a Kitty APC envelope. APC uses the same terminators
// as OSC; we use BEL here for parser-parity with the OSC tests.
std::string apcEnv(const std::string &params, const std::string &data) {
    std::string s;
    s += "\x1b_G";
    s += params;
    s += ";";
    s += data;
    s += '\x07';
    return s;
}

void testOsc8HappyPath() {
    TerminalGrid grid(24, 80);
    VtParser parser = makeParser(grid);
    // INV-OSC8-A: short URI below cap. Open, print "X", close. Row 0
    // should end up with one HyperlinkSpan pointing at http://example.
    feed(parser, osc8Open("http://example.com/page"));
    feed(parser, "X");
    feed(parser, kOsc8Close);
    const auto &spans = grid.screenHyperlinks(0);
    if (spans.size() != 1) {
        fail("OSC8-A happy path produced no span",
             std::to_string(spans.size()).c_str());
        return;
    }
    if (spans[0].uri != "http://example.com/page") {
        fail("OSC8-A URI mismatch");
    }
}

void testOsc8OversizedDropped() {
    TerminalGrid grid(24, 80);
    VtParser parser = makeParser(grid);
    // INV-OSC8-B + INV-OSC8-C: URI well above cap. Same shape as happy
    // path (open, print X, close) but the URI exceeds MAX_OSC8_URI_BYTES.
    // Post-fix: the open is dropped, close is a no-op, row 0 has no spans.
    const size_t oversized = TerminalGrid::MAX_OSC8_URI_BYTES + 1024;
    std::string bigUri = "http://example.com/";
    bigUri.append(oversized - bigUri.size(), 'A');
    feed(parser, osc8Open(bigUri));
    feed(parser, "X");
    feed(parser, kOsc8Close);

    const auto &spans = grid.screenHyperlinks(0);
    if (!spans.empty()) {
        char detail[128];
        std::snprintf(detail, sizeof(detail),
                      "row 0 has %zu spans, first uri len = %zu",
                      spans.size(), spans[0].uri.size());
        fail("OSC8-B/C oversized URI leaked into row", detail);
    }
    // The 'X' still prints — the OSC 8 being dropped doesn't swallow text.
    if (grid.cellAt(0, 0).codepoint != 'X') {
        fail("OSC8-B/C print after dropped OSC 8 failed");
    }
}

void testOsc8AtCapAccepted() {
    TerminalGrid grid(24, 80);
    VtParser parser = makeParser(grid);
    // Exactly-at-cap URI must still be accepted — the cap is the first
    // size that rejects, not the last size that passes.
    std::string atCap = "http://example.com/";
    atCap.append(TerminalGrid::MAX_OSC8_URI_BYTES - atCap.size(), 'B');
    feed(parser, osc8Open(atCap));
    feed(parser, "Y");
    feed(parser, kOsc8Close);
    const auto &spans = grid.screenHyperlinks(0);
    if (spans.size() != 1) {
        fail("OSC8 at-cap URI was rejected", "should accept up to and including MAX");
    }
}

void testApcSingleFrameNotAffected() {
    TerminalGrid grid(24, 80);
    VtParser parser = makeParser(grid);
    // INV-APC-A: a single `m=0` APC frame below the cap should leave
    // the chunk buffer empty regardless of image decode outcome. The
    // cap path must not be the reason it's empty — this is the sanity
    // check that the guard doesn't fire in the normal case.
    // We use a bogus delete action (a=d) to avoid dragging in a real
    // PNG decoder; APC parsing still consumes `data` and clears state.
    feed(parser, apcEnv("a=d,d=a", "ignored"));
    if (grid.kittyChunkBufferSizeForTest() != 0) {
        fail("APC-A single frame left buffer non-empty");
    }
}

void testApcOverflowDropped() {
    TerminalGrid grid(24, 80);
    VtParser parser = makeParser(grid);
    // INV-APC-B: chain enough `m=1` frames to exceed MAX_KITTY_CHUNK_BYTES.
    // Post-fix: the buffer gets reset on the first over-cap frame, so
    // the size is back to 0 while more frames can still arrive. Feed
    // one more frame: buffer must still be 0 (fresh start, not
    // resurrected).
    //
    // Each per-frame data payload is ~1 MiB of 'A' — well below the
    // parser's 10 MB per-envelope cap but enough that 40 frames > 32 MiB
    // force the accumulator to trip.
    const size_t kFrameBytes = 1 * 1024 * 1024;
    const std::string frameData(kFrameBytes, 'A');
    const size_t frameCount =
        (TerminalGrid::MAX_KITTY_CHUNK_BYTES / kFrameBytes) + 8;

    for (size_t i = 0; i < frameCount; ++i) {
        feed(parser, apcEnv("a=t,f=100,m=1,i=99", frameData));
    }
    // After exceeding the cap, the fix clears the buffer. A subsequent
    // `m=1` frame landing on an empty buffer would re-accumulate; we
    // expect the cumulative effect to stay bounded well below N × 1 MiB.
    const size_t afterSize = grid.kittyChunkBufferSizeForTest();
    if (afterSize >= TerminalGrid::MAX_KITTY_CHUNK_BYTES) {
        char detail[128];
        std::snprintf(detail, sizeof(detail),
                      "buffer size = %zu, cap = %zu", afterSize,
                      static_cast<size_t>(TerminalGrid::MAX_KITTY_CHUNK_BYTES));
        fail("APC-B buffer grew past cap", detail);
    }

    // Feed a closing `m=0` frame. The test only cares that the stale
    // accumulator isn't silently preserved across the cap trip — i.e.
    // the behaviour is predictable and bounded, not that any image gets
    // rendered.
    feed(parser, apcEnv("a=t,f=100,m=0,i=99", "end"));
    if (grid.kittyChunkBufferSizeForTest() != 0) {
        fail("APC-B final m=0 did not clear buffer");
    }
}

}  // namespace

int main() {
    testOsc8HappyPath();
    testOsc8OversizedDropped();
    testOsc8AtCapAccepted();
    testApcSingleFrameNotAffected();
    testApcOverflowDropped();

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d assertion(s) failed.\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "All assertions passed.\n");
    return 0;
}
