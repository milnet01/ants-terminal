// Feature-conformance test for spec.md — ANTS-5056.
//
// Why this exists: BriefDispatch, ColdEyesEngine, IndieReviewEngine and
// DebtSweepEngine each kept their own function-static QHash file-content
// cache, commented single-threaded — false since ANTS-2132 put their verbs
// on the MCP worker while the dialogs that fill the same shape of cache
// still run on the GUI thread. Three of the four copies also had no size
// bound. This locks the shared replacement's byte budget, its freshness /
// read-mode keying, a concurrency guard, and that all four engines are
// actually wired to it.

#include "filecontentcache.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QChar>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <fcntl.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "../../_support/srcgrep.h"

#ifndef SRC_BRIEFDISPATCH_CPP_PATH
#error "SRC_BRIEFDISPATCH_CPP_PATH compile definition required"
#endif
#ifndef SRC_COLDEYESENGINE_CPP_PATH
#error "SRC_COLDEYESENGINE_CPP_PATH compile definition required"
#endif
#ifndef SRC_INDIE_REVIEW_ENGINE_CPP_PATH
#error "SRC_INDIE_REVIEW_ENGINE_CPP_PATH compile definition required"
#endif
#ifndef SRC_DEBTSWEEPENGINE_CPP_PATH
#error "SRC_DEBTSWEEPENGINE_CPP_PATH compile definition required"
#endif

namespace {

// Linux-only: utimensat for ms-precision mtime control, matching
// tests/features/cold_eyes_partition_deterministic — QFile::setFileTime
// proved unreliable on the test sandbox (returns true without moving the
// inode time). Ants is Linux-only, so this is not a portability concern.
// Returns false (via ADD_FAILURE) rather than aborting, so a setup failure
// reads as a clear cause rather than a segfault deeper in the test.
bool writeFileAt(const QString &path, const QByteArray &bytes, qint64 mtimeMs) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        ADD_FAILURE() << "writeFileAt: cannot open " << qUtf8Printable(path);
        return false;
    }
    f.write(bytes);
    f.close();
    struct timespec ts[2];
    ts[0].tv_sec  = mtimeMs / 1000;
    ts[0].tv_nsec = (mtimeMs % 1000) * 1000000;
    ts[1] = ts[0];
    const int rc = utimensat(AT_FDCWD, path.toLocal8Bit().constData(), ts, 0);
    EXPECT_EQ(rc, 0) << "utimensat failed for " << qUtf8Printable(path);
    return rc == 0;
}

// Finds the argument list of the next `FileContentCache::slurpUtf8(...)`
// call at or after `from`, matching parens (not braces — slurpFunctionBody
// in srcgrep.h is the brace-matching sibling of this). Returns the raw
// argument text (no surrounding parens) and advances `from` past the call,
// so repeated calls walk every call site in a file. Empty string + `from`
// left at std::string::npos when no further call is found.
std::string nextSlurpCallArgs(const std::string &src, std::size_t &from) {
    static const std::string kAnchor = "FileContentCache::slurpUtf8(";
    const std::size_t pos = src.find(kAnchor, from);
    if (pos == std::string::npos) {
        from = std::string::npos;
        return {};
    }
    const std::size_t argStart = pos + kAnchor.size();
    int depth = 1;
    std::size_t i = argStart;
    for (; i < src.size() && depth > 0; ++i) {
        if (src[i] == '(') ++depth;
        else if (src[i] == ')') --depth;
    }
    from = i;
    if (depth != 0) return {};  // unbalanced — treat as not found
    return src.substr(argStart, (i - 1) - argStart);
}

}  // namespace

