// ANTS-3758 — the roadmap render. Spec: docs/specs/ANTS-3758-roadmap-render.md

#include "roadmaprender.h"

#include "roadmapparse.h"
#include "roadmapstore.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <memory>
#include <vector>

namespace RoadmapRender {
namespace {

bool fail(QString *error, const QString &msg) {
    if (error)
        *error = msg;
    return false;
}

}  // namespace  (ANTS-3793 — emojiFor is exported; see roadmaprender.h)

// roadmap-format.md § 3.3's four status emojis. `dropped` deliberately has no
// glyph: § 3.11 makes a fifth an anti-pattern, and INV-4 excludes those items
// from every rendered file, so a dropped item never reaches this function.
QString emojiFor(const QString &status) {
    if (status == QLatin1String("shipped"))     return QString::fromUtf8(RoadmapParse::kEmojiDone);
    if (status == QLatin1String("in-progress")) return QString::fromUtf8(RoadmapParse::kEmojiInProgress);
    if (status == QLatin1String("considered"))  return QString::fromUtf8(RoadmapParse::kEmojiConsidered);
    if (status == QLatin1String("planned"))     return QString::fromUtf8(RoadmapParse::kEmojiPlanned);
    return QString();
}

namespace {

bool isOpen(const QString &status) {
    // roadmap-data-model.md § 3.4, exactly.
    return status == QLatin1String("planned") || status == QLatin1String("in-progress")
        || status == QLatin1String("considered");
}

bool isRenderable(const RoadmapStore::ItemWrite &it) {
    return it.visibility != QLatin1String("internal") && it.status != QLatin1String("dropped");
}

// Continuation lines carry two spaces, per roadmap-format.md § 3.5's example.
void appendIndented(QStringList *out, const QString &text) {
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &l : lines)
        out->append(l.isEmpty() ? QString() : QStringLiteral("  ") + l);
}

} // namespace

// One bullet in full § 3.5 form.
//
// ANTS-3808 § 2.3 — each trailer key is emitted from its column UNLESS the
// body would already re-parse to that same value. Value equality and not mere
// presence: a body line reading `Kind: refactor` — stale prose, or a key the
// author typed into the body — has the key present and a value the store
// disagrees with, and suppressing on presence would drop the canonical column
// line and leave the STALE value as the only one in the file, which the next
// migration would read back and adopt. Comparing values means a mismatch always
// emits from the column, which is canonical.
//
// INV-12 still holds as written: it asserts against the RENDERED TEXT, and
// whichever branch the test takes, the required `Kind:` piece is in the output
// exactly once. `offset >= 0` is what keeps that true — an ABSENT key must
// never suppress, or an item with an empty `kind` would compare equal to an
// empty body and the required line would vanish.
QString bulletText(const RoadmapStore::ItemWrite &it) {
    QStringList lines;
    QString head = QStringLiteral("- ") + emojiFor(it.status);
    if (!it.id.isEmpty())
        head += QStringLiteral(" [") + it.id + QLatin1Char(']');
    head += QStringLiteral(" **") + it.headline + QStringLiteral("**");
    lines.append(head);

    // One accessor call per bullet, reused across all five comparisons: calling
    // it per key would be five passes and up to thirty matches per bullet.
    const RoadmapParse::TrailerValues tv = RoadmapParse::trailerValuesIn(it.body);
    const auto shadows = [](const RoadmapParse::TrailerMatch &m, const QString &v) {
        return m.offset >= 0 && m.value == v;
    };

    if (!it.body.isEmpty())
        appendIndented(&lines, it.body);
    if (!it.layman.isEmpty() && !shadows(tv.layman, it.layman))
        appendIndented(&lines, QStringLiteral("**Layman:** ") + it.layman);
    // Required piece (INV-12) — emitted whenever the body does not already
    // carry this exact value.
    if (!shadows(tv.kind, it.kind))
        appendIndented(&lines, QStringLiteral("Kind: ") + it.kind + QLatin1Char('.'));
    // ANTS-4065 § 2.4 — a `source` the IMPORT supplied is not written back into
    // the file. `roadmap-format.md` § 3.5.3 makes `planned` the default, so
    // rendering it turns "this bullet said nothing" into "this bullet says
    // planned", and the next import reads it as asserted: 363 lines the first
    // real migration invented.
    //
    // The test is `!= "defaulted"`, NOT `== "asserted"`, and the direction is
    // the whole finding. `provenance` is NOT NULL DEFAULT '{}', so a row written
    // by anything other than makeItem() — roadmap_log op:append, for one —
    // carries no `source` key at all; tested for equality with "asserted" every
    // such row would silently lose its Source: line. ABSENT provenance means
    // "not a defaultable field", never "defaulted".
    //
    // Scoped to `source` alone. `layman`, `lanes` and `evidence` never carry a
    // provenance entry, so a rule phrased over "not asserted" would suppress
    // three fields § 3.5 defines — a larger loss than the one this prevents.
    const bool assertedSource =
        it.provenance.value(QStringLiteral("source")).toString()
            != QLatin1String("defaulted");
    if (!it.source.isEmpty() && !shadows(tv.source, it.source) && assertedSource)
        appendIndented(&lines, QStringLiteral("Source: ") + it.source + QLatin1Char('.'));
    // The two list-valued keys compare ELEMENT BY ELEMENT, never as a join:
    // comparing a pre-split string against a joined column diverges on
    // separator spacing alone, so suppression would never fire for them and
    // half the duplication would survive a passing test.
    if (!it.lanes.isEmpty() && !(tv.lanes.offset >= 0 && tv.lanesList == it.lanes))
        appendIndented(&lines, QStringLiteral("Lanes: ") + it.lanes.join(QStringLiteral(", ")) + QLatin1Char('.'));
    // Rendered WITHOUT a trailing period: paths contain dots, so a sentence
    // period would read as part of the last path (roadmap-format.md § 3.5).
    if (!it.evidence.isEmpty() && !(tv.evidence.offset >= 0 && tv.evidenceList == it.evidence))
        appendIndented(&lines, QStringLiteral("Evidence: ") + it.evidence.join(QStringLiteral(", ")));

    return lines.join(QLatin1Char('\n'));
}

