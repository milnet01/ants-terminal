// Feature-conformance test for spec.md (canonical:
// docs/specs/ANTS-1347.md). Pins the cwdHasBadByte helper +
// the validatePath wiring in cmdLaunch / cmdNewTab.
//
// CB-1..CB-7 exercise cwdHasBadByte directly (pure helper).
// WI-1..WI-5 grep remotecontrol.h / .cpp for the wiring via the
// brace-matched function-body slurp helper (srcgrep.h).

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "remotecontrol.h"

#include <QChar>
#include <QString>

#include <string>

#include <gtest/gtest.h>

#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

// ---- Engine tests --------------------------------------------------

void testEngine() {
    // CB-1 empty input → false (caller treats absence of cwd as
    // "inherit focused cwd" — no rejection needed here).
    {
        expect(!RemoteControl::cwdHasBadByte(QString()),
               "CB-1 empty input → false");
    }

    // CB-2 pure ASCII project-relative path → false.
    {
        expect(!RemoteControl::cwdHasBadByte(
                   QStringLiteral("docs/specs/ANTS-1347.md")),
               "CB-2 pure ASCII → false");
        expect(!RemoteControl::cwdHasBadByte(
                   QStringLiteral("/home/user/project")),
               "CB-2 absolute ASCII → false");
    }

    // CB-3 C0 reject — any byte in U+0000..U+001F (including HT/LF/CR;
    // unlike filterControlChars which allowlists those, paths reject
    // them all because a newline-in-path is never legitimate).
    {
        QString s; s.append(QChar(0x01));
        expect(RemoteControl::cwdHasBadByte(s),
               "CB-3a SOH (U+0001) rejected");
        QString s2; s2.append(QChar(0x1B));
        expect(RemoteControl::cwdHasBadByte(s2),
               "CB-3b ESC (U+001B) rejected");
        QString s3; s3.append('a').append(QChar(0x09));
        expect(RemoteControl::cwdHasBadByte(s3),
               "CB-3c HT (U+0009) rejected even in path");
        QString s4; s4.append('a').append(QChar(0x0A));
        expect(RemoteControl::cwdHasBadByte(s4),
               "CB-3d LF (U+000A) rejected even in path");
    }

    // CB-4 backslash reject (Windows-path-confusion vector).
    {
        expect(RemoteControl::cwdHasBadByte(QStringLiteral("foo\\bar")),
               "CB-4 backslash rejected");
    }

    // CB-5 C1 reject (U+0080..U+009F) — path-side counterpart to
    // ANTS-1335's byte-strip on text payloads.
    {
        QString s; s.append('a').append(QChar(0x80));
        expect(RemoteControl::cwdHasBadByte(s),
               "CB-5a C1 low (U+0080) rejected");
        QString s2; s2.append('a').append(QChar(0x9B));
        expect(RemoteControl::cwdHasBadByte(s2),
               "CB-5b CSI 8-bit (U+009B) rejected");
        QString s3; s3.append('a').append(QChar(0x9F));
        expect(RemoteControl::cwdHasBadByte(s3),
               "CB-5c C1 high (U+009F) rejected");
    }

    // CB-6 legitimate non-ASCII preserved — é (U+00E9), CJK, emoji.
    {
        expect(!RemoteControl::cwdHasBadByte(
                   QStringLiteral("café/résumé")),
               "CB-6a Latin-1 supplement preserved");
        expect(!RemoteControl::cwdHasBadByte(
                   QStringLiteral("日本語/path")),
               "CB-6b CJK preserved");
        expect(!RemoteControl::cwdHasBadByte(
                   QStringLiteral("🦀/crustaceans")),
               "CB-6c emoji preserved");
    }

    // CB-7 NFC normalisation — composed and decomposed forms of the
    // same character (é) should both pass equally. The helper NFCs
    // its input before scanning, so a decomposed-form path doesn't
    // sneak around the check (and conversely, a benign decomposed
    // path isn't falsely rejected).
    {
        // Composed: U+00E9 directly.
        QString composed = QStringLiteral("café");
        // Decomposed: U+0065 (e) + U+0301 (combining acute).
        QString decomposed = QStringLiteral("café");
        expect(!RemoteControl::cwdHasBadByte(composed),
               "CB-7a composed form passes");
        expect(!RemoteControl::cwdHasBadByte(decomposed),
               "CB-7b decomposed form passes (NFC-normalised internally)");
    }
}

// ---- Wiring tests (source-grep) -----------------------------------

