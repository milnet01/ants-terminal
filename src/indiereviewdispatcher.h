// ANTS-1352 — server-side `indie_review_dispatch` orchestrator.
//
// Fires N parallel HTTP POSTs to a project-configured AI endpoint
// (OpenAI Chat Completions shape), saves each response to disk,
// returns a structured manifest. Displaces the per-Agent
// orchestration tax onto the upstream provider's billing.
//
// See docs/specs/ANTS-1352.md for the full design + invariants.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace IndieReviewDispatcher {

struct LaneRequest {
    QString name;
    QString brief;        // Assembled via IndieReviewEngine::assembleBriefForDispatch.
    // No projectPath; the dispatcher does not re-assemble (INV-23).
};

struct LaneResult {
    QString name;
    QString path;         // Project-relative on success, "" on failure.
    QString status;       // "ok" | "http_error" | "network_error" |
                          // "timeout" | "write_failed" | "prompt_too_large"
    QString error;        // Populated when status != "ok".
    qint64  bytes = 0;
    qint64  elapsedMs = 0;
    qint64  inputTokens = 0;
    qint64  outputTokens = 0;
};

struct DispatchRequest {
    QString projectRoot;           // Canonical absolute (pre-validated).
    QString reportsDir;            // Project-relative (pre-validated).
    QList<LaneRequest> lanes;
    QString model;                 // Resolved (already "llama3" if auto-from-Config).
    QString endpoint;              // Resolved Config::aiEndpoint().
    QString apiKey;                // Resolved Config::aiApiKey() — NEVER LOGGED.
    QString systemPrompt;          // Pinned text + system_extras already joined.
    int     concurrency = 4;
    int     maxTokens   = 64000;
    int     perLaneTimeoutMs = 300000;   // 5 min hard cap.
};

struct DispatchResult {
    bool                ok = true;
    QString             code;
    QString             error;
    QList<LaneResult>   reports;
    QString             resolvedModel;
    qint64              totalElapsedMs = 0;
};

DispatchResult dispatchLanes(const DispatchRequest &req);

// ANTS-1352-INV-9 — test probe. Returns the dispatcher's current
// in-flight queue depth (process-global; safe under
// per-(verb, cwd) in-flight gate). Returns 0 when no dispatch is
// in flight.
int inFlightCountForTest();

}  // namespace IndieReviewDispatcher
