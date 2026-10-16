// ANTS-5734 — see extract.h.

#include "trace/extract.h"

#include "trace/languages.h"

#include <tree_sitter/api.h>

#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <list>
#include <tuple>
#include <utility>

namespace trace {

namespace {

constexpr qint64 kCacheCapBytes = 64LL * 1024 * 1024;   // § 5
constexpr int kSignatureMax = 200;

struct Deadline {
    QElapsedTimer timer;
    qint64 ms = 0;
};

bool cancelPastDeadline(TSParseState *state) {
    const auto *d = static_cast<const Deadline *>(state->payload);
    return d->timer.elapsed() > d->ms;   // true abandons the parse
}

QString nodeText(const QByteArray &text, TSNode n) {
    const uint32_t s = ts_node_start_byte(n), e = ts_node_end_byte(n);
    return QString::fromUtf8(text.constData() + s, int(e - s));
}

ByteRange rangeOf(TSNode n) { return {ts_node_start_byte(n), ts_node_end_byte(n)}; }

// The node's text up to its body (the first child that is a block, a field
// list or a class body), folded to one line and cut at kSignatureMax bytes.
QString signatureOf(const QByteArray &text, TSNode node) {
    uint32_t end = ts_node_end_byte(node);
    const uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i) {
        const TSNode c = ts_node_child(node, i);
        const char *t = ts_node_type(c);
        const QByteArray type(t);
        if (type.contains("body") || type == "block" || type == "compound_statement" ||
            type == "field_declaration_list" || type == "declaration_list" ||
            type == "statement_block" || type == "class_body" ||
            type == "enumerator_list" || type == "do_group") {
            end = ts_node_start_byte(c);
            break;
        }
    }
    const uint32_t start = ts_node_start_byte(node);
    QByteArray raw = text.mid(int(start), int(end - start)).simplified();
    if (raw.size() > kSignatureMax) {
        raw.truncate(kSignatureMax);
        // Do not leave half a UTF-8 sequence.
        while (!raw.isEmpty() && (uchar(raw.back()) & 0xC0) == 0x80) raw.chop(1);
        if (!raw.isEmpty() && (uchar(raw.back()) & 0x80)) raw.chop(1);
    }
    return QString::fromUtf8(raw);
}

bool isCommentType(const QByteArray &t) { return t.contains("comment"); }

bool isStringType(const QByteArray &t) {
    return t.endsWith("string") || t.endsWith("string_literal") ||
           t == "template_string" || t == "char_literal" || t == "heredoc_body" ||
           t == "system_lib_string" || t == "raw_string" ||
           t == "string_value";   // CSS
}

// Comment and string ranges, outermost only (a string inside a comment is
// part of the comment).
void collectCommentsAndStrings(TSNode root, FileRows &rows) {
    TSTreeCursor cur = ts_tree_cursor_new(root);
    for (;;) {
        const TSNode n = ts_tree_cursor_current_node(&cur);
        bool descend = true;
        if (ts_node_is_named(n)) {
            const QByteArray t(ts_node_type(n));
            if (isCommentType(t)) {
                rows.comments.append(rangeOf(n));
                descend = false;
            } else if (isStringType(t)) {
                rows.strings.append(rangeOf(n));
                descend = false;
            }
        }
        if (descend && ts_tree_cursor_goto_first_child(&cur)) continue;
        bool moved = false;
        while (!moved) {
            if (ts_tree_cursor_goto_next_sibling(&cur)) {
                moved = true;
            } else if (!ts_tree_cursor_goto_parent(&cur)) {
                ts_tree_cursor_delete(&cur);
                return;
            }
        }
    }
}

struct ScopeNode {
    ByteRange range;
    QString name;
};

// Strict containment: a node is not its own scope.
bool strictlyInside(const ByteRange &inner, const ByteRange &outer) {
    return outer.start <= inner.start && inner.end <= outer.end &&
           !(outer.start == inner.start && outer.end == inner.end);
}

}  // namespace

qint64 FileRows::bytes() const {
    qint64 b = sizeof(FileRows);
    for (const DefRow &d : defs)
        b += qint64(sizeof(DefRow)) + 2 * (d.name.size() + d.kind.size() + d.signature.size()) +
             64 * d.scope.size();
    for (const CallRow &c : calls) b += qint64(sizeof(CallRow)) + 2 * c.name.size();
    b += qint64(sizeof(ByteRange)) * (imports.size() + comments.size() + strings.size());
    return b;
}

