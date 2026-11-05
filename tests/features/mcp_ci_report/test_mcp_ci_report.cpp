// ANTS-6399 — ci_report. Contract: docs/specs/ANTS-6399-ci-report-verb.md;
// the INV-to-test map is spec.md beside this file.

#include <gtest/gtest.h>

#include "cihistory.h"
#include "cireport.h"
#include "ciworkflows.h"
#include "remotecontrol.h"
#include "sessionmemoryengine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>

#include <tuple>

#include "../../_support/srcgrep.h"

#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif

namespace {

const QDateTime kNow = QDateTime::fromString(QStringLiteral("2026-11-05T12:00:00Z"), Qt::ISODate);

QString iso(const QDateTime &t) { return t.toUTC().toString(Qt::ISODate); }

std::string dump(const QJsonValue &v) {
    return QJsonDocument(v.isArray() ? QJsonDocument(v.toArray()) : QJsonDocument(v.toObject()))
        .toJson().toStdString();
}

QJsonObject fixture(const char *name) {
    QFile f(QStringLiteral(ANTS_SOURCE_DIR "/tests/features/mcp_ci_report/fixtures/")
            + QLatin1String(name));
    EXPECT_TRUE(f.open(QIODevice::ReadOnly)) << name;
    return QJsonDocument::fromJson(f.readAll()).object();
}

CiWorkflows::Result workflow(const QString &yaml) {
    CiWorkflows::Result r;
    CiWorkflows::parseInto(r, QStringLiteral(".github/workflows/ci.yml"), yaml);
    return r;
}

QList<QJsonObject> ofCheck(const CiReport::Analysis &a, const char *check) {
    QList<QJsonObject> out;
    for (const QJsonValue &v : a.findings)
        if (v.toObject().value(QStringLiteral("check")).toString() == QLatin1String(check))
            out.append(v.toObject());
    return out;
}

// gh-shaped history built in code, for the timing checks.
struct History {
    QJsonArray runs;
    QJsonObject jobs;
    // One run of `wf` with one job; the job is created with the run, starts
    // `waitSecs` later and runs `secs`. Steps: {name, secs, conclusion}.
    void add(qint64 id, const QDateTime &created, const QString &job, int waitSecs, int secs,
             const QString &conclusion = QStringLiteral("success"),
             const QList<std::tuple<QString, int, QString>> &steps = {},
             const QString &event = QStringLiteral("push"),
             const QString &wf = QStringLiteral("CI")) {
        runs.append(QJsonObject{{"databaseId", double(id)}, {"headSha", QStringLiteral("sha%1").arg(id)},
                                {"conclusion", conclusion}, {"event", event}, {"workflowName", wf},
                                {"createdAt", iso(created)}, {"attempt", 1}, {"status", "completed"}});
        const QDateTime started = created.addSecs(waitSecs);
        QJsonArray stepArr;
        QDateTime at = started;
        for (const auto &[name, s, c] : steps) {
            stepArr.append(QJsonObject{{"name", name}, {"conclusion", c},
                                       {"started_at", iso(at)}, {"completed_at", iso(at.addSecs(s))}});
            at = at.addSecs(s);
        }
        jobs[QStringLiteral("%1/1").arg(id)] = QJsonObject{{"jobs", QJsonArray{QJsonObject{
            {"id", double(id * 10)}, {"name", job}, {"conclusion", conclusion},
            {"created_at", iso(created)}, {"started_at", iso(started)},
            {"completed_at", iso(started.addSecs(secs))}, {"steps", stepArr}}}}};
    }
    QJsonObject json() const { return {{"runs", runs}, {"jobs", jobs}}; }
};

const QString kBuild10 = QStringLiteral(
    "name: CI\n"
    "jobs:\n"
    "  build:\n"
    "    name: Build\n"
    "    runs-on: ubuntu-24.04\n"
    "    timeout-minutes: 10\n"
    "    steps:\n"
    "      - uses: actions/checkout@v4\n");

// Process-environment overrides, put back when the test ends.
class Env {
public:
    void set(const char *key, const QByteArray &value) {
        if (!m_saved.contains(key)) m_saved.insert(key, qgetenv(key));
        qputenv(key, value);
    }
    ~Env() {
        for (auto it = m_saved.cbegin(); it != m_saved.cend(); ++it)
            qputenv(it.key().constData(), it.value());
    }
private:
    QMap<QByteArray, QByteArray> m_saved;
};

// A fake `gh` first on PATH. It logs every call, answers `repo view` and
// `run list` from files in its directory, and an `api <path>` call from a file
// named by the path with / ? = & turned to _. FAKE_FAIL=list fails the listing.
struct FakeGh {
    QTemporaryDir dir;
    QString log() const { return dir.filePath(QStringLiteral("calls.log")); }
    void put(const QString &name, const QByteArray &body) const {
        QFile f(dir.filePath(name));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(body);
    }
    void install(Env &env, bool systemPath = true) {
        QFile gh(dir.filePath(QStringLiteral("gh")));
        ASSERT_TRUE(gh.open(QIODevice::WriteOnly));
        gh.write("#!/bin/bash\n"
                 "echo \"$*\" >> \"$FAKE_DIR/calls.log\"\n"
                 "if [[ $1 == repo ]]; then\n"
                 "  printf '%s' '{\"nameWithOwner\":\"o/r\",\"defaultBranchRef\":{\"name\":\"main\"}}'\n"
                 "elif [[ $1 == run ]]; then\n"
                 "  [[ $FAKE_FAIL == list ]] && { echo 'HTTP 401: Bad credentials' >&2; exit 1; }\n"
                 "  f=\"$FAKE_DIR/runs.json\"; [[ -f $f ]] && printf '%s' \"$(<\"$f\")\" || printf '[]'\n"
                 // Real gh refuses a log holding escape codes without this flag.
                 "elif [[ $1 == api && $2 == */logs && \" $* \" != *' --allow-escape-sequences '* ]]; then\n"
                 "  echo 'the response contains terminal escape sequences' >&2; exit 1\n"
                 "elif [[ $1 == api ]]; then\n"
                 "  f=\"$FAKE_DIR/${2//[\\/?=&]/_}\"; [[ -f $f ]] && printf '%s' \"$(<\"$f\")\" || printf '{}'\n"
                 "fi\n");
        gh.close();
        gh.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        env.set("FAKE_DIR", dir.path().toUtf8());
        env.set("PATH", systemPath ? dir.path().toUtf8() + ':' + qgetenv("PATH") : dir.path().toUtf8());
    }
    bool called() const { return QFileInfo::exists(log()); }
};

// A project with a workflow and a GitHub remote, and a private cache home.
struct Project {
    QTemporaryDir tmp, cache;
    QString root;
    explicit Project(bool withWorkflow = true, bool withRemote = true) {
        root = QFileInfo(tmp.path()).canonicalFilePath();
        if (withWorkflow) write(QStringLiteral(".github/workflows/ci.yml"), kBuild10.toUtf8());
        if (withRemote)
            write(QStringLiteral(".git/config"),
                  "[remote \"origin\"]\n\turl = https://github.com/o/r.git\n");
    }
    void write(const QString &rel, const QByteArray &body) const {
        QDir().mkpath(QFileInfo(root + QLatin1Char('/') + rel).path());
        QFile f(root + QLatin1Char('/') + rel);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(body);
    }
    QString savedPath() const {
        return cache.path() + QStringLiteral("/ants-terminal/ci-report/")
             + SessionMemoryEngine::cwdHash(root) + QStringLiteral(".json");
    }
    QJsonObject call(const QJsonObject &extra = {}) const {
        QJsonObject req = extra;
        req[QStringLiteral("caller_cwd")] = root;
        RemoteControl rc(nullptr);
        return rc.cmdCiReport(req).object();
    }
};

// Move every *At / *_at timestamp by `secs`, so a recorded fixture sits in
// the live verb's window whatever day the test runs.
QJsonValue shifted(const QJsonValue &v, qint64 secs) {
    if (v.isArray()) {
        QJsonArray out;
        for (const QJsonValue &e : v.toArray()) out.append(shifted(e, secs));
        return out;
    }
    if (!v.isObject()) return v;
    QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QString k = it.key();
        if ((k.endsWith(QLatin1String("At")) || k.endsWith(QLatin1String("_at"))) && it.value().isString())
            it.value() = iso(QDateTime::fromString(it.value().toString(), Qt::ISODate).addSecs(secs));
        else
            it.value() = shifted(it.value(), secs);
    }
    return o;
}

}  // namespace

