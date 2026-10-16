// ANTS-5734 — see languages.h.

#include "trace/languages.h"

extern "C" {
const TSLanguage *tree_sitter_c();
const TSLanguage *tree_sitter_cpp();
const TSLanguage *tree_sitter_python();
const TSLanguage *tree_sitter_bash();
const TSLanguage *tree_sitter_lua();
const TSLanguage *tree_sitter_glsl();
const TSLanguage *tree_sitter_javascript();
const TSLanguage *tree_sitter_typescript();
const TSLanguage *tree_sitter_tsx();
const TSLanguage *tree_sitter_rust();
const TSLanguage *tree_sitter_go();
const TSLanguage *tree_sitter_java();
}

namespace trace {

const std::vector<Language> &languages() {
    static const std::vector<Language> all = {
        {"c", &tree_sitter_c},
        {"cpp", &tree_sitter_cpp},
        {"python", &tree_sitter_python},
        {"bash", &tree_sitter_bash},
        {"lua", &tree_sitter_lua},
        {"glsl", &tree_sitter_glsl},
        {"javascript", &tree_sitter_javascript},
        {"typescript", &tree_sitter_typescript},
        {"tsx", &tree_sitter_tsx},
        {"rust", &tree_sitter_rust},
        {"go", &tree_sitter_go},
        {"java", &tree_sitter_java},
    };
    return all;
}

const TSLanguage *languageFor(const QString &id) {
    for (const Language &l : languages())
        if (id == QLatin1String(l.id)) return l.language();
    return nullptr;
}

}  // namespace trace
