// Source-grep harness for ANTS-1251 — locks the wiring contract for
// the new `subsystem` consolidated MCP tool (map / files /
// recent_changes). See spec.md.
//
// Exit 0 = all 12 invariants hold.

#include <cstdio>
#include <regex>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>

#include "../../_support/expect.h"
#include "subsystemmap.h"

// ANTS-5074 — concurrency-guard support.
#include <fcntl.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_H_PATH
#error "SRC_CLAUDE_INTEGRATION_H_PATH compile definition required"
#endif
#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_SUBSYSTEMMAP_CPP_PATH
#error "SRC_SUBSYSTEMMAP_CPP_PATH compile definition required"
#endif
#ifndef SRC_SUBSYSTEMMAP_H_PATH
#error "SRC_SUBSYSTEMMAP_H_PATH compile definition required"
#endif
#ifndef ANTS_CLAUDE_MD_PATH
#error "ANTS_CLAUDE_MD_PATH compile definition required"
#endif
#ifndef CMAKELISTS_PATH
#error "CMAKELISTS_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}



}  // namespace

TEST(McpSubsystem, WiringContract) {
    expect_reset();

    const std::string ciCpp  = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string ciHdr  = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_H_PATH);
    const std::string rcHdr  = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp  = ants_test::slurpRemoteControl();
    const std::string mwCpp  = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string smCpp  = ants_test::slurpFile(SRC_SUBSYSTEMMAP_CPP_PATH);
    const std::string smHdr  = ants_test::slurpFile(SRC_SUBSYSTEMMAP_H_PATH);
    const std::string cmake  = ants_test::slurpFile(CMAKELISTS_PATH);

    // INV-1 — cmdSubsystem declared public on RemoteControl.
    expect(contains(rcHdr, "cmdSubsystem(const QJsonObject &req)"),
           "INV-1",
           "cmdSubsystem decl missing from src/remotecontrol.h "
           "(must sit next to cmdGitState in the public block)");

    // INV-2 — body has >= 7 INV anchors across remotecontrol.cpp +
    // subsystemmap.{cpp,h}.
    std::regex anchorRe(R"(//\s*ANTS-1251-INV-\d+)");
    auto countAnchors = [&](const std::string &s) -> long {
        auto begin = std::sregex_iterator(s.begin(), s.end(), anchorRe);
        auto end   = std::sregex_iterator();
        return std::distance(begin, end);
    };
    const long anchorCount = countAnchors(rcCpp) + countAnchors(smCpp) +
                             countAnchors(smHdr);
    char detail2[200];
    std::snprintf(detail2, sizeof detail2,
                  "expected >=7 // ANTS-1251-INV-N anchors across "
                  "remotecontrol.cpp + subsystemmap.{cpp,h}, found %ld",
                  anchorCount);
    expect(anchorCount >= 7, "INV-2", detail2);

    // INV-3 — IPC dispatcher routes "subsystem".
    expect(contains(rcCpp, "\"subsystem\"") &&
           contains(rcCpp, "cmdSubsystem"),
           "INV-3",
           "remotecontrol.cpp dispatch missing \"subsystem\" → "
           "cmdSubsystem routing");

    // INV-4 — tools/list registers a single "subsystem" entry with
    // op enum {map, files, recent_changes} and op in required[].
    expect(contains(ciCpp, "\"subsystem\""),
           "INV-4a",
           "tools/list missing \"subsystem\" name registration");
    {
        const size_t pos = ciCpp.find("ssTool[\"name\"] = \"subsystem\"");
        bool ok = false;
        if (pos != std::string::npos) {
            const size_t windowEnd = std::min(ciCpp.size(), pos + 4000);
            const std::string window = ciCpp.substr(pos, windowEnd - pos);
            ok = contains(window, "\"map\"") &&
                 contains(window, "\"files\"") &&
                 contains(window, "\"recent_changes\"") &&
                 contains(window, "\"required\"") &&
                 contains(window, "\"op\"");
        }
        expect(ok, "INV-4b",
               "subsystem inputSchema does not declare op enum "
               "{map, files, recent_changes} + op in required[]");
    }

    // INV-5 — tools/list schema declares the subsystem tool.
    // ANTS-1253 collapsed the per-tool dispatch into a registry lookup.
    std::regex schemaRe(R"("name"\]\s*=\s*"subsystem")");
    expect(std::regex_search(ciCpp, schemaRe),
           "INV-5",
           "claudeintegration.cpp missing tools/list schema entry for subsystem");

    // INV-6 — header has the single registry surface (ANTS-1253).
    expect(contains(ciHdr, "registerToolProvider(const QString &name"),
           "INV-6a",
           "claudeintegration.h missing registerToolProvider declaration (ANTS-1253)");
    expect(contains(ciHdr, "m_toolProviders"),
           "INV-6b",
           "claudeintegration.h missing m_toolProviders registry member (ANTS-1253)");

    // INV-7 — mainwindow.cpp registers subsystem via the registry.
    expect(contains(mwCpp, "registerToolProvider(\"subsystem\""),
           "INV-7a",
           "mainwindow.cpp does not register subsystem in "
           "setupClaudeMcpProviders (ANTS-1253)");
    expect(contains(mwCpp, "cmdSubsystem"),
           "INV-7b",
           "mainwindow.cpp does not delegate the provider lambda to "
           "m_remoteControl->cmdSubsystem");

    // INV-8 — cmdSubsystem dispatches on op ∈ {map, files,
    // recent_changes} and surfaces both error codes.
    expect(contains(rcCpp, "\"map\"") &&
           contains(rcCpp, "\"files\"") &&
           contains(rcCpp, "\"recent_changes\""),
           "INV-8a",
           "remotecontrol.cpp does not dispatch on op string literals "
           "{map, files, recent_changes}");
    expect(contains(rcCpp, "\"bad_op\"") &&
           contains(rcCpp, "\"unknown_lane\""),
           "INV-8b",
           "remotecontrol.cpp does not surface {bad_op, unknown_lane} "
           "error codes (ANTS-1251 § 5 trailing paragraph)");

    // INV-9 — cmdSubsystem composes cmdGitState for recent_changes.
    {
        const size_t pos = rcCpp.find("RemoteControl::cmdSubsystem");
        bool ok = false;
        if (pos != std::string::npos) {
            const std::string window = rcCpp.substr(pos);
            ok = contains(window, "cmdGitState");
        }
        expect(ok, "INV-9",
               "cmdSubsystem body does not compose cmdGitState for "
               "the recent_changes op (spec § 3 / INV-5)");
    }

    // INV-10 — subsystemmap.h exposes Lane / parse / cachedLanes.
    expect(contains(smHdr, "namespace SubsystemMap") &&
           contains(smHdr, "struct Lane") &&
           contains(smHdr, "parse(") &&
           contains(smHdr, "cachedLanes("),
           "INV-10",
           "subsystemmap.h surface missing Lane / parse / cachedLanes");

    // INV-11 — CMake wires src/subsystemmap.cpp into ants_core_lib.
    expect(contains(cmake, "src/subsystemmap.cpp"),
           "INV-11",
           "CMakeLists.txt does not list src/subsystemmap.cpp in "
           "ants_core_lib SOURCES");

    // INV-12 — the resolved module-map source parses to >= 15 unique
    // lanes including "vtparser" (spec § 10 step 2 floor). Post-ANTS-1292
    // the source is docs/subsystems.md; resolveSource() finds it.
    SubsystemMap::clearCacheForTests();
    const QString claudeMdPath = QString::fromUtf8(ANTS_CLAUDE_MD_PATH);
    const QString resolvedSrc  = SubsystemMap::resolveSource(claudeMdPath);
    const QVector<SubsystemMap::Lane> lanes =
        SubsystemMap::cachedLanes(resolvedSrc);
    bool haveVtparser = false;
    for (const auto &l : lanes) {
        if (l.name == QStringLiteral("vtparser")) { haveVtparser = true; break; }
    }
    char detail12[200];
    std::snprintf(detail12, sizeof detail12,
                  "parsed %lld lanes from resolved source, expected >=15 "
                  "including \"vtparser\" (haveVtparser=%s)",
                  static_cast<long long>(lanes.size()),
                  haveVtparser ? "true" : "false");
    expect(lanes.size() >= 15 && haveVtparser, "INV-12", detail12);

    // ANTS-1292 INV-13 — resolveSource() prefers docs/subsystems.md when
    // present (the canonical home), not CLAUDE.md.
    expect(resolvedSrc.endsWith(QStringLiteral("docs/subsystems.md")),
           "ANTS-1292/INV-13",
           "resolveSource(CLAUDE.md) did not resolve to docs/subsystems.md");

    // ANTS-1292 INV-14 — the catalogue moved OUT of CLAUDE.md: parsing the
    // (stub) CLAUDE.md directly yields no lanes, so it no longer bloats the
    // session preamble.
    SubsystemMap::clearCacheForTests();
    const QVector<SubsystemMap::Lane> claudeMdLanes =
        SubsystemMap::cachedLanes(claudeMdPath);
    char detail14[160];
    std::snprintf(detail14, sizeof detail14,
                  "CLAUDE.md still parses to %lld lanes; the module map "
                  "should be a stub pointing at docs/subsystems.md",
                  static_cast<long long>(claudeMdLanes.size()));
    expect(claudeMdLanes.isEmpty(), "ANTS-1292/INV-14", detail14);

    // ANTS-1292 INV-2 — empty input → empty output (no crash, no
    // fabricated path).
    expect(SubsystemMap::resolveSource(QString()).isEmpty(),
           "ANTS-1292/INV-2",
           "resolveSource(\"\") must return empty, not a fabricated path");

    // ANTS-1292 INV-3 — un-migrated project (no sibling docs/subsystems.md)
    // falls back to the given CLAUDE.md path unchanged. Use a path whose
    // directory has no docs/subsystems.md sibling.
    const QString unmigrated =
        QStringLiteral("/nonexistent-ants-1292-probe/CLAUDE.md");
    expect(SubsystemMap::resolveSource(unmigrated) == unmigrated,
           "ANTS-1292/INV-3",
           "resolveSource must return the CLAUDE.md path unchanged when no "
           "docs/subsystems.md sibling exists (back-compat fallback)");

    // ANTS-3414 — op:map honours an optional `name` substring filter.
    // The schema declares a `name` property (so it is no longer flagged in
    // ignored_args), and the op:map branch filters lanesJson by
    // name.contains(nameFilter, Qt::CaseInsensitive) and echoes `name`.
    expect(contains(ciCpp, "props[\"name\"] = nameProp"),
           "ANTS-3414/schema",
           "subsystem inputSchema does not declare the op:map `name` prop");
    {
        const size_t pos = rcCpp.find("RemoteControl::cmdSubsystem");
        bool ok = false;
        if (pos != std::string::npos) {
            const std::string window = rcCpp.substr(pos);
            ok = contains(window, "req.value(\"name\")") &&
                 contains(window, "Qt::CaseInsensitive");
        }
        expect(ok, "ANTS-3414/handler",
               "cmdSubsystem op:map does not apply the case-insensitive "
               "`name` substring filter");
    }

    EXPECT_EQ(0, expect_failures()) << expect_failures() << " ANTS-1251/ANTS-1292 invariant(s) failed";
}

