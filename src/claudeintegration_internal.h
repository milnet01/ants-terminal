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

}  // namespace claudeintegrationdetail
