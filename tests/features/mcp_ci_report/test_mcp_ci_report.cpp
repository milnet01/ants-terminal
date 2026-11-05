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
// named by the path with / ? = & turned to _. FAKE_FAIL=list fails the listing;
// a file of that name plus ".fail" fails that api call, and plus ".sleep"
// makes it outlast a short per-call limit.
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
                 "elif [[ $1 == api && -f \"$FAKE_DIR/${2//[\\/?=&]/_}.sleep\" ]]; then\n"
                 "  sleep 5\n"
                 "elif [[ $1 == api && -f \"$FAKE_DIR/${2//[\\/?=&]/_}.fail\" ]]; then\n"
                 "  echo 'HTTP 500: Server Error' >&2; exit 1\n"
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
    for (auto it = jobs.constBegin(); it != jobs.constEnd(); ++it) {
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

    const std::string ci = ants_test::slurpClaudeIntegration();
    EXPECT_NE(ci.find("t[\"name\"] = \"ci_report\""), std::string::npos);
    EXPECT_NE(ci.find("toolName == QStringLiteral(\"ci_report\")) return C::Required"), std::string::npos);
    EXPECT_NE(ci.find("toolName == QStringLiteral(\"ci_report\"))                return R::Expensive"),
              std::string::npos);
}

// ---- ANTS-6402: claude-d5's cold review of 8b1cb9c4 ----------------------

namespace {

QJsonObject runRow(qint64 id, const char *sha, const char *event, const char *conclusion,
                   const QDateTime &created, int attempt = 1) {
    return {{"databaseId", double(id)}, {"headSha", sha}, {"event", event},
            {"conclusion", conclusion}, {"workflowName", "CI"}, {"createdAt", iso(created)},
            {"attempt", attempt}, {"status", "completed"}};
}

QJsonObject jobRow(qint64 id, const char *name, const char *conclusion, const QDateTime &at) {
    return {{"id", double(id)}, {"name", name}, {"conclusion", conclusion},
            {"created_at", iso(at)}, {"started_at", iso(at)},
            {"completed_at", iso(at.addSecs(60))}, {"steps", QJsonArray()}};
}

}  // namespace

TEST(McpCiReport, Ants6402FlakyNeedsTheSameJobToPass) {
    const QDateTime t = kNow.addSecs(-86400);
    QJsonObject jobs;
    // s1: a PR run fails ASan; the push run on that SHA skips ASan.
    jobs["1/1"] = QJsonObject{{"jobs", QJsonArray{jobRow(11, "ASan", "failure", t),
                                                  jobRow(12, "Build", "success", t)}}};
    jobs["2/1"] = QJsonObject{{"jobs", QJsonArray{jobRow(21, "ASan", "skipped", t.addSecs(600)),
                                                  jobRow(22, "Build", "success", t.addSecs(600))}}};
    // s2: two push runs; ASan fails, then is skipped. It never passed.
    jobs["3/1"] = QJsonObject{{"jobs", QJsonArray{jobRow(31, "ASan", "failure", t.addSecs(1200))}}};
    jobs["4/1"] = QJsonObject{{"jobs", QJsonArray{jobRow(41, "ASan", "skipped", t.addSecs(1800)),
                                                  jobRow(42, "Build", "success", t.addSecs(1800))}}};
    // s3: Build fails, then the same job passes on the same SHA: a flake.
    jobs["5/1"] = QJsonObject{{"jobs", QJsonArray{jobRow(51, "Build", "failure", t.addSecs(2400))}}};
    jobs["6/1"] = QJsonObject{{"jobs", QJsonArray{jobRow(61, "Build", "success", t.addSecs(3000))}}};
    const QJsonObject h{
        {"runs", QJsonArray{runRow(1, "s1", "pull_request", "failure", t),
                            runRow(2, "s1", "push", "success", t.addSecs(600)),
                            runRow(3, "s2", "push", "failure", t.addSecs(1200)),
                            runRow(4, "s2", "push", "success", t.addSecs(1800)),
                            runRow(5, "s3", "push", "failure", t.addSecs(2400)),
                            runRow(6, "s3", "push", "success", t.addSecs(3000))}},
        {"jobs", jobs}};
    const CiReport::Analysis a = CiReport::analyse(h, CiWorkflows::Result(), kNow, 30);
    const auto flaky = ofCheck(a, "flaky");
    ASSERT_EQ(flaky.size(), 1) << dump(a.findings) << "only s3's Build failed and then passed";
    EXPECT_EQ(flaky[0].value("shas").toArray(), QJsonArray{QStringLiteral("s3")});
    EXPECT_EQ(flaky[0].value("job").toString(), QStringLiteral("Build"));
    EXPECT_TRUE(flaky[0].value("message").toString().contains(QLatin1String("runner")))
        << "a lost runner fails a job too; the message must not blame only the test";
}

