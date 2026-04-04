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

int Config::scrollbackLines() const {
    return m_data.value("scrollback_lines").toInt(50000);
}

void Config::setScrollbackLines(int lines) {
    m_data["scrollback_lines"] = qBound(1000, lines, 1000000);
    save();
}

double Config::opacity() const {
    return m_data.value("opacity").toDouble(1.0);
}

void Config::setOpacity(double value) {
    m_data["opacity"] = qBound(0.1, value, 1.0);
    save();
}

bool Config::sessionLogging() const {
    return m_data.value("session_logging").toBool(false);
}

void Config::setSessionLogging(bool enabled) {
    m_data["session_logging"] = enabled;
    save();
}

bool Config::autoCopyOnSelect() const {
    return m_data.value("auto_copy_on_select").toBool(true);
}

void Config::setAutoCopyOnSelect(bool enabled) {
    m_data["auto_copy_on_select"] = enabled;
    save();
}

QString Config::editorCommand() const {
    return m_data.value("editor_command").toString("");
}

void Config::setEditorCommand(const QString &cmd) {
    m_data["editor_command"] = cmd;
    save();
}

QString Config::imagePasteDir() const {
    return m_data.value("image_paste_dir").toString("");
}

void Config::setImagePasteDir(const QString &dir) {
    m_data["image_paste_dir"] = dir;
    save();
}

bool Config::backgroundBlur() const {
    return m_data.value("background_blur").toBool(false);
}

void Config::setBackgroundBlur(bool enabled) {
    m_data["background_blur"] = enabled;
    save();
}

QString Config::keybinding(const QString &action, const QString &defaultKey) const {
    QJsonObject kb = m_data.value("keybindings").toObject();
    return kb.value(action).toString(defaultKey);
}

void Config::setKeybinding(const QString &action, const QString &key) {
    QJsonObject kb = m_data.value("keybindings").toObject();
    kb[action] = key;
    m_data["keybindings"] = kb;
    save();
}
