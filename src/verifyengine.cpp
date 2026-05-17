// VerifyEngine implementation — see verifyengine.h + docs/specs/ANTS-1289.md.

#include "verifyengine.h"
#include "verifytrust.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>

namespace VerifyEngine {

namespace {

constexpr int kHardByteCap   = 16 * 1024;   // INV-3 floor
constexpr int kMaxLogLinesHi = 500;
constexpr int kMaxLogLinesLo = 10;
constexpr int kTimeoutHi     = 1800;
// Lower floors live on `VerifyOptions` (minPerGateSec /
// minTotalTimeoutSec, both default 10) so timeout-enforcement tests
// can drop them to 1 without redefining production behaviour.

QString gateName(GateName g) {
    switch (g) {
        case GateName::Build: return QStringLiteral("build");
        case GateName::Tests: return QStringLiteral("tests");
        case GateName::Lint:  return QStringLiteral("lint");
    }
    return {};
}

bool readFile(const QString &absPath, QByteArray *out) {
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    *out = f.readAll();
    return true;
}

// INV-4 guard. Returns true iff `absPath`, after canonicalisation,
// resolves strictly below `rootCanonical`. Mirrors
// IndieReviewEngine::assembleBriefManifest (strict-below; equality
// rejected because the config file is always one level under the
// root by construction).
bool pathStrictlyBelow(const QString &absPath, const QString &rootCanonical) {
    if (rootCanonical.isEmpty()) return false;
    const QString canon = QFileInfo(absPath).canonicalFilePath();
    if (canon.isEmpty()) return false;
    return canon.startsWith(rootCanonical + QChar('/'));
}

// Parse `.ants/verify.json` per § 2.4 schema. Returns empty list on
// any parse error so the caller can fall through to auto-detection.
QList<GateConfig> parseVerifyJson(const QByteArray &raw, bool *parseError) {
    *parseError = false;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        *parseError = true;
        return {};
    }
    const QJsonObject root = doc.object();

    QList<GateConfig> gates;
    const auto readGate = [&](const QString &key, GateName g) {
        if (!root.contains(key)) return;
        const QJsonObject o = root.value(key).toObject();
        const QString cmd  = o.value(QStringLiteral("command")).toString();
        QString fmt        = o.value(QStringLiteral("format")).toString();
        if (cmd.isEmpty()) return;
        if (fmt.isEmpty()) fmt = QStringLiteral("plain");
        GateConfig gc;
        gc.name    = g;
        gc.command = cmd;
        gc.format  = fmt;
        gates.append(gc);
    };
    readGate(QStringLiteral("build"), GateName::Build);
    readGate(QStringLiteral("tests"), GateName::Tests);
    readGate(QStringLiteral("lint"),  GateName::Lint);
    return gates;
}

// `pyproject.toml` test-section probe — § 2.5 row 6. Cheap substring
// check: a `[tool.pytest...]` table presence implies the project
// authored pytest config, so `pytest -q` is the right invocation.
// Avoids pulling in a TOML parser for one bit of intel.
bool pyprojectHasPytest(const QString &absPath) {
    QByteArray raw;
    if (!readFile(absPath, &raw)) return false;
    return raw.contains("[tool.pytest");
}

bool packageJsonHasScript(const QString &absPath, const char *scriptKey) {
    QByteArray raw;
    if (!readFile(absPath, &raw)) return false;
    QJsonParseError err{};
    const QJsonDocument d = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !d.isObject()) return false;
    const QJsonObject scripts =
        d.object().value(QStringLiteral("scripts")).toObject();
    return scripts.contains(QString::fromUtf8(scriptKey));
}