TEST(McpCiReport, Ants6402MatrixNameMatchesOneJobOnly) {
    const CiWorkflows::Result wf = workflow(QStringLiteral(
        "name: CI\n"
        "jobs:\n"
        "  a:\n"
        "    name: Build\n"
        "    timeout-minutes: 10\n"
        "  b:\n"
        "    name: Build (${{ matrix.san }})\n"
        "    timeout-minutes: 60\n"));
    History h;
    h.add(1, kNow.addSecs(-3600), QStringLiteral("Build (asan)"), 0, 30 * 60);
    const CiReport::Analysis a = CiReport::analyse(h.json(), wf, kNow, 30);
    EXPECT_TRUE(ofCheck(a, "near_budget").isEmpty())
        << "\"Build (asan)\" is job b's matrix leg, not job a" << dump(a.findings);
}

TEST(McpCiReport, Ants6402SubmoduleUrlIsNoGithubRemote) {
    Env env;
    FakeGh gh;
    gh.install(env);
    Project p(/*withWorkflow=*/true, /*withRemote=*/false);
    env.set("XDG_CACHE_HOME", p.cache.path().toUtf8());
    p.write(QStringLiteral(".git/config"),
            "[remote \"origin\"]\n\turl = https://gitlab.com/o/r.git\n"
            "[submodule \"lib\"]\n\turl = https://github.com/a/b.git\n");
    const QJsonObject r = p.call();
    EXPECT_EQ(r.value("ci_none_reason").toString(), QStringLiteral("no_github_remote")) << dump(r);
    EXPECT_FALSE(gh.called());
}

TEST(McpCiReport, Ants6402TestParallelismReadsTheCtestCommand) {
    auto fires = [](const char *run) {
        const CiWorkflows::Result wf = workflow(QStringLiteral(
            "name: CI\njobs:\n  t:\n    steps:\n      - name: T\n        run: %1\n").arg(QLatin1String(run)));
        return !ofCheck(CiReport::analyse(QJsonObject(), wf, kNow, 30), "test_parallelism").isEmpty();
    };
    EXPECT_FALSE(fires("CTEST_PARALLEL_LEVEL=4 ctest --timeout 60")) << "inline level";
    EXPECT_TRUE(fires("cmake --build b --parallel && ctest --timeout 60")) << "cmake's --parallel is not ctest's";
    EXPECT_FALSE(fires("./run-ctest.sh --timeout 60")) << "ctest inside another word";
    EXPECT_FALSE(fires("ctest -j4 --timeout 60"));
    EXPECT_TRUE(fires("ctest --timeout 60"));
    const CiWorkflows::Result flowEnv = workflow(QStringLiteral(
        "name: CI\njobs:\n  t:\n    env: {CTEST_PARALLEL_LEVEL: 4}\n    steps:\n"
        "      - run: ctest --timeout 60\n"));
    EXPECT_TRUE(ofCheck(CiReport::analyse(QJsonObject(), flowEnv, kNow, 30), "test_parallelism").isEmpty())
        << "a flow-map env sets the level too";
}

TEST(McpCiReport, Ants6402SaveAfterFailureIsNotSkipped) {
    for (const char *cond : {"${{ !cancelled() }}", "success() || failure()", "always()"}) {
        const CiWorkflows::Result wf = workflow(QStringLiteral(
            "name: CI\njobs:\n  t:\n    steps:\n      - uses: actions/cache/save@v4\n"
            "        if: %1\n        with:\n          key: k-${{ github.sha }}\n").arg(QLatin1String(cond)));
        EXPECT_TRUE(ofCheck(CiReport::analyse(QJsonObject(), wf, kNow, 30), "cache_save_skipped").isEmpty())
            << cond;
    }
}

TEST(McpCiReport, Ants6402BlockRunIsNoShellGuard) {
    const CiWorkflows::Result wf = workflow(QStringLiteral(
        "name: CI\njobs:\n  build:\n    name: Build\n    timeout-minutes: 60\n    steps:\n"
        "      - name: Guarded\n        run: |\n          timeout 10m ctest\n"));
    ASSERT_EQ(wf.workflows.size(), 1);
    EXPECT_LT(wf.workflows[0].jobs[0].steps[0].budgetMin, 0) << "a guard in a | block is not read";
}