void testWiring() {
    const std::string hdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rc  = ants_test::slurpRemoteControl();

    // WI-1 — cwdHasBadByte defined exactly once in remotecontrol.h.
    {
        const std::size_t count =
            ants_test::countOccurrences(hdr, "cwdHasBadByte(");
        // Header contains the definition (1 occurrence at the signature
        // line). Test-file references don't appear in the header. Other
        // call-sites are in the .cpp body and aren't counted here.
        expect(count >= 1,
               (std::string("WI-1 expected >=1 cwdHasBadByte declaration "
                            "in remotecontrol.h, found ")
                + std::to_string(count)).c_str());
    }

    // WI-2 — old per-verb cwdHasControl lambdas removed.
    {
        const std::size_t count =
            ants_test::countOccurrences(rc, "cwdHasControl");
        expect(count == 0,
               (std::string("WI-2 expected 0 cwdHasControl occurrences "
                            "in remotecontrol.cpp, found ")
                + std::to_string(count)).c_str());
    }

    // WI-3 — PathValidation::validatePath call-site count rises from
    // ANTS-1295's 8 to 10 (cmdLaunch + cmdNewTab add one each), then
    // to 11 with ANTS-1352's cmdIndieReviewDispatch, then to 13 with
    // ANTS-1413's cmdColdEyesSingleDoc + ANTS-1414's cmdCrossDocDiff
    // (the new lane-source-agnostic cross-doc-diff alias), then to 14
    // with ANTS-1302's cmdFocusedTest (validates the build_dir arg),
    // then to 15 with ANTS-1831's cmdColdEyesBrief (the ad-hoc-lane
    // doc_paths entries now route through the chokepoint), then to 16
    // with ANTS-1855's cmdReadLog (validates the `path` arg), then to 18
    // with ANTS-1962's resolveFeedbackPath helper (feedback_query /
    // feedback_log share it) + ANTS-1963's cmdSpecLog (validates the
    // `path` arg), then to 20 with ANTS-2021's cmdReadRegion + ANTS-2022's
    // cmdApplyEdits (each validates its `path` arg), then to 21 with
    // ANTS-1637's cmdCodebaseIndex (validates the `file_path` selector),
    // then to 22 with ANTS-1995's cmdSpecQuery `path` arg (the manual
    // ".."-substring check was replaced by the canonical chokepoint to
    // close a symlink-escape).
    {
        // ANTS-2068 — floor, not exact: the security invariant is "every
        // path-typed arg routes through the chokepoint", so MORE call-sites
        // are fine (a newly-validated arg shouldn't false-fail this test);
        // FEWER means a guard was dropped. 22 is the count at the time of
        // writing — relational, so it stops silently drifting on additions.
        const std::size_t count = ants_test::countOccurrences(
            rc, "PathValidation::validatePath(");
        expect(count >= 22,
               (std::string("WI-3 expected >= 22 validatePath call-sites "
                            "(a guard may have been dropped), found ")
                + std::to_string(count)).c_str());
    }

    // WI-4 — allow_outside_root appears in cmdLaunch + cmdNewTab
    // bodies. The brace-matched slurp keeps this robust to future
    // body growth.
    {
        const std::string launchBody =
            ants_test::slurpFunctionBody(rc, "RemoteControl::cmdLaunch");
        const std::string newTabBody =
            ants_test::slurpFunctionBody(rc, "RemoteControl::cmdNewTab");
        expect(!launchBody.empty(),
               "WI-4 anchor: cmdLaunch body slurped");
        expect(!newTabBody.empty(),
               "WI-4 anchor: cmdNewTab body slurped");
        expect(launchBody.find("\"allow_outside_root\"") != std::string::npos,
               "WI-4a cmdLaunch references \"allow_outside_root\"");
        expect(newTabBody.find("\"allow_outside_root\"") != std::string::npos,
               "WI-4b cmdNewTab references \"allow_outside_root\"");
    }

    // WI-5 — both verbs call PathValidation::validatePath inside
    // their bodies (the two new call-sites that bring the total to
    // 10 per WI-3).
    {
        const std::string launchBody =
            ants_test::slurpFunctionBody(rc, "RemoteControl::cmdLaunch");
        const std::string newTabBody =
            ants_test::slurpFunctionBody(rc, "RemoteControl::cmdNewTab");
        expect(launchBody.find("PathValidation::validatePath(")
                   != std::string::npos,
               "WI-5a cmdLaunch calls PathValidation::validatePath");
        expect(newTabBody.find("PathValidation::validatePath(")
                   != std::string::npos,
               "WI-5b cmdNewTab calls PathValidation::validatePath");
    }
}

int runMain() {
    expect_reset();
    testEngine();
    testWiring();
    return expect_finish();
}

}  // namespace

TEST(RcLaunchCwdAnchor, Main) { ASSERT_EQ(0, runMain()); }