// § 2.5 auto-detection. First-match-wins, top-to-bottom.
QList<GateConfig> autoDetect(const QString &projectPath) {
    QList<GateConfig> gates;
    const QString cml = projectPath + QStringLiteral("/CMakeLists.txt");
    const QString buildDir = projectPath + QStringLiteral("/build");
    const QString presets = projectPath + QStringLiteral("/CMakePresets.json");
    const QString pkg = projectPath + QStringLiteral("/package.json");
    const QString cargo = projectPath + QStringLiteral("/Cargo.toml");
    const QString py = projectPath + QStringLiteral("/pyproject.toml");

    if (QFileInfo::exists(cml) && QFileInfo(buildDir).isDir()) {
        GateConfig b; b.name = GateName::Build;
        // ANTS-1388: `--quiet` is not accepted by `cmake --build`. Drop
        // it; Ninja (the recommended generator) is already quiet by
        // default. Token-frugality comes from the `tail -20` at the
        // caller, not from a per-tool quiet flag.
        b.command = QStringLiteral("cmake --build build");
        b.format  = QStringLiteral("plain");
        gates.append(b);
        GateConfig t; t.name = GateName::Tests;
        t.command = QStringLiteral("ctest --test-dir build --output-on-failure");
        t.format  = QStringLiteral("ctest");
        gates.append(t);
        return gates;
    }
    if (QFileInfo::exists(presets)) {
        GateConfig b; b.name = GateName::Build;
        b.command = QStringLiteral(
            "sh -c 'cmake --preset=default && cmake --build --preset=default'");
        b.format  = QStringLiteral("plain");
        gates.append(b);
        GateConfig t; t.name = GateName::Tests;
        t.command = QStringLiteral("ctest --preset=default --output-on-failure");
        t.format  = QStringLiteral("ctest");
        gates.append(t);
        return gates;
    }
    bool added = false;
    if (QFileInfo::exists(pkg)) {
        if (packageJsonHasScript(pkg, "build")) {
            GateConfig b; b.name = GateName::Build;
            b.command = QStringLiteral("npm run build");
            b.format  = QStringLiteral("plain");
            gates.append(b);
            added = true;
        }
        if (packageJsonHasScript(pkg, "test")) {
            GateConfig t; t.name = GateName::Tests;
            t.command = QStringLiteral("npm test");
            t.format  = QStringLiteral("plain");
            gates.append(t);
            added = true;
        }
        if (added) return gates;
    }
    if (QFileInfo::exists(cargo)) {
        GateConfig b; b.name = GateName::Build;
        b.command = QStringLiteral("cargo build --quiet");
        b.format  = QStringLiteral("plain");
        gates.append(b);
        GateConfig t; t.name = GateName::Tests;
        t.command = QStringLiteral("cargo test");
        t.format  = QStringLiteral("plain");
        gates.append(t);
        return gates;
    }
    if (QFileInfo::exists(py) && pyprojectHasPytest(py)) {
        GateConfig t; t.name = GateName::Tests;
        t.command = QStringLiteral("pytest -q");
        t.format  = QStringLiteral("plain");
        gates.append(t);
        return gates;
    }
    return gates;
}

// Trim `lines` down to fit within `maxLines` and `kHardByteCap` bytes
// (the byte cap acts as a floor per INV-3 — when both bind, the byte
// cap wins and we drop more lines than `maxLines` would alone).
// Single lines longer than the byte cap are mid-truncated with a
// `… [N bytes elided] …` marker.
QString assembleLogTail(const QStringList &lines, int maxLines,
                        bool *truncatedOut) {
    *truncatedOut = false;
    const int total = lines.size();
    int start = qMax(0, total - maxLines);
    if (start > 0) *truncatedOut = true;

    QStringList tail;
    tail.reserve(total - start);
    qsizetype byteCount = 0;
    for (int i = start; i < total; ++i) {
        QString l = lines.at(i);
        const qsizetype lineBytes = l.toUtf8().size();
        if (lineBytes > kHardByteCap) {
            const int keep = kHardByteCap / 2 - 64;
            const qsizetype elided = lineBytes - 2 * keep;
            l = l.left(keep)
                + QStringLiteral(" … [%1 bytes elided] … ").arg(elided)
                + l.right(keep);
        }
        tail.append(l);
        byteCount += l.toUtf8().size() + 1;  // +1 for the join '\n'
    }

    // Byte-cap pass: drop from the head until we're under kHardByteCap.
    while (byteCount > kHardByteCap && !tail.isEmpty()) {
        *truncatedOut = true;
        const qsizetype removed = tail.first().toUtf8().size() + 1;
        tail.removeFirst();
        byteCount -= removed;
    }
    return tail.join(QChar('\n'));
}