// INV-1 — after clear(), reading several files whose combined decoded size
// runs well past half of kByteBudget must never let cachedBytes() exceed
// kByteBudget, and every read must still return the right content. The
// stub (tests/features/file_content_cache/spec.md "Out of scope" — no size
// bound at all pre-fix) fails this: cachedBytes() keeps climbing past the
// budget as each file is read.
TEST(FileContentCache, ByteBudgetHoldsUnderLoad) {
    FileContentCache::clear();

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const qint64 budget = FileContentCache::kByteBudget;
    // Each file decodes to budget/3 UTF-16 bytes (QChar is 2 bytes), i.e.
    // budget/6 ASCII characters. Four of them sum to ~4/3 of the budget —
    // well past half, and past the whole budget, so an unbounded cache
    // (the stub) is caught and a bounded one is forced to evict.
    const qint64 perFileChars = budget / 6;
    const int kFileCount = 4;

    QStringList paths;
    QStringList expected;
    for (int i = 0; i < kFileCount; ++i) {
        const QString path = dir.filePath(QStringLiteral("budget_%1.txt").arg(i));
        const QString content(static_cast<int>(perFileChars),
                              QChar(QLatin1Char(static_cast<char>('A' + i))));
        ASSERT_TRUE(writeFileAt(path, content.toLatin1(), 1700000000000LL + i))
            << "setup: could not write " << qUtf8Printable(path);
        paths << path;
        expected << content;
    }

    for (int i = 0; i < kFileCount; ++i) {
        const QString got = FileContentCache::slurpUtf8(paths[i]);
        EXPECT_EQ(got, expected[i])
            << "INV-1: slurpUtf8(" << qUtf8Printable(paths[i])
            << ") returned the wrong content on a fresh read.";

        const qint64 bytes = FileContentCache::cachedBytes();
        EXPECT_LE(bytes, budget)
            << "INV-1: cachedBytes() == " << bytes
            << " after reading file " << i << " of " << kFileCount
            << ". Expected: <= kByteBudget (" << budget
            << "). Actual: over budget — the cache is not honouring its "
               "byte bound.";
    }
}

// INV-2 — a file whose own decoded size exceeds kByteBudget is still
// returned whole and correctly, and must not be retained: cachedBytes()
// stays within budget after reading it. Pre-fix the stub caches everything
// unconditionally, so cachedBytes() after this read equals the oversized
// body's own size, which is greater than kByteBudget.
TEST(FileContentCache, OversizedFileServedNotRetained) {
    FileContentCache::clear();

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const qint64 budget = FileContentCache::kByteBudget;
    // UTF-16 bytes = 2 * char count, so budget/2 + a small margin of ASCII
    // characters decodes to just over kByteBudget bytes — the smallest
    // file that exercises "larger than the budget".
    const qint64 overBudgetChars = budget / 2 + 4096;
    const QString path = dir.filePath(QStringLiteral("oversized.txt"));
    const QString content(static_cast<int>(overBudgetChars), QLatin1Char('Z'));
    ASSERT_TRUE(writeFileAt(path, content.toLatin1(), 1700000001000LL))
        << "setup: could not write " << qUtf8Printable(path);

    const QString got = FileContentCache::slurpUtf8(path);
    EXPECT_EQ(got, content)
        << "INV-2: an oversized file must still be returned in full and "
           "correctly, even though it is too large to cache.";

    const qint64 bytes = FileContentCache::cachedBytes();
    EXPECT_LE(bytes, budget)
        << "INV-2: cachedBytes() == " << bytes
        << " after reading a file whose own decoded size ("
        << (overBudgetChars * 2) << " UTF-16 bytes) exceeds kByteBudget ("
        << budget << "). Expected: it is not retained. Actual: it was "
           "cached anyway.";
}

