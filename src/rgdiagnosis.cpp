#include "rgdiagnosis.h"

namespace RgDiagnosis {

QString explain(const StartFailure &f) {
    // Order matters: each branch rules out the ones below it. A vanished
    // working directory is checked FIRST because it fails the spawn however
    // healthy the binary is, and it is the cause the old message hid most
    // completely.
    if (!f.workingDirExists) {
        return QStringLiteral(
            "the search root no longer exists (%1), so no scanner could be "
            "started there — re-resolve the project root")
            .arg(f.workingDir);
    }
    if (f.exePath.isEmpty()) {
        return QStringLiteral(
            "ripgrep (rg) was not found on PATH (%1) — install it, or fix the "
            "PATH the Ants process inherited")
            .arg(f.pathEnv.isEmpty() ? QStringLiteral("<empty>") : f.pathEnv);
    }
    if (f.stillStarting) {
        // The 2026-08-25 report landed while a full ninja build was running.
        // Whether the start budget is too short on a loaded host is a real
        // question; this says so in the reply instead of guessing at it here.
        return QStringLiteral(
            "ripgrep (%1) is installed but had not reported started after "
            "%2 ms, and was still starting — a loaded host, not a missing "
            "package. Retry rather than falling back to grep")
            .arg(f.exePath, QString::number(f.startTimeoutMs));
    }
    return QStringLiteral(
        "ripgrep (%1) is installed but failed to start: %2 — the executable "
        "exists, so look at the spawn environment (fork limits, load), not at "
        "the package")
        .arg(f.exePath, f.errorString.isEmpty() ? QStringLiteral("unknown error")
                                                : f.errorString);
}

}  // namespace RgDiagnosis
