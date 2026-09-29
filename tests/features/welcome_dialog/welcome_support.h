// ANTS-5558 — shared fixtures for the welcome_dialog conformance tests.
// Contract: docs/specs/ANTS-5558-welcome-dialog.md (§ 3, § 5).
//
// Every test that touches HOME builds a Sandbox first: a temporary HOME,
// XDG_CONFIG_HOME under it, no XDG_DATA_HOME, no APPIMAGE, SHELL=/bin/bash,
// and QStandardPaths test mode off. The XdgGuard inside restores every
// variable on scope exit, so nothing leaks to sibling tests in the bundle and
// nothing ever reads or writes the real ~/.claude or ~/.claude.json.

#pragma once

#include "../../_support/xdg_guard.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

namespace welcome_test {

inline bool writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(bytes) == bytes.size();
}

inline QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

// A tiny executable, for "the registered command exists" fixtures.
inline bool makeExecutable(const QString &path) {
    if (!writeFile(path, "#!/bin/sh\nexit 0\n")) return false;
    return QFile::setPermissions(
        path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                  QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                  QFileDevice::ExeGroup | QFileDevice::ReadOther |
                  QFileDevice::ExeOther);
}

inline int countOf(const QByteArray &hay, const QByteArray &needle) {
    int n = 0;
    qsizetype i = 0;
    while ((i = hay.indexOf(needle, i)) >= 0) { ++n; i += needle.size(); }
    return n;
}

inline QJsonObject readJson(const QString &path) {
    return QJsonDocument::fromJson(readFile(path)).object();
}

// First 300 bytes, for failure messages.
inline QString clip(const QByteArray &b) {
    return QString::fromUtf8(b.left(300)).replace(QLatin1Char('\n'),
                                                  QStringLiteral("\\n"));
}

inline QString compact(const QJsonObject &o) {
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact))
        .left(300);
}

struct Sandbox {
    QTemporaryDir root;
    QString home;
    ants_test::XdgGuard env;  // declared after `root`: restored first

    Sandbox() {
        if (!root.isValid()) return;
        home = root.path() + QStringLiteral("/home");
        QDir().mkpath(home);
        env.setTestMode(false);
        env.setEnv("HOME", home.toUtf8());
        env.setEnv("XDG_CONFIG_HOME", (home + QStringLiteral("/.config")).toUtf8());
        env.unsetEnv("XDG_DATA_HOME");
        env.unsetEnv("APPIMAGE");
        env.setEnv("SHELL", QByteArrayLiteral("/bin/bash"));
    }
    bool valid() const { return root.isValid(); }
    // A path under the sandbox root (outside HOME).
    QString path(const QString &rel) const {
        return root.path() + QLatin1Char('/') + rel;
    }
};

}  // namespace welcome_test
