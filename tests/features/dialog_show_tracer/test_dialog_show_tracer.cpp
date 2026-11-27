// The dialog-show trace line — see spec.md. ANTS-6633.

#include "dialogshowtracer.h"

#include <QString>
#include <QWidget>

#include <gtest/gtest.h>

#include <ios>

// INV-1 — a title's %N is kept as written.
TEST(DialogShowTracer, TitlePlaceholdersStayLiteral) {
    QWidget w;
    w.setWindowTitle(QStringLiteral("a%1b%9c"));
    w.setGeometry(10, 20, 300, 200);
    const QString line = DialogShowTracer::showLine(&w);
    EXPECT_TRUE(line.contains(QStringLiteral("title=\"a%1b%9c\"")))
        << line.toStdString();
    EXPECT_TRUE(line.contains(QStringLiteral("geom=300x200+10+20")))
        << line.toStdString();
}

// INV-2 — one Show is one line.
TEST(DialogShowTracer, ControlCharactersAreEscaped) {
    QWidget w;
    w.setWindowTitle(QStringLiteral("top\nfake line\r\x1b[0m"));
    const QString line = DialogShowTracer::showLine(&w);
    for (const QChar c : line)
        EXPECT_NE(c.category(), QChar::Other_Control)
            << "raw control U+" << std::hex << int(c.unicode()) << " in " << line.toStdString();
    EXPECT_TRUE(line.contains(QStringLiteral("top\\x0afake line\\x0d\\x1b[0m")))
        << line.toStdString();
}