TEST(McpCiReport, Ants6399FlakyIsOneWorkflowOnOneSha) {
    const CiReport::Analysis a =
        CiReport::analyse(fixture("flaky_attempts.json"), CiWorkflows::Result(), kNow, 30);
    const auto flaky = ofCheck(a, "flaky");
    ASSERT_EQ(flaky.size(), 1) << dump(a.findings)
        << "only aaa1 is a flake: bbb2 spans two workflows, ccc3 never passed, ddd4 never failed";
    EXPECT_EQ(flaky[0].value("runs").toArray(), QJsonArray{101});
    EXPECT_EQ(flaky[0].value("shas").toArray(), QJsonArray{QStringLiteral("aaa1")});
    EXPECT_EQ(flaky[0].value("job").toString(), QStringLiteral("Build and test"));
    EXPECT_EQ(flaky[0].value("step").toString(), QStringLiteral("Test"));
    EXPECT_EQ(flaky[0].value("severity").toString(), QStringLiteral("problem"));
    EXPECT_TRUE(flaky[0].value("message").toString().contains(QLatin1String("test_sockets")))
        << "the failed job's log names its failing ctest";
    EXPECT_EQ(a.logJobs, QList<qint64>{1011});
}

TEST(McpCiReport, Ants6399NearBudgetUsesTheJobSpan) {
    const CiWorkflows::Result wf = workflow(kBuild10);
    auto run = [&](int secs, const QString &conclusion) {
        History h;
        // Starts 3 minutes after its run is created: a run-span ratio reads
        // 79% as over 100%.
        h.add(1, kNow.addSecs(-3600), QStringLiteral("Build"), 180, secs, conclusion);
        return ofCheck(CiReport::analyse(h.json(), wf, kNow, 30), "near_budget");
    };
    EXPECT_TRUE(run(474, QStringLiteral("success")).isEmpty()) << "79% of 10 minutes";
    const auto at80 = run(480, QStringLiteral("success"));
    ASSERT_EQ(at80.size(), 1) << "80% of 10 minutes";
    EXPECT_EQ(at80[0].value("job").toString(), QStringLiteral("Build"));
    EXPECT_TRUE(at80[0].value("step").isNull());
    EXPECT_EQ(at80[0].value("cache").toString(), QStringLiteral("unknown"));
    EXPECT_EQ(run(600, QStringLiteral("cancelled")).size(), 1) << "cancelled at its budget";
}