// ANTS-1796 — a backticked library-name qualifier inside a parenthetical
// (e.g. ``- `auditautofix` (Qt6::Core, `ants_audit_lib`) — ...``) must NOT
// be harvested as a separate subsystem lane. The docs/subsystems.md format
// contract says qualifier-paren forms are *tolerated* (ignored), not parsed
// for names. Pre-fix, parse() harvested every backticked token in the
// prefix, so `subsystem op=map` emitted bogus lanes named after CMake
// library targets (ants_audit_lib / ants_core_lib / ants_dialogs_lib).
TEST(McpSubsystem, ParenQualifierLibNameNotHarvested) {
    expect_reset();

    const QString body = QStringLiteral(
        "## Module map (src/)\n"
        "\n"
        "- `auditautofix` (Qt6::Core, `ants_audit_lib`) — native safe-list "
        "auto-fixer.\n"
        "- `llmclient` (Qt6::Core+Network, `ants_core_lib`) — streaming chat "
        "client.\n"
        "- `luaengine` / `pluginmanager` — sandboxed Lua 5.4.\n");

    const QVector<SubsystemMap::Lane> lanes = SubsystemMap::parse(body);

    bool sawLibQualifier = false;
    bool sawAutofix = false, sawLlm = false, sawLua = false, sawPlugin = false;
    for (const auto &l : lanes) {
        if (l.name == QStringLiteral("ants_audit_lib") ||
            l.name == QStringLiteral("ants_core_lib") ||
            l.name == QStringLiteral("ants_dialogs_lib")) {
            sawLibQualifier = true;
        }
        if (l.name == QStringLiteral("auditautofix"))   sawAutofix = true;
        if (l.name == QStringLiteral("llmclient"))      sawLlm = true;
        if (l.name == QStringLiteral("luaengine"))      sawLua = true;
        if (l.name == QStringLiteral("pluginmanager"))  sawPlugin = true;
    }

    expect(!sawLibQualifier, "ANTS-1796/no-lib-qualifier",
           "parse() harvested a backticked library-name qualifier inside "
           "parens as a subsystem lane (must be ignored)");
    // The legitimate names — including both halves of a ` / `-joined
    // multi-name bullet — must survive.
    expect(sawAutofix && sawLlm && sawLua && sawPlugin,
           "ANTS-1796/legit-names-kept",
           "parse() dropped a legitimate subsystem name while stripping "
           "paren qualifiers");

    EXPECT_EQ(0, expect_failures()) << expect_failures()
        << " ANTS-1796 invariant(s) failed";
}

