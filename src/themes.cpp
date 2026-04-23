#include "themes.h"
#include <QDebug>
#include <QStringList>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>

static std::vector<Theme> buildThemes() {
    std::vector<Theme> t;

    // --- Dark (Catppuccin Mocha) ---
    {
        Theme th;
        th.name = "Dark";
        th.bgPrimary   = QColor(0x1E, 0x1E, 0x2E);
        th.bgSecondary = QColor(0x18, 0x18, 0x25);
        th.textPrimary = QColor(0xCD, 0xD6, 0xF4);
        th.textSecondary = QColor(0xA6, 0xAD, 0xC8);
        th.accent      = QColor(0x89, 0xB4, 0xFA);
        th.border      = QColor(0x45, 0x47, 0x5A);
        th.cursor      = QColor(0x89, 0xB4, 0xFA);
        th.ansi[0]  = QColor(0x45, 0x47, 0x5A); th.ansi[8]  = QColor(0x58, 0x5B, 0x70);
        th.ansi[1]  = QColor(0xF3, 0x8B, 0xA8); th.ansi[9]  = QColor(0xF3, 0x8B, 0xA8);
        th.ansi[2]  = QColor(0xA6, 0xE3, 0xA1); th.ansi[10] = QColor(0xA6, 0xE3, 0xA1);
        th.ansi[3]  = QColor(0xF9, 0xE2, 0xAF); th.ansi[11] = QColor(0xF9, 0xE2, 0xAF);
        th.ansi[4]  = QColor(0x89, 0xB4, 0xFA); th.ansi[12] = QColor(0x89, 0xB4, 0xFA);
        th.ansi[5]  = QColor(0xF5, 0xC2, 0xE7); th.ansi[13] = QColor(0xF5, 0xC2, 0xE7);
        th.ansi[6]  = QColor(0x94, 0xE2, 0xD5); th.ansi[14] = QColor(0x94, 0xE2, 0xD5);
        th.ansi[7]  = QColor(0xBA, 0xC2, 0xDE); th.ansi[15] = QColor(0xA6, 0xAD, 0xC8);
        t.push_back(th);
    }

    // --- Light ---
    {
        Theme th;
        th.name = "Light";
        th.bgPrimary   = QColor(0xFF, 0xFF, 0xFF);
        th.bgSecondary = QColor(0xF5, 0xF5, 0xF5);
        th.textPrimary = QColor(0x1E, 0x1E, 0x2E);
        th.textSecondary = QColor(0x4A, 0x4A, 0x5A);
        th.accent      = QColor(0x1A, 0x73, 0xE8);
        th.border      = QColor(0xD0, 0xD0, 0xD0);
        th.cursor      = QColor(0x1A, 0x73, 0xE8);
        th.ansi[0]  = QColor(0x00, 0x00, 0x00); th.ansi[8]  = QColor(0x55, 0x55, 0x55);
        th.ansi[1]  = QColor(0xC4, 0x1A, 0x16); th.ansi[9]  = QColor(0xDE, 0x38, 0x2B);
        th.ansi[2]  = QColor(0x2E, 0x7D, 0x32); th.ansi[10] = QColor(0x4E, 0x9A, 0x06);
        th.ansi[3]  = QColor(0xE6, 0x5E, 0x00); th.ansi[11] = QColor(0xFC, 0xE9, 0x4F);
        th.ansi[4]  = QColor(0x1A, 0x73, 0xE8); th.ansi[12] = QColor(0x36, 0x89, 0xE6);
        th.ansi[5]  = QColor(0x79, 0x5D, 0xAB); th.ansi[13] = QColor(0xAD, 0x7F, 0xA8);
        th.ansi[6]  = QColor(0x06, 0x98, 0x9A); th.ansi[14] = QColor(0x34, 0xE2, 0xE2);
        th.ansi[7]  = QColor(0xD3, 0xD7, 0xCF); th.ansi[15] = QColor(0xEE, 0xEE, 0xEC);
        t.push_back(th);
    }

    // --- Nord ---
    {
        Theme th;
        th.name = "Nord";
        th.bgPrimary   = QColor(0x2E, 0x34, 0x40);
        th.bgSecondary = QColor(0x27, 0x2C, 0x36);
        th.textPrimary = QColor(0xEC, 0xEF, 0xF4);
        th.textSecondary = QColor(0xD8, 0xDE, 0xE9);
        th.accent      = QColor(0x88, 0xC0, 0xD0);
        th.border      = QColor(0x43, 0x4C, 0x5E);
        th.cursor      = QColor(0x88, 0xC0, 0xD0);
        th.ansi[0]  = QColor(0x3B, 0x42, 0x52); th.ansi[8]  = QColor(0x4C, 0x56, 0x6A);
        th.ansi[1]  = QColor(0xBF, 0x61, 0x6A); th.ansi[9]  = QColor(0xBF, 0x61, 0x6A);
        th.ansi[2]  = QColor(0xA3, 0xBE, 0x8C); th.ansi[10] = QColor(0xA3, 0xBE, 0x8C);
        th.ansi[3]  = QColor(0xEB, 0xCB, 0x8B); th.ansi[11] = QColor(0xEB, 0xCB, 0x8B);
        th.ansi[4]  = QColor(0x81, 0xA1, 0xC1); th.ansi[12] = QColor(0x81, 0xA1, 0xC1);
        th.ansi[5]  = QColor(0xB4, 0x8E, 0xAD); th.ansi[13] = QColor(0xB4, 0x8E, 0xAD);
        th.ansi[6]  = QColor(0x88, 0xC0, 0xD0); th.ansi[14] = QColor(0x8F, 0xBC, 0xBB);
        th.ansi[7]  = QColor(0xE5, 0xE9, 0xF0); th.ansi[15] = QColor(0xEC, 0xEF, 0xF4);
        t.push_back(th);
    }

    // --- Dracula ---
    {
        Theme th;
        th.name = "Dracula";
        th.bgPrimary   = QColor(0x28, 0x2A, 0x36);
        th.bgSecondary = QColor(0x21, 0x22, 0x2C);
        th.textPrimary = QColor(0xF8, 0xF8, 0xF2);
        th.textSecondary = QColor(0xBF, 0xBF, 0xBF);
        th.accent      = QColor(0xBD, 0x93, 0xF9);
        th.border      = QColor(0x44, 0x47, 0x5A);
        th.cursor      = QColor(0xBD, 0x93, 0xF9);
        th.ansi[0]  = QColor(0x21, 0x22, 0x2C); th.ansi[8]  = QColor(0x62, 0x72, 0xA4);
        th.ansi[1]  = QColor(0xFF, 0x55, 0x55); th.ansi[9]  = QColor(0xFF, 0x6E, 0x6E);
        th.ansi[2]  = QColor(0x50, 0xFA, 0x7B); th.ansi[10] = QColor(0x69, 0xFF, 0x94);
        th.ansi[3]  = QColor(0xF1, 0xFA, 0x8C); th.ansi[11] = QColor(0xFF, 0xFF, 0xA5);
        th.ansi[4]  = QColor(0xBD, 0x93, 0xF9); th.ansi[12] = QColor(0xD6, 0xAC, 0xFF);
        th.ansi[5]  = QColor(0xFF, 0x79, 0xC6); th.ansi[13] = QColor(0xFF, 0x92, 0xDF);
        th.ansi[6]  = QColor(0x8B, 0xE9, 0xFD); th.ansi[14] = QColor(0xA4, 0xFF, 0xFF);
        th.ansi[7]  = QColor(0xF8, 0xF8, 0xF2); th.ansi[15] = QColor(0xFF, 0xFF, 0xFF);
        t.push_back(th);
    }

    // --- Monokai ---
    {
        Theme th;
        th.name = "Monokai";
        th.bgPrimary   = QColor(0x27, 0x28, 0x22);
        th.bgSecondary = QColor(0x1E, 0x1F, 0x1C);
        th.textPrimary = QColor(0xF8, 0xF8, 0xF2);
        th.textSecondary = QColor(0xC0, 0xC0, 0xB0);
        th.accent      = QColor(0x66, 0xD9, 0xEF);
        th.border      = QColor(0x3E, 0x3D, 0x32);
        th.cursor      = QColor(0xF8, 0xF8, 0xF0);
        th.ansi[0]  = QColor(0x27, 0x28, 0x22); th.ansi[8]  = QColor(0x75, 0x71, 0x5E);
        th.ansi[1]  = QColor(0xF9, 0x26, 0x72); th.ansi[9]  = QColor(0xF9, 0x26, 0x72);
        th.ansi[2]  = QColor(0xA6, 0xE2, 0x2E); th.ansi[10] = QColor(0xA6, 0xE2, 0x2E);
        th.ansi[3]  = QColor(0xF4, 0xBF, 0x75); th.ansi[11] = QColor(0xF4, 0xBF, 0x75);
        th.ansi[4]  = QColor(0x66, 0xD9, 0xEF); th.ansi[12] = QColor(0x66, 0xD9, 0xEF);
        th.ansi[5]  = QColor(0xAE, 0x81, 0xFF); th.ansi[13] = QColor(0xAE, 0x81, 0xFF);
        th.ansi[6]  = QColor(0xA1, 0xEF, 0xE4); th.ansi[14] = QColor(0xA1, 0xEF, 0xE4);
        th.ansi[7]  = QColor(0xF8, 0xF8, 0xF2); th.ansi[15] = QColor(0xF9, 0xF8, 0xF5);
        t.push_back(th);
    }

    // --- Solarized Dark ---
    {
        Theme th;
        th.name = "Solarized Dark";
        th.bgPrimary   = QColor(0x00, 0x2B, 0x36);
        th.bgSecondary = QColor(0x00, 0x1E, 0x27);
        th.textPrimary = QColor(0x83, 0x94, 0x96);
        th.textSecondary = QColor(0x93, 0xA1, 0xA1);
        th.accent      = QColor(0x26, 0x8B, 0xD2);
        th.border      = QColor(0x07, 0x36, 0x42);
        th.cursor      = QColor(0x26, 0x8B, 0xD2);
        th.ansi[0]  = QColor(0x07, 0x36, 0x42); th.ansi[8]  = QColor(0x00, 0x2B, 0x36);
        th.ansi[1]  = QColor(0xDC, 0x32, 0x2F); th.ansi[9]  = QColor(0xCB, 0x4B, 0x16);
        th.ansi[2]  = QColor(0x85, 0x99, 0x00); th.ansi[10] = QColor(0x58, 0x6E, 0x75);
        th.ansi[3]  = QColor(0xB5, 0x89, 0x00); th.ansi[11] = QColor(0x65, 0x7B, 0x83);
        th.ansi[4]  = QColor(0x26, 0x8B, 0xD2); th.ansi[12] = QColor(0x83, 0x94, 0x96);
        th.ansi[5]  = QColor(0xD3, 0x36, 0x82); th.ansi[13] = QColor(0x6C, 0x71, 0xC4);
        th.ansi[6]  = QColor(0x2A, 0xA1, 0x98); th.ansi[14] = QColor(0x93, 0xA1, 0xA1);
        th.ansi[7]  = QColor(0xEE, 0xE8, 0xD5); th.ansi[15] = QColor(0xFD, 0xF6, 0xE3);
        t.push_back(th);
    }

    // --- Gruvbox ---
    {
        Theme th;
        th.name = "Gruvbox";
        th.bgPrimary   = QColor(0x28, 0x28, 0x28);
        th.bgSecondary = QColor(0x1D, 0x20, 0x21);
        th.textPrimary = QColor(0xEB, 0xDB, 0xB2);
        th.textSecondary = QColor(0xD5, 0xC4, 0xA1);
        th.accent      = QColor(0xFA, 0xBD, 0x2F);
        th.border      = QColor(0x50, 0x49, 0x45);
        th.cursor      = QColor(0xFA, 0xBD, 0x2F);
        th.ansi[0]  = QColor(0x28, 0x28, 0x28); th.ansi[8]  = QColor(0x92, 0x83, 0x74);
        th.ansi[1]  = QColor(0xCC, 0x24, 0x1D); th.ansi[9]  = QColor(0xFB, 0x49, 0x34);
        th.ansi[2]  = QColor(0x98, 0x97, 0x1A); th.ansi[10] = QColor(0xB8, 0xBB, 0x26);
        th.ansi[3]  = QColor(0xD7, 0x99, 0x21); th.ansi[11] = QColor(0xFA, 0xBD, 0x2F);
        th.ansi[4]  = QColor(0x45, 0x85, 0x88); th.ansi[12] = QColor(0x83, 0xA5, 0x98);
        th.ansi[5]  = QColor(0xB1, 0x62, 0x86); th.ansi[13] = QColor(0xD3, 0x86, 0x9B);
        th.ansi[6]  = QColor(0x68, 0x9D, 0x6A); th.ansi[14] = QColor(0x8E, 0xC0, 0x7C);
        th.ansi[7]  = QColor(0xA8, 0x99, 0x84); th.ansi[15] = QColor(0xEB, 0xDB, 0xB2);
        t.push_back(th);
    }

    // --- Tokyo Night ---
    {
        Theme th;
        th.name = "Tokyo Night";
        th.bgPrimary   = QColor(0x1A, 0x1B, 0x26);
        th.bgSecondary = QColor(0x16, 0x16, 0x1E);
        th.textPrimary = QColor(0xA9, 0xB1, 0xD6);
        th.textSecondary = QColor(0x56, 0x5F, 0x89);
        th.accent      = QColor(0x7A, 0xA2, 0xF7);
        th.border      = QColor(0x29, 0x2E, 0x42);
        th.cursor      = QColor(0xC0, 0xCA, 0xF5);
        th.ansi[0]  = QColor(0x15, 0x16, 0x1E); th.ansi[8]  = QColor(0x41, 0x4D, 0x68);
        th.ansi[1]  = QColor(0xF7, 0x76, 0x8E); th.ansi[9]  = QColor(0xF7, 0x76, 0x8E);
        th.ansi[2]  = QColor(0x9E, 0xCE, 0x6A); th.ansi[10] = QColor(0x9E, 0xCE, 0x6A);
        th.ansi[3]  = QColor(0xE0, 0xAF, 0x68); th.ansi[11] = QColor(0xE0, 0xAF, 0x68);
        th.ansi[4]  = QColor(0x7A, 0xA2, 0xF7); th.ansi[12] = QColor(0x7A, 0xA2, 0xF7);
        th.ansi[5]  = QColor(0xBB, 0x9A, 0xF7); th.ansi[13] = QColor(0xBB, 0x9A, 0xF7);
        th.ansi[6]  = QColor(0x7D, 0xCF, 0xFF); th.ansi[14] = QColor(0x7D, 0xCF, 0xFF);
        th.ansi[7]  = QColor(0xA9, 0xB1, 0xD6); th.ansi[15] = QColor(0xC0, 0xCA, 0xF5);
        t.push_back(th);
    }

    // --- Catppuccin Latte (light) ---
    {
        Theme th;
        th.name = "Catppuccin Latte";
        th.bgPrimary   = QColor(0xEF, 0xF1, 0xF5);
        th.bgSecondary = QColor(0xE6, 0xE9, 0xEF);
        th.textPrimary = QColor(0x4C, 0x4F, 0x69);
        th.textSecondary = QColor(0x6C, 0x6F, 0x85);
        th.accent      = QColor(0x1E, 0x66, 0xF5);
        th.border      = QColor(0xCC, 0xD0, 0xDA);
        th.cursor      = QColor(0xDC, 0x86, 0x84);
        th.ansi[0]  = QColor(0x5C, 0x5F, 0x77); th.ansi[8]  = QColor(0x6C, 0x6F, 0x85);
        th.ansi[1]  = QColor(0xD2, 0x0F, 0x39); th.ansi[9]  = QColor(0xD2, 0x0F, 0x39);
        th.ansi[2]  = QColor(0x40, 0xA0, 0x2B); th.ansi[10] = QColor(0x40, 0xA0, 0x2B);
        th.ansi[3]  = QColor(0xDF, 0x8E, 0x1D); th.ansi[11] = QColor(0xDF, 0x8E, 0x1D);
        th.ansi[4]  = QColor(0x1E, 0x66, 0xF5); th.ansi[12] = QColor(0x1E, 0x66, 0xF5);
        th.ansi[5]  = QColor(0x88, 0x39, 0xEF); th.ansi[13] = QColor(0x88, 0x39, 0xEF);
        th.ansi[6]  = QColor(0x17, 0x9E, 0x99); th.ansi[14] = QColor(0x17, 0x9E, 0x99);
        th.ansi[7]  = QColor(0xAC, 0xB0, 0xBE); th.ansi[15] = QColor(0xBC, 0xC0, 0xCC);
        t.push_back(th);
    }

    // --- One Dark ---
    {
        Theme th;
        th.name = "One Dark";
        th.bgPrimary   = QColor(0x28, 0x2C, 0x34);
        th.bgSecondary = QColor(0x21, 0x25, 0x2B);
        th.textPrimary = QColor(0xAB, 0xB2, 0xBF);
        th.textSecondary = QColor(0x5C, 0x63, 0x70);
        th.accent      = QColor(0x61, 0xAF, 0xEF);
        th.border      = QColor(0x3E, 0x44, 0x52);
        th.cursor      = QColor(0x52, 0x8B, 0xFF);
        th.ansi[0]  = QColor(0x3F, 0x44, 0x51); th.ansi[8]  = QColor(0x4B, 0x52, 0x63);
        th.ansi[1]  = QColor(0xE0, 0x6C, 0x75); th.ansi[9]  = QColor(0xE0, 0x6C, 0x75);
        th.ansi[2]  = QColor(0x98, 0xC3, 0x79); th.ansi[10] = QColor(0x98, 0xC3, 0x79);
        th.ansi[3]  = QColor(0xE5, 0xC0, 0x7B); th.ansi[11] = QColor(0xE5, 0xC0, 0x7B);
        th.ansi[4]  = QColor(0x61, 0xAF, 0xEF); th.ansi[12] = QColor(0x61, 0xAF, 0xEF);
        th.ansi[5]  = QColor(0xC6, 0x78, 0xDD); th.ansi[13] = QColor(0xC6, 0x78, 0xDD);
        th.ansi[6]  = QColor(0x56, 0xB6, 0xC2); th.ansi[14] = QColor(0x56, 0xB6, 0xC2);
        th.ansi[7]  = QColor(0xAB, 0xB2, 0xBF); th.ansi[15] = QColor(0xBE, 0xC5, 0xD4);
        t.push_back(th);
    }

    // --- Kanagawa ---
    {
        Theme th;
        th.name = "Kanagawa";
        th.bgPrimary   = QColor(0x1F, 0x1F, 0x28);
        th.bgSecondary = QColor(0x16, 0x16, 0x1D);
        th.textPrimary = QColor(0xDC, 0xD7, 0xBA);
        th.textSecondary = QColor(0x72, 0x73, 0x69);
        th.accent      = QColor(0x7E, 0x9C, 0xD8);
        th.border      = QColor(0x2A, 0x2A, 0x37);
        th.cursor      = QColor(0xC8, 0xC0, 0x93);
        th.ansi[0]  = QColor(0x09, 0x09, 0x0F); th.ansi[8]  = QColor(0x72, 0x73, 0x69);
        th.ansi[1]  = QColor(0xC3, 0x40, 0x43); th.ansi[9]  = QColor(0xE8, 0x2C, 0x2C);
        th.ansi[2]  = QColor(0x76, 0x94, 0x6A); th.ansi[10] = QColor(0x98, 0xBB, 0x6C);
        th.ansi[3]  = QColor(0xC0, 0xA3, 0x6E); th.ansi[11] = QColor(0xE6, 0xC3, 0x84);
        th.ansi[4]  = QColor(0x7E, 0x9C, 0xD8); th.ansi[12] = QColor(0x7F, 0xB4, 0xCA);
        th.ansi[5]  = QColor(0x95, 0x7F, 0xB8); th.ansi[13] = QColor(0x93, 0x8A, 0xA9);
        th.ansi[6]  = QColor(0x6A, 0x95, 0x89); th.ansi[14] = QColor(0x7A, 0xA8, 0x9F);
        th.ansi[7]  = QColor(0xC8, 0xC0, 0x93); th.ansi[15] = QColor(0xDC, 0xD7, 0xBA);
        t.push_back(th);
    }

    return t;
}

