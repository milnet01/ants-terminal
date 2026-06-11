// Feature-conformance test for spec.md — ANTS-1324.
//
// Static source-grep test: enforces that mainwindow.cpp does NOT
// reintroduce the destroyed→m_allTerminals.removeIf pattern that
// trips UBSan -fsanitize=vptr (qpointer.h:75 downcast during
// ~QWidget()'s emit destroyed window), AND that liveTerminals()
// does its own null-compact pass, AND that m_allTerminals carries
// the `mutable` qualifier needed for the const accessor to compact.

#include "../../_support/expect.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_MAINWINDOW_CPP_PATH
#  error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_H_PATH
#  error "SRC_MAINWINDOW_H_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {




}  // namespace

TEST(QPointerDestroyedSafe, Main) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string hdr = ants_test::slurpFile(SRC_MAINWINDOW_H_PATH);

    // C-1 / I-5 — regression lock. No `&QObject::destroyed,` block
    // whose body reaches into `m_allTerminals.removeIf`. We don't
    // try to parse the lambda; we assert the unsafe textual shape
    // does not co-occur. Specifically, the dangerous pattern is:
    //
    //   connect(<x>, &QObject::destroyed, this, [<caps>](QObject *obj) {
    //       ...
    //       m_allTerminals.removeIf(...);
    //   });
    //
    // The simplest sufficient check: any line containing
    // `m_allTerminals.removeIf` MUST appear inside `liveTerminals()`,
    // NOT inside a destroyed lambda. We assert by counting:
    //   - total occurrences of `m_allTerminals.removeIf` == 1
    //     (the liveTerminals lazy-compact)
    //   - that single occurrence appears AFTER the
    //     `MainWindow::liveTerminals` definition line.
    {
        const std::string needle = "m_allTerminals.removeIf";
        size_t pos = 0;
        int count = 0;
        size_t lastAt = std::string::npos;
        while ((pos = cpp.find(needle, pos)) != std::string::npos) {
            ++count;
            lastAt = pos;
            ++pos;
        }
        expect(count == 1,
               "C-1/exactly-one-removeIf-call",
               std::string("found ") + std::to_string(count));

        const auto liveAt =
            cpp.find("MainWindow::liveTerminals(");
        expect(liveAt != std::string::npos,
               "C-1/liveTerminals-definition-present");
        if (liveAt != std::string::npos && lastAt != std::string::npos) {
            // The single removeIf must live inside the
            // liveTerminals() function body — i.e. after its
            // signature.
            expect(lastAt > liveAt,
                   "C-1/removeIf-lives-inside-liveTerminals");
        }
    }

    // C-1b — explicit regression-lock on the exact prior shape.
    // ANTS-1320 wrote: `connect(terminal, &QObject::destroyed,
    // this, [this](QObject *obj) { m_allTerminals.removeIf(...);
    // });` — assert that the destroyed→removeIf pair never co-occurs
    // within ~150 characters.
    {
        std::regex destroyedThenRemoveIf(
            R"(&QObject::destroyed[\s\S]{0,400}m_allTerminals\.removeIf)");
        expect(!std::regex_search(cpp, destroyedThenRemoveIf),
               "C-1b/no-destroyed-then-removeIf-within-400-chars");
    }

    // C-2 / I-3 — mutable qualifier on m_allTerminals so the
    // `const` liveTerminals can compact lazily.
    {
        std::regex mutableDecl(
            R"(mutable\s+QList\s*<\s*QPointer\s*<\s*TerminalWidget\s*>\s*>\s*m_allTerminals)");
        expect(std::regex_search(hdr, mutableDecl),
               "C-2/m_allTerminals-is-mutable");
    }

    // C-3 / I-2 — liveTerminals body contains the null-compact
    // call with a predicate that returns p.isNull().
    {
        std::regex compactCall(
            R"(m_allTerminals\.removeIf\([\s\S]{0,200}p\.isNull\(\))");
        expect(std::regex_search(cpp, compactCall),
               "C-3/liveTerminals-compacts-null-entries");
    }

    EXPECT_EQ(0, expect_failures());
}