std::shared_ptr<const FileRows> extractRows(const QString &langId, const TSQuery *query,
                                            const QByteArray &text, int timeoutMs) {
    auto rows = std::make_shared<FileRows>();
    const TSLanguage *lang = languageFor(langId);
    if (!lang || !query) return rows;

    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, lang);
    Deadline deadline;
    deadline.ms = timeoutMs;
    deadline.timer.start();
    TSParseOptions opts{&deadline, &cancelPastDeadline};
    struct Payload { const QByteArray *text; } payload{&text};
    TSInput input{&payload,
                  [](void *p, uint32_t byte, TSPoint, uint32_t *read) -> const char * {
                      const QByteArray *t = static_cast<Payload *>(p)->text;
                      if (byte >= uint32_t(t->size())) {
                          *read = 0;
                          return "";
                      }
                      *read = uint32_t(t->size()) - byte;
                      return t->constData() + byte;
                  },
                  TSInputEncodingUTF8, nullptr};
    TSTree *tree = ts_parser_parse_with_options(parser, nullptr, input, opts);
    ts_parser_delete(parser);
    if (!tree) {
        rows->timedOut = true;
        return rows;
    }
    const TSNode root = ts_tree_root_node(tree);
    rows->hasErrors = ts_node_has_error(root);

    // Capture ids by role, resolved once per query.
    const uint32_t captureCount = ts_query_capture_count(query);
    QVector<QByteArray> capName(static_cast<int>(captureCount));
    for (uint32_t i = 0; i < captureCount; ++i) {
        uint32_t len = 0;
        const char *s = ts_query_capture_name_for_id(query, i, &len);
        capName[int(i)] = QByteArray(s, int(len));
    }

    QVector<ScopeNode> scopes;
    TSQueryCursor *qc = ts_query_cursor_new();
    ts_query_cursor_exec(qc, query, root);
    TSQueryMatch m;
    while (ts_query_cursor_next_match(qc, &m)) {
        TSNode outer{}, name{};
        bool haveOuter = false, haveName = false;
        QByteArray role;
        QString qualifier;
        for (uint16_t i = 0; i < m.capture_count; ++i) {
            const QByteArray &cn = capName[int(m.captures[i].index)];
            if (cn == "name" || cn == "path") {
                name = m.captures[i].node;
                haveName = true;
            } else if (cn == "qualifier") {
                qualifier = nodeText(text, m.captures[i].node);
            } else {
                outer = m.captures[i].node;
                role = cn;
                haveOuter = true;
            }
        }
        if (!haveOuter) continue;
        if (role == "scope") {
            if (haveName) scopes.append({rangeOf(outer), nodeText(text, name)});
        } else if (role.startsWith("definition.") || role.startsWith("declaration.")) {
            if (!haveName) continue;
            DefRow d;
            d.name = nodeText(text, name);
            d.isDeclaration = role.startsWith("declaration.");
            d.kind = QString::fromLatin1(role.mid(role.indexOf('.') + 1));
            const TSPoint sp = ts_node_start_point(outer), ep = ts_node_end_point(outer);
            d.line = int(sp.row) + 1;
            // A node ending at column 0 ends on the line before (its newline).
            d.endLine = int(ep.row) + (ep.column == 0 && ep.row > sp.row ? 0 : 1);
            d.column = int(sp.column);
            d.node = rangeOf(outer);
            d.nameRange = rangeOf(name);
            d.signature = signatureOf(text, outer);
            d.qualifier = qualifier;
            rows->defs.append(d);
        } else if (role == "reference.call") {
            if (!haveName) continue;
            const TSPoint sp = ts_node_start_point(name);
            rows->calls.append({nodeText(text, name), int(sp.row) + 1, int(sp.column),
                                rangeOf(name)});
        } else if (role == "reference.import") {
            rows->imports.append(rangeOf(outer));
        }
    }
    ts_query_cursor_delete(qc);
    collectCommentsAndStrings(root, *rows);
    ts_tree_delete(tree);

    // A match can repeat (two patterns on one node): keep one row per node
    // and name.
    auto dedupe = [](auto &v, auto key) {
        std::stable_sort(v.begin(), v.end(), [&](const auto &a, const auto &b) {
            return key(a) < key(b);
        });
        v.erase(std::unique(v.begin(), v.end(),
                            [&](const auto &a, const auto &b) { return key(a) == key(b); }),
                v.end());
    };
    dedupe(rows->defs, [](const DefRow &d) {
        return std::make_tuple(d.node.start, d.node.end, d.nameRange.start, d.isDeclaration);
    });
    dedupe(rows->calls, [](const CallRow &c) { return c.nameRange.start; });
    dedupe(scopes, [](const ScopeNode &s) { return std::make_pair(s.range.start, s.range.end); });

    // Scopes, outermost first; a qualifier (`Foo::` in `void Foo::bar()`)
    // adds its parts innermost.
    for (DefRow &d : rows->defs) {
        for (const ScopeNode &s : std::as_const(scopes))
            if (strictlyInside(d.node, s.range)) d.scope << s.name;
        if (d.qualifier.isEmpty()) continue;
        // "ns::Foo<T>" -> "ns", "Foo": template arguments are not names.
        for (QString part : d.qualifier.split(QStringLiteral("::"), Qt::SkipEmptyParts)) {
            const qsizetype lt = part.indexOf(QLatin1Char('<'));
            if (lt >= 0) part.truncate(lt);
            part = part.trimmed();
            if (!part.isEmpty()) d.scope << part;
        }
    }
    // Function definitions directly inside a class are methods.
    for (DefRow &d : rows->defs) {
        if (d.kind != QLatin1String("function")) continue;
        for (const DefRow &c : std::as_const(rows->defs))
            if (c.kind == QLatin1String("class") && strictlyInside(d.node, c.node)) {
                d.kind = QStringLiteral("method");
                break;
            }
    }
    return rows;
}