// INV-3 — freshness: a file rewritten with a new mtime returns the new
// content on the next read. Read-mode key: textMode=true collapses CRLF to
// LF while textMode=false keeps the CR, and both must be readable from the
// same path because the mode is part of the cache key. Passes before and
// after the fix — this is a guard against the fix accidentally dropping
// mtime or mode from the key, not a lock on the byte-budget defect.
TEST(FileContentCache, FreshnessAndReadModeAreBothPartOfTheKey) {
    FileContentCache::clear();

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("fresh.txt"));

    ASSERT_TRUE(writeFileAt(path, QByteArrayLiteral("one\r\ntwo\r\n"),
                            1700000002000LL));

    const QString gotBinary1 = FileContentCache::slurpUtf8(path, false);
    EXPECT_EQ(gotBinary1, QStringLiteral("one\r\ntwo\r\n"))
        << "INV-3: textMode=false must keep the CR.";

    const QString gotText1 = FileContentCache::slurpUtf8(path, true);
    EXPECT_EQ(gotText1, QStringLiteral("one\ntwo\n"))
        << "INV-3: textMode=true must collapse CRLF to LF, and must not "
           "share a cache entry with the textMode=false read of the same "
           "unchanged path — got: " << gotText1.toStdString();

    // Freshness: same path, new mtime, new content — a stale cache entry
    // (keyed only on path/mode, ignoring mtime) would return the old body.
    ASSERT_TRUE(writeFileAt(path, QByteArrayLiteral("three\r\nfour\r\n"),
                            1700000003000LL));

    const QString gotBinary2 = FileContentCache::slurpUtf8(path, false);
    EXPECT_EQ(gotBinary2, QStringLiteral("three\r\nfour\r\n"))
        << "INV-3: a rewritten file with a new mtime must return the new "
           "content on the next read. Expected: \"three\\r\\nfour\\r\\n\". "
           "Actual: " << gotBinary2.toStdString();

    const QString gotText2 = FileContentCache::slurpUtf8(path, true);
    EXPECT_EQ(gotText2, QStringLiteral("three\nfour\n"))
        << "INV-3: the textMode=true read must also see the rewritten "
           "content, with CRLF collapsed. Actual: " << gotText2.toStdString();
}

