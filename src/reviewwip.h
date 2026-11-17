// reviewwip — the Review Changes "Work in progress" lines (ANTS-6806).
//
// Names uncommitted work the way its commit will be named: one
// "<ID>: <summary>" line per roadmap id the diff adds. The summary is the
// CHANGELOG bullet the diff adds citing that id, else the id's roadmap
// headline. Qt6::Core only, so it is testable without the dialog.
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace reviewwip {

// `diff` is `git diff --patch HEAD` output. Only added lines count: an id in
// a removed or context line is not this work. Only ids `headlineById` holds
// count, so a token shaped like an id ("UTF-8") is not one. Each id once, in
// the order the diff first adds it.
QStringList lines(const QString &diff,
                  const QHash<QString, QString> &headlineById);

// The roadmap at `roadmapPath`, id -> headline; empty when it is absent or
// unreadable. Re-read only when the file's size or mtime changes.
QHash<QString, QString> headlines(const QString &roadmapPath);

}  // namespace reviewwip