TEST(McpCiReport, Ants6402JobsArePaginated) {
    Env env;
    FakeGh gh;
    gh.install(env);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    gh.put(QStringLiteral("runs.json"),
           QJsonDocument(QJsonArray{runRow(7, "s", "push", "failure", now.addSecs(-600))}).toJson());
    QJsonArray first;
    for (int i = 0; i < 100; ++i) first.append(jobRow(700 + i, "Leg", "success", now.addSecs(-500)));
    gh.put(QStringLiteral("repos_o_r_actions_runs_7_attempts_1_jobs_per_page_100"),
           QJsonDocument(QJsonObject{{"total_count", 101}, {"jobs", first}}).toJson());
    gh.put(QStringLiteral("repos_o_r_actions_runs_7_attempts_1_jobs_per_page_100_page_2"),
           QJsonDocument(QJsonObject{{"total_count", 101},
                                     {"jobs", QJsonArray{jobRow(900, "Last", "failure", now.addSecs(-500))}}})
               .toJson());
    Project p;
    CiHistory::Options o;
    o.now = now;
    const CiHistory::Result r = CiHistory::fetch(p.root, o, {});
    EXPECT_EQ(r.history.value("jobs").toObject().value("7/1").toObject().value("jobs").toArray().size(), 101)
        << "the 101st job was dropped";
}

TEST(McpCiReport, Ants6402ReaderEdgeCases) {
    // A tab inside a block scalar is text, not indentation.
    const CiWorkflows::Result tabbed = workflow(QStringLiteral(
        "name: CI\njobs:\n  a:\n    timeout-minutes: 5\n    steps:\n      - run: |\n"
        "          cat <<EOF\n\tindented\n          EOF\n"));
    EXPECT_EQ(tabbed.workflows.size(), 1) << dump(tabbed.unread);
    // A merge key loses keys silently, so the file is unread instead.
    const CiWorkflows::Result merged = workflow(QStringLiteral(
        "name: CI\njobs:\n  a: &base\n    timeout-minutes: 5\n  b:\n    <<: *base\n"));
    EXPECT_TRUE(merged.workflows.isEmpty());
    ASSERT_EQ(merged.unread.size(), 1);
    EXPECT_EQ(merged.unread.at(0).toObject().value("field").toString(), QStringLiteral("file"));
    // An escaped quote does not end a double-quoted value.
    const CiWorkflows::Result quoted = workflow(QStringLiteral(
        "name: \"say \\\"hi\\\" now\"\njobs:\n  a:\n    timeout-minutes: 5\n"));
    ASSERT_EQ(quoted.workflows.size(), 1);
    EXPECT_EQ(quoted.workflows[0].name, QStringLiteral("say \"hi\" now"));
    // A flow map is read.
    const CiWorkflows::Result flow = workflow(QStringLiteral(
        "name: CI\njobs:\n  a:\n    steps:\n"
        "      - uses: actions/cache@v4\n        with: {path: x, key: \"${{ runner.os }}-cc-${{ github.sha }}\"}\n"));
    ASSERT_EQ(flow.workflows.size(), 1) << dump(flow.unread);
    EXPECT_EQ(flow.workflows[0].jobs[0].steps[0].cacheKey,
              QStringLiteral("${{ runner.os }}-cc-${{ github.sha }}"));
    // A block-map runs-on names its labels.
    const CiWorkflows::Result group = workflow(QStringLiteral(
        "name: CI\njobs:\n  a:\n    runs-on:\n      group: big\n      labels: ubuntu-latest\n"));
    EXPECT_EQ(ofCheck(CiReport::analyse(QJsonObject(), group, kNow, 30), "runner_latest").size(), 1);
}

TEST(McpCiReport, Ants6402LogKeepsHeadAndTail) {
    Env env;
    FakeGh gh;
    gh.install(env);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    gh.put(QStringLiteral("runs.json"),
           QJsonDocument(QJsonArray{runRow(8, "s", "push", "failure", now.addSecs(-600))}).toJson());
    gh.put(QStringLiteral("repos_o_r_actions_runs_8_attempts_1_jobs_per_page_100"),
           QJsonDocument(QJsonObject{{"total_count", 1},
                                     {"jobs", QJsonArray{jobRow(80, "Build", "failure", now.addSecs(-500))}}})
               .toJson());
    gh.put(QStringLiteral("repos_o_r_actions_jobs_80_logs"),
           "Cache restored from key: k\n" + QByteArray(20000, 'x') + "\nThe following tests FAILED:\n");
    Project p;
    CiHistory::Options o;
    o.now = now;
    o.maxLogBytes = 4096;
    const CiHistory::Result r = CiHistory::fetch(p.root, o, [](const QJsonObject &) {
        return QList<qint64>{80};
    });
    const QString log = r.history.value("logs").toObject().value("80").toString();
    EXPECT_TRUE(log.contains(QLatin1String("Cache restored from key"))) << "the head holds the cache line";
    EXPECT_TRUE(log.contains(QLatin1String("The following tests FAILED:"))) << "the tail holds ctest's list";
    EXPECT_LE(log.size(), 4096 + 64);
}

