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

    int scrollbackLines() const;
    void setScrollbackLines(int lines);

    double opacity() const;
    void setOpacity(double value);

    bool sessionLogging() const;
    void setSessionLogging(bool enabled);

    bool autoCopyOnSelect() const;
    void setAutoCopyOnSelect(bool enabled);

    QString editorCommand() const;
    void setEditorCommand(const QString &cmd);

    // Image paste auto-save directory
    QString imagePasteDir() const;
    void setImagePasteDir(const QString &dir);

    // Background blur (KDE/KWin)
    bool backgroundBlur() const;
    void setBackgroundBlur(bool enabled);

    // Custom keybindings (action -> key sequence string)
    QString keybinding(const QString &action, const QString &defaultKey) const;
    void setKeybinding(const QString &action, const QString &key);

    void save();

private:
    void load();
    QString configPath() const;

    QJsonObject m_data;
};