TEST(McpCiReport, Ants6399UnreadBudgetIsNeverGuessed) {
    const CiWorkflows::Result wf = workflow(QStringLiteral(
        "name: CI\n"
        "jobs:\n"
        "  build:\n"
        "    name: Build\n"
        "    runs-on: ubuntu-24.04\n"
        "    timeout-minutes: ${{ matrix.t }}\n"
        "    steps:\n"
        "      - run: make\n"));
    History h;
    h.add(1, kNow.addSecs(-3600), QStringLiteral("Build"), 0, 594);
    // 99% of GitHub's 360-minute default: only a guessed default would fire.
    h.add(2, kNow.addSecs(-86400), QStringLiteral("Build"), 0, 356 * 60);
    const CiReport::Analysis a = CiReport::analyse(h.json(), wf, kNow, 30);
    EXPECT_TRUE(ofCheck(a, "near_budget").isEmpty()) << dump(a.findings);
    ASSERT_EQ(wf.unread.size(), 1) << dump(wf.unread);
    const QJsonObject u = wf.unread.at(0).toObject();
    EXPECT_EQ(u.value("file").toString(), QStringLiteral(".github/workflows/ci.yml"));
    EXPECT_EQ(u.value("job").toString(), QStringLiteral("build"));
    EXPECT_EQ(u.value("field").toString(), QStringLiteral("timeout-minutes"));
}

