// ANTS-1222 + ANTS-1394 + ANTS-3582 — build metadata for Help → About
// and the MCP server-identity stamp (session_orient / serverInfo).
//
// STABLE header (ANTS-3582): these are `extern` DECLARATIONS only. The
// DEFINITIONS live in the generated build_info_values.cpp (see
// cmake/GenerateBuildInfoValues.cmake), compiled into ants_core_lib.
//
// Why decouple: this file used to be *generated*, with each value baked in as
// a preprocessor macro refreshed every build. Because the build minute changed
// on every cross-minute rebuild, the preprocessed form ccache hashes changed
// too, so the two ~1 MB translation units that stamp the build minute into the
// MCP serverInfo (remotecontrol.cpp, claudeintegration.cpp) paid a full
// recompile (~1-2 min) on essentially every real rebuild. Moving the values
// out of the header into a separately-compiled .cpp means a per-build value
// change recompiles ONLY build_info_values.o — never any consumer TU.
//
// Consumers keep `#include "build_info.h"` unchanged, but because the symbols
// are runtime `const char[]` arrays (NOT string literals), read them with
// QString::fromLatin1(ANTS_BUILD_*) — never QStringLiteral, which requires a
// compile-time string literal.
//
// Tarball builds (no .git) get ANTS_BUILD_COMMIT = "unknown".

#pragma once

extern const char ANTS_BUILD_DATE[];    // "YYYY-MM-DD"
extern const char ANTS_BUILD_TIME[];    // "HH:MM" (local; UTC under SOURCE_DATE_EPOCH)
extern const char ANTS_BUILD_TYPE[];    // "Release" / "Debug" / ...
extern const char ANTS_BUILD_COMMIT[];  // short git SHA, or "unknown"