static std::vector<Theme> &allThemes() {
    static std::vector<Theme> themes;
    if (themes.empty()) {
        themes = buildThemes();
        auto user = Themes::loadUserThemes();
        themes.insert(themes.end(), user.begin(), user.end());
    }
    return themes;
}

const std::vector<Theme> &Themes::all() {
    return allThemes();
}

const Theme &Themes::byName(const QString &name) {
    for (const auto &t : all()) {
        if (t.name == name) return t;
    }
    return defaultTheme();
}

const Theme &Themes::defaultTheme() {
    return all().front();
}

QStringList Themes::names() {
    QStringList result;
    for (const auto &t : all()) {
        result.append(t.name);
    }
    return result;
}

std::vector<Theme> Themes::loadUserThemes() {
    std::vector<Theme> result;
    QString dir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                  + "/ants-terminal/themes";
    QDir themeDir(dir);
    if (!themeDir.exists()) return result;

    QStringList files = themeDir.entryList({"*.json"}, QDir::Files);
    for (const QString &file : files) {
        const QString filePath = themeDir.filePath(file);
        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly)) continue;
        QJsonParseError err{};
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (!doc.isObject()) {
            qWarning("Ants: user theme %s failed to parse: %s (byte offset %d)",
                     qUtf8Printable(filePath),
                     qUtf8Printable(err.errorString()), err.offset);
            continue;
        }
        QJsonObject obj = doc.object();

        Theme th;
        th.name = obj.value("name").toString(file.chopped(5)); // strip .json
        th.isUserTheme = true;

        auto parseColor = [](const QJsonValue &v, const QColor &fallback = QColor()) -> QColor {
            if (v.isString()) return QColor(v.toString());
            return fallback;
        };

        th.bgPrimary   = parseColor(obj["bg_primary"], QColor(0x1E, 0x1E, 0x2E));
        th.bgSecondary = parseColor(obj["bg_secondary"], th.bgPrimary.darker(120));
        th.textPrimary = parseColor(obj["text_primary"], QColor(0xCD, 0xD6, 0xF4));
        th.textSecondary = parseColor(obj["text_secondary"], th.textPrimary.darker(130));
        th.accent      = parseColor(obj["accent"], QColor(0x89, 0xB4, 0xFA));
        th.border      = parseColor(obj["border"], th.bgPrimary.lighter(150));
        th.cursor      = parseColor(obj["cursor"], th.accent);

        // ANSI palette (optional)
        QJsonArray ansiArr = obj["ansi"].toArray();
        // Default ANSI colors (xterm-like)
        QColor defaultAnsi[16] = {
            QColor(0x45,0x47,0x5A), QColor(0xF3,0x8B,0xA8),
            QColor(0xA6,0xE3,0xA1), QColor(0xF9,0xE2,0xAF),
            QColor(0x89,0xB4,0xFA), QColor(0xF5,0xC2,0xE7),
            QColor(0x94,0xE2,0xD5), QColor(0xBA,0xC2,0xDE),
            QColor(0x58,0x5B,0x70), QColor(0xF3,0x8B,0xA8),
            QColor(0xA6,0xE3,0xA1), QColor(0xF9,0xE2,0xAF),
            QColor(0x89,0xB4,0xFA), QColor(0xF5,0xC2,0xE7),
            QColor(0x94,0xE2,0xD5), QColor(0xA6,0xAD,0xC8),
        };
        for (int i = 0; i < 16; ++i) {
            if (i < ansiArr.size())
                th.ansi[i] = parseColor(ansiArr[i], defaultAnsi[i]);
            else
                th.ansi[i] = defaultAnsi[i];
        }

        result.push_back(th);
    }
    return result;
}

void Themes::reload() {
    auto &themes = allThemes();
    themes = buildThemes();
    auto user = loadUserThemes();
    themes.insert(themes.end(), user.begin(), user.end());
}
