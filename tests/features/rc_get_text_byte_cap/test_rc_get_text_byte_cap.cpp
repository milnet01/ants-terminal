// Feature-conformance test for spec.md (canonical:
// docs/specs/ANTS-1348.md). Pins the server-side byte cap on
// cmdGetText via the pure RemoteControl::trimScrollbackForGetText
// helper, plus source-grep on the cmdGetText body.
//
// GT-1..GT-10 exercise the helper directly (no Qt event loop).
// WI-1..WI-3 grep remotecontrol.cpp / .h for the wiring.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <QByteArray>
#include <QString>

#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef SRC_RC_CPP
#error "SRC_RC_CPP compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


// Build a scrollback fixture: N lines of `length` ASCII chars each
// (plus a trailing newline per line). Lines are numbered so trim
// direction (head vs tail) is verifiable: line 0 is oldest, line
// N-1 is newest.
QString fixture(int lines, int chars_per_line) {
    QString out;
    out.reserve((chars_per_line + 1) * lines);
    for (int i = 0; i < lines; ++i) {
        QString line = QString::number(i).rightJustified(4, '0') + ':';
        const int pad = chars_per_line - line.size();
        if (pad > 0) line += QString(pad, 'x');
        out += line;
        out += '\n';
    }
    return out;
}

// ---- Engine tests --------------------------------------------------

void testEngine() {
    // GT-1 happy path — small scrollback, no truncation.
    {
        const QString raw = fixture(32, 80);
        const auto r =
            RemoteControl::trimScrollbackForGetText(raw, 1 * 1024 * 1024);
        expect(!r.truncated && r.bytesDropped == 0 && r.linesDropped == 0,
               "GT-1 happy path — no truncation flags");
        expect(r.text == raw, "GT-1 happy path — text unchanged");
    }

    // GT-2 trip cap at default 1 MiB.
    // Construct a scrollback large enough to exceed 1 MiB: 14000 lines
    // of 80 chars = ~1.14 MiB.
    {
        const QString raw = fixture(14000, 80);
        const int maxBytes = 1 * 1024 * 1024;
        const auto r =
            RemoteControl::trimScrollbackForGetText(raw, maxBytes);
        expect(r.truncated, "GT-2 cap tripped");
        expect(r.bytesDropped > 0, "GT-2 bytesDropped > 0");
        expect(r.linesDropped > 0, "GT-2 linesDropped > 0");
        expect(r.text.toUtf8().size() <= maxBytes + 64 /* sentinel slack */,
               "GT-2 returned text bounded by cap (+ small sentinel)");
        expect(r.text.startsWith("<truncated "),
               "GT-2 sentinel prefix present");
    }

    // GT-3 custom max_bytes below default.
    {
        const QString raw = fixture(20, 80);  // ~1.6 KiB
        const auto r = RemoteControl::trimScrollbackForGetText(raw, 1024);
        expect(r.truncated, "GT-3 small cap tripped");
        expect(r.text.toUtf8().size() <= 1024 + 64,
               "GT-3 text within cap + sentinel slack");
        expect(r.text.startsWith("<truncated "),
               "GT-3 sentinel prefix");
    }

    // GT-4 trim snaps to \n boundary.
    // Three short lines, cap that lands mid-line-1; expect line-1
    // dropped entirely, kept output starts at line-2 boundary.
    {
        // "aaaaaaaaa\nbbbbbbbbb\nccccccccc\n" — 30 bytes
        const QString raw = QStringLiteral(
            "aaaaaaaaa\nbbbbbbbbb\nccccccccc\n");
        const auto r = RemoteControl::trimScrollbackForGetText(raw, 15);
        expect(r.truncated, "GT-4 cap tripped");
        // Kept text (after the sentinel line) must NOT contain `a`s
        // (the oldest line was dropped) and MUST start with the
        // `b` or `c` line (newest survives).
        // Sentinel ends at first \n; everything after that is kept.
        const int nl = r.text.indexOf('\n');
        expect(nl >= 0, "GT-4 sentinel line ends with newline");
        const QString kept = r.text.mid(nl + 1);
        expect(!kept.contains('a'), "GT-4 oldest line dropped");
        expect(kept.contains('c'), "GT-4 newest line preserved");
    }

    // GT-5 single long line, no newline → hard cut + sentinel.
    {
        QString raw;
        raw.fill('z', 2 * 1024 * 1024);  // 2 MiB of 'z', no \n
        const auto r =
            RemoteControl::trimScrollbackForGetText(raw, 1 * 1024 * 1024);
        expect(r.truncated, "GT-5 cap tripped on single-long-line");
        expect(r.text.startsWith("<truncated "),
               "GT-5 sentinel prefix");
        // bytesDropped should be ~1 MiB; the kept payload is the tail
        // of the 'z' buffer.
        expect(r.bytesDropped >= 1 * 1024 * 1024 - 64,
               "GT-5 bytesDropped reflects head-cut");
    }

    // GT-6 ceiling clamp — over-ceiling silently clamped, flagged.
    // Helper returns the clamp flag in the bytesDropped/linesDropped
    // pair indirectly: we test via the boolean `capClamped` field.
    {
        const QString raw = fixture(32, 80);
        const auto r =
            RemoteControl::trimScrollbackForGetText(raw,
                256 * 1024 * 1024);  // 256 MiB → clamped to 16 MiB
        expect(r.capClamped, "GT-6 cap-clamped flag set");
        expect(!r.truncated, "GT-6 small fixture still fits — no truncation");
    }

    // GT-7 truncated == false on happy path (always present).
    {
        const QString raw = fixture(10, 40);
        const auto r =
            RemoteControl::trimScrollbackForGetText(raw, 1 * 1024 * 1024);
        expect(!r.truncated,
               "GT-7 happy-path truncated flag is false (still present)");
    }

    // GT-8 linesDropped counts correctly.
    // 100 lines of 100 chars (~10 KiB total). Cap at 5 KiB → drop
    // approximately the first 50 lines.
    {
        const QString raw = fixture(100, 100);
        const auto r = RemoteControl::trimScrollbackForGetText(raw, 5 * 1024);
        expect(r.truncated, "GT-8 cap tripped");
        expect(r.linesDropped >= 40 && r.linesDropped <= 60,
               (std::string("GT-8 linesDropped in expected range, got ")
                + std::to_string(r.linesDropped)).c_str());
    }

    // GT-9 byte accounting: bytesDropped + kept_payload_bytes +
    // sentinel_bytes == original_utf8_bytes (within ±1 for the
    // newline-snap rounding).
    {
        const QString raw = fixture(200, 100);
        const qint64 origBytes = raw.toUtf8().size();
        const auto r = RemoteControl::trimScrollbackForGetText(raw, 5 * 1024);
        expect(r.truncated, "GT-9 cap tripped");
        // The sentinel is the first line of r.text up through the
        // first '\n'.
        const int nl = r.text.indexOf('\n');
        const qint64 sentinelBytes =
            r.text.left(nl + 1).toUtf8().size();
        const qint64 keptBytes =
            r.text.mid(nl + 1).toUtf8().size();
        const qint64 acctTotal =
            r.bytesDropped + keptBytes + sentinelBytes - sentinelBytes;
        // Original bytes = dropped + kept (sentinel is added on top,
        // so it doesn't enter the conservation equation).
        expect(r.bytesDropped + keptBytes == origBytes,
               (std::string("GT-9 bytes_dropped (")
                + std::to_string(r.bytesDropped) + ") + kept ("
                + std::to_string(keptBytes) + ") == orig ("
                + std::to_string(origBytes) + ")").c_str());
        (void)acctTotal;
    }

    // GT-10 multi-byte UTF-8 cut safety. Scrollback contains CJK +
    // emoji; the \n snap means we never split a multi-byte sequence,
    // so the result must round-trip through QString::toUtf8 without
    // U+FFFD replacement characters appearing in the kept tail.
    {
        QString raw;
        for (int i = 0; i < 500; ++i) {
            raw += QStringLiteral("hello 日本語 🦀 line ");
            raw += QString::number(i);
            raw += '\n';
        }
        const auto r =
            RemoteControl::trimScrollbackForGetText(raw, 2 * 1024);
        expect(r.truncated, "GT-10 cap tripped");
        // Kept tail (post-sentinel) must contain the proper CJK
        // codepoints, no replacement chars.
        const int nl = r.text.indexOf('\n');
        const QString kept = r.text.mid(nl + 1);
        expect(!kept.contains(QChar(0xFFFD)),
               "GT-10 no U+FFFD in kept tail");
        expect(kept.contains(QStringLiteral("日本語")),
               "GT-10 CJK round-trip preserved");
    }
}

