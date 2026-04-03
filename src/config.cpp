#include "config.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>

Config::Config() {
    load();
}

QString Config::configPath() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                  + "/ants-terminal";
    QDir().mkpath(dir);
    return dir + "/config.json";
}

void Config::load() {
    QFile file(configPath());
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            m_data = doc.object();
        }
    }
}

void Config::save() {
    QString path = configPath();
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        file.write(QJsonDocument(m_data).toJson());
    }
}

QString Config::theme() const {
    return m_data.value("theme").toString("Dark");
}

void Config::setTheme(const QString &name) {
    m_data["theme"] = name;
    save();
}

int Config::fontSize() const {
    return m_data.value("font_size").toInt(11);
}

void Config::setFontSize(int size) {
    size = qBound(8, size, 32);
    m_data["font_size"] = size;
    save();
}

int Config::windowWidth() const  { return m_data.value("window_w").toInt(900); }
int Config::windowHeight() const { return m_data.value("window_h").toInt(700); }
int Config::windowX() const      { return m_data.value("window_x").toInt(-1); }
int Config::windowY() const      { return m_data.value("window_y").toInt(-1); }

void Config::setWindowGeometry(int x, int y, int w, int h) {
    m_data["window_x"] = x;
    m_data["window_y"] = y;
    m_data["window_w"] = w;
    m_data["window_h"] = h;
    save();
}
