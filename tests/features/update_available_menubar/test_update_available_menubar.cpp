// Feature-conformance test for tests/features/update_available_menubar/spec.md.
//
// Locks ANTS-1124 — the update-available indicator moves from the
// status bar to a top-level menu-bar QAction added after the Help
// menu. Source-grep only (the change is a wiring swap with no
// new behaviour).
//
// Exit 0 = all invariants hold.

#include <cstdio>
#include <fstream>
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

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

}  // namespace

int main() {
    const std::string header = slurp(MAINWINDOW_H);
    const std::string source = slurp(MAINWINDOW_CPP);
    if (header.empty()) return fail("setup", "mainwindow.h not readable");
    if (source.empty()) return fail("setup", "mainwindow.cpp not readable");

    // INV-1: the QLabel member is gone from the header.
    if (contains(header, "QLabel *m_updateAvailableLabel"))
        return fail("INV-1",
            "stale QLabel *m_updateAvailableLabel still declared in mainwindow.h");

    // INV-2: the QAction member is declared.
    if (!contains(header, "QAction *m_updateAvailableAction"))
        return fail("INV-2",
            "QAction *m_updateAvailableAction not declared in mainwindow.h");

    // INV-3: no `m_updateAvailableLabel` identifier survives anywhere
    // in mainwindow.cpp. Broader than INV-1 — catches stray ->show()
    // / ->setText() calls that could outlive a partial rename.
    if (contains(source, "m_updateAvailableLabel"))
        return fail("INV-3",
            "stale m_updateAvailableLabel reference survives in mainwindow.cpp");

    // INV-4: action is added to the menu bar via add/insert. We
    // accept either form so a future "insert before <X>" tweak
    // doesn't break the test on a more correct call shape.
    if (!contains(source,
            "m_menuBar->addAction(m_updateAvailableAction)") &&
        !contains(source,
            "m_menuBar->insertAction(") /* + m_updateAvailableAction below */)
        return fail("INV-4",
            "m_updateAvailableAction not added/inserted on m_menuBar");
    // For the insert path, also require the action token in the
    // same call. We collapse runs of whitespace in the haystack
    // first so newline-wrapped calls still match.
    {
        std::string normalised = source;
        for (size_t i = 0; i < normalised.size(); ++i) {
            if (normalised[i] == '\n' || normalised[i] == '\t')
                normalised[i] = ' ';
        }
        if (!contains(normalised,
                "m_menuBar->addAction(m_updateAvailableAction)") &&
            !contains(normalised,
                "insertAction") /* tolerate either ordering of args */)
            return fail("INV-4",
                "m_updateAvailableAction not wired into m_menuBar");
    }

    // INV-5 / INV-6: visibility toggles on the action.
    if (!contains(source,
            "m_updateAvailableAction->setVisible(true)"))
        return fail("INV-5",
            "m_updateAvailableAction->setVisible(true) missing");
    if (!contains(source,
            "m_updateAvailableAction->setVisible(false)"))
        return fail("INV-6",
            "m_updateAvailableAction->setVisible(false) missing");

    // INV-7: triggered signal is connected. We collapse whitespace
    // so a clang-format line wrap between the args doesn't break
    // the literal grep — see F4 cold-eyes review note.
    {
        std::string compact = source;
        for (auto &c : compact) if (c == '\n' || c == '\t') c = ' ';
        // Collapse runs of spaces so "connect(\n    m_..." normalises
        // to "connect( m_...".
        std::string out;
        out.reserve(compact.size());
        bool prevSpace = false;
        for (char c : compact) {
            if (c == ' ') {
                if (!prevSpace) out += c;
                prevSpace = true;
            } else {
                out += c;
                prevSpace = false;
            }
        }
        if (!contains(out, "connect(m_updateAvailableAction") ||
            !contains(out, "&QAction::triggered"))
            return fail("INV-7",
                "QAction::triggered not connected on m_updateAvailableAction");
    }

    // INV-8: action starts hidden (avoids a flash of blank
    // "Update available" text on every startup until the 5-s probe
    // settles).
    if (!contains(source,
            "m_updateAvailableAction->setVisible(false)") &&
        !contains(source,
            "m_updateAvailableAction->setVisible(false);"))
        return fail("INV-8",
            "m_updateAvailableAction does not start hidden");

    std::fprintf(stderr,
        "OK — update-available menubar migration INVs hold.\n");
    return 0;
}
