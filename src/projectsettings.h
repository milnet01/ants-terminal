// ANTS-2160 — per-project settings file (.ants/project.json) pure loader.
// Qt6::Core-only, FS-reading, no widgets / no subprocess (mirrors
// findsources.cpp / codebaseindex.cpp). Lets a project declare where its
// source / docs / roadmap / changelog / specs live so the MCP read verbs
// stop guessing on a non-src/ layout. See docs/specs/ANTS-2160.md.

#ifndef ANTS_PROJECTSETTINGS_H
#define ANTS_PROJECTSETTINGS_H

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <optional>

namespace ProjectSettings {

// All keys optional. A nullopt field means "no override" → the consumer
// uses its existing heuristic (so an absent / partial file is zero-change
// for projects that don't ship one).
struct Settings {
    std::optional<QStringList> sourceRoots;   // dirs; codebase_index
    std::optional<QStringList> testRoots;     // dirs; codebase_index
    std::optional<QString>     docsDir;       // dir;  docs_index / project_layout
    std::optional<QString>     roadmap;       // file; roadmap_query / _log / project_layout
    std::optional<QString>     changelog;     // file; changelog_log / project_layout
    std::optional<QString>     specsDir;      // dir;  spec_query / _log / current_state / project_layout
};

// Reads + parses <rootCanonical>/.ants/project.json on every call (no
// cache — the file is tiny and read at points that already touch the FS).
// Returns an all-nullopt Settings on an absent / unreadable / malformed /
// non-object file (fail-safe). Per entry: a JSON null, an empty/blank
// string, a wrong-typed value, or a path that fails
// PathValidation::isInsideProject (root-escape OR a canonicalisation miss,
// i.e. does not exist) is DROPPED — not the whole file. An array emptied
// by dropping becomes nullopt. The dir-vs-file type check is left to the
// consumer.
Settings load(const QString &rootCanonical);

// ANTS-2161 — layout-suggestion detector. See docs/specs/ANTS-2161.md § 2.1.
struct Suggestion {
    bool                       present = false;   // .ants/project.json already on disk
    std::optional<QStringList> sourceRoots;       // suggested dirs, when the layout looks misplaced
    QString                    reason;            // human string incl. counts; empty ⟺ no suggestion
    int                        defaultSourceCount = 0;  // admitted files under literal src/+tests/
    int                        totalSourceCount   = 0;  // admitted files repo-wide (bounded)
};

// Bounded shallow analysis of <rootCanonical>. If .ants/project.json
// already exists → {present:true} with NO walk and NO suggestion. Else
// counts source files (CodebaseIndex::isIndexableSuffix) per top-level dir
// (skipping noise dirs, capped at kDetectFileCeiling) and suggests
// `sourceRoots` = the subdir set covering >= kDominanceRatio of total
// source, but only when the default src/+tests/ walk would miss more than
// kMissRatio of it and such a subdir set exists. Suggests subdirs only,
// never the repo root.
Suggestion detect(const QString &rootCanonical);

// Merge `changes` into `existing` for an op:"set"/"init" write. `changes`
// carries only the caller-supplied keys; a JSON-null value REMOVES the key.
// Recognised keys are validated at write-time under the canonical root
// (source_roots/test_roots/docs_dir/specs_dir must be existing dirs;
// roadmap/changelog existing files) — STRICT, unlike load()'s lenient
// read-time drop. Unrecognised keys already in `existing` are preserved.
// On a validation failure returns nullopt and sets *errCode ("bad_args"
// for a wrong-typed/shape value, "bad_path" for an escaping / non-existent
// / wrong-type path) + *errKey + *errVal. nullptr out-params are tolerated.
std::optional<QJsonObject> applyWrite(const QJsonObject &existing,
                                      const QJsonObject &changes,
                                      const QString &rootCanonical,
                                      QString *errCode, QString *errKey,
                                      QString *errVal);

}  // namespace ProjectSettings

#endif  // ANTS_PROJECTSETTINGS_H
