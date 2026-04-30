// Feature-conformance test for tests/features/roadmap_viewer_tabs/spec.md.
//
// Locks the ANTS-1100 contract for the redesigned RoadmapDialog:
// faceted preset tabs (Full / History / Current / Next / FarFuture /
// Custom), search predicate with `id:NNNN` shorthand, larger default
// size, and persisted geometry.
//
// Exit 0 = all 12 invariants hold.

#include "roadmapdialog.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

bool qcontains(const QString &hay, const char *needle) {
    return hay.contains(QString::fromUtf8(needle));
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    const std::string source = slurp(ROADMAPDIALOG_CPP);
    const std::string header = slurp(ROADMAPDIALOG_H);
    if (source.empty()) return fail("INV-1", "roadmapdialog.cpp not readable");
    if (header.empty()) return fail("INV-1", "roadmapdialog.h not readable");

    using Preset = RoadmapDialog::Preset;
    using SortOrder = RoadmapDialog::SortOrder;

    // INV-1: Full preset = all five bits.
    {
        const unsigned want =
            RoadmapDialog::ShowDone | RoadmapDialog::ShowPlanned |
            RoadmapDialog::ShowInProgress | RoadmapDialog::ShowConsidered |
            RoadmapDialog::ShowCurrent;
        if (RoadmapDialog::filterFor(Preset::Full) != want)
            return fail("INV-1", "Full preset should OR all five Show* bits");
    }

    // INV-2: History preset = ShowDone only.
    if (RoadmapDialog::filterFor(Preset::History) != RoadmapDialog::ShowDone)
        return fail("INV-2", "History preset should be ShowDone alone");

    // INV-3: Current preset = ShowInProgress | ShowCurrent.
    {
        const unsigned want =
            RoadmapDialog::ShowInProgress | RoadmapDialog::ShowCurrent;
        if (RoadmapDialog::filterFor(Preset::Current) != want)
            return fail("INV-3", "Current preset should be InProgress | Current");
    }

    // INV-4: Next preset = ShowPlanned only.
    if (RoadmapDialog::filterFor(Preset::Next) != RoadmapDialog::ShowPlanned)
        return fail("INV-4", "Next preset should be ShowPlanned alone");

    // INV-5: FarFuture preset = ShowConsidered only.
    if (RoadmapDialog::filterFor(Preset::FarFuture) != RoadmapDialog::ShowConsidered)
        return fail("INV-5", "FarFuture preset should be ShowConsidered alone");

    // INV-6: sortFor(History) is descending chronological; the other
    // named presets are document order.
    if (RoadmapDialog::sortFor(Preset::History) !=
        SortOrder::DescendingChronological)
        return fail("INV-6", "History sort should be DescendingChronological");
    const Preset documentSorted[] = {
        Preset::Full, Preset::Current, Preset::Next, Preset::FarFuture
    };
    for (Preset p : documentSorted) {
        if (RoadmapDialog::sortFor(p) != SortOrder::Document)
            return fail("INV-6", "non-history preset should be Document order");
    }

    // INV-7: Tab bar is the first widget in the dialog's vertical
    // layout. Source-grep: the QTabBar add to the root layout must
    // appear before the filter checkbox addLayout call.
    const size_t tabsAdd = source.find("addWidget(m_tabs)");
    const size_t filterAdd = source.find("addLayout(filterRow)");
    if (tabsAdd == std::string::npos)
        return fail("INV-7", "m_tabs not added to root layout");
    if (filterAdd == std::string::npos)
        return fail("INV-7", "filterRow not added to root layout");
    if (tabsAdd > filterAdd)
        return fail("INV-7", "m_tabs must be added before filterRow");

    // INV-8: presetMatching returns Custom when filter+sort matches no
    // named preset. Use a deliberately weird combo: Done|Considered with
    // descending sort.
    {
        const unsigned weird =
            RoadmapDialog::ShowDone | RoadmapDialog::ShowConsidered;
        if (RoadmapDialog::presetMatching(weird, SortOrder::Document) !=
            Preset::Custom)
            return fail("INV-8", "weird filter combo should match Custom");
        // Sanity: the Full preset values DO map back to Full.
        const unsigned full = RoadmapDialog::filterFor(Preset::Full);
        if (RoadmapDialog::presetMatching(full, SortOrder::Document) !=
            Preset::Full)
            return fail("INV-8", "Full filter+Document sort must round-trip");
        // History must round-trip too.
        if (RoadmapDialog::presetMatching(
                RoadmapDialog::filterFor(Preset::History),
                RoadmapDialog::sortFor(Preset::History)) != Preset::History)
            return fail("INV-8", "History filter+sort must round-trip");
    }

    // INV-9: DescendingChronological reverses top-level (`## `) section
    // order. Build a synthetic doc with two sections.
    {
        const QString doc = QStringLiteral(
            "## 0.5.0 — shipped\n"
            "- ✅ **Old item.** body.\n"
            "\n"
            "## 0.7.0 — recent\n"
            "- ✅ **New item.** body.\n");
        const QString rendered = RoadmapDialog::renderHtml(
            doc,
            RoadmapDialog::filterFor(Preset::History),
            {},
            QStringLiteral("default"),
            SortOrder::DescendingChronological,
            QString());
        const int posOld = rendered.indexOf(QStringLiteral("Old item"));
        const int posNew = rendered.indexOf(QStringLiteral("New item"));
        if (posOld < 0 || posNew < 0)
            return fail("INV-9", "expected both items rendered");
        if (posNew >= posOld)
            return fail("INV-9",
                        "DescendingChronological should put 0.7.0 before 0.5.0");

        // INV-9b (negative case per debt-sweep finding 2.1):
        // Document order against the same input must keep the
        // sections in their authored order (no implicit reverse).
        const QString docOrdered = RoadmapDialog::renderHtml(
            doc,
            RoadmapDialog::filterFor(Preset::History),
            {},
            QStringLiteral("default"),
            SortOrder::Document,
            QString());
        const int posOldD = docOrdered.indexOf(QStringLiteral("Old item"));
        const int posNewD = docOrdered.indexOf(QStringLiteral("New item"));
        if (posOldD < 0 || posNewD < 0)
            return fail("INV-9b", "expected both items rendered (Document)");
        if (posOldD >= posNewD)
            return fail("INV-9b",
                        "Document sort must preserve authored order");
    }

    // INV-10: Substring search predicate. Two bullets: one with `OSC 8`,
    // one without — only the matching bullet renders.
    {
        const QString doc = QStringLiteral(
            "## Section\n"
            "- 📋 **Hyperlink (OSC 8) support.** body.\n"
            "- 📋 **Quake mode.** body.\n");
        const unsigned all = RoadmapDialog::filterFor(Preset::Full);
        const QString hit = RoadmapDialog::renderHtml(
            doc, all, {}, QStringLiteral("default"),
            SortOrder::Document, QStringLiteral("OSC 8"));
        if (!qcontains(hit, "Hyperlink"))
            return fail("INV-10", "matching bullet should remain rendered");
        if (qcontains(hit, "Quake"))
            return fail("INV-10", "non-matching bullet should be filtered out");
    }

    // INV-11: id:NNNN shorthand matches the `[ANTS-NNNN]` bullet
    // regardless of headline content.
    {
        const QString doc = QStringLiteral(
            "## Section\n"
            "- 📋 [ANTS-1042] **Tabbed roadmap dialog.** body.\n"
            "- 📋 [ANTS-9999] **Other thing.** body.\n");
        const unsigned all = RoadmapDialog::filterFor(Preset::Full);
        const QString hit = RoadmapDialog::renderHtml(
            doc, all, {}, QStringLiteral("default"),
            SortOrder::Document, QStringLiteral("id:1042"));
        if (!qcontains(hit, "Tabbed roadmap"))
            return fail("INV-11", "id:1042 should keep [ANTS-1042] bullet");
        if (qcontains(hit, "Other thing"))
            return fail("INV-11", "id:1042 should drop the [ANTS-9999] bullet");
    }

    // INV-12: Dialog default size ≥ 1100x720; geometry persistence wired.
    // Per debt-sweep finding 2.2: parse the resize() literal and
    // assert numerically rather than matching a single magic-number
    // string — a future polish bump (e.g. 1400x900) shouldn't break
    // the test as long as the spec floor is respected.
    {
        std::regex resizeRx(R"(resize\(\s*(\d+)\s*,\s*(\d+)\s*\))");
        std::smatch m;
        if (!std::regex_search(source, m, resizeRx))
            return fail("INV-12",
                        "no resize(W, H) call found in roadmapdialog.cpp");
        const int width = std::stoi(m[1].str());
        const int height = std::stoi(m[2].str());
        if (width < 1100 || height < 720) {
            std::fprintf(stderr,
                         "[INV-12] resize=%dx%d below 1100x720 floor\n",
                         width, height);
            return 1;
        }
    }
    if (!contains(source, "roadmapDialogGeometry"))
        return fail("INV-12", "roadmapDialogGeometry persistence missing");
    if (!contains(source, "saveGeometry") ||
        !contains(source, "restoreGeometry"))
        return fail("INV-12", "saveGeometry/restoreGeometry round-trip missing");

    // INV-13 (debt-sweep finding 2.3): sortFor(Custom) is Document.
    // The Custom preset is the "user has diverged via checkboxes"
    // tab; it inherits document order so the dialog doesn't silently
    // re-sort when the user clicks a checkbox.
    if (RoadmapDialog::sortFor(Preset::Custom) != SortOrder::Document)
        return fail("INV-13",
                    "Custom preset must default to Document sort");

    std::puts("OK roadmap_viewer_tabs: 13/13 invariants");
    return 0;
}