// ANTS-3481 — sourceHasModuleMap distinguishes "heading present" from
// "heading absent", so indie_review_orchestrate can report an empty partition
// with an honest cause (module_map_unparseable vs no_lanes).
TEST(McpSubsystem, SourceHasModuleMapDetectsHeading) {
    expect_reset();
    SubsystemMap::clearCacheForTests();

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto put = [&](const QString &rel, const QByteArray &body) -> QString {
        const QString p = dir.path() + QLatin1Char('/') + rel;
        QFile f(p);
        EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(body);
        f.close();
        return p;
    };

    // A finbreak-style CLAUDE.md: HAS `## Module map` but as a `- path —
    // description` list the lane parser can't harvest → present, 0 lanes.
    const QString finbreak = put(QStringLiteral("CLAUDE.md"),
        "# finbreak\n\n## Module map\n\n"
        "- src/api/main.py — FastAPI entry point.\n"
        "- src/filter/rules.py — rule engine.\n");
    expect(SubsystemMap::sourceHasModuleMap(finbreak),
           "ANTS-3481/present",
           "sourceHasModuleMap must return true when the heading exists");
    expect(SubsystemMap::cachedLanes(finbreak).isEmpty(),
           "ANTS-3481/unparseable",
           "the path-list bullets must derive zero lanes (the repro)");

    // A doc with NO module-map heading → absent.
    const QString noMap = put(QStringLiteral("nomap.md"),
        "# proj\n\n## Build\n\nsome text.\n");
    expect(!SubsystemMap::sourceHasModuleMap(noMap),
           "ANTS-3481/absent",
           "sourceHasModuleMap must return false with no heading");
    // A non-existent path → false, no crash.
    expect(!SubsystemMap::sourceHasModuleMap(dir.path() + "/nope.md"),
           "ANTS-3481/missing-file",
           "sourceHasModuleMap must return false for a missing file");

    // Wiring: cmdIndieReviewOrchestrate branches on sourceHasModuleMap and
    // emits the distinct module_map_unparseable code.
    const std::string rc = ants_test::slurpRemoteControl();
    expect(rc.find("module_map_unparseable") != std::string::npos,
           "ANTS-3481/wiring-code",
           "cmdIndieReviewOrchestrate must emit module_map_unparseable");
    expect(rc.find("SubsystemMap::sourceHasModuleMap") != std::string::npos,
           "ANTS-3481/wiring-call",
           "orchestrate must call SubsystemMap::sourceHasModuleMap");

    EXPECT_EQ(0, expect_failures()) << expect_failures()
        << " ANTS-3481 invariant(s) failed";
}

