// ANTS-5734 § 7 — the load test: every vendored language sets on a parser
// once and parses a line. It locks § 4's ABI failure mode ("a grammar's ABI
// is outside 13-15 ... ts_parser_set_language fails at first use"), so a
// grammar refresh that breaks a language fails here, before release.
// Contract: docs/specs/ANTS-5734-trace-verb.md.

#include "trace/languages.h"
#include "trace/queries.h"

#include <tree_sitter/api.h>

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

TEST(TraceGrammars, EveryShipOneLanguageLoads) {
    // § 2.3's ship-1 table, then ship 3's first batch, in order.
    const std::vector<std::string> expected = {
        "c", "cpp", "python", "bash", "lua", "glsl",
        "javascript", "typescript", "tsx", "rust", "go", "java",
        "c_sharp", "ruby", "qmljs", "make", "css"};
    std::vector<std::string> ids;
    for (const trace::Language &l : trace::languages()) ids.emplace_back(l.id);
    EXPECT_EQ(expected, ids) << "the vendored set must be § 2.3's ship-1 list and ship-3 batch";

    for (const trace::Language &l : trace::languages()) {
        SCOPED_TRACE(l.id);
        const TSLanguage *lang = l.language();
        ASSERT_NE(lang, nullptr);
        const uint32_t abi = ts_language_abi_version(lang);
        EXPECT_GE(abi, uint32_t(TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION));
        EXPECT_LE(abi, uint32_t(TREE_SITTER_LANGUAGE_VERSION));

        TSParser *parser = ts_parser_new();
        EXPECT_TRUE(ts_parser_set_language(parser, lang))
            << "ts_parser_set_language refused, ABI " << abi;
        const char *src = "x\n";
        TSTree *tree = ts_parser_parse_string(parser, nullptr, src,
                                              uint32_t(std::strlen(src)));
        EXPECT_NE(tree, nullptr);
        if (tree) {
            EXPECT_GT(ts_node_end_byte(ts_tree_root_node(tree)), 0u);
            ts_tree_delete(tree);
        }
        ts_parser_delete(parser);
        EXPECT_EQ(trace::languageFor(QString::fromLatin1(l.id)), lang);
    }
    EXPECT_EQ(trace::languageFor(QStringLiteral("zig")), nullptr);
}

// § 2.4 — every built-in query file compiles against its vendored grammar,
// and carries the `;; limits:` sentence the not_covered entry reads.
TEST(TraceGrammars, EveryBuiltinQueryCompiles) {
    for (const trace::Language &l : trace::languages()) {
        SCOPED_TRACE(l.id);
        const QString id = QString::fromLatin1(l.id);
        EXPECT_FALSE(trace::builtinQueryText(id).isEmpty()) << "no built-in query";
        const trace::Query q = trace::queryFor(id);
        EXPECT_NE(q.query, nullptr) << q.error.toStdString();
        EXPECT_FALSE(q.limits.isEmpty()) << "no ;; limits: line";
    }
}