// ---- Wiring tests (source-grep) -----------------------------------

void testWiring() {
    const std::string rc = ants_test::slurpFile(SRC_RC_CPP);
    const std::string hdr = ants_test::slurpFile(SRC_RC_HEADER);

    // Anchor on the cmdGetText body in remotecontrol.cpp.
    const size_t gtPos = rc.find("RemoteControl::cmdGetText");
    expect(gtPos != std::string::npos,
           "wiring anchor: cmdGetText definition present");
    if (gtPos == std::string::npos) return;
    const std::string body = rc.substr(gtPos, 3000);

    // WI-1 — cmdGetText body references "max_bytes" exactly once
    // (the optional new request field).
    {
        size_t count = 0;
        size_t pos = 0;
        const std::string needle = "\"max_bytes\"";
        while ((pos = body.find(needle, pos)) != std::string::npos) {
            ++count; pos += needle.size();
        }
        expect(count == 1,
               (std::string("WI-1 expected 1 'max_bytes' literal in "
                            "cmdGetText body, found ")
                + std::to_string(count)).c_str());
    }

    // WI-2 — default + ceiling constants present in remotecontrol.h
    // (the helper lives in the header inline alongside
    // filterControlChars).
    {
        expect(hdr.find("1 * 1024 * 1024") != std::string::npos
               || hdr.find("1048576") != std::string::npos,
               "WI-2a 1 MiB default cap constant present in header");
        expect(hdr.find("16 * 1024 * 1024") != std::string::npos
               || hdr.find("16777216") != std::string::npos,
               "WI-2b 16 MiB ceiling constant present in header");
    }

    // WI-3 — cmdGetText body sets out["truncated"] so the field is
    // always emitted.
    {
        expect(body.find("\"truncated\"") != std::string::npos,
               "WI-3 cmdGetText sets out[\"truncated\"]");
    }
}

int runMain() {
    expect_reset();
    testEngine();
    testWiring();
    return expect_finish();
}

}  // namespace

TEST(RcGetTextByteCap, Main) { ASSERT_EQ(0, runMain()); }
