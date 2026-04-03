#pragma once

#include <QColor>
#include <QString>
#include <vector>

struct Theme {
    QString name;
    QColor bgPrimary;
    QColor bgSecondary;
    QColor textPrimary;
    QColor textSecondary;
    QColor accent;
    QColor border;
    QColor cursor;
    // ANSI color overrides (indices 0-15)
    QColor ansi[16];
};

class Themes {
public:
    static const std::vector<Theme> &all();
    static const Theme &byName(const QString &name);
    static const Theme &defaultTheme();
    static QStringList names();
};
