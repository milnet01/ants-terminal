#include "shellhistory.h"

#include <QFile>
#include <QSet>

namespace shellhistory {

QByteArray readTail(const QString &path, qint64 maxBytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const qint64 size = file.size();
    if (size <= maxBytes) return file.readAll();
    // One byte before the cut is read too, so a cut that falls exactly on a
    // line start keeps that line.
    if (!file.seek(size - maxBytes - 1)) return {};
    const QByteArray window = file.read(maxBytes + 1);
    const qsizetype newline = window.indexOf('\n');
    return newline < 0 ? QByteArray() : window.mid(newline + 1);
}

QStringList parse(const QByteArray &content) {
    QStringList entries;
    const QStringList lines = QString::fromUtf8(content).split('\n', Qt::SkipEmptyParts);

    // ANTS-1841 — O(1) dedup via a seen-set. A linear membership scan over
    // the list cost O(n²) on a large history. The first occurrence wins its
    // prepend position; later duplicates skip.
    QSet<QString> seen;
    seen.reserve(lines.size());
    for (const QString &line : lines) {
        QString cmd = line.trimmed();
        // zsh extended history format: ": timestamp:0;command"
        if (cmd.startsWith(": ")) {
            int semi = cmd.indexOf(';');
            if (semi >= 0) cmd = cmd.mid(semi + 1);
            else continue;
        }
        // fish history format: "- cmd: command"
        if (cmd.startsWith("- cmd: ")) {
            cmd = cmd.mid(7);
        }
        if (!cmd.isEmpty() && !seen.contains(cmd)) {
            seen.insert(cmd);
            entries.prepend(cmd); // Most recent first
        }
    }
    return entries;
}

QStringList parseFish(const QByteArray &content) {
    QByteArray commands;
    for (const QByteArray &line : content.split('\n'))
        if (line.trimmed().startsWith("- cmd: ")) commands += line + '\n';
    return parse(commands);
}

QStringList load(const QString &homeDir) {
    for (const QString &name : {QStringLiteral("/.zsh_history"),
                                QStringLiteral("/.bash_history")}) {
        QStringList entries = parse(readTail(homeDir + name));
        if (!entries.isEmpty()) return entries;
    }
    return parseFish(readTail(homeDir + "/.local/share/fish/fish_history"));
}

}  // namespace shellhistory
