// ANTS-5734 § 2.3 — the tree-sitter languages vendored into ants-mcpd, by the
// ids the trace verb uses. The grammars live in ants_treesitter_lib, which
// links into ants-mcpd and the trace test bundle only, never ants-terminal.
// Contract: docs/specs/ANTS-5734-trace-verb.md.

#pragma once

#include <QString>

#include <vector>

struct TSLanguage;

namespace trace {

struct Language {
    const char *id;                    // "cpp", "python", ...
    const TSLanguage *(*language)();   // the grammar's tree_sitter_<id>()
};

// Every vendored language, in § 2.3's ship-1 order.
const std::vector<Language> &languages();

// The grammar for `id`, or nullptr when no vendored language has that id.
const TSLanguage *languageFor(const QString &id);

}  // namespace trace
