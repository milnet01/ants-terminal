// ANTS-4919 — the helpers more than one claudeintegration translation unit
// uses, in `namespace claudeintegrationdetail`. The rules are ANTS-3833
// § 2.3's, applied by docs/specs/ANTS-1677-large-file-decomposition.md § 2.4:
//
//  1. A definition stays in a .cpp. The test tree scrapes the claudeintegration
//     sources for helper text, and anything whose text leaves those files
//     disappears from every such assertion silently.
//  2. A constant is declared `extern const`, never bare `const`: a bare
//     namespace-scope const has internal linkage and links clean anyway.
//
// INV-11: included ONLY by the files listed in ANTS_CLAUDEINTEGRATION_SOURCES.

#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace claudeintegrationdetail {

// tools/list schema-property helpers, shared by its descriptor pieces.
QJsonObject makeCallerCwdReadProp();
QString     callerCwdSuffix();
QJsonObject makeEtagMatchProp();
QJsonObject makeSymbolLangProp();
QJsonObject makeFieldsProp();
QJsonObject makeCompactProp();
QJsonObject makeRawProp();
QJsonObject makeWalkCallerCwdProp();
QJsonObject makeWalkPathsProp(const QString &description);
QJsonObject makeWalkIncludeProp(const QString &files);
QJsonObject makeDryRunProp();
QJsonObject makeEncodingProp();

// tools/list descriptor pieces (ANTS-1677 § 2.2 kind B). Each appends one
// contiguous run of today's descriptor order; handleMcpRequest calls them in
// this order, with the `trace` descriptor inline between Store and Reports
// because it reads ClaudeIntegration::m_toolProviders.
void appendToolSchemasTerminal(QJsonArray &tools);     // get_scrollback .. last_selection
void appendToolSchemasCodeNav(QJsonArray &tools);      // find_sources .. apply_edits
void appendToolSchemasDocs(QJsonArray &tools);         // codebase_index .. last_audit_summary
void appendToolSchemasOrientation(QJsonArray &tools);  // session_orient .. caller_cwd_info
void appendToolSchemasStore(QJsonArray &tools);        // roadmap_log .. session_cost
void appendToolSchemasReports(QJsonArray &tools);      // dependency_inventory .. session_memory

}  // namespace claudeintegrationdetail
