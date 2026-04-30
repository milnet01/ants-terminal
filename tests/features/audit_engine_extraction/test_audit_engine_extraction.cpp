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

#include <QCoreApplication>

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

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
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

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    const std::string engineHdr = slurp(SRC_AUDIT_ENGINE_H);
    const std::string engineSrc = slurp(SRC_AUDIT_ENGINE_CPP);
    const std::string dialogHdr = slurp(SRC_AUDIT_H);
    const std::string dialogSrc = slurp(SRC_AUDIT_CPP);

    if (engineHdr.empty()) return fail("INV-1", "auditengine.h not readable");
    if (engineSrc.empty()) return fail("INV-2", "auditengine.cpp not readable");
    if (dialogHdr.empty()) return fail("INV-5", "auditdialog.h not readable");
    if (dialogSrc.empty()) return fail("INV-6", "auditdialog.cpp not readable");

    // INV-1: auditengine.h includes only Qt6::Core headers.
    if (hasGuiInclude(engineHdr))
        return fail("INV-1",
                    "auditengine.h includes a Qt6::Gui or Qt6::Widgets header");

    // INV-2: auditengine.cpp includes only Qt6::Core headers.
    if (hasGuiInclude(engineSrc))
        return fail("INV-2",
                    "auditengine.cpp includes a Qt6::Gui or Qt6::Widgets header");

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
            return fail("INV-3", t);
    }

    // INV-4: free functions inside namespace AuditEngine.
    if (!contains(engineHdr, "namespace AuditEngine"))
        return fail("INV-4",
                    "auditengine.h missing namespace AuditEngine declaration");
    const char *funcs[] = {
        "FilterResult applyFilter",
        "QList<Finding> parseFindings",
        "void capFindings",
    };
    for (const char *f : funcs) {
        if (!contains(engineHdr, f))
            return fail("INV-4", f);
    }

    // INV-5: auditdialog.h includes auditengine.h.
    if (!contains(dialogHdr, "#include \"auditengine.h\""))
        return fail("INV-5",
                    "auditdialog.h must #include auditengine.h");

    // INV-6: auditdialog.cpp no longer owns the moved bodies.
    if (contains(dialogSrc,
            "AuditDialog::FilterResult AuditDialog::applyFilter"))
        return fail("INV-6",
                    "auditdialog.cpp still defines AuditDialog::applyFilter — "
                    "should have moved to AuditEngine::applyFilter");
    if (contains(dialogSrc,
            "QList<Finding> AuditDialog::parseFindings"))
        return fail("INV-6",
                    "auditdialog.cpp still defines AuditDialog::parseFindings — "
                    "should have moved to AuditEngine::parseFindings");
    if (contains(dialogSrc,
            "void AuditDialog::capFindings"))
        return fail("INV-6",
                    "auditdialog.cpp still defines AuditDialog::capFindings — "
                    "should have moved to AuditEngine::capFindings");

    // INV-7: orchestration calls the engine.
    const char *callSites[] = {
        "AuditEngine::applyFilter",
        "AuditEngine::parseFindings",
        "AuditEngine::capFindings",
    };
    for (const char *c : callSites) {
        if (!contains(dialogSrc, c))
            return fail("INV-7", c);
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
            return fail("INV-8", "expected 3 findings retained after cap");
        if (r.omittedCount != 7)
            return fail("INV-8",
                        "expected omittedCount=7 (10 - 3 trimmed)");
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
            return fail("INV-9", "expected exactly one finding");
        const Finding &f = findings.first();
        if (f.file != QStringLiteral("src/foo.cpp"))
            return fail("INV-9", "expected file = src/foo.cpp");
        if (f.line != 42)
            return fail("INV-9", "expected line = 42");
        if (f.dedupKey.size() != 24)
            return fail("INV-9",
                        "expected dedupKey to be 24 hex chars (96 bits)");
        // Hex character check.
        std::regex hexOnly(R"(^[0-9a-f]{24}$)");
        if (!std::regex_match(f.dedupKey.toStdString(), hexOnly))
            return fail("INV-9",
                        "dedupKey not lowercase 24-char hex");
    }

    std::puts("OK audit_engine_extraction: 9/9 invariants");
    return 0;
}