namespace {

// The status legend is stored STRUCTURED (roadmap-data-model.md § 5.1 — status
// value → that project's wording), so rendering it is the inverse of the
// migration's parse, not a verbatim replay: emitting `project.legend`'s JSON
// into markdown would publish a JSON object. Key order is the export's
// kStatusOrder, so two renders of one store agree (INV-7) and a re-load
// reproduces the same object (INV-1).
//
// `dropped` is skipped: roadmap-format.md § 3.11 makes a fifth status emoji an
// anti-pattern, so it has no markdown form to emit a legend line in.
QString renderLegend(const QJsonObject &legend) {
    static const char *const kStatusOrder[] = {"planned", "in-progress", "shipped",
                                               "considered"};
    QStringList lines;
    for (const char *status : kStatusOrder) {
        const QString key = QLatin1String(status);
        if (!legend.contains(key))
            continue;
        lines.append(QStringLiteral("- ") + emojiFor(key) + QLatin1Char(' ')
                     + legend.value(key).toString());
    }
    return lines.join(QLatin1Char('\n'));
}

// § 3.1's marker. The live file REPLAYS it out of the synthetic root's intro;
// only a file with no root section of its own is given this constant (see the
// per-file assembly below).
QString formatMarker() {
    return QStringLiteral("<!-- ants-roadmap-format: 1 -->");
}

bool hasMarkerInHead(const QString &text) {
    const QStringList lines = text.split(QLatin1Char('\n'));
    const int n = std::min(5, int(lines.size()));
    for (int i = 0; i < n; ++i)
        if (lines.at(i).contains(QLatin1String("ants-roadmap-format")))
            return true;
    return false;
}

// ANTS-3832 — a `table` element is the one kind the render SERIALISES rather
// than replays. Its payload is roadmap-data-model.md § 5.2's canonical
// {header, rows} JSON, not the author's bytes, so emitting it verbatim (as
// § 2.2 used to say) writes raw JSON where the rows belong.
//
// The separator row is delimiter rather than content, so the migration never
// stored one and it is synthesised here. A literal `|` in a cell is spelled
// `\|`, which the migration's tableCells() inverts exactly — the escaping has
// to be invertible or INV-1's round-trip fails on the first pipe-bearing cell.
// A newline cannot be spelled in a GFM row invertibly at all, which is why it
// is folded to `<br>` at the WRITE boundary (roadmap_log's bundle_row) and
// never reaches here.
QString escapeCell(const QString &cell) {
    QString s = cell;
    s.replace(QLatin1Char('|'), QLatin1String("\\|"));
    return s;
}

QString tableRow(const QJsonArray &cells) {
    QStringList out;
    out.reserve(int(cells.size()));
    for (const QJsonValue &c : cells)
        out.append(escapeCell(c.toString()));
    return QStringLiteral("| ") + out.join(QStringLiteral(" | ")) + QStringLiteral(" |");
}

// A payload the store accepted but this cannot render is corrupt, and the
// render REFUSES it rather than emitting a malformed table — the same posture
// INV-9 takes on a store that disagrees with itself, and the one that would
// have caught this defect had it existed.
std::optional<QString> tableText(const QString &payload, QString *error) {
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        fail(error, QStringLiteral("table payload is not a JSON object: %1").arg(pe.errorString()));
        return std::nullopt;
    }
    const QJsonObject o = doc.object();
    const QJsonArray header = o.value(QStringLiteral("header")).toArray();
    const QJsonArray rows = o.value(QStringLiteral("rows")).toArray();
    if (header.isEmpty()) {
        fail(error, QStringLiteral("table payload carries no \"header\" columns"));
        return std::nullopt;
    }