// INV-5 — apply the ctest summary regex per-line.
void parseCtest(const QStringList &lines, GateResult *r) {
    static const QRegularExpression kSummary(
        QStringLiteral(R"((\d+)% tests passed, (\d+) tests failed out of (\d+))"));
    static const QRegularExpression kFailLine(
        QStringLiteral(R"(^\s*\d+ - (\S+) )"));
    bool inFailBlock = false;
    for (const QString &line : lines) {
        const auto sm = kSummary.match(line);
        if (sm.hasMatch()) {
            const int failed = sm.captured(2).toInt();
            const int total  = sm.captured(3).toInt();
            r->totalCount  = total;
            r->passedCount = total - failed;
        }
        if (line.contains(QLatin1String("The following tests FAILED:"))) {
            inFailBlock = true;
            continue;
        }
        if (inFailBlock) {
            const auto fm = kFailLine.match(line);
            if (fm.hasMatch()) {
                r->failingTests.append(fm.captured(1));
            } else if (line.trimmed().isEmpty()) {
                inFailBlock = false;
            }
        }
    }
}

// Run one gate. Streams stdout+stderr (merged), keeps a rolling
// last-N-line window per INV-3, enforces a per-gate wall-clock timeout
// per INV-2, parses ctest output per INV-5.
GateResult runOneGate(const QString &projectPath,
                      const GateConfig &gc,
                      int maxLogLines,
                      int perGateTimeoutSec) {
    GateResult r;
    r.name = gc.name;

    QProcess p;
    p.setWorkingDirectory(projectPath);
    p.setProcessChannelMode(QProcess::MergedChannels);

    QStringList lines;
    QString carry;                 // partial last line (no trailing '\n' yet)
    int totalLines = 0;
    constexpr qsizetype kSingleLineCap = kHardByteCap * 2;

    QObject::connect(&p, &QProcess::readyReadStandardOutput,
                     [&]() {
        QByteArray chunk = p.readAllStandardOutput();
        carry += QString::fromUtf8(chunk);
        // Hard-cap an unterminated single line so an attacker can't
        // make us buffer multi-MiB of garbage. Force-split.
        if (carry.toUtf8().size() > kSingleLineCap) {
            lines.append(carry);
            ++totalLines;
            if (lines.size() > maxLogLines) lines.removeFirst();
            carry.clear();
        }
        int idx;
        while ((idx = carry.indexOf(QChar('\n'))) >= 0) {
            lines.append(carry.left(idx));
            ++totalLines;
            if (lines.size() > maxLogLines) lines.removeFirst();
            carry.remove(0, idx + 1);
        }
    });

    QElapsedTimer wallClock;
    wallClock.start();

    p.start(QStringLiteral("/bin/sh"),
            {QStringLiteral("-c"), gc.command});
    if (!p.waitForStarted(2000)) {
        r.ran = false;
        r.skippedReason = QStringLiteral("command not resolvable");
        return r;
    }

    // Floor enforcement happens at the caller (`runVerify` clamps via
    // `opts.minPerGateSec`); runOneGate trusts the value passed in so
    // tests can drive a sub-floor budget without a second clamp here.
    const int perGateMs = perGateTimeoutSec * 1000;
    if (!p.waitForFinished(perGateMs)) {
        p.kill();
        p.waitForFinished(2000);
        r.ran = true;
        r.passed = false;
        r.exitCode = -1;
        r.skippedReason =
            QStringLiteral("timeout after %1s").arg(perGateTimeoutSec);
    } else {
        r.ran = true;
        const int code = p.exitCode();
        r.exitCode = code;
        if (p.exitStatus() != QProcess::NormalExit) {
            r.passed = false;
        } else {
            r.passed = (code == 0);
        }
    }
    r.durationSec = wallClock.elapsed() / 1000.0;

    // Flush any remaining carry as a final line.
    if (!carry.isEmpty()) {
        lines.append(carry);
        ++totalLines;
        if (lines.size() > maxLogLines) lines.removeFirst();
    }
    r.logTotalLines = totalLines;
    bool truncated = false;
    r.logTail = assembleLogTail(lines, maxLogLines, &truncated);
    r.logTruncated = (totalLines > lines.size()) || truncated;

    if (gc.format == QStringLiteral("ctest")) {
        parseCtest(lines, &r);
        // INV-5: if the parser found a summary, override `passed` to
        // reflect the parsed test result rather than ctest's exit code
        // alone — keeps the gate honest when ctest exits 0 with no
        // tests run.
        if (r.totalCount >= 0 && r.passedCount >= 0) {
            r.passed = r.passed && (r.passedCount == r.totalCount);
        }
    }
    return r;
}

}  // anonymous