namespace {

struct CacheEntry {
    qint64 mtimeMs;
    qint64 size;
    QByteArray queryHash;
    std::shared_ptr<const FileRows> rows;
    qint64 bytes;
    std::list<QString>::iterator lru;
};

QMutex g_cacheMutex;
QHash<QString, CacheEntry> g_cache;
std::list<QString> g_lru;   // front = most recently used
qint64 g_cacheBytes = 0;

}  // namespace

std::shared_ptr<const FileRows> rowsFor(const QString &absPath, qint64 mtimeMs, qint64 size,
                                        const QString &langId, const TSQuery *query,
                                        const QByteArray &queryHash,
                                        const QByteArray &text) {
    {
        QMutexLocker lock(&g_cacheMutex);
        auto it = g_cache.find(absPath);
        if (it != g_cache.end()) {
            if (it->mtimeMs == mtimeMs && it->size == size && it->queryHash == queryHash) {
                g_lru.splice(g_lru.begin(), g_lru, it->lru);
                return it->rows;
            }
            g_cacheBytes -= it->bytes;
            g_lru.erase(it->lru);
            g_cache.erase(it);
        }
    }
    auto rows = extractRows(langId, query, text, 2000);
    if (rows->timedOut) return rows;   // not cached: a retry may finish
    QMutexLocker lock(&g_cacheMutex);
    if (g_cache.contains(absPath)) return rows;   // another thread got there
    const qint64 bytes = rows->bytes() + 2 * absPath.size();
    g_lru.push_front(absPath);
    g_cache.insert(absPath, {mtimeMs, size, queryHash, rows, bytes, g_lru.begin()});
    g_cacheBytes += bytes;
    while (g_cacheBytes > kCacheCapBytes && g_lru.size() > 1) {
        const QString victim = g_lru.back();
        g_lru.pop_back();
        auto v = g_cache.find(victim);
        g_cacheBytes -= v->bytes;
        g_cache.erase(v);
    }
    return rows;
}

qint64 rowCacheBytes() {
    QMutexLocker lock(&g_cacheMutex);
    return g_cacheBytes;
}

void clearRowCache() {
    QMutexLocker lock(&g_cacheMutex);
    g_cache.clear();
    g_lru.clear();
    g_cacheBytes = 0;
}

}  // namespace trace