// INV-4 (guard) — several threads reading a shared set of small files many
// times, while another thread repeatedly clears the cache, must not
// corrupt what a read returns and must not crash the process. Every ctest
// entry from gtest_discover_tests is its own process invocation
// (CMakeLists.txt ANTS-1217), so a crash here fails only this one entry,
// not the shared bundle. Bounded by iteration count and a wall-clock cap
// so a failure reads as a diagnosable assertion rather than a hang.
//
// This is explicitly NOT a strict pass/fail lock on the pre-fix stub: an
// unlocked QHash under light concurrent load can run clean by luck as
// easily as it can corrupt or crash. spec.md says so — it is meant to be
// run (and re-run) under the sanitizer build, where a real data race is
// far likelier to be caught. A clean run here does not mean the stub is
// safe; a crash or content mismatch does mean it is not.
TEST(FileContentCache, ConcurrentReadersAndClearerDoNotCorruptOrCrash) {
    FileContentCache::clear();

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const int kFileCount = 4;
    QStringList paths;
    QStringList expected;
    for (int i = 0; i < kFileCount; ++i) {
        const QString path = dir.filePath(QStringLiteral("concurrent_%1.txt").arg(i));
        const QString content = QStringLiteral("payload-%1").arg(i);
        ASSERT_TRUE(writeFileAt(path, content.toUtf8(), 1700000004000LL + i));
        paths << path;
        expected << content;
    }

    constexpr int kReaderThreads = 4;
    constexpr int kIterationsPerReader = 500;
    constexpr auto kClearInterval = std::chrono::microseconds(200);
    constexpr auto kWallClockCap = std::chrono::seconds(20);

    std::atomic<bool> stop{false};
    std::atomic<int> mismatches{0};

    std::vector<std::thread> readers;
    readers.reserve(kReaderThreads);
    for (int t = 0; t < kReaderThreads; ++t) {
        readers.emplace_back([&, t]() {
            for (int it = 0; it < kIterationsPerReader && !stop.load(); ++it) {
                const int idx = (t + it) % kFileCount;
                const QString got = FileContentCache::slurpUtf8(paths[idx]);
                if (!got.isEmpty() && got != expected[idx]) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::thread clearer([&]() {
        const auto deadline = std::chrono::steady_clock::now() + kWallClockCap;
        while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
            FileContentCache::clear();
            std::this_thread::sleep_for(kClearInterval);
        }
    });

    for (auto &r : readers) r.join();
    stop.store(true);
    clearer.join();

    EXPECT_EQ(mismatches.load(), 0)
        << "INV-4 (guard): " << mismatches.load()
        << " read(s) returned content that did not match any expected "
           "file body while a concurrent clear() was in flight. Expected: "
           "0 mismatches (an empty string from a raced-with-clear open "
           "failure is tolerated; a WRONG non-empty body is not). Actual: "
        << mismatches.load()
        << " — reaching this assertion at all means the process did not "
           "crash; a crash instead is this guard's other failure mode "
           "(see spec.md).";
}

// INV-5 — wiring: none of the four engine .cpp files defines its own
// per-file static cache any more; each calls FileContentCache::slurpUtf8,
// and BriefDispatch's call passes textMode=true. Pre-fix every one of the
// four still defines `static QHash<QString, QPair<qint64, QString>>
// s_cache;` and none calls FileContentCache::slurpUtf8 at all, so this
// fails on both counts against the current tree.
TEST(FileContentCache, EveryEngineGoesThroughTheSharedCache) {
    const char *kRetiredCacheShape =
        "static QHash<QString, QPair<qint64, QString>>";
    const char *kSharedCall = "FileContentCache::slurpUtf8(";

    const struct { const char *label; const char *path; } engines[] = {
        {"briefdispatch.cpp", SRC_BRIEFDISPATCH_CPP_PATH},
        {"coldeyesengine.cpp", SRC_COLDEYESENGINE_CPP_PATH},
        {"indiereviewengine.cpp", SRC_INDIE_REVIEW_ENGINE_CPP_PATH},
        {"debtsweepengine.cpp", SRC_DEBTSWEEPENGINE_CPP_PATH},
    };

    for (const auto &e : engines) {
        const std::string raw = ants_test::slurpFile(e.path);
        ASSERT_FALSE(raw.empty()) << "could not read " << e.path;
        const std::string src = ants_test::stripComments(raw);

        EXPECT_EQ(src.find(kRetiredCacheShape), std::string::npos)
            << "INV-5: " << e.label
            << " still defines its own retired static cache ("
            << kRetiredCacheShape << "). Expected: gone, in favour of the "
               "shared FileContentCache. Actual: still present.";

        EXPECT_NE(src.find(kSharedCall), std::string::npos)
            << "INV-5: " << e.label << " never calls " << kSharedCall
            << " — expected at least one call site routing through the "
               "shared cache. Actual: none found.";
    }

    // BriefDispatch specifically must pass textMode=true — the mode its
    // own retired cache got by opening with QIODevice::Text directly.
    const std::string briefRaw =
        ants_test::slurpFile(SRC_BRIEFDISPATCH_CPP_PATH);
    ASSERT_FALSE(briefRaw.empty())
        << "could not read " << SRC_BRIEFDISPATCH_CPP_PATH;
    const std::string briefSrc = ants_test::stripComments(briefRaw);

    std::size_t from = 0;
    bool sawTextModeTrue = false;
    int callCount = 0;
    for (;;) {
        const std::string args = nextSlurpCallArgs(briefSrc, from);
        if (from == std::string::npos) break;
        ++callCount;
        static const std::regex kTrueArg(R"(\btrue\b)");
        if (std::regex_search(args, kTrueArg)) sawTextModeTrue = true;
    }

    EXPECT_GT(callCount, 0)
        << "INV-5: briefdispatch.cpp calls " << kSharedCall
        << " zero times by paren-matched scan (found by substring above, "
           "but the argument-list scan disagrees) — unbalanced parens in "
           "a call site, or the call was not found at all.";
    EXPECT_TRUE(sawTextModeTrue)
        << "INV-5: none of briefdispatch.cpp's " << callCount
        << " call(s) to " << kSharedCall << " pass textMode=true. "
           "Expected: at least one call passing `true`, matching the "
           "QIODevice::Text mode its own retired cache used to open with. "
           "Actual: none found.";
}
