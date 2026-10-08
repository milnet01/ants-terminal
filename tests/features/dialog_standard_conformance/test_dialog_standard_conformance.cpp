// Feature-conformance test for ANTS-5109's dialog-standard findings:
// Review Changes has a minimum size (D2), Settings and Review Changes take
// status colours from the theme (D1), and Settings' colour pickers have a
// parent. Source scrape only. See spec.md.

#include <regex>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_DIFFVIEWER_CPP_PATH
#error "SRC_DIFFVIEWER_CPP_PATH compile definition required"
#endif
#ifndef SRC_SETTINGSDIALOG_CPP_PATH
#error "SRC_SETTINGSDIALOG_CPP_PATH compile definition required"
#endif

namespace {

const std::regex kHexColour(R"(color:\s*#[0-9a-fA-F]{6})");

}  // namespace

TEST(DialogStandardConformance, ReviewChangesHasMinimumSize) {
    const std::string dv = ants_test::slurpFile(SRC_DIFFVIEWER_CPP_PATH);
    ASSERT_FALSE(dv.empty());
    EXPECT_NE(dv.find("dialog->setMinimumSize("), std::string::npos)
        << "[INV-1] Review Changes sets no minimum size";
}

TEST(DialogStandardConformance, NoLiteralStatusColours) {
    for (const char *path : {SRC_DIFFVIEWER_CPP_PATH, SRC_SETTINGSDIALOG_CPP_PATH}) {
        const std::string src = ants_test::slurpFile(path);
        ASSERT_FALSE(src.empty()) << path;
        std::smatch m;
        EXPECT_FALSE(std::regex_search(src, m, kHexColour))
            << "[INV-2] " << path << " has a literal colour `" << m.str() << "`";
    }
}

TEST(DialogStandardConformance, ColourPickersHaveAParent) {
    const std::string sd = ants_test::slurpFile(SRC_SETTINGSDIALOG_CPP_PATH);
    ASSERT_FALSE(sd.empty());
    const std::regex unparented(R"(QColorDialog::getColor\([^;]*?,\s*nullptr\s*,)");
    std::smatch m;
    EXPECT_FALSE(std::regex_search(sd, m, unparented))
        << "[INV-3] unparented colour picker: `" << m.str() << "`";
}