    QStringList lines;
    lines.append(tableRow(header));
    lines.append(QStringLiteral("|") + QStringLiteral(" --- |").repeated(int(header.size())));
    for (const QJsonValue &r : rows) {
        const QJsonArray cells = r.toArray();
        if (cells.size() != header.size()) {
            fail(error, QStringLiteral("table row has %1 cells against %2 columns")
                            .arg(cells.size()).arg(header.size()));
            return std::nullopt;
        }
        lines.append(tableRow(cells));
    }
    return lines.join(QLatin1Char('\n'));
}

// INV-13. Both source_path and liveRoadmapPath go through this: the live
// roadmap is the one path every project uses, and exempting it would hollow the
// invariant out. Canonicalises BOTH sides — canonicalising only one computes a
// path out of the project (the ANTS-3782 § 2.4 lesson, on the write side).
bool resolveUnderRoot(const QString &root, const QString &rel, QString *abs, QString *error) {
    if (rel.isEmpty())
        return fail(error, QStringLiteral("empty render path is a refusal, not a default"));

    const QString canonRoot = QFileInfo(root).canonicalFilePath();
    if (canonRoot.isEmpty())
        return fail(error, QStringLiteral("project root does not exist: %1").arg(root));

    const QString joined = QFileInfo(rel).isAbsolute() ? rel : QDir(canonRoot).filePath(rel);
    // The target need not exist yet, so canonicalise its PARENT and re-append
    // the file name; canonicalFilePath() on a missing file returns empty.
    const QFileInfo fi(joined);
    QDir parent = fi.dir();
    if (!parent.exists() && !parent.mkpath(QStringLiteral(".")))
        return fail(error, QStringLiteral("could not create directory: %1").arg(parent.path()));

    const QString canonParent = QFileInfo(parent.path()).canonicalFilePath();
    if (canonParent.isEmpty())
        return fail(error, QStringLiteral("could not resolve directory: %1").arg(parent.path()));

    const QString resolved = QDir(canonParent).filePath(fi.fileName());
    if (resolved != canonRoot && !resolved.startsWith(canonRoot + QLatin1Char('/')))
        return fail(error, QStringLiteral("render path escapes the project root: %1").arg(rel));
    if (QFileInfo::exists(resolved) && !QFileInfo(resolved).isFile())
        return fail(error, QStringLiteral("render target is not a regular file: %1").arg(resolved));

    *abs = resolved;
    return true;
}

} // namespace

