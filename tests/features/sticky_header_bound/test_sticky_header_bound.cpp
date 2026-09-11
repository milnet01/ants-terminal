// Feature-conformance test for spec.md — ANTS-5027.
//
// Why this exists: TerminalWidget::paintEvent rebuilds the pinned OSC 133
// command header by joining lineText() for every line from a prompt
// region's A marker to its B marker, and truncates to maxChars only AFTER
// the whole span is read. B lands wherever the cursor is, and the forgery
// check is off unless ANTS_OSC133_KEY is set, so any printed output can
// stretch the span over the whole scrollback — every cell of every line in
// the span, per paint, on the GUI thread. This test locks
// src/stickycommandtext.h's own contract: lineAt is called a bounded number
// of times regardless of span length, and output for text that already
// fits is unchanged by the bound.
//
// Behavioural, not a source scrape: stickyCommandText is pure and takes
// lineAt as an injected callback, so the call count the defect is about is
// directly observable — no need to reach into TerminalWidget::paintEvent.

#include "../../_support/expect.h"
ANTS_TEST_SCOPE();

#include <gtest/gtest.h>

#include <QChar>
#include <QString>
#include <QStringList>

#include "stickycommandtext.h"

namespace {

// A lineAt callback that counts calls and serves from a QStringList,
// returning an empty string past the end.
struct CountingLineSource {
    QStringList lines;
    int calls = 0;

    QString operator()(int gl) {
        ++calls;
        if (gl < 0 || gl >= lines.size()) return QString();
        return lines.at(gl);
    }
};

void runChecks() {
    // ANTS-5027-INV-1a — bounded read over a large non-empty span.
    {
        QStringList lines;
        lines.reserve(100000);
        for (int i = 0; i < 100000; ++i) lines << QStringLiteral("x");
        CountingLineSource src{lines};
        const int maxChars = 80;
        const QString result = stickyCommandText(
            0, lines.size() - 1, maxChars,
            [&src](int gl) { return src(gl); });
        expect(src.calls <= maxChars + 2, "ANTS-5027-INV-1/nonblank-calls",
               QStringLiteral("calls=%1 want<=%2 (result.length=%3)")
                   .arg(src.calls)
                   .arg(maxChars + 2)
                   .arg(result.length()));
    }

    // ANTS-5027-INV-1b — bounded read over a large blank span; result empty.
    {
        QStringList lines;
        lines.reserve(100000);
        for (int i = 0; i < 100000; ++i) lines << QString();
        CountingLineSource src{lines};
        const int maxChars = 80;
        const QString result = stickyCommandText(
            0, lines.size() - 1, maxChars,
            [&src](int gl) { return src(gl); });
        expect(src.calls <= maxChars + 2, "ANTS-5027-INV-1/blank-calls",
               QStringLiteral("calls=%1 want<=%2")
                   .arg(src.calls)
                   .arg(maxChars + 2));
        expect(result.isEmpty(), "ANTS-5027-INV-1/blank-result",
               QStringLiteral("result=%1 (len=%2)")
                   .arg(result)
                   .arg(result.length()));
    }

    // ANTS-5027-INV-2 — output unchanged for text that fits (and for the
    // same text truncated), independent of the bound.
    {
        QStringList lines{QStringLiteral("git"), QStringLiteral("commit"),
                           QStringLiteral("-m"), QStringLiteral("msg")};

        {
            CountingLineSource src{lines};
            const QString result = stickyCommandText(
                0, lines.size() - 1, 80,
                [&src](int gl) { return src(gl); });
            const QString want = QStringLiteral("git commit -m msg");
            expect(result == want, "ANTS-5027-INV-2/fits",
                   QStringLiteral("got=[%1] want=[%2]").arg(result, want));
        }
        {
            CountingLineSource src{lines};
            const QString result = stickyCommandText(
                0, lines.size() - 1, 10,
                [&src](int gl) { return src(gl); });
            // first 9 chars of "git commit -m msg" + U+2026, built via
            // QChar rather than a raw literal (QStringLiteral with a raw
            // non-ASCII/emoji literal is a known encoding trap on this
            // project).
            const QString want =
                QStringLiteral("git commi") + QChar(0x2026);
            expect(result == want, "ANTS-5027-INV-2/truncated",
                   QStringLiteral("got=[%1] want=[%2]").arg(result, want));
        }
    }

    // ANTS-5027-INV-3 — trailing blank lines do not change the result, and
    // reading them stays bounded.
    {
        QStringList lines{QStringLiteral("ls"), QStringLiteral("-la")};
        for (int i = 0; i < 5000; ++i) lines << QString();
        CountingLineSource src{lines};
        const int maxChars = 80;
        const QString result = stickyCommandText(
            0, lines.size() - 1, maxChars,
            [&src](int gl) { return src(gl); });
        const QString want = QStringLiteral("ls -la");
        expect(result == want, "ANTS-5027-INV-3/result",
               QStringLiteral("got=[%1] want=[%2]").arg(result, want));
        expect(src.calls <= maxChars + 2, "ANTS-5027-INV-3/calls",
               QStringLiteral("calls=%1 want<=%2")
                   .arg(src.calls)
                   .arg(maxChars + 2));
    }

    // ANTS-5027-INV-4 — each line trimmed, lines join with exactly one
    // space.
    {
        QStringList lines{QStringLiteral("  a  "), QStringLiteral("  b  ")};
        CountingLineSource src{lines};
        const QString result = stickyCommandText(
            0, lines.size() - 1, 80,
            [&src](int gl) { return src(gl); });
        const QString want = QStringLiteral("a b");
        expect(result == want, "ANTS-5027-INV-4",
               QStringLiteral("got=[%1] want=[%2]").arg(result, want));
    }

    // ANTS-5027-INV-5 — a non-positive maxChars never truncates (current,
    // pre-fix semantics; the fix bounds the READ, not this rule).
    {
        QStringList lines{QStringLiteral("hello"), QStringLiteral("world")};
        const QString want = QStringLiteral("hello world");

        CountingLineSource srcZero{lines};
        const QString resultZero = stickyCommandText(
            0, lines.size() - 1, 0,
            [&srcZero](int gl) { return srcZero(gl); });
        expect(resultZero == want, "ANTS-5027-INV-5/zero",
               QStringLiteral("got=[%1] want=[%2]").arg(resultZero, want));

        CountingLineSource srcNeg{lines};
        const QString resultNeg = stickyCommandText(
            0, lines.size() - 1, -5,
            [&srcNeg](int gl) { return srcNeg(gl); });
        expect(resultNeg == want, "ANTS-5027-INV-5/negative",
               QStringLiteral("got=[%1] want=[%2]").arg(resultNeg, want));
    }
}

int runMain() {
    expect_reset();
    runChecks();
    return expect_finish();
}

}  // namespace

TEST(StickyHeaderBound, Main) { ASSERT_EQ(0, runMain()); }
