// Feature-conformance test for spec.md —
//
// Source-grep checks. PluginManager::scanAndLoad must:
//   I1: cap manifest reads at 1 MiB via kMaxManifestBytes (in
//       readManifestInto, which scanAndLoad calls).
//   I2: pass QDir::NoSymLinks to entryList.
//   I3: anchor on canonicalFilePath of m_pluginDir.
//   I4: per-entry canonical-containment reject.
// Runtime (ANTS-6645): a FIFO manifest.json or init.lua is never opened.

#include "pluginmanager.h"

#include "../../_support/expect.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QUuid>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#ifndef SRC_PLUGINMANAGER_CPP_PATH
#  error "SRC_PLUGINMANAGER_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {




bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}

std::string extractFn(const std::string &src, const std::string &fnSig) {
    const size_t at = src.find(fnSig);
    if (at == std::string::npos) return {};
    const size_t openBrace = src.find('{', at);
    if (openBrace == std::string::npos) return {};
    int depth = 1;
    size_t i = openBrace + 1;
    while (i < src.size() && depth > 0) {
        char c = src[i];
        if (c == '"') {
            ++i;
            while (i < src.size() && src[i] != '"') {
                if (src[i] == '\\') ++i;
                if (i < src.size()) ++i;
            }
            if (i < src.size()) ++i;
            continue;
        }
        if (c == '{') ++depth;
        else if (c == '}') --depth;
        ++i;
    }
    return src.substr(openBrace, i - openBrace);
}

}  // namespace

static int runMain() {
    expect_reset();
    const std::string src = ants_test::slurpFile(SRC_PLUGINMANAGER_CPP_PATH);
    const std::string body = extractFn(src,
        "void PluginManager::scanAndLoad(const QStringList &enabledList)");
    expect(!body.empty(), "extract/scanAndLoad-body-found");

    // I1 — manifest cap. scanAndLoad reads the manifest through
    // readManifestInto, which holds the cap.
    expect(contains(body, "readManifestInto(info, manifestPath)"),
           "I1/scanAndLoad-reads-manifest-via-helper");
    const std::string manifest = extractFn(src,
        "static void readManifestInto(PluginInfo &info, const QString &manifestPath)");
    expect(!manifest.empty(), "extract/readManifestInto-body-found");
    expect(contains(manifest, "kMaxManifestBytes"),
           "I1/kMaxManifestBytes-named-constant");
    expect(contains(manifest, "1024 * 1024"),
           "I1/cap-value-1MiB");
    expect(contains(manifest, "f.read(kMaxManifestBytes)"),
           "I1/uses-bounded-read-not-readAll");
    expect(!contains(manifest, "f.readAll(), &err"),
           "I1/no-unbounded-readAll-feeding-fromJson");

    // I2 — NoSymLinks flag.
    expect(contains(body, "QDir::NoSymLinks"),
           "I2/entryList-passes-NoSymLinks");

    // I3 — canonical root anchor.
    expect(contains(body,
               "QFileInfo(m_pluginDir).canonicalFilePath()"),
           "I3/canonicalFilePath-on-m_pluginDir");
    expect(contains(body, "canonicalRoot"),
           "I3/canonicalRoot-named");

    // I4 — per-entry canonical-containment.
    expect(contains(body, "QFileInfo(pluginPath).canonicalFilePath()"),
           "I4/per-entry-canonical-resolution");
    expect(contains(body, "canonicalRootPrefix"),
           "I4/canonical-root-prefix-named");
    expect(contains(body, "startsWith(canonicalRootPrefix)"),
           "I4/canonical-startsWith-containment-check");
    // The qWarning format string spans two adjacent C-string
    // literals at the line wrap, so "resolves outside the plugin"
    // is the contiguous substring we can grep for. The full
    // sentence "resolves outside the plugin root" only exists
    // post-preprocessor concatenation.
    expect(contains(body, "resolves outside the plugin"),
           "I4/reject-warning-message");

    return expect_finish();
}

