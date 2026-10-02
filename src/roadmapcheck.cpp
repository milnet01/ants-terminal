// ANTS-3810 § 2.2 — see roadmapcheck.h.

#include "roadmapcheck.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <cstdint>

namespace RoadmapCheck {

namespace {

// roadmap-data-model.md § 6 names exactly these four as acyclic. relates-to is
// symmetric (a triangle of related items is an ordinary undirected cycle), and
// specified-by addresses a document, so neither is walked. Types are never
// folded: `A blocked-by B` with `B duplicate-of A` is not a cycle.
const QStringList &acyclicTypes() {
    static const QStringList kTypes = {
        QStringLiteral("splits-from"), QStringLiteral("blocked-by"),
        QStringLiteral("duplicate-of"), QStringLiteral("supersedes")};
    return kTypes;
}

using Key = QPair<QString, QString>;   // (export_slug, id_fold)

bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}

// item_pk -> (export_slug, id_fold), across the whole store.
bool itemKeys(RoadmapStore &store, QHash<qint64, Key> *out, QString *error) {
    QString err;
    const QVector<RoadmapStore::ProjectRow> projects = store.listProjects(&err);
    // listProjects() returns an empty list on a query error, not nullopt, so
    // the error string is the only signal that separates failed from empty.
    if (!err.isEmpty())
        return fail(error, err);
    for (const RoadmapStore::ProjectRow &p : projects) {
        const auto items = store.listItems(p.projectId, &err);
        if (!items)
            return fail(error, err);
        for (const RoadmapStore::ItemRef &r : *items)
            out->insert(r.itemPk, {p.exportSlug, r.idFold});
    }
    return true;
}

// Rotated so the smallest element leads; the cycle closes implicitly.
QVector<Key> canonical(QVector<Key> path) {
    const auto smallest = std::min_element(path.cbegin(), path.cend()) - path.cbegin();
    std::rotate(path.begin(), path.begin() + smallest, path.end());
    return path;
}

}  // namespace

std::optional<AcyclicityReport> findRelationshipCycles(RoadmapStore &store, QString *error) {
    QHash<qint64, Key> keys;
    if (!itemKeys(store, &keys, error))
        return std::nullopt;

    AcyclicityReport report;
    for (const QString &type : acyclicTypes()) {
        QString err;
        const auto edges = store.edgesOfType(type, &err);
        if (!edges) {
            fail(error, err);
            return std::nullopt;
        }
        const auto unresolved = store.unresolvedEdgeCount(type, &err);
        if (!unresolved) {
            fail(error, err);
            return std::nullopt;
        }
        report.unresolvedEdges += *unresolved;

        // De-duplicated first: edgesOfType()'s UNION ALL can return one pair
        // through two rows, and a duplicate edge would be a second back edge
        // naming the same cycle.
        QHash<qint64, QVector<qint64>> adj;
        QSet<QPair<qint64, qint64>> seen;
        for (const auto &e : *edges)
            if (!seen.contains(e)) {
                seen.insert(e);
                adj[e.first].append(e.second);
            }

        // Visit in key order, and each node's targets in key order, so "first
        // found wins" names the same cycle on every run.
        const auto byKey = [&keys](qint64 a, qint64 b) { return keys.value(a) < keys.value(b); };
        QVector<qint64> nodes = adj.keys().toVector();
        std::sort(nodes.begin(), nodes.end(), byKey);
        for (auto it = adj.begin(); it != adj.end(); ++it)
            std::sort(it->begin(), it->end(), byKey);

        // Iterative three-colour DFS: the depth is the store's, not the code's.
        enum Colour : std::uint8_t { White, Grey, Black };
        QHash<qint64, Colour> colour;
        int found = 0;
        bool capped = false;
        for (qint64 start : nodes) {
            if (capped)
                break;
            if (colour.value(start, White) != White)
                continue;
            struct Frame {
                qint64 node;
                int next = 0;
            };
            QVector<Frame> stack{Frame{start}};
            colour.insert(start, Grey);
            while (!stack.isEmpty() && !capped) {
                Frame &top = stack.last();
                const auto out = adj.constFind(top.node);
                if (out == adj.cend() || top.next >= out->size()) {
                    colour.insert(top.node, Black);
                    stack.removeLast();
                    continue;
                }
                const qint64 to = out->at(top.next++);
                const Colour c = colour.value(to, White);
                if (c == White) {
                    colour.insert(to, Grey);
                    stack.append(Frame{to});
                } else if (c == Grey) {
                    // Back edge: the cycle is the stack from `to` to the top.
                    QVector<Key> path;
                    bool on = false;
                    for (const Frame &f : stack) {
                        on = on || f.node == to;
                        if (on)
                            path.append(keys.value(f.node));
                    }
                    report.cycles.append({type, canonical(path)});
                    if (++found >= kMaxCyclesPerType) {
                        report.truncated = true;
                        capped = true;
                    }
                }
            }
        }
    }
    return report;
}

}  // namespace RoadmapCheck