// ANTS-5074 — SubsystemMap::cachedLanes()'s cache (a function-static QHash)
// is read and written with no lock, from the GUI thread (Independent Review
// dialog → derivePartition) and the MCP worker (subsystem / indie_review_partition
// verbs) alike, and from the GUI thread again via the remote-control socket's
// subsystem route. A find concurrent with an insert that rehashes the QHash
// is undefined behaviour. Fix: a QMutex held around every cache access.
namespace ants5074 {

// Write `bytes` to `path` and pin its mtime to exactly `mtimeMs` via
// utimensat — ms-precision mtime control, matching
// tests/features/file_content_cache's writeFileAt. QFile::setFileTime
// proved unreliable on the test sandbox. Ants is Linux-only.
//
// No GTEST macros in here deliberately — this is also called from the
// rewriter background thread in the concurrency guard below, and gtest's
// fatal/non-fatal assertion macros are for the main test thread. Failures
// are reported via the plain bool return; callers on the main thread wrap
// it in ASSERT_TRUE, and the background-thread caller counts failures in
// an atomic instead (see Ants5074ConcurrentAccessDoesNotCorruptOrCrash).
bool writeFileAtQuiet(const QString &path, const QByteArray &bytes, qint64 mtimeMs) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(bytes);
    f.close();
    struct timespec ts[2];
    ts[0].tv_sec  = mtimeMs / 1000;
    ts[0].tv_nsec = (mtimeMs % 1000) * 1000000;
    ts[1] = ts[0];
    const int rc = utimensat(AT_FDCWD, path.toLocal8Bit().constData(), ts, 0);
    return rc == 0;
}

