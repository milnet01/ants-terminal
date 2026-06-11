// Feature-conformance test for tests/features/audit_engine_extraction/spec.md.
//
// Verifies the ANTS-1119 v1 audit-engine extraction:
//   - auditengine.{h,cpp} compile against Qt6::Core only (source-grep)
//   - the v1 free functions are declared in namespace AuditEngine
//   - auditdialog.{h,cpp} forward to the engine instead of owning bodies
//   - capFindings + parseFindings produce the expected output shapes
//
// Exit 0 = all 9 invariants hold.

#include "auditengine.h"

#include <gtest/gtest.h>


#include <cstdio>
#include <fstream>
#include <regex>
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

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

void fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
}

// Reject any include line for a known QtGui/QtWidgets header.
bool hasGuiInclude(const std::string &src) {
    static const char *kGuiHeaders[] = {
        "<QWidget>", "<QDialog>", "<QPainter>", "<QPushButton>",
        "<QTextBrowser>", "<QPaintEvent>", "<QStyleOption",
        "<QApplication>", "<QGuiApplication>", "<QImage>",
        "<QPixmap>", "<QFont>", "<QFontMetrics>",
    };
    for (const char *h : kGuiHeaders) {
        if (contains(src, h)) return true;
    }
    return false;
}

}  // namespace

TEST(AuditEngineExtraction, Main) {

    const std::string engineHdr = slurp(SRC_AUDIT_ENGINE_H);
    const std::string engineSrc = slurp(SRC_AUDIT_ENGINE_CPP);
    const std::string dialogHdr = slurp(SRC_AUDIT_H);
    const std::string dialogSrc = slurp(SRC_AUDIT_CPP);

    if (engineHdr.empty()) { fail("INV-1", "auditengine.h not readable"); FAIL(); return; }
    if (engineSrc.empty()) { fail("INV-2", "auditengine.cpp not readable"); FAIL(); return; }
    if (dialogHdr.empty()) { fail("INV-5", "auditdialog.h not readable"); FAIL(); return; }
    if (dialogSrc.empty()) { fail("INV-6", "auditdialog.cpp not readable"); FAIL(); return; }

    // INV-1: auditengine.h includes only Qt6::Core headers.
    if (hasGuiInclude(engineHdr))
        { fail("INV-1",
                    "auditengine.h includes a Qt6::Gui or Qt6::Widgets header"); FAIL(); return; }

    // INV-2: auditengine.cpp includes only Qt6::Core headers.
    if (hasGuiInclude(engineSrc))
        { fail("INV-2",
                    "auditengine.cpp includes a Qt6::Gui or Qt6::Widgets header"); FAIL(); return; }

    // INV-3: data types are declared in auditengine.h.
    const char *types[] = {
        "enum class CheckType",
        "enum class Severity",
        "struct OutputFilter",
        "struct AuditCheck",
        "struct Finding",
        "struct CheckResult",
    };
    for (const char *t : types) {
        if (!contains(engineHdr, t))
            { fail("INV-3", t); FAIL(); return; }
    }

    // INV-4: free functions inside namespace AuditEngine.
    if (!contains(engineHdr, "namespace AuditEngine"))
        { fail("INV-4",
                    "auditengine.h missing namespace AuditEngine declaration"); FAIL(); return; }
    const char *funcs[] = {
        "FilterResult applyFilter",
        "QList<Finding> parseFindings",
        "void capFindings",
    };
    for (const char *f : funcs) {
        if (!contains(engineHdr, f))
            { fail("INV-4", f); FAIL(); return; }
    }

    // INV-5: auditdialog.h includes auditengine.h.
    if (!contains(dialogHdr, "#include \"auditengine.h\""))
        { fail("INV-5",
                    "auditdialog.h must #include auditengine.h"); FAIL(); return; }

    // INV-6: auditdialog.cpp no longer owns the moved bodies.
    if (contains(dialogSrc,
            "AuditDialog::FilterResult AuditDialog::applyFilter"))
        { fail("INV-6",
                    "auditdialog.cpp still defines AuditDialog::applyFilter — "
                    "should have moved to AuditEngine::applyFilter"); FAIL(); return; }
    if (contains(dialogSrc,
            "QList<Finding> AuditDialog::parseFindings"))
        { fail("INV-6",
                    "auditdialog.cpp still defines AuditDialog::parseFindings — "
                    "should have moved to AuditEngine::parseFindings"); FAIL(); return; }
    if (contains(dialogSrc,
            "void AuditDialog::capFindings"))
        { fail("INV-6",
                    "auditdialog.cpp still defines AuditDialog::capFindings — "
                    "should have moved to AuditEngine::capFindings"); FAIL(); return; }

    // INV-7: orchestration calls the engine.
    const char *callSites[] = {
        "AuditEngine::applyFilter",
        "AuditEngine::parseFindings",
        "AuditEngine::capFindings",
    };
    for (const char *c : callSites) {
        if (!contains(dialogSrc, c))
            { fail("INV-7", c); FAIL(); return; }
    }

    // INV-8: capFindings behaviour.
    {
        CheckResult r;
        r.checkId = QStringLiteral("probe");
        for (int i = 0; i < 10; ++i) {
            Finding f;
            f.checkId = r.checkId;
            f.line = i + 1;
            r.findings.append(f);
        }
        AuditEngine::capFindings(r, 3);
        if (r.findings.size() != 3)
            { fail("INV-8", "expected 3 findings retained after cap"); FAIL(); return; }
        if (r.omittedCount != 7)
            { fail("INV-8",
                        "expected omittedCount=7 (10 - 3 trimmed)"); FAIL(); return; }
    }

    // INV-9: parseFindings behaviour against a synthetic input.
    {
        AuditCheck check;
        check.id = QStringLiteral("clang_tidy");
        check.name = QStringLiteral("clang-tidy");
        check.category = QStringLiteral("C/C++");
        check.severity = Severity::Major;
        check.type = CheckType::Bug;

        const QString body = QStringLiteral(
            "src/foo.cpp:42:7: warning: bogus thing [-Wunused]");
        const auto findings = AuditEngine::parseFindings(body, check);
        if (findings.size() != 1)
            { fail("INV-9", "expected exactly one finding"); FAIL(); return; }
        const Finding &f = findings.first();
        if (f.file != QStringLiteral("src/foo.cpp"))
            { fail("INV-9", "expected file = src/foo.cpp"); FAIL(); return; }
        if (f.line != 42)
            { fail("INV-9", "expected line = 42"); FAIL(); return; }
        if (f.dedupKey.size() != 24)
            { fail("INV-9",
                        "expected dedupKey to be 24 hex chars (96 bits)"); FAIL(); return; }
        // Hex character check.
        std::regex hexOnly(R"(^[0-9a-f]{24}$)");
        if (!std::regex_match(f.dedupKey.toStdString(), hexOnly))
            { fail("INV-9",
                        "dedupKey not lowercase 24-char hex"); FAIL(); return; }
    }

}
