#pragma once

// The shell history autocomplete suggests from (ANTS-5685).
//
// Nothing here touches a widget, so TerminalWidget runs load() on a worker
// thread: a long history file no longer stalls a new tab's first output.

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace shellhistory {

// Bytes read from the end of a history file. Older commands are not
// suggested; 2 MiB holds tens of thousands of typical commands.
inline constexpr qint64 kMaxBytes = qint64{2} * 1024 * 1024;

// The last maxBytes of the file, starting at a line start. Empty when the
// file cannot be opened.
QByteArray readTail(const QString &path, qint64 maxBytes = kMaxBytes);

// Each command in content once, newest first. Understands zsh's extended
// format (": <time>:0;<command>") and fish's ("- cmd: <command>").
QStringList parse(const QByteArray &content);

// parse() over a fish history file's "- cmd: " lines only; its "when:" and
// "paths:" lines are not commands (ANTS-6654).
QStringList parseFish(const QByteArray &content);

// The commands of the first of zsh, bash and fish history under homeDir
// that holds any.
QStringList load(const QString &homeDir);

}  // namespace shellhistory