TEST(McpCiReport, Ants6402UnreadNamesAMissingWorkflowApart) {
    History h;
    h.add(1, kNow.addSecs(-3600), QStringLiteral("Build"), 0, 60);
    const CiReport::Analysis a = CiReport::analyse(h.json(), CiWorkflows::Result(), kNow, 30);
    ASSERT_EQ(a.unread.size(), 1) << dump(a.unread);
    const QJsonObject u = a.unread.at(0).toObject();
    EXPECT_TRUE(u.value("file").isNull());
    EXPECT_EQ(u.value("field").toString(), QStringLiteral("workflow"));
    EXPECT_EQ(u.value("workflow").toString(), QStringLiteral("CI"));
}

// ---- ANTS-6405: claude-d5's re-review of 85c4e720 ------------------------

TEST(McpCiReport, Ants6405CtestCommandForms) {
    auto fires = [](const char *run) {
        const CiWorkflows::Result wf = workflow(QStringLiteral(
            "name: CI\njobs:\n  t:\n    steps:\n      - name: T\n        run: |\n          %1\n")
            .arg(QString::fromLatin1(run).replace(QLatin1Char('\n'), QStringLiteral("\n          "))));
        return !ofCheck(CiReport::analyse(QJsonObject(), wf, kNow, 30), "test_parallelism").isEmpty();
    };
    EXPECT_FALSE(fires("ctest --timeout 300 \\\n  -j4")) << "B1: a continued line is one command";
    EXPECT_TRUE(fires("xvfb-run -a ctest --timeout 60")) << "B2";
    EXPECT_TRUE(fires("sudo ctest --timeout 60")) << "B2";
    EXPECT_TRUE(fires("if true; then ctest --timeout 60; fi")) << "B2";
    EXPECT_TRUE(fires("(cd build && ctest --timeout 60)")) << "B2";
    EXPECT_TRUE(fires("timeout -k 10 8m ctest --timeout 60")) << "B2";
    EXPECT_FALSE(fires("xvfb-run -a ctest -j4 --timeout 60"));
}

// ---- ANTS-6403: a fork whose gh has no default repository -------------------

// With two GitHub remotes and no `gh repo set-default`, gh reads `upstream`
// (measured 2026-11-05, gh 2.103.3), so the reply says which repo it read
// and how to pick the other. A set default, or one remote, says nothing.
TEST(McpCiReport, Ants6403AmbiguousRemotesNameTheRepoRead) {
    auto note = [](const QByteArray &config) {
        Env env;
        FakeGh gh;
        gh.install(env);
        Project p(/*withWorkflow=*/true, /*withRemote=*/false);
        env.set("XDG_CACHE_HOME", p.cache.path().toUtf8());
        p.write(QStringLiteral(".git/config"), config);
        const QJsonObject r = p.call();
        EXPECT_TRUE(r.value("ok").toBool()) << dump(r);
        return r.value("repo_note");
    };
    const QByteArray two = "[remote \"origin\"]\n\turl = https://github.com/me/r.git\n"
                           "[remote \"upstream\"]\n\turl = https://github.com/o/r.git\n";
    const QJsonValue ambiguous = note(two);
    ASSERT_TRUE(ambiguous.isString()) << "two GitHub remotes, no default";
    EXPECT_TRUE(ambiguous.toString().contains(QLatin1String("o/r")));
    EXPECT_TRUE(ambiguous.toString().contains(QLatin1String("gh repo set-default")));

    const QByteArray resolved = "[remote \"origin\"]\n\turl = https://github.com/me/r.git\n"
                                "\tgh-resolved = base\n"
                                "[remote \"upstream\"]\n\turl = https://github.com/o/r.git\n";
    EXPECT_TRUE(note(resolved).isUndefined()) << "gh repo set-default chose";
    EXPECT_TRUE(note("[remote \"origin\"]\n\turl = https://github.com/o/r.git\n"
                     "[remote \"mirror\"]\n\turl = https://gitlab.com/o/r.git\n").isUndefined())
        << "one GitHub remote";
}