// Main-thread convenience wrapper: same as writeFileAtQuiet, but records a
// gtest failure with a diagnosis on error.
bool writeFileAt(const QString &path, const QByteArray &bytes, qint64 mtimeMs) {
    const bool ok = writeFileAtQuiet(path, bytes, mtimeMs);
    EXPECT_TRUE(ok) << "ants5074::writeFileAt: open/write/utimensat failed for "
                     << qUtf8Printable(path);
    return ok;
}

// Atomically replace `path`'s content+mtime: write to a sibling temp file,
// pin ITS mtime, then rename() over `path`. rename() is atomic on the same
// filesystem, so a concurrent reader sees either the whole old file or the
// whole new one — never a torn write — which keeps the concurrency guard
// below honest about what it is testing (the cache's own locking) rather
// than picking up noise from a non-atomic in-place rewrite. No GTEST
// macros — background-thread-safe, see writeFileAtQuiet above.
bool replaceFileAtomicallyQuiet(const QString &path, const QByteArray &bytes, qint64 mtimeMs) {
    const QString tmp = path + QStringLiteral(".tmp-ants5074");
    if (!writeFileAtQuiet(tmp, bytes, mtimeMs)) return false;
    return ::rename(tmp.toLocal8Bit().constData(),
                    path.toLocal8Bit().constData()) == 0;
}

// A one-bullet module-map body naming a single lane `laneName`, matching
// the bullet shape SubsystemMap::parse() reads.
QByteArray moduleMapBody(const QString &laneName) {
    const QString text = QStringLiteral("## Module map (src/)\n\n"
                                        "- `%1` -- lane %1 summary.\n")
                              .arg(laneName);
    return text.toUtf8();
}