QString gateKey(GateName g) {
    return gateName(g);
}

QList<GateConfig> loadGateConfig(const QString &projectPath,
                                 QString *configSourceOut,
                                 VerifyTrust::Client *trustClient,
                                 bool *verifyUntrustedOut) {
    auto setSource = [&](const QString &v) {
        if (configSourceOut) *configSourceOut = v;
    };
    auto setUntrusted = [&](bool v) {
        if (verifyUntrustedOut) *verifyUntrustedOut = v;
    };

    const QFileInfo rootInfo(projectPath);
    const QString rootCanon = rootInfo.canonicalFilePath();
    const QString cfgPath =
        projectPath + QStringLiteral("/.ants/verify.json");
    QFileInfo cfgInfo(cfgPath);

    if (cfgInfo.exists()) {
        // INV-4: reject configs that resolve outside the project root.
        if (!pathStrictlyBelow(cfgPath, rootCanon)) {
            // Fall through to auto-detect (treat as "no config").
        } else {
            QByteArray raw;
            if (readFile(cfgPath, &raw)) {
                bool parseErr = false;
                QList<GateConfig> gates = parseVerifyJson(raw, &parseErr);
                if (parseErr) {
                    setSource(QStringLiteral("bad_config"));
                    return {};
                }
                if (!gates.isEmpty()) {
                    // ANTS-1337 — trust gate. When a client is
                    // wired, the bespoke config is honoured only if
                    // the SHA is trusted. Untrusted falls back to
                    // auto-detect AND surfaces verifyUntrusted=true
                    // in the report so the caller knows why the
                    // bespoke commands were skipped.
                    if (trustClient) {
                        const auto decision =
                            trustClient->outcomeForConfig(rootCanon, raw);
                        if (decision.outcome ==
                            VerifyTrust::Outcome::Trusted) {
                            setSource(QStringLiteral(".ants/verify.json"));
                            return gates;
                        }
                        // Untrusted / Headless → fall through to
                        // auto-detect with the untrusted flag set.
                        setUntrusted(true);
                        QList<GateConfig> auto_ = autoDetect(projectPath);
                        if (auto_.isEmpty()) {
                            setSource(QStringLiteral("auto (untrusted-bespoke)"));
                            return {};
                        }
                        setSource(QStringLiteral("auto (untrusted-bespoke)"));
                        return auto_;
                    }
                    // Back-compat: no client → honour unconditionally
                    // (Phase 1; Phase 2 wires the client through
                    // cmdVerifyChanges).
                    setSource(QStringLiteral(".ants/verify.json"));
                    return gates;
                }
                // Valid JSON but no usable gates — treat as "none"
                // rather than fall through (INV-1: explicit config
                // wins, including the choice to configure nothing).
                setSource(QStringLiteral(".ants/verify.json"));
                return {};
            }
        }
    }

    QList<GateConfig> auto_ = autoDetect(projectPath);
    if (auto_.isEmpty()) {
        setSource(QStringLiteral("none"));
        return {};
    }
    setSource(QStringLiteral("auto-detected"));
    return auto_;
}