// ---- ANTS-6406: a wrapper option that takes a value ------------------------

TEST(McpCiReport, Ants6406WrapperOptionValues) {
    auto fires = [](const char *run) {
        const CiWorkflows::Result wf = workflow(QStringLiteral(
            "name: CI\njobs:\n  t:\n    steps:\n      - name: T\n        run: |\n          %1\n")
            .arg(QString::fromLatin1(run)));
        return !ofCheck(CiReport::analyse(QJsonObject(), wf, kNow, 30), "test_parallelism").isEmpty();
    };
    EXPECT_TRUE(fires("sudo -u builder ctest --timeout 60")) << "sudo -u";
    EXPECT_TRUE(fires("nice -n 10 ctest --timeout 60")) << "nice -n";
    EXPECT_TRUE(fires("env -u HOME ctest --timeout 60")) << "env -u";
    EXPECT_TRUE(fires("xvfb-run -s \"-screen 0 1024x768x24\" ctest --timeout 60")) << "quoted -s";
    EXPECT_TRUE(fires("xvfb-run --server-args='-screen 0 640x480x8' ctest --timeout 60"))
        << "quoted long form";
    EXPECT_FALSE(fires("sudo -u builder ctest -j4 --timeout 60"));
    EXPECT_FALSE(fires("env echo ctest --timeout 60")) << "ctest as an argument is not ctest";
}

TEST(McpCiReport, Ants6405IncompletePagesAreAFetchError) {
    Env env;
    FakeGh gh;
    gh.install(env);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    gh.put(QStringLiteral("runs.json"),
           QJsonDocument(QJsonArray{runRow(9, "s", "push", "failure", now.addSecs(-600))}).toJson());
    QJsonArray first;
    for (int i = 0; i < 100; ++i) first.append(jobRow(900 + i, "Leg", "success", now.addSecs(-500)));
    gh.put(QStringLiteral("repos_o_r_actions_runs_9_attempts_1_jobs_per_page_100"),
           QJsonDocument(QJsonObject{{"total_count", 101}, {"jobs", first}}).toJson());
    // Page 2 answers with no jobs: the fake's default '{}'.
    Project p;
    CiHistory::Options o;
    o.now = now;
    const CiHistory::Result r = CiHistory::fetch(p.root, o, {});
    EXPECT_FALSE(r.history.value("jobs").toObject().contains("9/1"));
    EXPECT_FALSE(r.fetchErrors.isEmpty()) << "a dropped attempt must stop the report saving as complete";
}

TEST(McpCiReport, Ants6405NotFailureIsNotAfterFailure) {
    const CiWorkflows::Result wf = workflow(QStringLiteral(
        "name: CI\njobs:\n  t:\n    steps:\n      - uses: actions/cache/save@v4\n"
        "        if: ${{ !failure() && github.ref == 'refs/heads/main' }}\n"
        "        with:\n          key: k-${{ github.sha }}\n"));
    EXPECT_EQ(ofCheck(CiReport::analyse(QJsonObject(), wf, kNow, 30), "cache_save_skipped").size(), 1);
}

TEST(McpCiReport, Ants6405FlowMapEdges) {
    const CiWorkflows::Result commented = workflow(QStringLiteral(
        "name: CI\njobs:\n  t:\n    env: {CTEST_PARALLEL_LEVEL: 4}  # parallel\n    steps:\n"
        "      - run: ctest --timeout 60\n"));
    ASSERT_EQ(commented.workflows.size(), 1);
    EXPECT_EQ(commented.workflows[0].jobs[0].envKeys, QStringList{QStringLiteral("CTEST_PARALLEL_LEVEL")})
        << "B6: a comment after a flow map";
    const CiWorkflows::Result escaped = workflow(QStringLiteral(
        "name: CI\njobs:\n  t:\n    steps:\n      - uses: actions/cache@v4\n"
        "        with: {key: \"a\\\"b, c\", path: x}\n"));
    ASSERT_EQ(escaped.workflows.size(), 1);
    EXPECT_EQ(escaped.workflows[0].jobs[0].steps[0].cacheKey, QStringLiteral("a\"b, c")) << "B7";
}

