// ANTS-5734 — see languages.h.

#include "trace/languages.h"

#include <QHash>
#include <QList>

#include <initializer_list>

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
const TSLanguage *tree_sitter_c_sharp();
const TSLanguage *tree_sitter_ruby();
const TSLanguage *tree_sitter_qmljs();
const TSLanguage *tree_sitter_make();
const TSLanguage *tree_sitter_css();
const TSLanguage *tree_sitter_scala();
const TSLanguage *tree_sitter_zig();
const TSLanguage *tree_sitter_julia();
const TSLanguage *tree_sitter_objc();
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
        // Ship 3, first batch.
        {"c_sharp", &tree_sitter_c_sharp},
        {"ruby", &tree_sitter_ruby},
        {"qmljs", &tree_sitter_qmljs},
        {"make", &tree_sitter_make},
        {"css", &tree_sitter_css},
        // Ship 3, second batch.
        {"scala", &tree_sitter_scala},
        {"zig", &tree_sitter_zig},
        {"julia", &tree_sitter_julia},
        {"objc", &tree_sitter_objc},
    };
    return all;
}

const TSLanguage *languageFor(const QString &id) {
    for (const Language &l : languages())
        if (id == QLatin1String(l.id)) return l.language();
    return nullptr;
}

namespace {

// Extension -> language id, for every § 2.3 language of every ship.
const QHash<QString, QString> &extensionMap() {
    static const QHash<QString, QString> map = [] {
        QHash<QString, QString> m;
        auto add = [&m](const char *id, std::initializer_list<const char *> exts) {
            for (const char *e : exts)
                m.insert(QString::fromLatin1(e), QString::fromLatin1(id));
        };
        add("c", {"c"});
        add("cpp", {"cpp", "cc", "cxx", "h", "hpp", "hh", "hxx"});
        add("python", {"py", "pyi"});
        add("bash", {"sh", "bash"});
        add("lua", {"lua"});
        // SymbolQuery's ANTS-3558 list: the unambiguous shader stages only.
        add("glsl", {"glsl", "comp", "frag", "vert", "geom", "tesc", "tese",
                     "vsh", "fsh", "mesh", "task", "rgen", "rchit", "rmiss",
                     "rahit", "rint", "rcall"});
        add("javascript", {"js", "jsx", "mjs", "cjs"});
        add("typescript", {"ts", "mts", "cts"});
        add("tsx", {"tsx"});
        add("rust", {"rs"});
        add("go", {"go"});
        add("java", {"java"});
        // Ship 3, first batch.
        add("c_sharp", {"cs"});
        add("make", {"mk", "mak"});
        add("css", {"css"});
        add("ruby", {"rb"});
        add("qmljs", {"qml"});
        // Ship 3, second batch.
        add("scala", {"scala", "sc"});
        add("zig", {"zig"});
        add("julia", {"jl"});
        add("objc", {"m", "mm"});
        // Ship 3: known, not yet vendored, so reported as no_grammar.
        add("kotlin", {"kt", "kts"});
        add("swift", {"swift"});
        add("php", {"php"});
        add("cmake", {"cmake"});
        add("dart", {"dart"});
        add("elixir", {"ex", "exs"});
        add("haskell", {"hs"});
        add("ocaml", {"ml", "mli"});
        add("r", {"r"});
        add("nix", {"nix"});
        add("sql", {"sql"});
        add("perl", {"pl", "pm"});
        return m;
    }();
    return map;
}

}  // namespace

bool isKnownLanguage(const QString &id) {
    for (const QString &v : extensionMap())
        if (v == id) return true;
    return false;
}

QString languageForFile(const QString &fileName, const QByteArray &head) {
    if (fileName == QLatin1String("CMakeLists.txt")) return QStringLiteral("cmake");
    if (fileName == QLatin1String("Makefile") || fileName == QLatin1String("makefile") ||
        fileName == QLatin1String("GNUmakefile"))
        return QStringLiteral("make");
    const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
    if (dot > 0)
        return extensionMap().value(fileName.mid(dot + 1).toLower());
    if (!head.startsWith("#!")) return {};
    // "#!/usr/bin/env python3", "#!/bin/bash": the interpreter's basename.
    const QByteArray line = head.left(head.indexOf('\n')).mid(2).trimmed();
    QList<QByteArray> words = line.split(' ');
    words.removeAll(QByteArray());
    QByteArray interp = words.isEmpty() ? QByteArray() : words.first();
    interp = interp.mid(interp.lastIndexOf('/') + 1);
    if (interp == "env" && words.size() > 1) interp = words.at(1);
    if (interp.startsWith("python")) return QStringLiteral("python");
    if (interp == "bash" || interp == "sh" || interp == "zsh" || interp == "dash")
        return QStringLiteral("bash");
    if (interp.startsWith("lua")) return QStringLiteral("lua");
    if (interp == "node") return QStringLiteral("javascript");
    if (interp == "perl") return QStringLiteral("perl");
    if (interp == "ruby") return QStringLiteral("ruby");
    return {};
}

}  // namespace trace
