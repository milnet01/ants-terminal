#pragma once

// ANTS-1677 — helpers shared by the files of AuditDialog's source list,
// ANTS_AUDITDIALOG_SOURCES_REL in CMakeLists.txt. Include it only from those
// files; tests/features/split_sources holds that line. This header declares;
// every definition stays in a .cpp of the list.

#include "auditengine.h"

#include <QString>

class Config;

namespace auditdialogdetail {

// find(1) and grep(1) exclude expressions, defined in auditdialog.cpp.
extern const QString kFindExcl;
extern const QString kGrepExcl;

// Display labels, defined in auditdialog.cpp.
QString severityLabel(Severity s);
QString typeLabel(CheckType t);

// A roadmap heading with its trailing CR/LF removed, as the fold paths
// match and insert it. Defined in auditdialog.cpp.
QString trimmedHeading(const QString &heading);

// AI triage, defined in auditdialog.cpp. aiConfigured: AI is on and has an
// endpoint. aiConfirmTarget: what a send-confirmation names as the
// destination — the endpoint's host, else the endpoint itself.
bool aiConfigured(const Config &cfg);
QString aiConfirmTarget(const Config &cfg);

}  // namespace auditdialogdetail