TEST(McpCiReport, Ants6405MatrixPrefixAndBlockTab) {
    // Two literal names both fit "Build (x)": no match, so no budget is guessed.
    const CiWorkflows::Result two = workflow(QStringLiteral(
        "name: CI\njobs:\n  a:\n    name: Build\n    timeout-minutes: 10\n"
        "  b:\n    name: Build (x)\n    timeout-minutes: 10\n"));
    ASSERT_EQ(two.workflows.size(), 1) << dump(two.unread);
    History h;
    h.add(1, kNow.addSecs(-3600), QStringLiteral("Build (x) (y)"), 0, 540);
    EXPECT_TRUE(ofCheck(CiReport::analyse(h.json(), two, kNow, 30), "near_budget").isEmpty());
    // Spaces then a tab inside a | block is text.
    const CiWorkflows::Result tab = workflow(QStringLiteral(
        "name: CI\njobs:\n  a:\n    steps:\n      - run: |\n          make\n          \tindented\n"));
    EXPECT_EQ(tab.workflows.size(), 1) << dump(tab.unread);
}

// ---- ANTS-6407: claude-config's cold review of 2026-11-05 ------------------

// A page call that fails is one fetch error, with gh's own exit code; it is
// not also reported as an attempt whose pages ran out.
TEST(McpCiReport, Ants6407FailedPageIsOneFetchError) {
    Env env;
    FakeGh gh;
    gh.install(env);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    gh.put(QStringLiteral("runs.json"),
           QJsonDocument(QJsonArray{runRow(9, "s", "push", "failure", now.addSecs(-600))}).toJson());
    QJsonArray first;
    for (int i = 0; i < 100; ++i) first.append(jobRow(900 + i, "Leg", "success", now.addSecs(-500)));
    gh.put(QStringLiteral("repos_o_r_actions_runs_9_attempts_1_jobs_per_page_100"),
           QJsonDocument(QJsonObject{{"total_count", 101}, {"jobs", first}}).toJson());
    gh.put(QStringLiteral("repos_o_r_actions_runs_9_attempts_1_jobs_per_page_100_page_2.fail"), "");
    Project p;
    CiHistory::Options o;
    o.now = now;
    const CiHistory::Result r = CiHistory::fetch(p.root, o, {});
    EXPECT_FALSE(r.history.value("jobs").toObject().contains("9/1"));
    ASSERT_EQ(r.fetchErrors.size(), 1) << dump(r.fetchErrors);
    EXPECT_EQ(r.fetchErrors.at(0).toObject().value("exit_code").toInt(), 1) << dump(r.fetchErrors);
}

// The annotations calls leave the logs their share of the limits (spec § 4.5).
TEST(McpCiReport, Ants6407AnnotationsLeaveRoomForLogs) {
    Env env;
    FakeGh gh;
    gh.install(env);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    gh.put(QStringLiteral("runs.json"),
           QJsonDocument(QJsonArray{runRow(7, "s", "push", "failure", now.addSecs(-600))}).toJson());
    QJsonArray jobs;
    for (int i = 0; i < 20; ++i) jobs.append(jobRow(700 + i, "Leg", "success", now.addSecs(-500)));
    gh.put(QStringLiteral("repos_o_r_actions_runs_7_attempts_1_jobs_per_page_100"),
           QJsonDocument(QJsonObject{{"total_count", 20}, {"jobs", jobs}}).toJson());
    gh.put(QStringLiteral("repos_o_r_actions_jobs_705_logs"), "log body");
    Project p;
    CiHistory::Options o;
    o.now = now;
    o.callCap = 12;
    o.reserveCalls = 8;
    o.maxLogs = 2;
    const CiHistory::Result r =
        CiHistory::fetch(p.root, o, [](const QJsonObject &) { return QList<qint64>{705}; });
    EXPECT_TRUE(r.history.value("logs").toObject().contains("705"))
        << "the annotations spent the logs' calls";
    EXPECT_EQ(r.truncatedReason, QStringLiteral("call_cap"));
}

// Annotations are read 100 to a page, so a warning past the 30th is seen.
TEST(McpCiReport, Ants6407AnnotationsAskForAFullPage) {
    Env env;
    FakeGh gh;
    gh.install(env);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    gh.put(QStringLiteral("runs.json"),
           QJsonDocument(QJsonArray{runRow(7, "s", "push", "success", now.addSecs(-600))}).toJson());
    gh.put(QStringLiteral("repos_o_r_actions_runs_7_attempts_1_jobs_per_page_100"),
           QJsonDocument(QJsonObject{{"total_count", 1},
                                     {"jobs", QJsonArray{jobRow(701, "Leg", "success", now.addSecs(-500))}}})
               .toJson());
    Project p;
    CiHistory::Options o;
    o.now = now;
    CiHistory::fetch(p.root, o, {});
    QFile log(gh.log());
    ASSERT_TRUE(log.open(QIODevice::ReadOnly));
    EXPECT_TRUE(log.readAll().contains("check-runs/701/annotations?per_page=100"));
}

