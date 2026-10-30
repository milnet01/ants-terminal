#pragma once

// version_check (ANTS-5510) — does every file `.claude/bump.json` lists hold the
// version its `version_source` holds, and does a tracked file it does not name
// hold that version too? Contract: tests/features/mcp_version_check/spec.md.
// Its own translation unit so a test links it without RemoteControl.

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace VersionCheck {

// `root` is the canonical project root. `trackedFiles` are project-relative. Its
// emptiness means the file list could not be had: the unlisted scan is then named
// in `skipped` rather than read as clean.
QJsonObject run(const QString &root, const QStringList &trackedFiles);

}  // namespace VersionCheck
