// Feature-conformance test for spec.md (canonical:
// docs/specs/ANTS-1335.md). Pins the C1 8-bit byte strip in
// RemoteControl::filterControlChars + the source-grep wiring.
//
// PV-1..PV-16 exercise the filter directly (pure, Qt::Core only).
// WI-1..WI-3 grep remotecontrol.h / .cpp for the invariants.
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <QByteArray>

#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef SRC_RC_CPP
#error "SRC_RC_CPP compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

// Small helper for byte-literal QByteArrays — keeps the test
// readable when the payload contains non-printable bytes.
QByteArray ba(std::initializer_list<unsigned char> bytes) {
    QByteArray out;
    out.reserve(static_cast<int>(bytes.size()));
    for (unsigned char b : bytes) out.append(static_cast<char>(b));
    return out;
}

std::string slurp(const char *path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---- Engine tests --------------------------------------------------

void testEngine() {
    // PV-1 empty input
    {
        int n = 0;
        QByteArray out = RemoteControl::filterControlChars(QByteArray(), &n);
        expect(out.isEmpty() && n == 0, "PV-1 empty input");
    }

    // PV-2 pure ASCII
    {
        int n = 0;
        QByteArray out = RemoteControl::filterControlChars(
            QByteArray("hello world\n"), &n);
        expect(out == "hello world\n" && n == 0, "PV-2 pure ASCII");
    }

    // PV-3 C0 strip alone (regression for ANTS-0.7.52 baseline)
    {
        int n = 0;
        QByteArray in = ba({'a', 0x01, 'b', 0x1B, 'c'});
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out == "abc" && n == 2, "PV-3 C0 strip alone");
    }

    // PV-4 CSI 8-bit strip (the headline vector)
    {
        int n = 0;
        QByteArray in = ba({'a', 0xC2, 0x9B, 'b'});
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out == "ab" && n == 2, "PV-4 CSI 8-bit strip");
    }

    // PV-5 OSC 8-bit strip — the introducer goes; the printable
    // payload survives. BEL (0x07) is also stripped (it's C0, not in
    // the HT/LF/CR allow-list) so the expected n counts both: 2 for
    // the C1 introducer + 1 for BEL = 3.
    {
        int n = 0;
        QByteArray in;
        in.append(static_cast<char>(0xC2)).append(static_cast<char>(0x9D));
        in.append("52;c;ZGF0YQ==").append(static_cast<char>(0x07));
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out == "52;c;ZGF0YQ==" && n == 3,
               "PV-5 OSC 8-bit strip");
    }

    // PV-6 APC 8-bit strip — both introducer and ST terminator
    {
        int n = 0;
        QByteArray in;
        in.append('x').append(static_cast<char>(0xC2)).append(static_cast<char>(0x9F));
        in.append("!_payload");
        in.append(static_cast<char>(0xC2)).append(static_cast<char>(0x9C));
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out == "x!_payload" && n == 4, "PV-6 APC 8-bit strip");
    }

    // PV-7 0xC3 NOT stripped — U+00C0..U+00FF passes through
    {
        int n = 0;
        QByteArray in = ba({0xC3, 0x80, 0xC3, 0xBF});
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out == in && n == 0, "PV-7 0xC3 not stripped");
    }

    // PV-8 3-byte UTF-8 untouched — Japanese 日本語
    {
        int n = 0;
        QByteArray in = ba({0xE6, 0x97, 0xA5,   // 日
                            0xE6, 0x9C, 0xAC,   // 本
                            0xE8, 0xAA, 0x9E}); // 語
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out == in && n == 0, "PV-8 3-byte UTF-8 untouched");
    }

    // PV-9 4-byte UTF-8 untouched — crab emoji 🦀
    {
        int n = 0;
        QByteArray in = ba({0xF0, 0x9F, 0xA6, 0x80});
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out == in && n == 0, "PV-9 4-byte UTF-8 untouched");
    }

    // PV-10 bare 0xC2 at EOI is preserved
    {
        int n = 0;
        QByteArray in = ba({'a', 0xC2});
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out == in && n == 0, "PV-10 bare 0xC2 at EOI preserved");
    }

    // PV-11 bare 0xC2 followed by ASCII is preserved
    {
        int n = 0;
        QByteArray in = ba({'a', 0xC2, '!'});
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out == in && n == 0, "PV-11 bare 0xC2 + ASCII preserved");
    }

    // PV-12 multiple C1 in a row
    {
        int n = 0;
        QByteArray in = ba({0xC2, 0x9B, 0xC2, 0x9D, 0xC2, 0x9E});
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out.isEmpty() && n == 6, "PV-12 multiple C1 in a row");
    }

    // PV-13 mixed C0 + C1 + UTF-8
    {
        int n = 0;
        QByteArray in;
        in.append('a').append(static_cast<char>(0x01));
        in.append(static_cast<char>(0xC2)).append(static_cast<char>(0x9B));
        in.append(ba({0xE6, 0x97, 0xA5}));   // 日
        in.append(static_cast<char>(0xC2)).append(static_cast<char>(0x9F));
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        QByteArray expected;
        expected.append('a').append(ba({0xE6, 0x97, 0xA5}));
        expect(out == expected && n == 5, "PV-13 mixed C0 + C1 + UTF-8");
    }

    // PV-14 HT / LF / CR preserved (ANTS-0.7.52 baseline)
    {
        int n = 0;
        QByteArray in = ba({'a', 0x09, 'b', 0x0A, 'c', 0x0D, 'd'});
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out == in && n == 0, "PV-14 HT/LF/CR preserved");
    }

    // PV-15 DEL stripped (ANTS-0.7.52 baseline)
    {
        int n = 0;
        QByteArray in = ba({'a', 0x7F, 'b'});
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out == "ab" && n == 1, "PV-15 DEL stripped");
    }

    // PV-16 reserve bound — 1 MiB random ASCII produces ≤ 1 MiB out
    {
        constexpr int kSize = 1 * 1024 * 1024;
        QByteArray in(kSize, 'a');
        int n = 0;
        QByteArray out = RemoteControl::filterControlChars(in, &n);
        expect(out.size() <= kSize && n == 0,
               "PV-16 reserve bound (1 MiB)");
    }
}