// True iff `lanes` is exactly the single lane parsed from `moduleMapBody(laneName)`.
bool isExactlyLane(const QVector<SubsystemMap::Lane> &lanes, const QString &laneName) {
    if (lanes.size() != 1) return false;
    return lanes.at(0).name == laneName;
}

}  // namespace ants5074

// ANTS-5074/wiring — cachedLanes() takes a lock before its first access to
// the shared cache. Source-grepped, comment-stripped (ANTS-3662) so a
// comment mentioning "QMutexLocker" cannot pass this by accident, and
// anchored on the FIRST "cachedLanes(" match (srcgrep.h contract) — there
// is exactly one function of that name in subsystemmap.cpp.
//
// Pre-fix: cachedLanes() calls `cache()` (returns the function-static
// QHash) and then `c.find(key)` / `c.insert(key, entry)` with no lock
// construct anywhere in the body, so this is red against the current tree.
TEST(McpSubsystem, Ants5074CacheAccessIsLocked) {
    expect_reset();

    const std::string smCppNoComments =
        ants_test::stripComments(ants_test::slurpFile(SRC_SUBSYSTEMMAP_CPP_PATH));
    const std::string body =
        ants_test::slurpFunctionBody(smCppNoComments, "QVector<Lane> cachedLanes(");

    bool haveBody = !body.empty();
    expect(haveBody, "ANTS-5074/wiring-body-found",
           "could not locate the cachedLanes() function body in "
           "subsystemmap.cpp (anchor \"QVector<Lane> cachedLanes(\" not "
           "found, or braces unbalanced) — cannot check locking without it");

    if (haveBody) {
        // First point the body touches the shared cache: the call to
        // cache(), which returns the function-static QHash by reference.
        // Both the pre-fix code and the described fix still call cache()
        // to obtain the QHash, so this is a stable anchor across the fix.
        const std::size_t cachePos = body.find("cache()");

        // Earliest lock-construct keyword in the body, if any.
        std::size_t lockPos = std::string::npos;
        for (const char *kw : {"QMutexLocker", "std::lock_guard", "std::scoped_lock"}) {
            const std::size_t p = body.find(kw);
            if (p != std::string::npos && (lockPos == std::string::npos || p < lockPos)) {
                lockPos = p;
            }
        }

        const bool cacheFound = cachePos != std::string::npos;
        const bool lockFound  = lockPos != std::string::npos;
        const bool lockedBeforeCacheAccess =
            cacheFound && lockFound && lockPos < cachePos;

        char detail[320];
        std::snprintf(detail, sizeof detail,
                      "cachedLanes() body: cache() call %s (pos %lld), "
                      "lock construct (QMutexLocker/std::lock_guard/"
                      "std::scoped_lock) %s (pos %lld) — lock must appear "
                      "strictly before the cache() call",
                      cacheFound ? "found" : "NOT found",
                      cacheFound ? static_cast<long long>(cachePos) : -1LL,
                      lockFound ? "found" : "NOT found",
                      lockFound ? static_cast<long long>(lockPos) : -1LL);
        expect(lockedBeforeCacheAccess, "ANTS-5074/wiring-lock-before-access", detail);
    }

    EXPECT_EQ(0, expect_failures()) << expect_failures()
        << " ANTS-5074 wiring invariant(s) failed";
}

