#include "appimagedesktop.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace AppImageDesktop {

namespace {

// Writes `data` to `path` unless it already holds exactly that.
bool writeIfChanged(const QString &path, const QByteArray &data) {
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == data)
        return true;
    existing.close();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(data);
    return out.commit();
}

}  // namespace

QByteArray rewriteEntry(const QByteArray &entry, const QString &appImage) {
    for (const QChar c : appImage)
        if (c == u'"' || c == u'`' || c == u'$' || c == u'\\' || c == u'\n')
            return {};
    const QByteArray path = appImage.toUtf8();
    // Exec quoting: the path in double quotes, with % doubled (field codes).
    QByteArray quoted = path;
    quoted.replace('%', "%%");
    quoted = '"' + quoted + '"';

    const QByteArray execPrefix = "Exec=ants-terminal";
    QList<QByteArray> lines = entry.split('\n');
    for (QByteArray &line : lines) {
        if (line.startsWith(execPrefix)
            && (line.size() == execPrefix.size()
                || line.at(execPrefix.size()) == ' ')) {
            line = "Exec=" + quoted + line.mid(execPrefix.size());
        } else if (line.startsWith("TryExec=")) {
            line = "TryExec=" + path;
        }
    }
    return lines.join('\n');
}

bool integrate(const QString &appDir, const QString &appImage,
               const QString &dataHome) {
    QFile bundled(appDir + QStringLiteral("/usr/share/applications/")
                  + QLatin1String(kEntryName));
    if (!bundled.open(QIODevice::ReadOnly))
        return false;
    const QByteArray entry = rewriteEntry(bundled.readAll(), appImage);
    if (entry.isEmpty())
        return false;

    bool ok = true;
    // Icons first, so the menu entry never names an icon not yet there.
    const QDir icons(appDir + QStringLiteral("/usr/share/icons/hicolor"));
    for (const QString &size : icons.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString rel = size + QStringLiteral("/apps/ants-terminal.png");
        QFile icon(icons.filePath(rel));
        if (!icon.open(QIODevice::ReadOnly))
            continue;  // e.g. scalable/, which holds no PNG
        ok &= writeIfChanged(dataHome + QStringLiteral("/icons/hicolor/") + rel,
                             icon.readAll());
    }
    ok &= writeIfChanged(dataHome + QStringLiteral("/applications/")
                         + QLatin1String(kEntryName), entry);
    return ok;
}

void integrateFromEnvironment() {
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    const QString appDir = qEnvironmentVariable("APPDIR");
    if (appImage.isEmpty() || appDir.isEmpty() || !QFileInfo(appImage).isFile())
        return;
    QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
    if (dataHome.isEmpty())
        dataHome = QDir::homePath() + QStringLiteral("/.local/share");
    integrate(appDir, QFileInfo(appImage).absoluteFilePath(), dataHome);
}

}  // namespace AppImageDesktop