std::optional<Outcome> render(RoadmapStore &store, qint64 projectId,
                              const QString &projectRoot, const Options &opts,
                              QString *error) {
    if (error)
        error->clear();

    const auto project = store.readProject(projectId, error);
    if (!project) {
        if (error && error->isEmpty())
            *error = QStringLiteral("no project with id %1").arg(projectId);
        return std::nullopt;
    }

    const auto sections = store.listSections(projectId, error);
    if (!sections)
        return std::nullopt;
    const auto itemRefs = store.listItems(projectId, error);
    if (!itemRefs)
        return std::nullopt;

    // Read every item once. The gate, the membership filter and the bullets all
    // need the full row, and listItems() carries only the matching key.
    QHash<qint64, RoadmapStore::ItemWrite> itemOf;
    Outcome out;
    for (const RoadmapStore::ItemRef &ref : *itemRefs) {
        // INV-4 — an unfiled item has no element row and so no place in any
        // section. Rendered nowhere and counted nowhere is the silent loss § 2.4
        // refuses to permit, so this is a refusal rather than a skip.
        if (ref.sectionId == 0) {
            fail(error, QStringLiteral("item %1 is filed in no section").arg(ref.idFold));
            return std::nullopt;
        }
        const auto it = store.readItem(ref.itemPk, error);
        if (!it)
            return std::nullopt;
        if (!isRenderable(*it)) {
            ++out.itemsExcluded;
            continue;
        }
        // INV-5 — the gate is per PROJECT, so collect every offender rather
        // than stopping at the first.
        if (isOpen(it->status) && it->layman.isEmpty())
            out.gateFailures.append(it->id.isEmpty() ? ref.idFold : it->id);
        itemOf.insert(ref.itemPk, *it);
    }

    if (!out.gateFailures.isEmpty()) {
        std::sort(out.gateFailures.begin(), out.gateFailures.end());
        out.itemsRendered = int(itemOf.size());
        return out;   // engaged, filesWritten empty — INV-5
    }

    // § 2.2 — document order is sectionOrderLess()'s (position, slug).
    QVector<RoadmapStore::SectionRow> ordered = *sections;
    std::sort(ordered.begin(), ordered.end(), sectionOrderLess);

    // § 2.3 — route each section to its file, preserving the order above.
    QStringList fileOrder;
    QHash<QString, QVector<RoadmapStore::SectionRow>> byFile;
    for (const RoadmapStore::SectionRow &s : ordered) {
        QString abs;
        if (!resolveUnderRoot(projectRoot, s.sourcePath.value_or(opts.liveRoadmapPath), &abs, error))
            return std::nullopt;
        if (!byFile.contains(abs))
            fileOrder.append(abs);
        byFile[abs].append(s);
    }

    QString liveAbs;
    if (!resolveUnderRoot(projectRoot, opts.liveRoadmapPath, &liveAbs, error))
        return std::nullopt;

    // § 2.8 + § 2.2 — assemble each file. Exactly one blank line between
    // blocks; the file ends with exactly one newline.
    QHash<QString, QString> contentOf;
    for (const QString &path : fileOrder) {
        QStringList blocks;
        bool sawRoot = false;

        for (const RoadmapStore::SectionRow &s : byFile.value(path)) {
            const bool isRoot = s.level == 0;
            if (isRoot) {
                sawRoot = true;
                // Replayed verbatim: it already carries § 3.1's marker and the
                // H1, because the migration builds intros from raw source lines
                // and filters neither. Emitting our own would duplicate both.
                if (!s.intro.isEmpty())
                    blocks.append(s.intro);
                if (!project->legendText.isEmpty() && path == liveAbs) {
                    const QJsonObject entries =
                        QJsonDocument::fromJson(project->legendText.toUtf8()).object();
                    const QString legend = renderLegend(entries);
                    if (!legend.isEmpty())
                        blocks.append(legend);
                }
            } else {
                // INV-9 — level is emitted, parent_id is only checked.
                if (s.parentId) {
                    const auto parent = store.readSection(*s.parentId, error);
                    if (!parent)
                        return std::nullopt;
                    if (s.level != parent->level + 1) {
                        fail(error, QStringLiteral("section '%1' has level %2 under a parent at level %3")
                                        .arg(s.slug).arg(s.level).arg(parent->level));
                        return std::nullopt;
                    }
                }
                blocks.append(QString(s.level, QLatin1Char('#')) + QLatin1Char(' ') + s.title);
                if (!s.intro.isEmpty())
                    blocks.append(s.intro);
            }
            ++out.sectionsRendered;

            // SectionRow carries no section_id (§ 2.1), so the id comes back
            // through the existing slug lookup rather than by widening a struct
            // ANTS-3765 § 2.4 fixed for the migration's compare-before-write.
            const auto sectionId = store.findSection(projectId, s.slug, error);
            if (!sectionId) {
                if (error && error->isEmpty())
                    *error = QStringLiteral("section '%1' vanished between reads").arg(s.slug);
                return std::nullopt;
            }
            const auto elements = store.listElements(*sectionId, error);
            if (!elements)
                return std::nullopt;

            // INV-10 — one pass in stored position order, so narration and
            // tables stay interleaved with items rather than being grouped.
            for (const RoadmapStore::ElementRow &e : *elements) {
                if (e.kind == QLatin1String("item")) {
                    const auto it = itemOf.constFind(e.itemPk);
                    if (it == itemOf.constEnd())
                        continue;   // excluded by INV-4; already counted
                    blocks.append(bulletText(*it));
                    ++out.itemsRendered;
                } else if (e.kind == QLatin1String("table")) {
                    // Serialised, not replayed — the store holds § 5.2's
                    // canonical JSON here rather than the author's bytes
                    // (ANTS-3832).
                    const auto text = tableText(e.payload.value_or(QString()), error);
                    if (!text)
                        return std::nullopt;
                    blocks.append(*text);
                } else if (e.payload) {
                    // Narration: verbatim — never re-wrapped, never
                    // re-canonicalised. The store holds the author's bytes and
                    // INV-1 and INV-7 both rest on the render having no
                    // opinion about them.
                    blocks.append(*e.payload);
                }
            }
        }

        // A file with no root section of its own has no stored marker to
        // replay, so it gets the renderer's constant. INV-8 holds for every
        // file either way.
        //
        // That is a file with no PRE-HEADING CONTENT, not every archive
        // (corrected under ANTS-3806, which had read the empty-slug root as one
        // row per project). Each source's root takes `ctx.prefix` as its slug —
        // "" live, "<M>-<N>" for an archive — so an archive that has a preamble
        // stores it and replays it here; `walkSource()` drops the root only
        // when its source put nothing in it, and that file lands on this line.
        QString text = blocks.join(QStringLiteral("\n\n"));
        if (!sawRoot || !hasMarkerInHead(text))
            text = formatMarker() + QStringLiteral("\n\n") + text;
        if (!text.endsWith(QLatin1Char('\n')))
            text += QLatin1Char('\n');
        contentOf.insert(path, text);
    }

    out.filesWritten = fileOrder;
    if (opts.dryRun) {
        // INV-14 — nothing is opened, so nothing can change, not even an mtime.
        out.committed = true;
        return out;
    }

    // § 2.7 / INV-6 — stage every file before committing any, so no failure in
    // rendering, gating or serialising can leave a half-updated project.
    std::vector<std::unique_ptr<QSaveFile>> staged;
    staged.reserve(size_t(fileOrder.size()));
    for (const QString &path : fileOrder) {
        auto f = std::make_unique<QSaveFile>(path);
        if (!f->open(QIODevice::WriteOnly | QIODevice::Text)) {
            fail(error, QStringLiteral("could not open %1: %2").arg(path, f->errorString()));
            return std::nullopt;   // pre-commit: nothing has landed
        }
        const QByteArray bytes = contentOf.value(path).toUtf8();
        if (f->write(bytes) != bytes.size()) {
            fail(error, QStringLiteral("could not write %1: %2").arg(path, f->errorString()));
            return std::nullopt;
        }
        staged.push_back(std::move(f));
    }

    // The one window staging cannot close: commit() is per file, so a failure
    // here can land some of them. § 2.7 reports it rather than hiding it — and
    // reporting needs a channel that survives, which nullopt is not.
    QStringList landed;
    for (int i = 0; i < fileOrder.size(); ++i) {
        if (!staged[size_t(i)]->commit()) {
            fail(error, QStringLiteral("commit failed for %1 after %2 file(s) had landed")
                            .arg(fileOrder.at(i)).arg(landed.size()));
            out.filesWritten = landed;
            out.committed = false;
            return out;   // ENGAGED, so filesWritten survives
        }
        landed.append(fileOrder.at(i));
    }

    out.filesWritten = landed;
    out.committed = true;
    return out;
}

} // namespace RoadmapRender
