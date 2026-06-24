// ANTS-2160 — per-project settings file (.ants/project.json) pure loader.
// Qt6::Core-only, FS-reading, no widgets / no subprocess (mirrors
// findsources.cpp / codebaseindex.cpp). Lets a project declare where its
// source / docs / roadmap / changelog / specs live so the MCP read verbs
// stop guessing on a non-src/ layout. See docs/specs/ANTS-2160.md.

#ifndef ANTS_PROJECTSETTINGS_H
#define ANTS_PROJECTSETTINGS_H

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

}  // namespace ProjectSettings

#endif  // ANTS_PROJECTSETTINGS_H
