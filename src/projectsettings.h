// ANTS-2160 — per-project settings file (.ants/project.json) pure loader.
// Qt6::Core-only, FS-reading, no widgets / no subprocess (mirrors
// findsources.cpp / codebaseindex.cpp). Lets a project declare where its
// source / docs / roadmap / changelog / specs live so the MCP read verbs
// stop guessing on a non-src/ layout. See docs/specs/ANTS-2160.md.

#ifndef ANTS_PROJECTSETTINGS_H
#define ANTS_PROJECTSETTINGS_H

#include "roadmapparse.h"

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
    // ANTS-3771 — the project's DECLARED id format. The first non-path member
    // here, and the reason this header now includes roadmapparse.h: parsing and
    // validating the declaration in ONE place is what makes idFormatFor() a
    // thin accessor rather than a second reader. nullopt when the key is
    // absent, or when every member of it was dropped by validation.
    std::optional<RoadmapParse::IdFormat> idFormat;
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

// ANTS-3771 — the SINGLE load of a project's declared id format
// (docs/specs/ANTS-3771-id-format-declaration.md § 2.2, user decision
// 2026-08-21). Every project-scoped roadmap read calls this and passes the
// result down to parseBullets() / bulletsFor(); nothing else reads the key.
//
// It lives here, and not in the read seam, because of the build graph:
// RoadmapSource::bulletsFor() is in ants_roadmapstore_lib, whose link list is
// `Qt6::Core Qt6::Sql ants_roadmapparse_lib` under the comment "the reader, NOT
// ants_core_lib" — and ProjectSettings is in ants_core_lib, which links the
// store PRIVATE. Reading .ants/project.json inside the seam would invert that
// edge. So the declaration travels as a VALUE and the load happens core-side.
//
// Default-constructed (undeclared) for a project with no file, no key, or a
// key whose every member load() dropped — which is the zero-change path INV-1
// asserts. Uncached, like load() itself.
RoadmapParse::IdFormat idFormatFor(const QString &rootCanonical);

// ANTS-2161 / ANTS-3393 — true for a top-level directory name the source
// walk should neither descend nor index: any dot-dir (.git/.ants/.venv),
// build*, a *-deps / *-prefix staging dir, and the conventional vendored /
// build-output / Python-virtualenv trees (node_modules, dist, target,
// vendor, venv, env, __pycache__, …). The single source of truth shared by
// op:detect (projectsettings.cpp) and codebase_index's walkSubtree
// (codebaseindex.cpp) so both prune the same set — a committed virtualenv
// no longer drowns a flat-root source_roots=["."] index.
bool isNoiseDir(const QString &name);

// ANTS-2161 — layout-suggestion detector. See docs/specs/ANTS-2161.md § 2.1.
struct Suggestion {
    bool                       present = false;   // .ants/project.json already on disk
    std::optional<QStringList> sourceRoots;       // suggested dirs, when the layout looks misplaced
    QString                    reason;            // human string incl. counts; ALWAYS non-empty after a walk/short-circuit (ANTS-3369). nullopt sourceRoots — not an empty reason — is the "no suggestion" signal.
    int                        defaultSourceCount = 0;  // admitted files under literal src/+tests/
    int                        totalSourceCount   = 0;  // admitted files repo-wide (bounded)
    std::optional<QStringList> wouldUseRoots;     // ANTS-3369: roots already in effect — declared source_roots when present:true, else whichever of src/ or tests/ actually hold source on the no-override path. Echoed so a caller can confirm the layout even when nothing is suggested.
    QStringList                excluded;          // ANTS-3369: every isNoiseDir match present on disk and skipped, minus dot-dirs (build* + vendored names listed; .git/.ants not). Names only, no descent.
    // ANTS-3588 (INV-19): conventional auxiliary layout keys that EXIST on disk,
    // proposed ONLY alongside a sourceRoots suggestion (a misplaced/flat layout)
    // so a single op:init writes the whole block. Each is nullopt when its
    // conventional path is absent — NEVER a present-but-empty value.
    std::optional<QStringList> testRoots;         // subset of {tests, test} present as dirs, tests-then-test
    std::optional<QString>     docsDir;           // "docs" if docs/ is a dir
    std::optional<QString>     specsDir;          // "docs/specs" if that dir exists
    std::optional<QString>     roadmap;           // "ROADMAP.md" if that file exists
    std::optional<QString>     changelog;         // "CHANGELOG.md" if that file exists
};

// Bounded shallow analysis of <rootCanonical>. If .ants/project.json
// already exists → {present:true} with NO directory walk — but loads the
// file to echo its declared source_roots in `wouldUseRoots` + a `reason`
// (ANTS-3369; an unparseable file → wouldUseRoots nullopt, reason still
// non-empty). Else counts source files (CodebaseIndex::isIndexableSuffix)
// per top-level dir (skipping the isNoiseDir set, capped at
// kDetectFileCeiling) and, when the default src/+tests/ walk would miss
// more than kMissRatio of the repo's source, suggests `sourceRoots` = ALL
// first-party source subdirs (every counted dir except the tests default,
// sorted count desc / name asc) — not a dominant-cover subset, so a
// low-count entry-point dir and a spread layout are both covered
// (ANTS-3369). Suggests subdirs only, never the repo root; source at the
// repo root is noted in `reason` as an un-suggestable remainder. `reason`
// is ALWAYS non-empty; `excluded`/`wouldUseRoots` echo what was skipped /
// already in effect.
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
