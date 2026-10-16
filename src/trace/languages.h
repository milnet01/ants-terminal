// ANTS-5734 § 2.3 — the tree-sitter languages vendored into ants-mcpd, by the
// ids the trace verb uses. The grammars live in ants_treesitter_lib, which
// links into ants-mcpd and the trace test bundle only, never ants-terminal.
// Contract: docs/specs/ANTS-5734-trace-verb.md.

#pragma once

#include <QByteArray>
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

// True when `id` names a § 2.3 language of any ship, vendored or not.
bool isKnownLanguage(const QString &id);

// The § 2.3 language of a file, by its extension, else (a file with no
// extension) by the shebang on its first line, `head`. Empty when the file
// is in no § 2.3 language. `.c` is c; every other extension SymbolQuery maps
// to C++, `.h` included, is cpp.
QString languageForFile(const QString &fileName, const QByteArray &head);

}  // namespace trace
