// ANTS-3374 — see buildfixhint.h.

#include "buildfixhint.h"

#include "symbolquery.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QRegularExpressionMatch>

#include <array>

namespace BuildFixHint {
namespace {

bool hasSuffix(const QString &path, std::initializer_list<QLatin1String> exts) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    for (const QLatin1String &e : exts)
        if (suffix == e) return true;
    return false;
}

bool isHeaderPath(const QString &path) {
    return hasSuffix(path, {QLatin1String("h"), QLatin1String("hpp"),
                            QLatin1String("hh"), QLatin1String("hxx"),
                            QLatin1String("h++"), QLatin1String("cuh")});
}

bool isSourcePath(const QString &path) {
    return hasSuffix(path, {QLatin1String("cpp"), QLatin1String("cc"),
                            QLatin1String("cxx"), QLatin1String("c++"),
                            QLatin1String("c"), QLatin1String("cu")});
}

// foo.cpp → foo.h (project-relative, same directory). "" if not a source.
QString siblingHeader(const QString &sourcePath) {
    if (!isSourcePath(sourcePath)) return {};
    const QFileInfo fi(sourcePath);
    const QString dir = fi.path();  // "" for a bare filename
    const QString stem = fi.completeBaseName();
    const QString name = stem + QStringLiteral(".h");
    return (dir.isEmpty() || dir == QLatin1String("."))
               ? name
               : dir + QLatin1Char('/') + name;
}

}  // namespace

QString undeclaredSymbol(const QString &message) {
    // group(1) captures the identifier in each recognised diagnostic form.
    static const std::array<QRegularExpression, 4> res = {
        QRegularExpression(QStringLiteral(
            "'([A-Za-z_][A-Za-z0-9_]*)' has not been declared")),
        QRegularExpression(QStringLiteral(
            "'([A-Za-z_][A-Za-z0-9_]*)' was not declared in this scope")),
        QRegularExpression(QStringLiteral(
            "unknown type name '([A-Za-z_][A-Za-z0-9_]*)'")),
        QRegularExpression(QStringLiteral(
            "use of undeclared identifier '([A-Za-z_][A-Za-z0-9_]*)'")),
    };
    for (const QRegularExpression &re : res) {
        const QRegularExpressionMatch m = re.match(message);
        if (m.hasMatch()) return m.captured(1);
    }
    return {};
}

QString resolveHeader(const QString &rootCanonical, const QString &symbol) {
    if (rootCanonical.isEmpty() || !SymbolQuery::isValidSymbol(symbol))
        return {};

    SymbolQuery::Options opts;  // tool defaults (maxResults 50, maxFiles 5000)
    const SymbolQuery::DefResult res =
        SymbolQuery::findDefinition(rootCanonical, symbol, opts);

    // 1. First header-suffixed match — the include you actually want.
    for (const SymbolQuery::DefMatch &d : res.definitions)
        if (isHeaderPath(d.file)) return d.file;

    // 2. Fallback: the on-disk sibling header of a source-only definition
    //    (a symbol defined in foo.cpp is conventionally declared in foo.h).
    const QDir root(rootCanonical);
    for (const SymbolQuery::DefMatch &d : res.definitions) {
        const QString sib = siblingHeader(d.file);
        if (!sib.isEmpty() && QFileInfo::exists(root.filePath(sib)))
            return sib;
    }
    return {};
}

}  // namespace BuildFixHint