VerifyReport runVerify(const QString &projectPath,
                       const VerifyOptions &opts) {
    VerifyReport rep;
    rep.gates.clear();

    QString cfgSource;
    bool verifyUntrusted = false;
    QList<GateConfig> configured = loadGateConfig(
        projectPath, &cfgSource, opts.trustClient, &verifyUntrusted);
    rep.configSource = cfgSource;
    rep.verifyUntrusted = verifyUntrusted;

    // Clamp opts to documented ranges.
    int maxLines = qBound(kMaxLogLinesLo, opts.maxLogLines, kMaxLogLinesHi);
    int timeoutTotal = qBound(opts.minTotalTimeoutSec, opts.timeoutSec, kTimeoutHi);

    // Apply `only` filter — if non-empty, keep only matching gates.
    if (!opts.only.isEmpty()) {
        QList<GateConfig> filtered;
        for (const auto &gc : configured) {
            if (opts.only.contains(gc.name)) filtered.append(gc);
        }
        configured = filtered;
    }

    // ANTS-1492 — per-gate budget is divided by the actual gates that will
    // run, not the configured-set size. Pre-fix bug: a caller passing
    // gates:["build"], timeout_sec:900 with 3 configured gates got
    // perGateSec=300 (900/3) instead of 900, then half the build wall-clock
    // budget silently vanished into "no gate ran" slots.
    int perGateSec =
        qMax(opts.minPerGateSec,
             timeoutTotal / qMax(1, configured.size()));

    // Always emit all three gate entries in the report (per § 4 — the
    // shape stays consistent so callers can branch on per-gate state
    // rather than on object emptiness). Unconfigured gates get
    // ran:false with the documented skipped_reason.
    auto findConfigured = [&](GateName g) -> const GateConfig * {
        for (const auto &gc : configured) {
            if (gc.name == g) return &gc;
        }
        return nullptr;
    };

    const GateName order[3] = {
        GateName::Build, GateName::Tests, GateName::Lint};

    int priorBuildIdx = -1;
    QList<GateResult> resultsByOrder;
    resultsByOrder.reserve(3);

    for (GateName g : order) {
        GateResult r;
        r.name = g;
        const GateConfig *gc = findConfigured(g);
        if (!gc) {
            r.ran = false;
            r.skippedReason = QStringLiteral("no command configured");
            resultsByOrder.append(r);
            continue;
        }
        // INV-6: tests skip iff prior build ran and failed. Indexing
        // into resultsByOrder (not a pointer/iterator) — append may
        // reallocate the underlying buffer.
        if (g == GateName::Tests && priorBuildIdx >= 0) {
            const auto &pb = resultsByOrder.at(priorBuildIdx);
            if (pb.ran && !pb.passed) {
                r.ran = false;
                r.skippedReason = QStringLiteral("prior gate failed");
                resultsByOrder.append(r);
                continue;
            }
        }
        r = runOneGate(projectPath, *gc, maxLines, perGateSec);
        resultsByOrder.append(r);
        if (g == GateName::Build) priorBuildIdx = resultsByOrder.size() - 1;
    }

    rep.gates = resultsByOrder;
    rep.allPassed = true;
    bool anyRan = false;
    for (const auto &r : rep.gates) {
        if (r.ran) {
            anyRan = true;
            if (!r.passed) {
                rep.allPassed = false;
                break;
            }
        }
    }
    // If no gate ran at all (no config, no auto-detect hit), keep
    // allPassed:true per § 4 last paragraph — the tool did its job,
    // there was just nothing to verify.
    if (!anyRan) rep.allPassed = true;
    return rep;
}

}  // namespace VerifyEngine
