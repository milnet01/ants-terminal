// ANTS-5078 — feature-conformance test; see spec.md. Source scrapes: a write
// failure needs a full disk, and Share Block runs from a context menu on a
// live TerminalWidget.

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <string>

namespace {

const std::size_t npos = std::string::npos;

std::string exporterSource() {
    return ants_test::stripComments(ants_test::slurpFile(SRC_SCROLLBACKEXPORTER_CPP_PATH));
}

}  // namespace

// INV-1
TEST(TerminalWidgetExportSafety, Inv1ExporterReplacesTheFileOnlyOnSuccess) {
    const std::string header = ants_test::slurpFile(SRC_SCROLLBACKEXPORTER_H_PATH);
    ASSERT_FALSE(header.empty()) << "setup: scrollbackexporter.h was not found";
    EXPECT_NE(header.find("QSaveFile m_file"), npos)
        << "INV-1: the exporter does not write through QSaveFile";
    EXPECT_EQ(header.find("QFile m_file"), npos)
        << "INV-1: the exporter truncates the target with QFile";

    const std::string step = ants_test::slurpFunctionBody(
        exporterSource(), "ScrollbackExporter::Step ScrollbackExporter::step(");
    ASSERT_FALSE(step.empty()) << "setup: ScrollbackExporter::step was not found";
    const std::size_t commit = step.find("m_file.commit()");
    ASSERT_NE(commit, npos) << "INV-1: the export is never committed";
    EXPECT_EQ(step.find("m_file.commit()", commit + 1), npos)
        << "INV-1: the export commits in more than one place";
    EXPECT_NE(step.find("if (!ok || !m_file.commit())"), npos)
        << "INV-1: commit() is not gated on the last write";
}

// INV-2
TEST(TerminalWidgetExportSafety, Inv2ShortWriteFailsTheExport) {
    const std::string write = ants_test::slurpFunctionBody(
        exporterSource(), "bool ScrollbackExporter::write(");
    ASSERT_FALSE(write.empty()) << "setup: ScrollbackExporter::write was not found";
    EXPECT_NE(write.find("m_file.write(bytes) != bytes.size()"), npos)
        << "INV-2: a short write is not detected";
}

// INV-3
TEST(TerminalWidgetExportSafety, Inv3ShareBlockReportsAMissingBlock) {
    const std::string menu = ants_test::stripComments(ants_test::slurpFunctionBody(
        SRC_TERMINALWIDGET_PATH, "void TerminalWidget::contextMenuEvent("));
    const std::size_t at = menu.find("\"Share Block as .cast...\"");
    ASSERT_NE(at, npos) << "setup: the Share Block handler was not found";
    const std::string h = menu.substr(at, menu.find("menu.addSeparator()", at) - at);
    EXPECT_NE(h.find("if (idx < 0) {"), npos)
        << "INV-3: Share Block does not check the block still exists";
    EXPECT_NE(h.find("emit captureFailed("), npos)
        << "INV-3: a missing block is not reported";
}
