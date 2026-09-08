// Why this exists: docs/subsystems.md described the model auto-switch
// actuator as live — injecting keystrokes and appending a ledger record —
// while kAutoSwitchActuatorParked returns before all of it. See spec.md.
//
// Bidirectional on purpose: un-parking the feature must force the map to
// stop saying parked, or the same defect returns with the sign flipped.

#include <cstdio>
#include <cctype>
#include <regex>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_MODELAUTOSWITCH_H_PATH
#  error "SRC_MODELAUTOSWITCH_H_PATH compile definition required"
#endif
#ifndef DOCS_SUBSYSTEMS_MD_PATH
#  error "DOCS_SUBSYSTEMS_MD_PATH compile definition required"
#endif

namespace {

// The `- \`modelautoswitch\` (...` bullet, up to the start of the next
// top-level bullet. Scoping matters: "parked" elsewhere in the map must
// not satisfy this.
std::string autoSwitchEntry(const std::string &doc) {
    const size_t start = doc.find("- `modelautoswitch`");
    if (start == std::string::npos) return {};
    const size_t next = doc.find("\n- `", start + 1);
    std::string entry =
        doc.substr(start,
                   next == std::string::npos ? std::string::npos
                                             : next - start);
    // Collapse whitespace runs. The map is hard-wrapped, so a phrase check
    // against the raw text is defeated by wherever the author happened to
    // break the line — "The live actuator" is split across a wrap in the
    // very entry INV-3 exists to catch.
    static const std::regex ws(R"RX(\s+)RX");
    return std::regex_replace(entry, ws, " ");
}

}  // namespace

TEST(ParkedFeatureDocTruth, Main) {
    const std::string hdr = ants_test::slurpFile(SRC_MODELAUTOSWITCH_H_PATH);
    const std::string doc = ants_test::slurpFile(DOCS_SUBSYSTEMS_MD_PATH);

    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1 — read the constant. Without it the checks below are vacuous.
    std::smatch m;
    const std::regex parkedRe(
        R"(kAutoSwitchActuatorParked\s*=\s*(true|false))");
    if (!std::regex_search(hdr, m, parkedRe)) {
        fail("INV-1: could not read kAutoSwitchActuatorParked's value from "
             "src/modelautoswitch.h — the map checks below would pass for "
             "the wrong reason.");
        ASSERT_EQ(0, failures);
        return;
    }
    const bool parked = (m[1].str() == "true");

    const std::string entry = autoSwitchEntry(doc);
    if (entry.empty()) {
        fail("precondition: the `modelautoswitch` entry was not found in "
             "docs/subsystems.md.");
        ASSERT_EQ(0, failures);
        return;
    }

    // Case-insensitive: the entry may emphasise the word, and "PARKED"
    // and "parked" say the same thing to a reader.
    std::string lower = entry;
    for (char &ch : lower) ch = static_cast<char>(std::tolower(
        static_cast<unsigned char>(ch)));
    const bool saysParked = lower.find("parked") != std::string::npos;

    if (parked) {
        // INV-2 — the map must say so.
        if (!saysParked) {
            fail("INV-2: kAutoSwitchActuatorParked is true, but the "
                 "modelautoswitch entry in docs/subsystems.md does not say "
                 "the actuator is parked. The guard returns before the "
                 "keystroke injection, the handshake, the surfacing and "
                 "the ledger append.");
        }
        // INV-3 — and must not claim the opposite.
        if (entry.find("The live actuator") != std::string::npos) {
            fail("INV-3: the entry still calls it \"The live actuator\" "
                 "while the actuator is parked in code.");
        }
    } else {
        // INV-4 — un-parking must force the map to stop saying parked.
        if (saysParked) {
            fail("INV-4: kAutoSwitchActuatorParked is false, but the "
                 "modelautoswitch entry still describes the actuator as "
                 "parked. Same defect, opposite sign.");
        }
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/parked_feature_doc_truth/spec.md\n", failures);
    }
    ASSERT_EQ(0, failures);
}