TEST(McpCiReport, Ants6399ShellGuardIsAStepBudget) {
    const CiWorkflows::Result wf = workflow(QStringLiteral(
        "name: CI\n"
        "jobs:\n"
        "  build:\n"
        "    name: Build\n"
        "    runs-on: ubuntu-24.04\n"
        "    timeout-minutes: 60\n"
        "    steps:\n"
        "      - name: Guarded\n"
        "        run: timeout 10m ctest\n"
        "      - name: Chained\n"
        "        run: cd build && timeout 10m ctest\n"));
    EXPECT_DOUBLE_EQ(CiWorkflows::shellGuardMinutes(QStringLiteral("timeout 90s x")), 1.5);
    EXPECT_DOUBLE_EQ(CiWorkflows::shellGuardMinutes(QStringLiteral("timeout 2h x")), 120);
    auto steps = [&](const QString &step, int secs, const QString &conclusion) {
        History h;
        h.add(1, kNow.addSecs(-7200), QStringLiteral("Build"), 0, secs + 10,
              QStringLiteral("success"), {{step, secs, conclusion}});
        return ofCheck(CiReport::analyse(h.json(), wf, kNow, 30), "near_budget");
    };
    const auto ran8 = steps(QStringLiteral("Guarded"), 480, QStringLiteral("success"));
    ASSERT_EQ(ran8.size(), 1) << "8 of the guard's 10 minutes";
    EXPECT_EQ(ran8[0].value("step").toString(), QStringLiteral("Guarded"));
    EXPECT_EQ(steps(QStringLiteral("Guarded"), 600, QStringLiteral("failure")).size(), 1)
        << "a guard that ran out fails the step rather than cancelling it";
    EXPECT_TRUE(steps(QStringLiteral("Chained"), 540, QStringLiteral("success")).isEmpty())
        << "a guard after && is not read";
}

