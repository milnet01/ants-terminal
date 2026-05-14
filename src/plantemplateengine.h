// PlanTemplateEngine — pure-function helper for the `plan_template` MCP
// tool (ANTS-1290). Qt::Core only, no widgets.
//
// Emits an Ants-conventional implementation-plan skeleton (header,
// task blocks, self-review checklist, execution handoff) with
// project-specific conventions pre-baked. Replaces the
// `writing-plans` skill's template emission. See
// docs/specs/ANTS-1290.md for the full invariant list.

#pragma once

#include <QString>

namespace PlanTemplateEngine {

// Source of the ANTS-ID in the returned PlanResult.
//   Provided  — caller passed `antsId` arg, used as-is.
//   Allocated — engine called RoadmapFoldIn::allocateIds(root, 1).
//   None      — counter unavailable or caller didn't ask; skeleton uses
//               literal `<ANTS-ID>` placeholders.
enum class AntsIdSource { Provided, Allocated, None };

// Stable lowercase key for the MCP response. Mirrors VerifyEngine::gateKey.
QString antsIdSourceKey(AntsIdSource s);

// Knobs the caller can pass to `buildPlan`. featureName is required.
struct PlanOptions {
    QString featureName;            // required, kebab-case (INV-1)
    QString goal;                   // empty = placeholder
    QString architecture;
    QString techStack;
    int     taskCountHint = 4;      // clamped [1, 12] (INV-6)
    bool    includesTests = true;
    QString antsId;                 // empty = allocate from .roadmap-counter
    bool    save = false;
};

// Output of `buildPlan`. On success, `planMarkdown` is the full
// skeleton; on failure, `errorCode` / `errorMessage` are set and
// `planMarkdown` is empty.
struct PlanResult {
    bool         ok = false;
    QString      planMarkdown;
    QString      planPath;          // suggested OR actual when saved
    QString      antsId;            // empty when antsIdSource == None
    AntsIdSource antsIdSource = AntsIdSource::None;
    bool         saved = false;
    int          taskCount = 0;
    QString      errorCode;
    QString      errorMessage;
};

// Pure template emission + optional disk write.
//
//   - featureName validated against `^[a-z0-9_-]+$`, max 64 chars
//     (INV-1).
//   - If opts.antsId is empty, calls RoadmapFoldIn::allocateIds(
//     projectRoot, 1) to grab the next ID under flock + QSaveFile
//     (INV-3).
//   - When opts.save == false, performs ZERO disk writes (other than
//     the counter increment when allocating an ID) (INV-9).
//   - When opts.save == true, writes the skeleton atomically to
//     docs/plans/<id>-<feature>.md via QSaveFile, refusing to
//     overwrite (INV-5) and enforcing the strict-below path guard
//     (INV-4).
//
// `projectRoot` must already be canonical (caller's responsibility,
// same as VerifyEngine).
PlanResult buildPlan(const QString &projectRoot, const PlanOptions &opts);

}  // namespace PlanTemplateEngine
