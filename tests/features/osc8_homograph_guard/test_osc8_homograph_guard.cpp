// Feature-conformance test for spec.md (ANTS-2119 terminalwidget M2).
//
// Pure classification of the OSC 8 phishing guard — no widget, no modal dialog.
// classifyHyperlink is a static policy function split out of openHyperlink.

#include <gtest/gtest.h>
#include "terminalwidget.h"

#include <QString>

namespace {
using HW = TerminalWidget::HyperlinkWarning;

HW classify(const char *label, const char *url) {
    return TerminalWidget::classifyHyperlink(QString::fromUtf8(label),
                                             QString::fromUtf8(url));
}
}  // namespace

// INV-1 — label host matches URL host → no warning (www. stripped).
TEST(Osc8HomographGuard, Inv1MatchingHostNoWarn) {
    EXPECT_EQ(classify("github.com", "https://github.com/anthropics"), HW::None);
    EXPECT_EQ(classify("www.github.com", "https://github.com/x"), HW::None);
    EXPECT_EQ(classify("github.com", "https://www.github.com/x"), HW::None);
}

// INV-2 — label looks like host A, URL is host B → LabelHostMismatch.
TEST(Osc8HomographGuard, Inv2MismatchedHostWarns) {
    EXPECT_EQ(classify("github.com", "https://evil.example/login"),
              HW::LabelHostMismatch);
    // Embedded host in a longer label still counts.
    EXPECT_EQ(classify("visit github.com now", "https://evil.example/x"),
              HW::LabelHostMismatch);
}

// INV-3 — benign descriptive label (no host token) over a benign host → no
// warning (no confirmation fatigue). NB: a label that itself looks like a host
// (e.g. "report.pdf" — dot + 2+ letters) is still treated as a host claim by the
// existing regex and can mismatch; that pre-existing ambiguity is out of scope
// for this M2 fix, which only broadens the label-*less* case.
TEST(Osc8HomographGuard, Inv3BenignLabelBenignHostNoWarn) {
    EXPECT_EQ(classify("Download", "https://github.com/releases"), HW::None);
    EXPECT_EQ(classify("click here", "https://docs.example.org/guide"), HW::None);
    EXPECT_EQ(classify("Open the release", "https://github.com/x"), HW::None);
}

// INV-4 — non-host label + punycode/IDN destination → SuspiciousDestination.
TEST(Osc8HomographGuard, Inv4PunycodeDestinationWarns) {
    EXPECT_EQ(classify("Download", "https://xn--pypal-4ve.com/login"),
              HW::SuspiciousDestination);
}

// INV-5 — non-host label + bare public IP → SuspiciousDestination; loopback and
// link-local stay None so dev-server links don't nag.
TEST(Osc8HomographGuard, Inv5RawIpDestination) {
    EXPECT_EQ(classify("click here", "http://203.0.113.7/payload"),
              HW::SuspiciousDestination);
    EXPECT_EQ(classify("open", "http://127.0.0.1:8888/notebook"), HW::None);
    EXPECT_EQ(classify("local", "http://[::1]:3000/"), HW::None);
    EXPECT_EQ(classify("dev", "http://169.254.10.1/"), HW::None);  // link-local
}

// INV-6 — no host at all (mailto:) → None.
TEST(Osc8HomographGuard, Inv6NoHostNoWarn) {
    EXPECT_EQ(classify("email me", "mailto:someone@example.com"), HW::None);
}
