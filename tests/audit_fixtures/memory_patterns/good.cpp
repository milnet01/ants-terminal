// Safe allocation patterns — MUST NOT match the memory_patterns grep pattern
// directly. Raw heap-allocation tokens are only tripped when they appear as
// standalone words; this file is structured so comments and code alike avoid
// the literal tokens. Comment convention: UPPER-CASE spellings so case-
// sensitive \b<word> boundaries don't fire.
#include <vector>

struct Widget { int x; };

void by_value() {
    Widget w{};
    (void)w;
}

void by_vector() {
    std::vector<Widget> ws(4);
    (void)ws;
}

// Smart-pointer helpers spell without a trailing space, and raw heap calls
// (MALLOC / CALLOC / REALLOC) are described in UPPER-CASE to avoid matching
// the case-sensitive word-boundary pattern.

// --- Qt parent-child idioms: MUST NOT match (2026-04-16 triage) ---------
// The 0.6.30 audit-tool update tightened the memory_patterns regex so
// that parented heap allocations are suppressed — only empty parens and
// NULL-style arguments still fire. Lines below would have produced 30
// false positives under the old substring-blacklist approach; they must
// produce zero hits now. Comments here use UPPER-CASE to avoid matching
// the case-sensitive regex themselves.
// Dummy stand-ins with a ctor that accepts any pointer — keeps the LSP
// happy while still exercising the parented-allocation shape the regex
// must not match.
struct QObject { QObject(const void * = nullptr) {} };
struct QWidget { QWidget(const void * = nullptr) {} };
struct QAction { QAction(const void * = nullptr) {} };
void qt_parent_child_idioms(QObject *parent, QWidget *dlg, QWidget *tab,
                            QWidget *row, QWidget *group) {
    auto *a = new QAction(parent);       // ident → suppressed
    auto *b = new QWidget(dlg);          // ident → suppressed
    auto *c = new QWidget(tab);          // ident → suppressed
    auto *d = new QWidget(row);          // ident → suppressed
    auto *e = new QWidget(group);        // ident → suppressed
    auto *f = new QWidget(&*parent);     // deref-ref → suppressed
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
}
