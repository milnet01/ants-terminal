// ANTS-5124 — a push killed mid-hook left tools/qt62-guard.sh's podman
// container compiling on its own against the named build volume, holding
// memory after the gate that started it was gone. See spec.md.
//
// Source scrape, not a live podman run: driving the real container needs
// podman and a multi-minute cold build, so this locks the shapes the fix
// must give the script's own text instead.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <cctype>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#ifndef SRC_QT62_GUARD_PATH
#error "SRC_QT62_GUARD_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

// Comment lines only (first non-space char '#'), matching ci_workflow_deps'
// codeLines(): the script's own header discusses `podman run`, traps and
// markers in prose, so a raw substring match would pass on the commentary
// alone with the real fix absent.
std::string codeLines(const std::string &text) {
    std::istringstream in(text);
    std::string line;
    std::string out;
    while (std::getline(in, line)) {
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] == '#') continue;
        out += line;
        out += '\n';
    }
    return out;
}

// The compile's `podman run` invocation, from the start of that line up to
// the `rc=$?` that reads its exit status. Empty on failure (anchor missing).
std::string podmanRunRegion(const std::string &code) {
    return ants_test::regionBetween(
        code, "podman run --rm --security-opt label=disable", "rc=$?");
}

// The `trap` statement immediately preceding the compile's `podman run` —
// the signal handler that is supposed to wrap it. Empty if there is no
// `trap '` anywhere before the compile (the pre-fix state).
std::string compileTrapRegion(const std::string &code) {
    const std::size_t podmanIdx =
        code.find("podman run --rm --security-opt label=disable");
    if (podmanIdx == std::string::npos) return {};
    const std::size_t trapIdx = code.rfind("trap '", podmanIdx);
    if (trapIdx == std::string::npos) return {};
    return code.substr(trapIdx, podmanIdx - trapIdx);
}

// The `--warm-only` precondition block: from the mode check up to the next
// function definition (`qt62_ensure_image() {`, real code rather than a
// comment, so it survives codeLines()'s stripping). Empty on failure.
std::string warmOnlyRegion(const std::string &code) {
    return ants_test::regionBetween(
        code, "if [[ \"$mode\" == \"warm-only\" ]]; then",
        "qt62_ensure_image() {");
}

// True if `text` contains "exit" followed (after optional whitespace) by a
// digit sequence that is not "0" — i.e. a non-zero exit, without pinning the
// exact code the fix chooses.
bool hasNonZeroExit(const std::string &text) {
    std::size_t pos = 0;
    while ((pos = text.find("exit", pos)) != std::string::npos) {
        std::size_t i = pos + 4;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        const std::size_t start = i;
        while (i < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[i])))
            ++i;
        if (i > start && text.substr(start, i - start) != "0") return true;
        pos += 4;
    }
    return false;
}

}  // namespace

// INV-1 — the compile's `podman run` line names its container and passes
// --init, so a `podman stop` reaches the container's process tree.
TEST(Qt62GuardInterrupt, Inv1PodmanRunNamesAndInits) {
    const std::string sh = ants_test::slurpFile(SRC_QT62_GUARD_PATH);
    ASSERT_FALSE(sh.empty()) << SRC_QT62_GUARD_PATH << " must be readable";
    const std::string code = codeLines(sh);

    const std::string region = podmanRunRegion(code);
    ASSERT_FALSE(region.empty())
        << "could not find the compile's `podman run` line in "
        << SRC_QT62_GUARD_PATH;

    EXPECT_TRUE(region.find("--name") != std::string::npos)
        << "the compile's podman run does not pass --name; a stop cannot "
           "target it by a fixed name. Line(s) examined:\n"
        << region;
    EXPECT_TRUE(region.find("--init") != std::string::npos)
        << "the compile's podman run does not pass --init; a stop signal "
           "would not reach ninja's children. Line(s) examined:\n"
        << region;
}

