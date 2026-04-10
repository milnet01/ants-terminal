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
    bool isUserTheme = false; // true if loaded from file
};

class Themes {
public:
    static const std::vector<Theme> &all();
    static const Theme &byName(const QString &name);
    static const Theme &defaultTheme();
    static QStringList names();

    // Load user themes from ~/.config/ants-terminal/themes/*.json
    static std::vector<Theme> loadUserThemes();
    // Reload all themes (built-in + user)
    static void reload();
};
