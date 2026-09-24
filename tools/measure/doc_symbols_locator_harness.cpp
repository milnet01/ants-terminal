// ANTS-5313 measurement harness: DocSymbols::scan + locate over one doc per
// call, as doc_symbols {path:<doc>, mode:"locator"} does, emitting JSONL.
// Links the real engine from build/*.a, so it measures what ships.
//
// Build (after `cmake --build build`):
//   c++ -std=c++20 -O2 -fPIC -Isrc -isystem /usr/include/qt6/QtCore \
//     -isystem /usr/include/qt6 -isystem /usr/lib64/qt6/mkspecs/linux-g++ \
//     tools/measure/doc_symbols_locator_harness.cpp \
//     -Wl,--start-group build/*.a -Wl,--end-group \
//     -lQt6Core -lQt6Gui -lQt6Widgets -lQt6Network -lQt6Sql -lQt6DBus -o /tmp/h
// Run:
//   grep -oP '\["name"\] = "\K[a-z_0-9]+' src/claudeintegration.cpp | sort -u > names
//   ls docs/specs/*.md docs/standards/*.md | /tmp/h "$PWD" names > out.jsonl
// Then tools/measure/doc_symbols_locator_sample.py <n> in the same directory.
#include "docsymbols.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>
#include <cstdio>

int main(int argc, char **argv) {
    const QString root = QString::fromUtf8(argv[1]);
    QSet<QString> excl;
    // refusal codes, as docSymbolsRefusalCodes does
    { QFile f(root + "/docs/standards/mcp-error-codes.md"); f.open(QIODevice::ReadOnly);
      QRegularExpression re("`([a-z][a-z0-9]*(?:_[a-z0-9]+)+)`");
      auto it = re.globalMatch(QString::fromUtf8(f.readAll()));
      while (it.hasNext()) excl.insert(it.next().captured(1)); }
    // registered tool names, passed one per line in argv[2]
    { QFile f(QString::fromUtf8(argv[2])); f.open(QIODevice::ReadOnly);
      for (const QByteArray &l : f.readAll().split('\n')) if (!l.isEmpty()) excl.insert(QString::fromUtf8(l)); }
    QTextStream in(stdin);
    while (!in.atEnd()) {
        const QString rel = in.readLine().trimmed();
        if (rel.isEmpty()) continue;
        QFile f(QDir(root).filePath(rel));
        if (!f.open(QIODevice::ReadOnly)) continue;
        DocSymbols::Options o; o.rootCanonical = root; o.excludedNames = excl;
        const auto r = DocSymbols::scan(QString::fromUtf8(f.readAll()), rel, o);
        const auto l = DocSymbols::locate(r.symbols);
        QJsonObject out; out["doc"] = rel; out["truncated"] = r.truncated;
        QJsonObject loc; for (auto it = l.located.cbegin(); it != l.located.cend(); ++it) loc[it.key()] = it.value();
        QJsonObject amb; for (auto it = l.ambiguous.cbegin(); it != l.ambiguous.cend(); ++it) amb[it.key()] = it.value();
        // first doc line of each symbol, for the context check
        QJsonObject at; for (const auto &s : r.symbols) if (!at.contains(s.symbol)) at[s.symbol] = s.docLine;
        out["locators"] = loc; out["ambiguous"] = amb;
        out["unresolved"] = QJsonArray::fromStringList(l.unresolved);
        out["not_checked"] = QJsonArray::fromStringList(l.notChecked);
        out["doc_line"] = at;
        std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        std::fflush(stdout);
    }
}