// ---- Wiring tests (source-grep) -----------------------------------

void testWiring() {
    const std::string hdr = slurp(SRC_RC_HEADER);
    const std::string cpp = slurp(SRC_RC_CPP);

    // WI-1 — the new 0xC2-guarded block is present exactly once.
    // We match the literal token sequence the patch introduces:
    // `b == 0xC2 &&` (with surrounding whitespace tolerated by the
    // single-string match — Loop 2 implementation must emit this
    // form). Count of substring occurrences in the header.
    {
        size_t count = 0;
        size_t pos = 0;
        const std::string needle = "b == 0xC2 &&";
        while ((pos = hdr.find(needle, pos)) != std::string::npos) {
            ++count; pos += needle.size();
        }
        expect(count == 1,
               (std::string("WI-1 expected 1 0xC2-guard block, found ")
                + std::to_string(count)).c_str());
    }

    // WI-2 — the obsolete AI-dialog comment is removed.
    {
        const bool stale = hdr.find(
            "Stripping C1 is the AI-dialog layer's job") != std::string::npos;
        expect(!stale,
               "WI-2 stale 'AI-dialog layer's job' comment must be removed");
    }

    // WI-3 — each rc verb still calls filterControlChars (regression
    // against accidental removal). Three call-sites in the .cpp body:
    // cmdSendText (1), cmdLaunch (1), cmdNewTab (1) = 3 invocations.
    {
        size_t count = 0;
        size_t pos = 0;
        const std::string needle = "filterControlChars(";
        while ((pos = cpp.find(needle, pos)) != std::string::npos) {
            ++count; pos += needle.size();
        }
        expect(count >= 3,
               (std::string("WI-3 expected >=3 filterControlChars call-sites "
                            "in remotecontrol.cpp, found ")
                + std::to_string(count)).c_str());
    }
}

int runMain() {
    expect_reset();
    testEngine();
    testWiring();
    return expect_finish();
}

}  // namespace

TEST(RcC1ByteFilter, Main) { ASSERT_EQ(0, runMain()); }
