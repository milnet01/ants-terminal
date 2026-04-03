#pragma once

#include <QString>
#include <QJsonObject>

class Config {
public:
    Config();

    QString theme() const;
    void setTheme(const QString &name);

    int fontSize() const;
    void setFontSize(int size);

    int windowWidth() const;
    int windowHeight() const;
    int windowX() const;
    int windowY() const;
    void setWindowGeometry(int x, int y, int w, int h);

    void save();

private:
    void load();
    QString configPath() const;

    QJsonObject m_data;
};