// A failed log call loses a detail, not a problem finding, so the report
// still saves; a failed jobs call does not (spec § 4.5).
TEST(McpCiReport, Ants6407OnlyAJobsErrorBlocksTheSave) {
    auto saves = [](const char *failing) {
        Env env;
        FakeGh gh;
        gh.install(env);
        Project p;
        env.set("XDG_CACHE_HOME", p.cache.path().toUtf8());
        const QDateTime now = QDateTime::currentDateTimeUtc();
        gh.put(QStringLiteral("runs.json"),
               QJsonDocument(QJsonArray{runRow(7, "s", "push", "failure", now.addSecs(-600)),
                                        runRow(8, "s", "push", "success", now.addSecs(-300))})
                   .toJson());
        for (int run : {7, 8})
            gh.put(QStringLiteral("repos_o_r_actions_runs_%1_attempts_1_jobs_per_page_100").arg(run),
                   QJsonDocument(QJsonObject{{"total_count", 1},
                                             {"jobs", QJsonArray{jobRow(run * 100, "Build", run == 7 ? "failure" : "success",
                                                                        now.addSecs(-200))}}})
                       .toJson());
        gh.put(QString::fromLatin1(failing) + QStringLiteral(".fail"), "");
        const QJsonObject r = p.call();
        EXPECT_TRUE(r.value("ok").toBool()) << dump(r);
        EXPECT_FALSE(r.value("fetch_errors").toArray().isEmpty()) << dump(r);
        return QFileInfo::exists(p.savedPath());
    };
    EXPECT_TRUE(saves("repos_o_r_actions_jobs_700_logs")) << "a log error blocked the save";
    EXPECT_FALSE(saves("repos_o_r_actions_runs_8_attempts_1_jobs_per_page_100"));
}

// One remote with two url lines is one remote.
TEST(McpCiReport, Ants6407TwoUrlsOnOneRemoteAreOneRemote) {
    Env env;
    FakeGh gh;
    gh.install(env);
    Project p(/*withWorkflow=*/true, /*withRemote=*/false);
    env.set("XDG_CACHE_HOME", p.cache.path().toUtf8());
    p.write(QStringLiteral(".git/config"), "[remote \"origin\"]\n\turl = https://github.com/o/r.git\n"
                                           "\turl = git@github.com:o/r.git\n");
    const QJsonObject r = p.call();
    EXPECT_TRUE(r.value("ok").toBool()) << dump(r);
    EXPECT_TRUE(r.value("repo_note").isUndefined()) << dump(r);
}

// ctest after the shell's own command-running words.
TEST(McpCiReport, Ants6407CtestAfterShellKeywords) {
    auto fires = [](const char *run) {
        const CiWorkflows::Result wf = workflow(QStringLiteral(
            "name: CI\njobs:\n  t:\n    steps:\n      - name: T\n        run: |\n          %1\n")
            .arg(QString::fromLatin1(run)));
        return !ofCheck(CiReport::analyse(QJsonObject(), wf, kNow, 30), "test_parallelism").isEmpty();
    };
    EXPECT_TRUE(fires("if ctest --timeout 60; then echo ok; fi")) << "if";
    EXPECT_TRUE(fires("! ctest --timeout 60")) << "!";
    EXPECT_TRUE(fires("while ctest --timeout 60; do break; done")) << "while";
    EXPECT_TRUE(fires("until ctest --timeout 60; do sleep 1; done")) << "until";
    EXPECT_TRUE(fires("if false; then :; elif ctest --timeout 60; then :; fi")) << "elif";
    EXPECT_FALSE(fires("if ctest -j4 --timeout 60; then echo ok; fi"));
}

// ---- ANTS-6408: the cold review's small items --------------------------------

// § 4.3: a timing finding reads its job's log for the cache line.
TEST(McpCiReport, Ants6408TimingFindingReadsTheCacheLine) {
    const CiWorkflows::Result wf = workflow(kBuild10);
    auto cache = [&](const char *log) {
        History h;
        h.add(1, kNow.addSecs(-3600), QStringLiteral("Build"), 0, 480, QStringLiteral("success"));
        QJsonObject history = h.json();
        history["logs"] = QJsonObject{{"10", QString::fromLatin1(log)}};
        const auto f = ofCheck(CiReport::analyse(history, wf, kNow, 30), "near_budget");
        return f.isEmpty() ? QString() : f[0].value("cache").toString();
    };
    EXPECT_EQ(cache("x\nCache restored from key: Linux-abc\ny"), QStringLiteral("hit"));
    EXPECT_EQ(cache("x\nCache not found for input keys: Linux-abc\ny"), QStringLiteral("miss"));
    EXPECT_EQ(cache("no cache step"), QStringLiteral("unknown"));
}

