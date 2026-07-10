// PlanTemplateEngine implementation — see plantemplateengine.h +
// docs/specs/ANTS-1290.md.

#include "plantemplateengine.h"

#include "roadmapfoldin.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

namespace PlanTemplateEngine {

namespace {

constexpr int  kTaskCountMin = 1;
constexpr int  kTaskCountMax = 12;
constexpr int  kFeatureNameMaxLen = 64;

bool isValidFeatureName(const QString &name) {
    if (name.isEmpty() || name.length() > kFeatureNameMaxLen) return false;
    static const QRegularExpression re(QStringLiteral("^[a-z0-9_-]+$"));
    return re.match(name).hasMatch();
}

// ANTS-1838 — a caller-supplied `ants_id` is spliced verbatim into the
// plan filename (docs/plans/<ants_id>-<feature>.md) and the rendered
// skeleton. The INV-4 canonicalisation guard only fires on save:true, so
// a dry-run call would otherwise echo a traversal id back unchecked.
// Validate at the trust boundary: project-prefixed roadmap-id shape
// (ANTS-1234, RETRO-42, …) — uppercase prefix, dash, digits. Empty is
// allowed (means "allocate from .roadmap-counter").
bool isValidAntsId(const QString &id) {
    if (id.isEmpty()) return true;
    static const QRegularExpression re(
        QStringLiteral("^[A-Z][A-Z0-9]{0,15}-[0-9]{1,9}$"));
    return re.match(id).hasMatch();
}

int clampTaskCount(int hint) {
    if (hint < kTaskCountMin) return kTaskCountMin;
    if (hint > kTaskCountMax) return kTaskCountMax;
    return hint;
}

// Title-case a kebab-case feature name for the h1 heading.
// "live-search" -> "Live Search"; "live_search" -> "Live Search".
QString titleCase(const QString &kebab) {
    QString out;
    out.reserve(kebab.size());
    bool capitalize = true;
    for (QChar c : kebab) {
        if (c == QLatin1Char('-') || c == QLatin1Char('_')) {
            out.append(QLatin1Char(' '));
            capitalize = true;
        } else if (capitalize) {
            out.append(c.toUpper());
            capitalize = false;
        } else {
            out.append(c);
        }
    }
    return out;
}

QString placeholderOr(const QString &val, const char *placeholder) {
    return val.isEmpty() ? QString::fromLatin1(placeholder) : val;
}

// INV-4 guard. Returns true iff `absPath`, after canonicalisation,
// resolves strictly below `<rootCanonical>/docs/plans`. Symlinks
// pointing outside the project are rejected.
bool pathStrictlyBelowPlans(const QString &absPath,
                            const QString &rootCanonical) {
    if (rootCanonical.isEmpty()) return false;
    // Canonical-path resolution fails if the target doesn't exist yet —
    // for the "save:true and no prior file" case we want to validate
    // the *parent* (which we create with mkpath) instead.
    const QFileInfo fi(absPath);
    const QString parentCanon = QFileInfo(fi.absolutePath()).canonicalFilePath();
    if (parentCanon.isEmpty()) return false;
    const QString plansRoot = rootCanonical + QLatin1String("/docs/plans");
    if (parentCanon != plansRoot &&
        !parentCanon.startsWith(plansRoot + QLatin1Char('/'))) {
        return false;
    }
    // If the file already exists (overwrite-attempt path), also check
    // its own canonical resolution — a symlink pointing outside the
    // project should still be rejected even though INV-5 will refuse
    // the write anyway.
    if (fi.exists()) {
        const QString fileCanon = fi.canonicalFilePath();
        if (fileCanon.isEmpty()) return false;
        if (!fileCanon.startsWith(plansRoot + QLatin1Char('/'))) return false;
    }
    return true;
}

QString renderTaskBlock(int n, const QString &featureName, const QString &antsIdLabel) {
    QString out;
    out += QStringLiteral("### Task %1: [Component Name]\n\n").arg(n);
    out += QStringLiteral("**Files:**\n");
    out += QStringLiteral("- Create: `[exact/path/to/file.cpp]`\n");
    out += QStringLiteral("- Modify: `[exact/path/to/existing.cpp:NNN-NNN]`\n");
    out += QStringLiteral("- Test: `tests/features/%1/test_%1.cpp`\n\n").arg(featureName);
    out += QStringLiteral("- [ ] **Step 1: Write the failing test**\n\n");
    out += QStringLiteral("```cpp\n");
    out += QStringLiteral("TEST(SuiteName, FeatureName) {\n");
    out += QStringLiteral("    // arrange\n");
    out += QStringLiteral("    // act\n");
    out += QStringLiteral("    // assert\n");
    out += QStringLiteral("}\n");
    out += QStringLiteral("```\n\n");
    out += QStringLiteral("- [ ] **Step 2: Run test to verify it fails**\n\n");
    out += QStringLiteral("Run: `ctest --test-dir build -R %1 --output-on-failure`\n").arg(featureName);
    out += QStringLiteral("Expected: FAIL with \"[expected failure message]\"\n\n");
    out += QStringLiteral("- [ ] **Step 3: Write minimal implementation**\n\n");
    out += QStringLiteral("```cpp\n");
    out += QStringLiteral("// minimal code that makes the test pass\n");
    out += QStringLiteral("```\n\n");
    out += QStringLiteral("- [ ] **Step 4: Run test to verify it passes**\n\n");
    out += QStringLiteral("Run: `ctest --test-dir build -R %1 --output-on-failure`\n").arg(featureName);
    out += QStringLiteral("Expected: PASS\n\n");
    out += QStringLiteral("- [ ] **Step 5: Commit**\n\n");
    out += QStringLiteral("```bash\n");
    out += QStringLiteral("git add tests/features/%1/ src/[path]/\n").arg(featureName);
    out += QStringLiteral("git commit -m \"%1: [present-tense description]\"\n").arg(antsIdLabel);
    out += QStringLiteral("```\n\n");
    out += QStringLiteral("---\n\n");
    return out;
}

QString renderSkeleton(const PlanOptions &opts,
                       const QString &antsIdLabel,
                       int taskCount,
                       const QString &planPath) {
    const QString title = titleCase(opts.featureName);
    const QString goal = placeholderOr(opts.goal, "[One sentence describing what this builds]");
    const QString arch = placeholderOr(opts.architecture, "[2-3 sentences about approach]");
    const QString stack = placeholderOr(opts.techStack, "[Key technologies/libraries]");

    QString out;
    out.reserve(8 * 1024);

    out += QStringLiteral("# %1 Implementation Plan\n\n").arg(title);
    out += QStringLiteral("> **For agentic workers:** REQUIRED SUB-SKILL: Use\n");
    out += QStringLiteral("> superpowers:subagent-driven-development (recommended) or\n");
    out += QStringLiteral("> superpowers:executing-plans to implement this plan task-by-task.\n");
    out += QStringLiteral("> Steps use checkbox (`- [ ]`) syntax for tracking.\n\n");
    out += QStringLiteral("**Roadmap item:** %1\n").arg(antsIdLabel);
    out += QStringLiteral("**Spec:** `docs/specs/%1.md` (write this first if not yet drafted)\n\n").arg(antsIdLabel);
    out += QStringLiteral("**Goal:** %1\n\n").arg(goal);
    out += QStringLiteral("**Architecture:** %1\n\n").arg(arch);
    out += QStringLiteral("**Tech Stack:** %1\n\n").arg(stack);
    out += QStringLiteral("**Commit format:** every commit on this plan leads with\n");
    out += QStringLiteral("`%1: <present-tense description>` per\n").arg(antsIdLabel);
    out += QStringLiteral("`docs/standards/commits.md` § 1.1.\n\n");
    out += QStringLiteral("---\n\n");

    for (int i = 1; i <= taskCount; ++i) {
        out += renderTaskBlock(i, opts.featureName, antsIdLabel);
    }

    out += QStringLiteral("## Self-Review Checklist\n\n");
    out += QStringLiteral("After all tasks above are drafted, walk the spec once with fresh eyes:\n\n");
    out += QStringLiteral("- **Spec coverage:** every requirement in `docs/specs/%1.md`\n").arg(antsIdLabel);
    out += QStringLiteral("  maps to at least one task above.\n");
    out += QStringLiteral("- **No placeholders:** no `TBD`, no `add error handling`, no \"write\n");
    out += QStringLiteral("  tests\" without code. Every step is concrete.\n");
    out += QStringLiteral("- **Type consistency:** signatures/names in later tasks match what\n");
    out += QStringLiteral("  earlier tasks defined.\n");
    out += QStringLiteral("- **Bundle assignment:** the test file is wired into the right\n");
    out += QStringLiteral("  consolidated bundle in `CMakeLists.txt` (`test_chrome` / `test_audit`\n");
    out += QStringLiteral("  / `test_claude` / `test_vt` / `test_dialogs` / `test_lua`). See\n");
    out += QStringLiteral("  `tests/features/README.md` § Bundles.\n");
    out += QStringLiteral("- **Verify-step plan per task:** each task's last step is a runnable\n");
    out += QStringLiteral("  `ctest` invocation, not \"verify it works.\"\n\n");
    out += QStringLiteral("Fix issues inline; no second pass needed.\n\n");
    out += QStringLiteral("---\n\n");

    out += QStringLiteral("## Execution Handoff\n\n");
    out += QStringLiteral("Plan complete and saved to `%1`. Two execution options:\n\n").arg(planPath);
    out += QStringLiteral("**1. Subagent-Driven (recommended)** — dispatch a fresh subagent per\n");
    out += QStringLiteral("task, review between tasks, fast iteration. Use\n");
    out += QStringLiteral("`superpowers:subagent-driven-development`.\n\n");
    out += QStringLiteral("**2. Inline Execution** — execute tasks in this session using\n");
    out += QStringLiteral("`superpowers:executing-plans`, batch execution with checkpoints for\n");
    out += QStringLiteral("review.\n\n");
    out += QStringLiteral("Which approach?\n");

    return out;
}

QString suggestedPlanPath(const QString &antsIdLabel,
                          const QString &featureName,
                          AntsIdSource src) {
    if (src == AntsIdSource::None) {
        return QStringLiteral("docs/plans/%1.md").arg(featureName);
    }
    return QStringLiteral("docs/plans/%1-%2.md").arg(antsIdLabel, featureName);
}

}  // anonymous

QString antsIdSourceKey(AntsIdSource s) {
    switch (s) {
        case AntsIdSource::Provided:  return QStringLiteral("provided");
        case AntsIdSource::Allocated: return QStringLiteral("allocated");
        case AntsIdSource::None:      return QStringLiteral("none");
    }
    return QStringLiteral("none");
}

PlanResult buildPlan(const QString &projectRoot, const PlanOptions &opts) {
    PlanResult r;

    if (!isValidFeatureName(opts.featureName)) {
        r.errorCode = QStringLiteral("bad_feature_name");
        r.errorMessage = QStringLiteral(
            "feature_name must match ^[a-z0-9_-]+$, max 64 chars");
        return r;
    }

    // ANTS-1838 — validate ants_id before it reaches the filename / body.
    if (!isValidAntsId(opts.antsId)) {
        r.errorCode = QStringLiteral("bad_args");
        r.errorMessage = QStringLiteral(
            "ants_id must match ^[A-Z][A-Z0-9]{0,15}-[0-9]{1,9}$ "
            "(e.g. ANTS-1234) or be empty to allocate one");
        return r;
    }

    const int taskCount = clampTaskCount(opts.taskCountHint);
    r.taskCount = taskCount;

    // ANTS-ID resolution.
    QString antsIdLabel;
    if (!opts.antsId.isEmpty()) {
        antsIdLabel = opts.antsId;
        r.antsId = opts.antsId;
        r.antsIdSource = AntsIdSource::Provided;
    } else {
        // Only allocate when the project has already opted into ID
        // tracking by checking in a `.roadmap-counter`. ANTS-1618
        // changed allocateIds to auto-init absent/empty counters from
        // 0 — that's the right shape for the test_audit fold-in
        // workflow but here it would surprise plan-template callers
        // by silently creating the counter file in a project that
        // doesn't use it. Probe first; fall through to AntsIdSource::
        // None when the file is absent.
        const QString counter = RoadmapFoldIn::counterFilePath(projectRoot);
        const bool counterPresent = !counter.isEmpty()
            && QFileInfo::exists(counter);
        QList<int> ids;
        if (counterPresent) {
            ids = RoadmapFoldIn::allocateIds(projectRoot, 1);
        }
        if (ids.isEmpty()) {
            r.antsId.clear();
            r.antsIdSource = AntsIdSource::None;
            antsIdLabel = QStringLiteral("<ANTS-ID>");
        } else {
            // ANTS-3473 — the project's own sniffed prefix, not a hardcoded
            // "ANTS", so a plan for a FIBR-NNNN project reads FIBR-<n>.
            r.antsId = QStringLiteral("%1-%2")
                           .arg(RoadmapFoldIn::sniffIdPrefix(projectRoot))
                           .arg(ids.at(0));
            r.antsIdSource = AntsIdSource::Allocated;
            antsIdLabel = r.antsId;
        }
    }

    const QString planPath = suggestedPlanPath(antsIdLabel, opts.featureName,
                                               r.antsIdSource);
    r.planPath = planPath;
    r.planMarkdown = renderSkeleton(opts, antsIdLabel, taskCount, planPath);

    // INV-9 dry-run path — no disk write unless opts.save.
    if (!opts.save) {
        r.ok = true;
        return r;
    }

    // INV-5: refuse to overwrite. Compute absolute path first.
    const QString rootCanonical = QFileInfo(projectRoot).canonicalFilePath();
    if (rootCanonical.isEmpty()) {
        r.errorCode = QStringLiteral("no_project");
        r.errorMessage = QStringLiteral(
            "plan_template: project root not resolvable");
        return r;
    }
    const QString absPath = rootCanonical + QLatin1Char('/') + planPath;

    // Make sure docs/plans/ exists.
    QDir plansDir(rootCanonical + QLatin1String("/docs/plans"));
    if (!plansDir.exists()) {
        if (!plansDir.mkpath(QStringLiteral("."))) {
            r.errorCode = QStringLiteral("mkdir_failed");
            r.errorMessage = QStringLiteral(
                "plan_template: could not create docs/plans/");
            return r;
        }
    }

    // INV-4 path-traversal guard (parent + existing-file canon checks).
    if (!pathStrictlyBelowPlans(absPath, rootCanonical)) {
        r.errorCode = QStringLiteral("bad_path");
        r.errorMessage = QStringLiteral(
            "plan_template: plan_path escapes docs/plans/");
        return r;
    }

    // INV-5: existence check.
    if (QFileInfo::exists(absPath)) {
        r.errorCode = QStringLiteral("plan_exists");
        r.errorMessage = QStringLiteral(
            "refusing to overwrite existing plan; pass save:false to retrieve "
            "the skeleton only");
        return r;
    }

    // Atomic write via QSaveFile (same idiom as RoadmapFoldIn).
    QSaveFile out(absPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        r.errorCode = QStringLiteral("write_failed");
        r.errorMessage = QStringLiteral(
            "plan_template: could not open plan file for writing");
        return r;
    }
    const QByteArray bytes = r.planMarkdown.toUtf8();
    if (out.write(bytes) != bytes.size() || !out.commit()) {
        r.errorCode = QStringLiteral("write_failed");
        r.errorMessage = QStringLiteral(
            "plan_template: write or commit failed");
        return r;
    }

    r.saved = true;
    r.ok = true;
    return r;
}

}  // namespace PlanTemplateEngine
