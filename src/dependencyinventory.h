#pragma once

// dependency_inventory (ANTS-5515) — what this project pins and where: workflow
// actions, runner labels, container images and runtime versions. It reads tracked
// files only and asks no network; the lookups stay with the check-dependencies
// skill. Contract: tests/features/mcp_dependency_inventory/spec.md.
// Its own translation unit so a test links it without RemoteControl.

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace DependencyInventory {

// `root` is the canonical project root. `trackedFiles` are project-relative. An
// empty list means git gave none: the reply names that in `skipped` rather than
// reading as a project that pins nothing.
QJsonObject run(const QString &root, const QStringList &trackedFiles);

}  // namespace DependencyInventory