TEST(McpCiReport, Ants6399EveryFindingNamesRunsAndAdvisesNoRetry) {
    const CiWorkflows::Result wf = workflow(QStringLiteral(
        "name: CI\n"
        "jobs:\n"
        "  build:\n"
        "    name: Build\n"
        "    runs-on: ubuntu-latest\n"
        "    timeout-minutes: 60\n"
        "    steps:\n"
        "      - uses: actions/cache@v4\n"
        "        with:\n"
        "          key: ${{ runner.os }}-ccache-${{ github.sha }}\n"
        "      - name: Test\n"
        "        run: ctest --timeout 300\n"
        "  slow:\n"
        "    name: Slow\n"
        "    runs-on: ubuntu-24.04\n"
        "    timeout-minutes: 10\n"
        "    steps:\n"
        "      - uses: actions/cache/save@v4\n"
        "        with:\n"
        "          key: ${{ runner.os }}-ccache-${{ github.sha }}\n"));
    History h;
    // Build: 12 green runs, the newest 5 a third slower, each queued 6 minutes.
    for (int i = 0; i < 12; ++i)
        h.add(100 + i, kNow.addSecs(-86400 + i * 3600), QStringLiteral("Build"), 360,
              i >= 7 ? 1200 : 900);
    h.add(200, kNow.addSecs(-1800), QStringLiteral("Slow"), 0, 590);   // near its budget
    h.add(300, kNow.addSecs(-900), QStringLiteral("Nightly"), 0, 60, QStringLiteral("failure"),
          {}, QStringLiteral("schedule"));
    QJsonObject json = h.json();
    json["annotations"] = QJsonObject{{QStringLiteral("3000"), QJsonArray{QJsonObject{
        {"annotation_level", "warning"},
        {"message", "The ubuntu-20.04 image will be removed; please push a retry."}}}}};
    json["cache_usage"] = QJsonObject{{"active_caches_size_in_bytes", 9.5e9}};
    const CiReport::Analysis a = CiReport::analyse(json, wf, kNow, 30);

    QSet<QString> checks;
    for (const QJsonValue &v : a.findings) checks.insert(v.toObject().value("check").toString());
    for (const char *want : {"near_budget", "budget_trend", "scheduled_red", "queue_wait", "annotation",
                             "runner_latest", "cache_size", "cache_key_shared", "cache_save_skipped",
                             "test_parallelism"})
        EXPECT_TRUE(checks.contains(QLatin1String(want))) << want << dump(a.findings);

    const QSet<QString> fromHistory{"flaky", "near_budget", "budget_trend", "scheduled_red",
                                    "queue_wait", "annotation"};
    static const QRegularExpression advice(QStringLiteral(R"((?i)\b(re-?run|retry|push|lower|reduce)\b)"));
    static const QRegularExpression quoted(QStringLiteral(R"re("[^"]*"|'[^']*')re"));
    QList<QJsonObject> all;
    for (const QJsonValue &v : a.findings) all.append(v.toObject());
    const CiReport::Analysis flaky =
        CiReport::analyse(fixture("flaky_attempts.json"), CiWorkflows::Result(), kNow, 30);
    for (const QJsonValue &v : flaky.findings) all.append(v.toObject());
    for (const QJsonObject &f : all) {
        const QString check = f.value("check").toString();
        if (fromHistory.contains(check)) {
            EXPECT_FALSE(f.value("runs").toArray().isEmpty()) << dump(f);
            EXPECT_FALSE(f.value("shas").toArray().isEmpty()) << dump(f);
        }
        EXPECT_LE(f.value("runs").toArray().size(), 10) << dump(f);
        EXPECT_LE(f.value("shas").toArray().size(), 10) << dump(f);
        // Names and quoted text come from the data; mask them.
        QString msg = f.value("message").toString();
        for (const char *key : {"workflow", "job", "step"})
            if (const QString s = f.value(QLatin1String(key)).toString(); !s.isEmpty())
                msg.replace(s, QStringLiteral("X"));
        msg.replace(quoted, QStringLiteral("X"));
        EXPECT_FALSE(advice.match(msg).hasMatch()) << check.toStdString() << ": " << msg.toStdString();
    }
}

TEST(McpCiReport, Ants6399CappedFetchStillReports) {
    Env env;
    FakeGh gh;
    gh.install(env);
    QJsonArray runs;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < 20; ++i)
        runs.append(QJsonObject{{"databaseId", 500 + i}, {"headSha", QStringLiteral("s%1").arg(i)},
                                {"conclusion", i == 0 ? "failure" : "success"}, {"event", "schedule"},
                                {"workflowName", "Nightly"}, {"createdAt", iso(now.addSecs(-60 - i * 3600))},
                                {"attempt", 1}, {"status", "completed"}});
    gh.put(QStringLiteral("runs.json"), QJsonDocument(runs).toJson());
    Project p;
    CiHistory::Options o;
    o.now = now;
    o.callCap = 6;
    o.reserveCalls = 2;
    const CiHistory::Result r = CiHistory::fetch(p.root, o, {});
    EXPECT_TRUE(r.refusal.isEmpty()) << r.refusal.toStdString();
    EXPECT_EQ(r.truncatedReason, QStringLiteral("call_cap"));
    EXPECT_EQ(r.history.value("runs").toArray().size(), 20);
    EXPECT_LT(r.history.value("jobs").toObject().size(), 20) << "the cap stopped the jobs calls";
    const CiReport::Analysis a = CiReport::analyse(r.history, CiWorkflows::Result(), now, 30);
    EXPECT_EQ(ofCheck(a, "scheduled_red").size(), 1) << dump(a.findings);
}

TEST(McpCiReport, Ants6399FailedFetchSavesNothing) {
    Env env;
    Project p;
    env.set("XDG_CACHE_HOME", p.cache.path().toUtf8());
    {
        FakeGh gh;
        gh.install(env);
        env.set("FAKE_FAIL", "list");
        const QJsonObject r = p.call();
        EXPECT_EQ(r.value("code").toString(), QStringLiteral("github_unavailable")) << dump(r);
        EXPECT_TRUE(r.value("stderr_tail").toString().contains(QLatin1String("Bad credentials"))) << dump(r);
        EXPECT_FALSE(QFileInfo::exists(p.savedPath()));
        env.set("FAKE_FAIL", "");
    }
    QTemporaryDir empty;
    env.set("PATH", empty.path().toUtf8());
    const QJsonObject r = p.call();
    EXPECT_EQ(r.value("code").toString(), QStringLiteral("runner_missing")) << dump(r);
    EXPECT_FALSE(QFileInfo::exists(p.savedPath()));
}

TEST(McpCiReport, Ants6399NoWorkflowsStartsNoProcess) {
    Env env;
    FakeGh gh;
    gh.install(env);
    Project p(/*withWorkflow=*/false);
    env.set("XDG_CACHE_HOME", p.cache.path().toUtf8());
    const QJsonObject r = p.call();
    EXPECT_TRUE(r.value("ok").toBool()) << dump(r);
    EXPECT_EQ(r.value("ci").toString(), QStringLiteral("none"));
    EXPECT_EQ(r.value("ci_none_reason").toString(), QStringLiteral("no_workflows"));
    EXPECT_FALSE(gh.called());

    Project noRemote(/*withWorkflow=*/true, /*withRemote=*/false);
    const QJsonObject r2 = noRemote.call();
    EXPECT_EQ(r2.value("ci_none_reason").toString(), QStringLiteral("no_github_remote")) << dump(r2);
    EXPECT_FALSE(gh.called());
}

TEST(McpCiReport, Ants6399SavedPathsStartNoProcess) {
    Env env;
    FakeGh gh;
    gh.install(env);
    Project p;
    env.set("XDG_CACHE_HOME", p.cache.path().toUtf8());
    EXPECT_EQ(p.call({{"source", "saved"}}).value("code").toString(), QStringLiteral("not_cached"));
    QDir().mkpath(QFileInfo(p.savedPath()).path());
    QFile f(p.savedPath());
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(R"({"ok":true,"ci":"github","generated_at":"2026-01-01T00:00:00Z","counts":{"problem":1,"advice":0},)"
            R"("findings":[{"check":"flaky","workflow":"CI","job":"Build","runs":[1],"shas":["a"]}]})");
    f.close();
    EXPECT_EQ(p.call({{"source", "saved"}}).value("ci").toString(), QStringLiteral("github"));
    EXPECT_FALSE(RemoteControl::buildCiReportBlock(p.root, kNow).isEmpty());
    EXPECT_FALSE(gh.called()) << "a saved read refreshed itself";
}

TEST(McpCiReport, Ants6399OrientBlockOnlyForProblems) {
    Env env;
    Project p;
    env.set("XDG_CACHE_HOME", p.cache.path().toUtf8());
    EXPECT_TRUE(RemoteControl::buildCiReportBlock(p.root, kNow).isEmpty()) << "no saved report";
    auto save = [&](const QByteArray &body) {
        QDir().mkpath(QFileInfo(p.savedPath()).path());
        QFile f(p.savedPath());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(body);
    };
    save(R"({"generated_at":"2026-11-02T09:10:00Z","counts":{"problem":0,"advice":1},)"
         R"("findings":[{"check":"queue_wait","severity":"advice","workflow":"CI","runs":[1]}]})");
    EXPECT_TRUE(RemoteControl::buildCiReportBlock(p.root, kNow).isEmpty()) << "advice only";
    save(R"({"generated_at":"2026-11-02T09:10:00Z","counts":{"problem":1,"advice":0},)"
         R"("findings":[{"check":"flaky","severity":"problem","workflow":"CI","job":"Build","runs":[1,2]}]})");
    const QJsonObject morning = RemoteControl::buildCiReportBlock(p.root, kNow);
    EXPECT_EQ(morning.value("problems").toInt(), 1);
    EXPECT_EQ(morning.value("first").toString(), QStringLiteral("flaky: CI / Build, 2 runs"));
    EXPECT_EQ(morning.value("age_days").toInt(), 3);
    EXPECT_EQ(QJsonDocument(morning).toJson(),
              QJsonDocument(RemoteControl::buildCiReportBlock(p.root, kNow.addSecs(3600 * 11))).toJson())
        << "the block moved within one UTC day, so orient's ETag would churn";
}

TEST(McpCiReport, Ants6399LiveRunSavesTheReport) {
    Env env;
    FakeGh gh;
    gh.install(env);
    Project p;
    env.set("XDG_CACHE_HOME", p.cache.path().toUtf8());
    const QJsonObject fx = fixture("flaky_attempts.json");
    const qint64 shift = QDateTime::fromString(fx.value("_now").toString(), Qt::ISODate)
                             .secsTo(QDateTime::currentDateTimeUtc());
    gh.put(QStringLiteral("runs.json"),
           QJsonDocument(shifted(fx.value("runs"), shift).toArray()).toJson());
    const QJsonObject jobs = shifted(fx.value("jobs"), shift).toObject();
    for (auto it = jobs.cbegin(); it != jobs.cend(); ++it) {
        const QStringList ra = it.key().split(QLatin1Char('/'));
        gh.put(QStringLiteral("repos_o_r_actions_runs_%1_attempts_%2_jobs_per_page_100").arg(ra[0], ra[1]),
               QJsonDocument(it.value().toObject()).toJson());
    }
    gh.put(QStringLiteral("repos_o_r_actions_jobs_1011_logs"),
           fx.value("logs").toObject().value("1011").toString().toUtf8());

    const QJsonObject r = p.call();
    ASSERT_TRUE(r.value("ok").toBool()) << dump(r);
    EXPECT_EQ(r.value("ci").toString(), QStringLiteral("github"));
    EXPECT_EQ(r.value("repo").toString(), QStringLiteral("o/r"));
    EXPECT_FALSE(r.value("truncated").toBool()) << dump(r);
    EXPECT_TRUE(r.value("fetch_errors").toArray().isEmpty()) << dump(r);
    ASSERT_EQ(r.value("counts").toObject().value("problem").toInt(), 1) << dump(r);
    EXPECT_TRUE(r.value("findings").toArray().at(0).toObject().value("message").toString()
                    .contains(QLatin1String("test_sockets"))) << dump(r);
    ASSERT_TRUE(QFileInfo::exists(p.savedPath()));
    EXPECT_EQ(QFile(p.savedPath()).permissions().toInt() & 0x0077, 0) << "saved report is owner-only";
    const QJsonObject block =
        RemoteControl::buildCiReportBlock(p.root, QDateTime::currentDateTimeUtc());
    EXPECT_EQ(block.value("problems").toInt(), 1) << dump(block);
    EXPECT_EQ(block.value("age_days").toInt(), 0);

    // A filtered run reports but leaves the saved report alone.
    QFile::remove(p.savedPath());
    EXPECT_TRUE(p.call({{"workflow", "CI"}}).value("ok").toBool());
    EXPECT_FALSE(QFileInfo::exists(p.savedPath()));
    EXPECT_EQ(p.call({{"days", 0}}).value("code").toString(), QStringLiteral("bad_args"));
}

TEST(McpCiReport, Ants6399ServedByMcpdWithoutMainWindow) {
    const std::string reg = ants_test::slurpFile(ANTS_MCP_REGISTRY_SOURCE);
    const std::size_t scoped = reg.find("void registerProjectScopedVerbs(");
    ASSERT_NE(scoped, std::string::npos);
    const std::size_t end = reg.find("\n}\n", scoped);
    const std::size_t at = reg.find("registerToolProvider(\"ci_report\"", scoped);
    EXPECT_TRUE(at != std::string::npos && at < end) << "ci_report is registered outside registerProjectScopedVerbs";

    const std::string handler = ants_test::slurpFile(ANTS_SOURCE_DIR "/src/remotecontrol_ci.cpp");
    const std::size_t body = handler.find("QJsonDocument RemoteControl::cmdCiReport(");
    ASSERT_NE(body, std::string::npos);
    const std::string verb = handler.substr(body, handler.find("\n}\n", body) - body);
    EXPECT_EQ(verb.find("m_main"), std::string::npos) << "cmdCiReport reaches MainWindow";

    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    EXPECT_NE(ci.find("t[\"name\"] = \"ci_report\""), std::string::npos);
    EXPECT_NE(ci.find("toolName == QStringLiteral(\"ci_report\")) return C::Required"), std::string::npos);
    EXPECT_NE(ci.find("toolName == QStringLiteral(\"ci_report\"))                return R::Expensive"),
              std::string::npos);
}
