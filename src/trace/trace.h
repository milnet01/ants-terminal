// ANTS-5734 — the `trace` verb's engine: one call returns a symbol's
// definitions, callers, references, tests, documents, spec invariants and
// roadmap items, or, in path mode, what names a file.
// Contract: docs/specs/ANTS-5734-trace-verb.md.

#pragma once

#include "roadmapparse.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

namespace trace {

// What the host supplies beyond the arguments.
struct Env {
    // The project's roadmap bullets, read the way roadmap_query reads them
    // (§ 2.7). nullopt with `*error` set: unreadable. nullopt with no error:
    // the project has no store copy, so serve parses its roadmap file. Unset:
    // serve parses the file. ants-mcpd binds it to the store-aware read.
    std::function<std::optional<QVector<RoadmapParse::BulletRecord>>(
        const QString &rootCanonical, QString *error)>
        roadmapBullets;
};

// The verb's whole answer for one call: {ok:true, ...} per § 2.2, or
// {ok:false, error, code} with code bad_args or bad_path. Read-only.
QJsonObject serve(const QJsonObject &args, const QString &rootCanonical,
                  const Env &env = {});

}  // namespace trace
