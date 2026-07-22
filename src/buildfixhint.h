// ANTS-3374 — likely_fix add_include hint for undeclared-symbol diagnostics.
//
// Stitches the existing diagnose→fix loop: on the most common C++ build
// error ("'X' has not been declared" / "unknown type name 'X'" and the
// GCC/clang siblings), find the header that declares X and suggest it as
// the missing #include. Powers the `likely_fix` field on `recent_errors`
// and `build_status` error entries.
//
// Pure, Qt6::Core-only — reuses SymbolQuery::findDefinition; lives in
// ants_core_lib next to symbolquery / scrollbackerrors / buildcache so
// the MCP dispatch path reaches it. Self-gating: resolveHeader returns a
// path ONLY when the symbol actually resolves to a project header, so a
// typo'd local (no matching symbol) never yields a spurious suggestion.
//
// Contract: tests/features/mcp_likely_fix/spec.md (ANTS-3374).

#ifndef ANTS_BUILDFIXHINT_H
#define ANTS_BUILDFIXHINT_H

#include <QString>

namespace BuildFixHint {

// If `message` is a "missing include" compiler diagnostic, return the
// undeclared symbol it names; empty otherwise. Recognised forms (GCC +
// clang), symbol captured from the quoted identifier:
//   'X' has not been declared            (GCC, qualified-id)
//   'X' was not declared in this scope   (GCC, bare identifier)
//   unknown type name 'X'                (clang, type position)
//   use of undeclared identifier 'X'     (clang, value position)
QString undeclaredSymbol(const QString &message);

// Resolve the project-relative header that declares `symbol`, or "" when
// it does not resolve to a header. `rootCanonical` is an already-canonical
// project root. Selection: the first header-suffixed definition/declaration
// SymbolQuery finds; failing that, the on-disk sibling header of a source
// definition (foo.cpp → foo.h). Returns the repo-relative path (e.g.
// "src/foo.h"), consistent with find_definition's file reporting.
QString resolveHeader(const QString &rootCanonical, const QString &symbol);

}  // namespace BuildFixHint

#endif  // ANTS_BUILDFIXHINT_H
