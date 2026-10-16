// ANTS-5734 § 2.1, § 2.8 — registers the verbs only ants-mcpd serves: today
// `trace`, whose engine (ants_trace_lib) ants-terminal does not link. The
// terminal never calls this, so its tools/list has no `trace` (INV-13).
// Contract: docs/specs/ANTS-5734-trace-verb.md.

#pragma once

#include "mcptoolregistry.h"
#include "trace/trace.h"

namespace mcp {

// `env` carries what the engine needs from the host beyond the arguments:
// ants-mcpd binds its roadmap read to RemoteControl::roadmapBulletsForTrace.
void registerMcpdOnlyVerbs(ToolSink &sink, const RegistryHost &host,
                           trace::Env env = {});

}  // namespace mcp