// ANTS-5074/mtime-guard — a locked cache must still honour its existing
// mtime-only invalidation contract (ANTS-1251-INV-2): an unchanged mtime
// serves the cached lanes even if the underlying bytes changed underneath
// it (proves the cache is consulted, not bypassed), and a changed mtime
// re-parses and returns the new lanes. Single-threaded and deterministic —
// this is not the concurrency guard below, it is a regression guard against
// a lock fix that accidentally reworks the invalidation logic it wraps.
TEST(McpSubsystem, Ants5074MtimeStillGatesInvalidationUnderTheLock) {
    expect_reset();
    SubsystemMap::clearCacheForTests();

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("guard.md"));
    constexpr qint64 kBaseMtimeMs = 1700000000000LL;

    ASSERT_TRUE(ants5074::writeFileAt(path, ants5074::moduleMapBody(QStringLiteral("alpha")),
                                      kBaseMtimeMs));
    const QVector<SubsystemMap::Lane> first = SubsystemMap::cachedLanes(path);
    expect(ants5074::isExactlyLane(first, QStringLiteral("alpha")),
           "ANTS-5074/mtime-guard-initial-parse",
           "first cachedLanes() call did not parse the seeded content into "
           "lane \"alpha\"");

    // Overwrite the BYTES but pin the SAME mtime. A correct cache serves
    // the stale "alpha" entry (mtime-keyed, no wall-clock TTL); it must not
    // silently re-read the file just because it was asked again.
    ASSERT_TRUE(ants5074::writeFileAt(path, ants5074::moduleMapBody(QStringLiteral("beta")),
                                      kBaseMtimeMs));
    const QVector<SubsystemMap::Lane> stillCached = SubsystemMap::cachedLanes(path);
    expect(ants5074::isExactlyLane(stillCached, QStringLiteral("alpha")),
           "ANTS-5074/mtime-guard-unchanged-mtime-serves-cache",
           "an unchanged mtime must still return the cached \"alpha\" lanes "
           "even though the file's bytes now say \"beta\"");

    // Now bump the mtime along with the content: the cache must invalidate
    // and return the new lanes.
    ASSERT_TRUE(ants5074::writeFileAt(path, ants5074::moduleMapBody(QStringLiteral("beta")),
                                      kBaseMtimeMs + 1000));
    const QVector<SubsystemMap::Lane> reparsed = SubsystemMap::cachedLanes(path);
    expect(ants5074::isExactlyLane(reparsed, QStringLiteral("beta")),
           "ANTS-5074/mtime-guard-changed-mtime-reparses",
           "a changed mtime must invalidate the cache and return the new "
           "\"beta\" lanes");

    EXPECT_EQ(0, expect_failures()) << expect_failures()
        << " ANTS-5074 mtime-guard invariant(s) failed";
}