// A call killed at its time limit says so in fetch_errors.
TEST(McpCiReport, Ants6408TimedOutCallNamesItsReason) {
    Env env;
    FakeGh gh;
    gh.install(env);
    gh.put(QStringLiteral("repos_o_r_actions_cache_usage.sleep"), "");
    Project p;
    CiHistory::Options o;
    o.now = QDateTime::currentDateTimeUtc();
    o.perCallMs = 300;
    const CiHistory::Result r = CiHistory::fetch(p.root, o, {});
    ASSERT_EQ(r.fetchErrors.size(), 1) << dump(r.fetchErrors);
    EXPECT_EQ(r.fetchErrors.at(0).toObject().value("reason").toString(), QStringLiteral("timeout"))
        << dump(r.fetchErrors);
}

// With workflow= set, the checks that read the files read that workflow only.
TEST(McpCiReport, Ants6408WorkflowArgumentNarrowsTheFileChecks) {
    Env env;
    FakeGh gh;
    gh.install(env);
    Project p;
    env.set("XDG_CACHE_HOME", p.cache.path().toUtf8());
    p.write(QStringLiteral(".github/workflows/ci.yml"),
            "name: CI\njobs:\n  b:\n    runs-on: ubuntu-latest\n    steps:\n      - run: make\n");
    p.write(QStringLiteral(".github/workflows/docs.yml"),
            "name: Docs\njobs:\n  d:\n    runs-on: ubuntu-latest\n    steps:\n      - run: make docs\n");
    auto workflowsOf = [](const QJsonObject &r) {
        QStringList names;
        for (const QJsonValue &f : r.value("findings").toArray())
            if (f.toObject().value("check").toString() == QLatin1String("runner_latest"))
                names.append(f.toObject().value("workflow").toString());
        names.sort();
        return names.join(QLatin1Char(','));
    };
    EXPECT_EQ(workflowsOf(p.call()), QStringLiteral("CI,Docs"));
    EXPECT_EQ(workflowsOf(p.call({{"workflow", "Docs"}})), QStringLiteral("Docs"));
}

// ---- ANTS-6410: values continued on the lines below their key -------------

// A plain scalar continued on deeper lines folds into one value, and a flow
// list over several lines is read whole; the file is not unread.
TEST(McpCiReport, Ants6410ContinuedValuesAreRead) {
    const CiWorkflows::Result wf = workflow(QStringLiteral(
        "name: CI\njobs:\n  t:\n    runs-on: [self-hosted,\n      linux]\n    timeout-minutes: 30\n"
        "    steps:\n      - uses: actions/cache/save@v4\n"
        "        if: github.ref == 'refs/heads/main' &&\n"
        "          github.event_name == 'push'  # main only\n"
        "        with:\n          key: k\n"
        "      - run: make\n\n          check\n"));
    ASSERT_EQ(wf.workflows.size(), 1) << dump(wf.unread);
    const CiWorkflows::Job &j = wf.workflows[0].jobs[0];
    EXPECT_EQ(j.runsOn, QStringLiteral("self-hosted, linux"));
    EXPECT_EQ(j.budgetMin, 30);
    ASSERT_EQ(j.steps.size(), 2);
    EXPECT_EQ(j.steps[0].ifExpr,
              QStringLiteral("github.ref == 'refs/heads/main' && github.event_name == 'push'"));
    EXPECT_EQ(j.steps[0].cacheKey, QStringLiteral("k"));
    EXPECT_EQ(j.steps[1].run, QStringLiteral("make\ncheck")) << "an empty line folds to a newline";
}

// What YAML itself rejects stays unread: a comment line inside a plain scalar,
// and a continuation line that reads as a key.
TEST(McpCiReport, Ants6410InvalidContinuationsStayUnread) {
    for (const char *yaml : {"name: CI\njobs:\n  t:\n    if: a &&\n    # note\n      b\n",
                             "name: CI\njobs:\n  t:\n    if: a\n      b: c\n"}) {
        EXPECT_TRUE(workflow(QString::fromLatin1(yaml)).workflows.isEmpty()) << yaml;
    }
}
