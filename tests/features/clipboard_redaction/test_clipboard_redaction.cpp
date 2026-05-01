// Feature-conformance test for tests/features/clipboard_redaction/spec.md.
//
// Locks ANTS-1014 — clipboard-write redaction funnel. Drives the
// pure clipboardguard::sanitize helper across the three Source
// shapes + source-greps terminalwidget.cpp / mainwindow.cpp for
// the absence of raw setText calls and the presence of
// well-classified clipboardguard::writeText sites.
//
// Exit 0 = all invariants hold.

#include "clipboardguard.h"

#include <QString>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

}  // namespace

int main() {
    // INV-1: helper shape exists in the header (we're including
    // it, so a structural check via behavioural drive is enough,
    // but also assert via slurp that the enum class and namespace
    // declarations are present so a refactor doesn't rename the
    // surface from under us).
    {
        const std::string hdr = slurp(CLIPBOARDGUARD_H);
        if (hdr.empty()) return fail("INV-1", "clipboardguard.h not readable");
        if (!contains(hdr, "namespace clipboardguard"))
            return fail("INV-1", "namespace clipboardguard missing in header");
        if (!contains(hdr, "enum class Source"))
            return fail("INV-1", "enum class Source missing");
        if (!contains(hdr, "Trusted") ||
            !contains(hdr, "UntrustedPty") ||
            !contains(hdr, "UntrustedPlugin"))
            return fail("INV-1", "Source enum missing one of Trusted / UntrustedPty / UntrustedPlugin");
        if (!contains(hdr, "sanitize"))
            return fail("INV-1", "sanitize free function missing");
    }

    // INV-2: NUL stripping on Trusted.
    {
        QString in;
        in.append(QLatin1Char('a'));
        in.append(QChar(0));
        in.append(QLatin1Char('b'));
        in.append(QChar(0));
        in.append(QLatin1Char('c'));
        const QString out = clipboardguard::sanitize(in, clipboardguard::Source::Trusted);
        if (out != QStringLiteral("abc"))
            return fail("INV-2",
                "Trusted sanitise should strip QChar(0) — expected \"abc\"");
    }

    // INV-3: UntrustedPty truncates at 1 MiB.
    {
        const int twoMiB = 2 * 1024 * 1024;
        QString in(twoMiB, QLatin1Char('A'));
        const QString out = clipboardguard::sanitize(
            in, clipboardguard::Source::UntrustedPty);
        if (out.size() != clipboardguard::kUntrustedMaxBytes)
            return fail("INV-3",
                "UntrustedPty sanitise should cap at 1 MiB");
    }

    // INV-4: UntrustedPlugin applies the same cap.
    {
        const int twoMiB = 2 * 1024 * 1024;
        QString in(twoMiB, QLatin1Char('B'));
        const QString out = clipboardguard::sanitize(
            in, clipboardguard::Source::UntrustedPlugin);
        if (out.size() != clipboardguard::kUntrustedMaxBytes)
            return fail("INV-4",
                "UntrustedPlugin sanitise should cap at 1 MiB");
    }

    // INV-5: Trusted does NOT truncate.
    {
        const int twoMiB = 2 * 1024 * 1024;
        QString in(twoMiB, QLatin1Char('C'));
        const QString out = clipboardguard::sanitize(
            in, clipboardguard::Source::Trusted);
        if (out.size() != twoMiB)
            return fail("INV-5",
                "Trusted sanitise must pass large inputs through unchanged");
    }

    // INV-6: no raw clipboard writes survive in terminalwidget.cpp.
    {
        const std::string src = slurp(TERMINALWIDGET_CPP);
        if (src.empty()) return fail("INV-6", "terminalwidget.cpp not readable");
        if (contains(src, "QApplication::clipboard()->setText("))
            return fail("INV-6",
                "raw QApplication::clipboard()->setText( still in terminalwidget.cpp");
    }

    // INV-7: no raw clipboard writes survive in mainwindow.cpp.
    {
        const std::string src = slurp(MAINWINDOW_CPP);
        if (src.empty()) return fail("INV-7", "mainwindow.cpp not readable");
        if (contains(src, "QApplication::clipboard()->setText("))
            return fail("INV-7",
                "raw QApplication::clipboard()->setText( still in mainwindow.cpp");
    }

    // INV-8: source classification at the two untrusted sites.
    {
        const std::string tw = slurp(TERMINALWIDGET_CPP);
        const std::string mw = slurp(MAINWINDOW_CPP);
        if (!contains(tw, "Source::UntrustedPty"))
            return fail("INV-8",
                "OSC 52 callback in terminalwidget.cpp does not classify as UntrustedPty");
        if (!contains(mw, "Source::UntrustedPlugin"))
            return fail("INV-8",
                "Lua plugin clipboard glue in mainwindow.cpp does not classify as UntrustedPlugin");
    }

    std::fprintf(stderr,
        "OK — clipboard redaction funnel INVs hold.\n");
    return 0;
}
