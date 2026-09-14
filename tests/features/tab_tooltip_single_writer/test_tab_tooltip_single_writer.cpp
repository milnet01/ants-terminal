// The tab tooltip has one writer — see spec.md. ANTS-5081.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gtest/gtest.h>


namespace {

QString sibling(const char *name) {
    const QDir src = QDir(QStringLiteral(SRC_DIR));
    QFile f(src.filePath(QString::fromLatin1(name)));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

QString functionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int brace = src.indexOf(QChar('{'), start);
    if (brace < 0) return QString();
    int depth = 1;
    int i = brace + 1;
    while (i < src.size() && depth > 0) {
        if (src.at(i) == QChar('{')) ++depth;
        else if (src.at(i) == QChar('}')) --depth;
        ++i;
    }
    return src.mid(brace, i - brace);
}

}  // namespace

// INV-1
TEST(TabTooltipSingleWriter, TabBarPaintWritesNoTooltip) {
    const QString body = functionBody(sibling("coloredtabbar.cpp"),
        QStringLiteral("void ColoredTabBar::paintEvent("));
    ASSERT_FALSE(body.isEmpty()) << "ColoredTabBar::paintEvent not found";
    EXPECT_FALSE(body.contains(QStringLiteral("setTabToolTip(")))
        << "paintEvent overwrites the controller's per-tool tab tooltip";
}

// INV-2
TEST(TabTooltipSingleWriter, ControllerWritesTheTooltip) {
    const QString src = sibling("claudestatuswidgets.cpp");
    ASSERT_FALSE(src.isEmpty()) << "claudestatuswidgets.cpp not readable";
    EXPECT_TRUE(src.contains(QStringLiteral("m_tabWidget->setTabToolTip(i, tip);")));
}