TEST(PluginManifestSafety, Main) {
    ASSERT_EQ(0, runMain());
}

// ANTS-6645 — opening a FIFO for reading blocks until a writer appears, so
// scanAndLoad must not open a plugin file that is not a regular file.
namespace {

// Opens `path` for writing without blocking, over and over until finish().
// That open succeeds only while something holds the FIFO open for reading,
// so opens() counts Ants' attempts to read it; each success also frees the
// blocked reader, so the old code fails instead of hanging.
class FifoWatch {
public:
    explicit FifoWatch(const QString &path)
        : m_path(QFile::encodeName(path)), m_thread([this] {
              while (!m_stop.load()) {
                  const int fd = ::open(m_path.constData(), O_WRONLY | O_NONBLOCK);
                  if (fd >= 0) { ++m_opens; ::close(fd); }
                  std::this_thread::sleep_for(std::chrono::milliseconds(5));
              }
          }) {}
    ~FifoWatch() { finish(); }
    FifoWatch(const FifoWatch &) = delete;
    FifoWatch &operator=(const FifoWatch &) = delete;
    int opens() const { return m_opens.load(); }
    void finish() {
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
    }

private:
    QByteArray m_path;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_opens{0};
    std::thread m_thread;   // last: it starts once the members above exist
};

void pump(int ms) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

// A plugins root holding one plugin, `piped`, whose `fifoName` is a FIFO and
// whose other file is a regular one. Returns the root, or empty on failure.
QString makePluginWithFifo(const QString &fifoName) {
    const QString root =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/ants-plugin-fifo-")
        + QUuid::createUuid().toString(QUuid::Id128);
    const QString plugin = root + QStringLiteral("/piped");
    QDir().mkpath(plugin);
    for (const QString &name : {QStringLiteral("init.lua"),
                                QStringLiteral("manifest.json")}) {
        const QString path = plugin + QLatin1Char('/') + name;
        if (name == fifoName) {
            if (::mkfifo(QFile::encodeName(path).constData(), 0600) != 0)
                return {};
        } else {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly)) return {};
            if (name == QStringLiteral("manifest.json")) f.write("{}");
        }
    }
    return root;
}

}  // namespace

TEST(PluginManifestSafety, Ants6645FifoManifestIsNotOpened) {
    const QString root = makePluginWithFifo(QStringLiteral("manifest.json"));
    ASSERT_FALSE(root.isEmpty()) << "could not build the FIFO plugin";
    FifoWatch watch(root + QStringLiteral("/piped/manifest.json"));
    {
        PluginManager pm;
        pm.setPluginDir(root);
        pm.scanAndLoad({QStringLiteral("piped")});
        pump(200);
    }
    watch.finish();
    QDir(root).removeRecursively();
    EXPECT_EQ(watch.opens(), 0)
        << "scanAndLoad opened a FIFO manifest.json on the GUI thread";
}

TEST(PluginManifestSafety, Ants6645FifoInitLuaIsNotLoaded) {
    const QString root = makePluginWithFifo(QStringLiteral("init.lua"));
    ASSERT_FALSE(root.isEmpty()) << "could not build the FIFO plugin";
    FifoWatch watch(root + QStringLiteral("/piped/init.lua"));
    bool listed = false;
    {
        PluginManager pm;
        pm.setPluginDir(root);
        pm.scanAndLoad({QStringLiteral("piped")});
        for (const PluginInfo &p : pm.plugins())
            if (p.name == QStringLiteral("piped")) listed = true;
        // Only the old code has a worker reading the FIFO; give the watch
        // time to free it before teardown.
        if (listed) pump(500);
    }
    watch.finish();
    QDir(root).removeRecursively();
    EXPECT_FALSE(listed) << "a plugin whose init.lua is a FIFO was listed";
    EXPECT_EQ(watch.opens(), 0) << "a FIFO init.lua was opened";
}
