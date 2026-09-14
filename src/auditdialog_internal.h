#pragma once

// ANTS-1677 — helpers shared by the files of AuditDialog's source list,
// ANTS_AUDITDIALOG_SOURCES_REL in CMakeLists.txt. Include it only from those
// files; tests/features/split_sources holds that line. This header declares;
// every definition stays in a .cpp of the list.

#include "auditengine.h"

#include <QString>

namespace auditdialogdetail {

// find(1) and grep(1) exclude expressions, defined in auditdialog.cpp.
extern const QString kFindExcl;
extern const QString kGrepExcl;
extern const QString kGrepExclSec;

// Display labels, defined in auditdialog.cpp.
QString severityLabel(Severity s);
QString typeLabel(CheckType t);

}  // namespace auditdialogdetail
