// Threaded response ordering — see spec.md for the contract.
//
// Pins the invariant that TerminalGrid fires its ResponseCallback in
// the order the parser consumed input sequences. End-to-end write
// order across the 0.7.0 worker boundary follows from this plus Qt's
// per-receiver FIFO event queue.

#include "terminalgrid.h"
#include "vtparser.h"

#include <QByteArray>

#include <cstdio>
#include <string>
#include <vector>

namespace {

static std::vector<std::string> feedAndCollect(const std::string &input,
                                                 size_t chunk = 0) {
    TerminalGrid grid(24, 80);
    std::vector<std::string> responses;
    grid.setResponseCallback([&responses](const std::string &r) {
        responses.push_back(r);
    });

    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });

    if (chunk == 0) {
        parser.feed(input.data(), static_cast<int>(input.size()));
    } else {
        for (size_t i = 0; i < input.size(); i += chunk) {
            size_t n = std::min(chunk, input.size() - i);
            parser.feed(input.data() + i, static_cast<int>(n));
        }
    }
    return responses;
}

struct Check {
    const char *label;
    const char *expectedPrefix;  // match first N bytes of response
};

static bool compareResponses(const std::vector<std::string> &got,
                              const std::vector<Check> &want,
                              const char *tag) {
    if (got.size() != want.size()) {
        std::fprintf(stderr,
                     "FAIL [%s]: response count %zu != expected %zu\n",
                     tag, got.size(), want.size());
        for (size_t i = 0; i < got.size(); ++i) {
            std::fprintf(stderr, "  got[%zu]: \"", i);
            for (char c : got[i]) {
                if (c >= 0x20 && c < 0x7f) std::fputc(c, stderr);
                else std::fprintf(stderr, "\\x%02x", static_cast<unsigned char>(c));
            }
            std::fprintf(stderr, "\"\n");
        }
        return false;
    }
    for (size_t i = 0; i < got.size(); ++i) {
        size_t n = std::strlen(want[i].expectedPrefix);
        if (got[i].size() < n || got[i].compare(0, n, want[i].expectedPrefix) != 0) {
            std::fprintf(stderr,
                         "FAIL [%s]: response[%zu] expected to start with %s (%s); got \"",
                         tag, i, want[i].label, want[i].expectedPrefix);
            for (char c : got[i]) {
                if (c >= 0x20 && c < 0x7f) std::fputc(c, stderr);
                else std::fprintf(stderr, "\\x%02x", static_cast<unsigned char>(c));
            }
            std::fprintf(stderr, "\"\n");
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    // Stream the grid processes from left to right. Each sequence
    // triggers exactly one response via TerminalGrid's CSI dispatch.
    // Order of the input is the order we expect responses to fire.
    const std::string input =
        "\x1B[c"          // DA1
        "\x1B[6n"         // CPR
        "\x1B[>c"         // DA2
        "\x1B[?996;1n"    // DEC report mode 996 (color scheme)
        "\x1B[5n";        // DSR operating status

    const std::vector<Check> expected = {
        {"DA1",          "\x1B[?62;22c"},
        {"CPR",          "\x1B["},            // starts with ESC[, exact R/C depends on cursor
        {"DA2",          "\x1B[>41;0;0c"},
        {"ColorSchemeN", "\x1B[?996;1n"},
        {"DSR-OK",       "\x1B[0n"},
    };

    int failures = 0;

    // Whole-buffer feed.
    if (!compareResponses(feedAndCollect(input), expected, "whole")) ++failures;

    // Byte-by-byte feed — chunking must not reorder responses.
    if (!compareResponses(feedAndCollect(input, 1), expected, "byte-by-byte")) ++failures;

    // Chunk at arbitrary 4-byte boundary which splits CSI sequences mid-stream.
    if (!compareResponses(feedAndCollect(input, 4), expected, "chunk-4")) ++failures;

    if (failures) {
        std::fprintf(stderr, "\n%d assertion(s) failed\n", failures);
        return 1;
    }
    std::printf("threaded_response_ordering: OK (%zu responses across 3 feed strategies)\n",
                expected.size());
    return 0;
}
