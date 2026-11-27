// ANTS-6979 — Settings warns about a Highlights or Triggers pattern it would
// not save, and saves nothing. Contract: spec.md beside this file.

#include "settingsdialog.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>

#ifndef SRC_SETTINGSDIALOG_CPP_PATH
#  error "SRC_SETTINGSDIALOG_CPP_PATH compile definition required"
#endif

// INV-1
TEST(SettingsBadPatternWarning, EachBadPatternIsNamed) {
    const QString broken = QStringLiteral("(foo");
    const QString runaway = QStringLiteral("(.+)+");
    const QStringList bad = SettingsDialog::unusablePatterns(
        QStringLiteral("Highlights"),
        {QStringLiteral("error"), QString(), broken, runaway});
    ASSERT_EQ(bad.size(), 2) << bad.join(QChar('\n')).toStdString();
    EXPECT_TRUE(bad[0].contains(QStringLiteral("Highlights")));
    EXPECT_TRUE(bad[0].contains(broken));
    EXPECT_TRUE(bad[0].contains(QRegularExpression(broken).errorString()))
        << bad[0].toStdString();
    EXPECT_TRUE(bad[1].contains(runaway));
    EXPECT_TRUE(bad[1].contains(QStringLiteral("freeze"))) << bad[1].toStdString();

    EXPECT_TRUE(SettingsDialog::unusablePatterns(
        QStringLiteral("Triggers"), {QStringLiteral("^\\$ "), QString()}).isEmpty());
}

// INV-2
TEST(SettingsBadPatternWarning, OkAndApplySaveNothingWhileOneIsBad) {
    const std::string src = ants_test::stripComments(
        ants_test::slurpFile(SRC_SETTINGSDIALOG_CPP_PATH));
    const size_t a = src.find("&QDialogButtonBox::accepted");
    ASSERT_NE(a, std::string::npos);
    const size_t b = src.find("mainLayout->addWidget(buttonBox)", a);
    ASSERT_NE(b, std::string::npos);
    const std::string handlers = src.substr(a, b - a);
    const std::string guard = "if (!patternsSaveable()) return;";
    const size_t okGuard = handlers.find(guard);
    ASSERT_NE(okGuard, std::string::npos) << "OK saves without checking the patterns";
    EXPECT_LT(okGuard, handlers.find("applySettings();"));
    const size_t apply = handlers.find("QDialogButtonBox::Apply");
    ASSERT_NE(apply, std::string::npos);
    const size_t applyGuard = handlers.find(guard, apply);
    ASSERT_NE(applyGuard, std::string::npos) << "Apply saves without checking the patterns";
    EXPECT_LT(applyGuard, handlers.find("applySettings();", apply));

    const std::string check =
        ants_test::slurpFunctionBody(src, "bool SettingsDialog::patternsSaveable(");
    ASSERT_FALSE(check.empty());
    EXPECT_NE(check.find("QMessageBox::Warning"), std::string::npos);
    EXPECT_NE(check.find("setTextFormat(Qt::PlainText)"), std::string::npos);
    EXPECT_NE(check.find("Settings not saved"), std::string::npos);
    EXPECT_NE(check.find("unusablePatterns("), std::string::npos);
}