// INV-2 — a trap naming INT, TERM and HUP wraps the compile and stops the
// named container; the trap is cleared after the compile succeeds.
TEST(Qt62GuardInterrupt, Inv2TrapWrapsCompileAndStopsContainer) {
    const std::string sh = ants_test::slurpFile(SRC_QT62_GUARD_PATH);
    ASSERT_FALSE(sh.empty()) << SRC_QT62_GUARD_PATH << " must be readable";
    const std::string code = codeLines(sh);

    const std::string trapRegion = compileTrapRegion(code);
    ASSERT_FALSE(trapRegion.empty())
        << "no `trap '...'` statement precedes the compile's podman run in "
        << SRC_QT62_GUARD_PATH
        << " — the compile runs with no signal handler at all.";

    EXPECT_TRUE(trapRegion.find("INT") != std::string::npos &&
                trapRegion.find("TERM") != std::string::npos &&
                trapRegion.find("HUP") != std::string::npos)
        << "the trap preceding the compile does not name INT, TERM and HUP. "
           "Trap statement examined:\n"
        << trapRegion;
    EXPECT_TRUE(trapRegion.find("podman stop") != std::string::npos)
        << "the trap's handler does not call `podman stop` — an interrupted "
           "push would leave the container running unattended. Trap "
           "statement examined:\n"
        << trapRegion;

    // Cleared after the compile: a `trap -` naming the same signals, found
    // after the podman-run block's own `rc=$?` line.
    const std::size_t podmanIdx =
        code.find("podman run --rm --security-opt label=disable");
    ASSERT_NE(podmanIdx, std::string::npos);
    const std::size_t afterCompile = code.find("rc=$?", podmanIdx);
    ASSERT_NE(afterCompile, std::string::npos);
    const std::size_t clearIdx = code.find("trap -", afterCompile);
    EXPECT_TRUE(clearIdx != std::string::npos)
        << "no `trap -` clears the compile's signal handler after it "
           "succeeds — every push after the first interrupted one would "
           "keep tearing down containers it no longer owns.";
    if (clearIdx != std::string::npos) {
        const std::size_t eol = code.find('\n', clearIdx);
        const std::string clearLine =
            code.substr(clearIdx, (eol == std::string::npos ? code.size()
                                                              : eol) -
                                       clearIdx);
        EXPECT_TRUE(clearLine.find("INT") != std::string::npos &&
                    clearLine.find("TERM") != std::string::npos &&
                    clearLine.find("HUP") != std::string::npos)
            << "the clearing `trap -` does not name the same signals "
               "(INT, TERM, HUP) the setup trap named. Line examined:\n"
            << clearLine;
    }
}

// INV-3 — the trap's handler writes an interrupted marker for the build
// volume and exits non-zero.
TEST(Qt62GuardInterrupt, Inv3HandlerMarksVolumeInterruptedAndExitsNonZero) {
    const std::string sh = ants_test::slurpFile(SRC_QT62_GUARD_PATH);
    ASSERT_FALSE(sh.empty()) << SRC_QT62_GUARD_PATH << " must be readable";
    const std::string code = codeLines(sh);

    const std::string trapRegion = compileTrapRegion(code);
    ASSERT_FALSE(trapRegion.empty())
        << "no trap precedes the compile in " << SRC_QT62_GUARD_PATH
        << " — there is no handler to mark anything.";

    EXPECT_TRUE(trapRegion.find("interrupted") != std::string::npos)
        << "the trap's handler does not write anything naming an "
           "'interrupted' marker for the build volume. Trap statement "
           "examined:\n"
        << trapRegion;
    EXPECT_TRUE(hasNonZeroExit(trapRegion))
        << "the trap's handler does not exit non-zero — an interrupted "
           "compile would report success. Trap statement examined:\n"
        << trapRegion;
}

// INV-4 — the --warm-only branch checks for the interrupted marker and
// skips (exit 0) when it is present.
TEST(Qt62GuardInterrupt, Inv4WarmOnlySkipsOnInterruptedMarker) {
    const std::string sh = ants_test::slurpFile(SRC_QT62_GUARD_PATH);
    ASSERT_FALSE(sh.empty()) << SRC_QT62_GUARD_PATH << " must be readable";
    const std::string code = codeLines(sh);

    const std::string region = warmOnlyRegion(code);
    ASSERT_FALSE(region.empty())
        << "could not find the --warm-only precondition block in "
        << SRC_QT62_GUARD_PATH;

    const std::size_t markerIdx = region.find("interrupted");
    EXPECT_TRUE(markerIdx != std::string::npos)
        << "the --warm-only branch never mentions an interrupted marker, so "
           "it cannot be refusing to trust one. Block examined:\n"
        << region;
    if (markerIdx != std::string::npos) {
        EXPECT_TRUE(region.find("exit 0", markerIdx) != std::string::npos)
            << "the --warm-only branch checks for the marker but does not "
               "skip (exit 0) once it names it. Block examined:\n"
            << region;
    }
}

// INV-5 (guard) — the --warm-only branch still skips when the cached image
// or build volume is missing; the marker check must not have displaced the
// existing `missing=` logic.
TEST(Qt62GuardInterrupt, Inv5WarmOnlyStillSkipsOnMissingImageOrVolume) {
    const std::string sh = ants_test::slurpFile(SRC_QT62_GUARD_PATH);
    ASSERT_FALSE(sh.empty()) << SRC_QT62_GUARD_PATH << " must be readable";
    const std::string code = codeLines(sh);

    const std::string region = warmOnlyRegion(code);
    ASSERT_FALSE(region.empty())
        << "could not find the --warm-only precondition block in "
        << SRC_QT62_GUARD_PATH;

    EXPECT_TRUE(region.find("podman image exists \"$qt62_image\"") !=
                std::string::npos)
        << "the image-existence check is gone from the --warm-only block. "
           "Block examined:\n"
        << region;
    EXPECT_TRUE(region.find("podman volume exists \"$qt62_volume\"") !=
                std::string::npos)
        << "the volume-existence check is gone from the --warm-only block. "
           "Block examined:\n"
        << region;
    EXPECT_TRUE(region.find("missing=") != std::string::npos)
        << "the missing= accumulation is gone from the --warm-only block. "
           "Block examined:\n"
        << region;
    EXPECT_TRUE(region.find("exit 0") != std::string::npos)
        << "the --warm-only block no longer skips with exit 0 on a missing "
           "cache. Block examined:\n"
        << region;
}
