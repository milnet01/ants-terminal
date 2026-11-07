// ANTS-6441 — where the OSC 133 HMAC key lives, and which source wins.
// Contract: docs/specs/ANTS-6441-osc133-key-file.md § 4.1, § 4.2.
//
// The shell hooks (packaging/shell-integration/ants-osc133.{bash,zsh}) apply
// the same rule; change the two together.

#pragma once

#include <QByteArray>
#include <QFile>
#include <QStandardPaths>
#include <QString>

// $XDG_CONFIG_HOME/ants-terminal/osc133.key (XDG_CONFIG_HOME defaults to
// ~/.config).
inline QString osc133KeyFilePath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/ants-terminal/osc133.key");
}

// The key file's first line when non-empty, else a non-empty
// $ANTS_OSC133_KEY, else empty (verifier off). An unreadable file counts as
// absent.
inline QByteArray osc133KeyFromSetup() {
    QFile f(osc133KeyFilePath());
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray key = f.readLine(4096).trimmed();
        if (!key.isEmpty()) return key;
    }
    return qgetenv("ANTS_OSC133_KEY").trimmed();
}