// ANTS-5074/concurrency-guard — several threads call cachedLanes() on a
// handful of distinct temporary module-map files, many times each, while
// one thread keeps replacing one file's content (and bumping its mtime)
// underneath the readers. Every result a reader gets back must equal the
// lanes that some content the file legitimately held (at time of read)
// parses to — never a mixed/garbage read — and the process must not crash.
//
// This is a GUARD, not a strict pass/fail lock on the pre-fix code: an
// unlocked QHash rehash race is undefined behaviour, and undefined
// behaviour can run clean by luck under light load exactly as easily as it
// can corrupt state or crash. A clean run here does NOT prove the fix is
// in; a mismatch or a crash DOES prove the race is still live. Per spec.md,
// this guard is most reliably tripped under the ASan sanitizer build
// (`debug` preset / `tools/ci-parity.sh --full`), which turns the heap
// corruption from a concurrent QHash rehash into a deterministic abort with
// a diagnosis — plain Release execution may pass by chance even against
// the unlocked code.
TEST(McpSubsystem, Ants5074ConcurrentAccessDoesNotCorruptOrCrash) {
    expect_reset();
    SubsystemMap::clearCacheForTests();

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // Three files whose content never changes for the duration of the test.
    constexpr int kStaticFileCount = 3;
    const QStringList staticLaneNames = {QStringLiteral("one"), QStringLiteral("two"),
                                         QStringLiteral("three")};
    QStringList staticPaths;
    for (int i = 0; i < kStaticFileCount; ++i) {
        const QString path = dir.filePath(QStringLiteral("static_%1.md").arg(i));
        ASSERT_TRUE(ants5074::writeFileAt(path, ants5074::moduleMapBody(staticLaneNames[i]),
                                          1700000004000LL + i));
        staticPaths << path;
    }

    // One file a dedicated writer thread keeps replacing, cycling through a
    // small CLOSED set of known-valid variants. Any non-empty result a
    // reader gets for this path must equal exactly one of these variants —
    // membership, not a single expected value, because a read can legally
    // land on the content that was current a moment ago.
    const QString rotatingPath = dir.filePath(QStringLiteral("rotating.md"));
    const QStringList rotatingVariants = {QStringLiteral("rot-a"), QStringLiteral("rot-b"),
                                          QStringLiteral("rot-c")};
    ASSERT_TRUE(ants5074::writeFileAt(rotatingPath,
                                      ants5074::moduleMapBody(rotatingVariants[0]),
                                      1700000009000LL));

    constexpr int kReaderThreads       = 6;
    constexpr int kIterationsPerReader = 400;
    constexpr auto kWallClockCap       = std::chrono::seconds(25);

    std::atomic<bool> stop{false};
    std::atomic<int>  mismatches{0};
    std::atomic<int>  writeFailures{0};
    std::atomic<int>  crashCanary{0};  // incremented just before/after each call

    std::vector<std::thread> readers;
    readers.reserve(kReaderThreads);
    for (int t = 0; t < kReaderThreads; ++t) {
        readers.emplace_back([&, t]() {
            for (int it = 0; it < kIterationsPerReader && !stop.load(); ++it) {
                const int which = (t + it) % (kStaticFileCount + 1);
                crashCanary.fetch_add(1, std::memory_order_relaxed);
                if (which < kStaticFileCount) {
                    const QVector<SubsystemMap::Lane> got =
                        SubsystemMap::cachedLanes(staticPaths[which]);
                    if (!got.isEmpty() &&
                        !ants5074::isExactlyLane(got, staticLaneNames[which])) {
                        mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    const QVector<SubsystemMap::Lane> got =
                        SubsystemMap::cachedLanes(rotatingPath);
                    if (!got.isEmpty()) {
                        bool matchesSomeVariant = false;
                        for (const QString &variant : rotatingVariants) {
                            if (ants5074::isExactlyLane(got, variant)) {
                                matchesSomeVariant = true;
                                break;
                            }
                        }
                        if (!matchesSomeVariant) {
                            mismatches.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                crashCanary.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::thread rewriter([&]() {
        const auto deadline = std::chrono::steady_clock::now() + kWallClockCap;
        int i = 1;
        qint64 mtime = 1700000009000LL;
        while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
            mtime += 1;  // strictly increasing: never two writes share an mtime
            const bool ok = ants5074::replaceFileAtomicallyQuiet(
                rotatingPath, ants5074::moduleMapBody(rotatingVariants[i % rotatingVariants.size()]),
                mtime);
            if (!ok) writeFailures.fetch_add(1, std::memory_order_relaxed);
            ++i;
        }
    });

    for (auto &r : readers) r.join();
    stop.store(true);
    rewriter.join();

    expect(writeFailures.load() == 0, "ANTS-5074/concurrency-rewrites-succeeded",
           "the rewriter thread's atomic replace (write-temp + rename) failed "
           "at least once — a setup problem, not the race under test");

    const std::string mismatchDetail =
        "a cachedLanes() read returned lanes that did not match ANY "
        "content the file legitimately held while a concurrent write "
        "was in flight — indicates the shared QHash cache was "
        "corrupted by a racing find/insert. mismatches=" +
        std::to_string(mismatches.load());
    expect(mismatches.load() == 0, "ANTS-5074/concurrency-no-mismatch", mismatchDetail);

    // Reaching here at all (rather than a sanitizer abort / segfault killing
    // the test process) is itself part of what this guard checks; ctest
    // records a crash as its own distinct failure for this entry.
    expect(crashCanary.load() > 0, "ANTS-5074/concurrency-completed",
           "reader threads made no cachedLanes() calls at all — the guard "
           "did not actually exercise the race");

    EXPECT_EQ(0, expect_failures()) << expect_failures()
        << " ANTS-5074 concurrency-guard invariant(s) failed";
}
