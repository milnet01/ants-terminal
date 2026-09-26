// ANTS-4501 TU 15/19 — roadmap_log op:"backfill_dates": the one-off git walk
// that dates the rows predating § 2.2's forward stamping.
//
// Its own TU because remotecontrol_roadmap_log.cpp reached ANTS-3833 INV-6's
// 6000-line cap with it inside, and § 2.2 of that spec asks for a new TU at a
// member boundary rather than a smaller one. Appended LAST in
// ANTS_RC_SOURCES_REL for remotecontrol_roadmap_migrate.cpp's reason: it is not
// a slice of the pre-split file but a member that never existed there, so no
// pre-split relative order can be violated by it.
#include "remotecontrol.h"
#include "remotecontrol_internal.h"
#include "projectsettings.h"   // ANTS-3771
#include "gitwrap.h"
#include "roadmapparse.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryFile>

using namespace rcdetail;

// ------------------------------------------------------------- ANTS-4501 ----
// op:"backfill_dates" — § 2.3. Date the past from git, once, on request.
//
// § 2.2 stamps going forward; every row that predates it is NULL, which on this
// project is most of them. A closure IS observable in version control: a
// bullet's status marker becomes ✅ in some commit, and that commit has a date.
//
// **The walk reads DIFFS, not the content of every revision, and the reason is
// measured.** § 2.3 describes it as "read the set of ids carrying a shipped
// marker" per revision and prices it at ~17 s from a `grep -c` sample. Reading
// each revision's content for real costs 49 s of `git show` alone on this repo
// (1525 revisions × 3 files, measured 2026-08-20) before one bullet is parsed,
// and the MCP bridge times out at 60 s (ANTS-3444). One
// `git log --reverse -p -U0` over the same pathspecs is 3.8 s and 8.5 MB.
//
// The semantics are identical because only a FIRST sighting is ever recorded.
// An id first appears in the `+` line that added its bullet, and first carries a
// shipped marker in the `+` line that flipped it. A later re-addition — an
// archive rotation re-emitting a ✅ bullet into a new file — is already seen and
// changes nothing, which is the same first-wins rule a content walk applies.
//
// § 4's memory rule holds by construction: the diff is consumed a line at a
// time and only one commit's added bullet lines are ever held.

namespace {

struct RlBackfillWalk {
    QHash<QString, QString> firstSeen;      // id -> YYYY-MM-DD, first appearance
    QHash<QString, QString> firstShipped;   // id -> YYYY-MM-DD, first ✅
    int     revisions = 0;
    bool    failed    = false;
    QString error;
};

// ANTS-5405 — a pass heading added in a hunk that did not also add its Status
// line: a renamed heading, or a block moved. Its status at that revision is
// not in the diff, so the one revision's file is read to learn it.
struct RlPassLookup { QString hash, path, date, id; };

// Only a line that could BEGIN a bullet reaches the grammar. Both readers key
// on the list marker, so a prose line citing an id in passing — a body
// reference to another item — can never be taken for a bullet. `+` is not a
// marker here: parseAntsV1Bullet's contract names `- ` and `* ` only, and a
// diff's own `+` prefix has already been stripped by the caller.
bool rlLooksLikeBulletLine(QStringView s) {
    int i = 0;
    while (i < s.size() && (s[i] == u' ' || s[i] == u'\t')) ++i;
    if (i + 1 >= s.size()) return false;
    return (s[i] == u'-' || s[i] == u'*') && s[i + 1] == u' ';
}

// The pathspecs the walk runs over: the live roadmap plus every archive the
// store's sections name, plus each archive's DIRECTORY. The directory is what
// covers a file that existed historically and no longer does — a rotation
// renaming `docs/roadmap/0.4.md`, say — which a list of today's files cannot.
QStringList rlRoadmapPathspecs(RoadmapStore &store, qint64 projectId,
                               const QString &root, const QString &roadmapPath) {
    QSet<QString> specs;
    const QDir rootDir(root);
    const QString liveRel = rootDir.relativeFilePath(roadmapPath);
    if (!liveRel.isEmpty() && !liveRel.startsWith(QLatin1String("..")))
        specs.insert(liveRel);

    QString err;
    // listSectionsOrdered and not listSections: ANTS-3818's guard asks a new
    // consumer to take the ordered enumerator unless it has a reason not to,
    // and this one has none — the paths go into a QSet and come out sorted.
    if (const auto sections = store.listSectionsOrdered(projectId, &err)) {
        for (const RoadmapStore::SectionRow &r : *sections) {
            if (!r.sourcePath || r.sourcePath->isEmpty()) continue;   // the live roadmap
            specs.insert(*r.sourcePath);
            const int slash = r.sourcePath->lastIndexOf(QLatin1Char('/'));
            if (slash > 0) specs.insert(r.sourcePath->left(slash));
        }
    }
    QStringList out(specs.cbegin(), specs.cend());
    out.sort();
    return out;
}

// ANTS-5405 — the pass id a line names, or empty: the reader's own heading
// grammar, so the walk and every other pass-headings read agree.
QString rlPassIdOf(const QString &line) {
    if (!line.startsWith(QLatin1String("####"))) return {};
    const auto rec = RoadmapParse::parsePassHeadingBlock({line});
    return rec ? rec->id : QString();
}

// ANTS-5405 — true when `line` is a `- **Status**:` line the reader reads as
// shipped (done / shipped / completed, or ✅).
bool rlPassStatusIsDone(const QString &line) {
    static const QRegularExpression rx(
        QStringLiteral("^\\s*[-*]\\s*\\*\\*Status\\*\\*\\s*:"),
        QRegularExpression::CaseInsensitiveOption);
    if (!rx.match(line).hasMatch()) return false;
    const auto rec = RoadmapParse::parsePassHeadingBlock(
        {QStringLiteral("#### Pass 0.0"), line});
    return rec && rec->status == QString::fromUtf8(RoadmapParse::kEmojiDone);
}

RlBackfillWalk rlWalkGitForDates(const QString &root, const QStringList &pathspecs,
                                 int budgetMs, bool passHeadings) {
    RlBackfillWalk w;
    // ANTS-5405 — on a pass-headings roadmap the id sits on the `#### Pass`
    // heading and a flip changes only the Status line below it, so a -U0 diff
    // of the flip never shows the heading. A diff driver whose function-name
    // pattern is that heading makes git print the ENCLOSING pass in every `@@`
    // hunk header. It is attached through a throwaway attributes file (a few
    // bytes, removed when the walk returns) so the user's repository is not
    // touched; core.attributesFile is the lowest-precedence attribute source,
    // so a project's own diff attribute for the file would win over it.
    QTemporaryFile attrs;
    QStringList argv;
    if (passHeadings) {
        if (!attrs.open()) {
            w.failed = true;
            w.error  = QStringLiteral("could not create the diff-driver attributes file");
            return w;
        }
        attrs.write("* diff=antspass\n");
        attrs.flush();
        argv << QStringLiteral("-c")
             << QStringLiteral("diff.antspass.xfuncname=^#### Pass .*$")
             << QStringLiteral("-c")
             << QStringLiteral("core.attributesFile=") + attrs.fileName();
    }
    argv << QStringList{
        QStringLiteral("--no-pager"),
        QStringLiteral("log"),
        QStringLiteral("--reverse"),
        QStringLiteral("-p"),
        QStringLiteral("-U0"),          // no context lines: only what changed
        QStringLiteral("--no-color"),
        // A merge shows no diff by default, so a bullet that reached the branch
        // only through one would be invisible. Against the first parent it is a
        // diff like any other. This repo has none touching the roadmap files;
        // other projects on this store do.
        QStringLiteral("--diff-merges=first-parent"),
        QStringLiteral("--date=short"),
        // § 2.3 pins the AUTHOR date: the committer date moves under every
        // rebase and cherry-pick, so a rewritten history would otherwise change
        // every figure this feeds. \x01 as the record marker because no diff
        // line can begin with it — `+`, `-`, `@@`, `diff`, `index` all can.
        QStringLiteral("--format=%x01%H %ad"),
        QStringLiteral("--"),
    };
    argv << pathspecs;

    QProcess p;
    p.setWorkingDirectory(root);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(QStringLiteral("git"), argv);
    if (!p.waitForStarted(2000)) {
        w.failed = true;
        w.error  = QStringLiteral("git failed to start");
        return w;
    }

    const RoadmapParse::IdFormat idFormat =
        ProjectSettings::idFormatFor(root);   // ANTS-3771

    QString curDate;
    QStringList added;   // this commit's added bullet lines, discarded per commit

    // ANTS-5405 — pass mode's per-hunk state.
    QString curPass;          // the pass the current hunk sits in
    QString curHash, curPath; // the commit and file the hunk belongs to
    QString headingNoStatus;  // a heading this hunk added, no Status line yet
    QVector<RlPassLookup> lookups;
    const auto closeHeading = [&] {
        if (!headingNoStatus.isEmpty() && !curHash.isEmpty() && !curPath.isEmpty())
            lookups.append({curHash, curPath, curDate, headingNoStatus});
        headingNoStatus.clear();
    };

    const auto flush = [&] {
        if (added.isEmpty()) return;
        // One grammar (ANTS-3808 INV-2): the added lines are handed to
        // parseBullets() as a document rather than matched with a second regex.
        // A continuation line with no bullet above it yields no record, so a
        // fragment cannot invent one.
        // ANTS-3771 — these are a PROJECT'S OWN roadmap lines, so they are
        // read under that project's declaration like any other project-scoped
        // read (§ 2.2). Loaded once outside the lambda: `root` cannot change
        // during a walk and load() reads the file every time it is asked.
        for (const RoadmapParse::BulletRecord &b :
                 RoadmapParse::parseBullets(added.join(QLatin1Char('\n')),
                                            idFormat)) {
            if (b.id.isEmpty() || curDate.isEmpty()) continue;
            if (!w.firstSeen.contains(b.id))
                w.firstSeen.insert(b.id, curDate);
            if (b.status == QString::fromUtf8(RoadmapParse::kEmojiDone) &&
                !w.firstShipped.contains(b.id))
                w.firstShipped.insert(b.id, curDate);
        }
        added.clear();
    };

    const auto consume = [&](const QByteArray &raw) {
        const QString line = QString::fromUtf8(raw);
        if (line.startsWith(QChar(0x01))) {
            flush();
            closeHeading();
            const QString rest = line.mid(1);
            const int sp = rest.indexOf(QLatin1Char(' '));
            curHash = (sp < 0) ? rest.trimmed() : rest.left(sp);
            curDate = (sp < 0) ? QString() : rest.mid(sp + 1).trimmed();
            curPass.clear();
            curPath.clear();
            ++w.revisions;
            return;
        }
        if (passHeadings) {
            if (line.startsWith(QLatin1String("+++ b/"))) {
                closeHeading();
                curPath = line.mid(6);
                return;
            }
            // `@@ -a,b +c,d @@ <function context>`: the driver above makes
            // the context the nearest `#### Pass` heading above the hunk.
            if (line.startsWith(QLatin1String("@@"))) {
                closeHeading();
                const qsizetype close = line.indexOf(QLatin1String("@@"), 2);
                curPass = close < 0 ? QString()
                                    : rlPassIdOf(line.mid(close + 2).trimmed());
                return;
            }
            if (!line.startsWith(QLatin1Char('+')) ||
                line.startsWith(QLatin1String("+++")) || curDate.isEmpty())
                return;
            const QString body = line.mid(1);
            if (const QString id = rlPassIdOf(body); !id.isEmpty()) {
                closeHeading();
                curPass = id;   // a heading added in this hunk owns what follows
                headingNoStatus = id;
                if (!w.firstSeen.contains(id)) w.firstSeen.insert(id, curDate);
                return;
            }
            const bool done = rlPassStatusIsDone(body);
            if (done || body.contains(QLatin1String("**Status**"), Qt::CaseInsensitive))
                headingNoStatus.clear();   // its Status line is in the diff
            if (!curPass.isEmpty() && !w.firstShipped.contains(curPass) && done)
                w.firstShipped.insert(curPass, curDate);
            return;
        }
        if (!line.startsWith(QLatin1Char('+')) ||
            line.startsWith(QLatin1String("+++")))
            return;
        const QStringView body = QStringView(line).mid(1);
        if (rlLooksLikeBulletLine(body)) added << body.toString();
    };

    QElapsedTimer clock;
    clock.start();
    QByteArray pending;
    const auto drain = [&] {
        // Index-scan and ONE trim per chunk. Removing each line from the front
        // is a memmove of the remainder and turns an 8.5 MB stream into
        // gigabytes of copying.
        int start = 0, nl;
        while ((nl = pending.indexOf('\n', start)) >= 0) {
            consume(pending.mid(start, nl - start));
            start = nl + 1;
        }
        pending.remove(0, start);
    };

    while (true) {
        if (clock.elapsed() > budgetMs) {
            p.kill();
            p.waitForFinished(500);
            w.failed = true;
            w.error  = QStringLiteral("git log exceeded the %1 s walk budget")
                           .arg(budgetMs / 1000);
            return w;
        }
        if (p.waitForReadyRead(200)) {
            pending += p.readAllStandardOutput();
            drain();
            continue;
        }
        if (p.state() == QProcess::NotRunning) {
            pending += p.readAllStandardOutput();
            drain();
            break;
        }
    }
    p.waitForFinished(1000);
    if (!pending.isEmpty()) consume(pending);   // a final line with no newline
    flush();

    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        w.failed = true;
        w.error  = QStringLiteral("git log exited %1: %2")
                       .arg(p.exitCode())
                       .arg(QString::fromUtf8(p.readAllStandardError().left(400)).trimmed());
        return w;
    }
    closeHeading();

    // ANTS-5405 — read each queued revision's file once, and date a pass that
    // was already done when its heading appeared: that commit is when this id
    // first carried a done marker, the rule the bullet walk applies to a
    // renamed id. First wins, so an earlier date is never replaced by a later.
    QHash<QString, QVector<const RlPassLookup *>> byRevision;
    for (const RlPassLookup &l : std::as_const(lookups))
        if (!w.firstShipped.contains(l.id) || w.firstShipped.value(l.id) > l.date)
            byRevision[l.hash + QLatin1Char(':') + l.path].append(&l);
    for (auto it = byRevision.cbegin(); it != byRevision.cend(); ++it) {
        if (clock.elapsed() > budgetMs) {
            w.failed = true;
            w.error  = QStringLiteral("git show exceeded the %1 s walk budget")
                           .arg(budgetMs / 1000);
            return w;
        }
        const GitWrap::Result shown = GitWrap::run(
            root, {QStringLiteral("show"), it.key()}, 32 * 1024 * 1024);
        if (!shown.started || shown.exitCode != 0 || shown.stdoutTruncated)
            continue;   // unreadable: leave it undated rather than guess
        const QStringList lines = QString::fromUtf8(shown.stdoutBytes).split(QLatin1Char('\n'));
        for (const RlPassLookup *l : it.value()) {
            for (int i = 0; i < lines.size(); ++i) {
                if (rlPassIdOf(lines.at(i)) != l->id) continue;
                QStringList block{lines.at(i)};
                for (int j = i + 1; j < lines.size(); ++j) {
                    if (lines.at(j).startsWith(QLatin1Char('#')) &&
                        !lines.at(j).startsWith(QLatin1String("#####")))
                        break;
                    block << lines.at(j);
                }
                const auto rec = RoadmapParse::parsePassHeadingBlock(block);
                if (rec && rec->status == QString::fromUtf8(RoadmapParse::kEmojiDone) &&
                    (!w.firstShipped.contains(l->id) || w.firstShipped.value(l->id) > l->date))
                    w.firstShipped.insert(l->id, l->date);
                break;
            }
        }
    }
    return w;
}

}  // namespace

QJsonDocument RemoteControl::cmdRoadmapLogBackfillDates(const QJsonObject &req) {
    QString root, roadmapPath;
    QJsonDocument refusal;
    const auto target = roadmapSectionOpTarget(req, &root, &roadmapPath, &refusal);
    if (!target) {
        // § 2.3 names `project_not_registered` for "the store holds no row for
        // that root", which is exactly the condition the shared prologue calls
        // `op_unsupported` (its own name for "not store-migrated"). Remapped
        // rather than duplicated: one prologue, and the code a caller branches
        // on is the one this op's contract promises.
        QJsonObject env = refusal.object();
        if (env.value(QStringLiteral("code")).toString() ==
                QLatin1String("op_unsupported")) {
            env[QStringLiteral("code")] = QStringLiteral("project_not_registered");
            env[QStringLiteral("error")] = QStringLiteral(
                "roadmap_log: backfill_dates needs a store-migrated project — "
                "the store holds no row for \"%1\". Run roadmap_migrate first.")
                    .arg(root.isEmpty() ? roadmapPath : root);
            return QJsonDocument(env);
        }
        return refusal;
    }
    RoadmapStore &store    = *target->store;
    const qint64 projectId = target->projectId;
    const bool dryRun      = req.value(QStringLiteral("dry_run")).toBool();

    const auto blErr = [](const QString &code, const QString &message) {
        QJsonObject env;
        env[QStringLiteral("ok")]    = false;
        env[QStringLiteral("code")]  = code;
        env[QStringLiteral("error")] = message;
        return QJsonDocument(env);
    };

    // § 2.3 — `not_a_git_repo` when the project root has no history to walk.
    // `rev-parse --git-dir` and not a `.git` stat: a worktree and a submodule
    // both carry a `.git` FILE, and a subdirectory of a repo carries neither.
    const GitWrap::Result probe =
        GitWrap::run(root, {QStringLiteral("rev-parse"), QStringLiteral("--git-dir")});
    if (!probe.started || probe.exitCode != 0) {
        return blErr(QStringLiteral("not_a_git_repo"),
            QStringLiteral("roadmap_log: \"%1\" has no git history to walk — "
                           "backfill_dates derives every date from commits")
                .arg(root));
    }

    const QStringList pathspecs = rlRoadmapPathspecs(store, projectId, root, roadmapPath);
    if (pathspecs.isEmpty()) {
        return blErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: no roadmap path under \"%1\" resolves "
                           "root-relative, so there is nothing to walk").arg(root));
    }

    // 180 s and not the bridge's 60: the walk is measured at ~4 s here, and the
    // budget exists to bound a pathological repository rather than to pace a
    // normal one. A caller that times out first still leaves the store
    // untouched — nothing is written until the walk completes.
    // ANTS-5405 — the walk parses by the live file's dialect, and says which,
    // so a zero on a dialect it cannot read is told apart from no history.
    QString dialect;
    {
        QFile live(roadmapPath);
        if (live.open(QIODevice::ReadOnly | QIODevice::Text))
            dialect = RoadmapParse::detectRoadmapFormat(
                QString::fromUtf8(live.readAll()).split(QLatin1Char('\n')));
    }
    const bool passHeadings = dialect == QLatin1String("pass-headings");

    QElapsedTimer wall;
    wall.start();
    const RlBackfillWalk walk =
        rlWalkGitForDates(root, pathspecs, 180000, passHeadings);
    if (walk.failed) {
        return blErr(QStringLiteral("git_failed"),
            QStringLiteral("roadmap_log: backfill_dates could not walk the "
                           "history — %1").arg(walk.error));
    }
    const qint64 walkMs = wall.elapsed();

    QString err;
    const auto targets = store.itemsForDateBackfill(projectId, &err);
    if (!targets)
        return blErr(QStringLiteral("store_failed"), err);

    // Decide every write before opening a transaction, so `dry_run` reports the
    // real run's counts rather than a second estimate of them.
    struct Write { qint64 pk = 0; QString created, shipped; };  // dates, empty = skip
    QVector<Write> plan;
    QStringList undated, shippedUndated;
    int createdWrites = 0, shippedWrites = 0;
    for (const RoadmapStore::DateTarget &t : *targets) {
        // INV-2 — a non-NULL date is never overwritten, whether a stamp or an
        // earlier backfill put it there. That is what makes a second run the
        // same operation as the first, and what protects a human correction.
        const bool wantCreated = t.createdIsNull && walk.firstSeen.contains(t.id);
        // § 2.3's fourth property. `shipped` is CLEARED when an item is
        // reopened, so its column is NULL, INV-2 does not protect it, and git
        // still holds the commit where its marker was ✅ — a backfill reasoning
        // only from git re-closes every reopened item, silently, on every run.
        // Skipping the COLUMN and not the id: `created` is still written for
        // every id observed, whatever its status, or `added` and `age_open`
        // stay unanswerable for exactly the backlog this report is about.
        const bool wantShipped = t.shippedIsNull &&
                                 t.status == QLatin1String("shipped") &&
                                 walk.firstShipped.contains(t.id);
        if (wantCreated) ++createdWrites;
        if (wantShipped) ++shippedWrites;
        if (wantCreated || wantShipped)
            plan.push_back({t.itemPk,
                            wantCreated ? walk.firstSeen.value(t.id) : QString(),
                            wantShipped ? walk.firstShipped.value(t.id) : QString()});
        // INV-3 — an id the walk never observed is left NULL and COUNTED, never
        // given a boundary commit's date as a fallback. A fabricated date is
        // indistinguishable from a real one ever after.
        if (t.createdIsNull && !walk.firstSeen.contains(t.id))
            undated.append(t.id);
        // ANTS-5405 — the same count for the `shipped` column: a shipped item
        // whose history shows no done marker, so the report stays blind to it.
        if (t.shippedIsNull && t.status == QLatin1String("shipped") &&
            !walk.firstShipped.contains(t.id))
            shippedUndated.append(t.id);
    }

    if (!dryRun && !plan.isEmpty()) {
        if (!store.begin(&err))
            return blErr(QStringLiteral("store_failed"), err);
        for (const Write &wr : plan) {
            // `store-generated` (roadmap-data-model.md § 7.7): no author
            // supplied these, and § 2.2's forward stamps say the same.
            if (!wr.created.isEmpty() &&
                !store.setItemField(wr.pk, QStringLiteral("created"), wr.created,
                                    QStringLiteral("store-generated"), &err)) {
                store.rollback(nullptr);
                return blErr(QStringLiteral("store_failed"), err);
            }
            if (!wr.shipped.isEmpty() &&
                !store.setItemField(wr.pk, QStringLiteral("shipped"), wr.shipped,
                                    QStringLiteral("store-generated"), &err)) {
                store.rollback(nullptr);
                return blErr(QStringLiteral("store_failed"), err);
            }
        }
        if (!store.commit(&err)) {
            store.rollback(nullptr);
            return blErr(QStringLiteral("store_failed"), err);
        }
    }

    QJsonObject env;
    env[QStringLiteral("ok")]              = true;
    env[QStringLiteral("op")]              = QStringLiteral("backfill_dates");
    env[QStringLiteral("project_root")]    = root;
    env[QStringLiteral("revisions_walked")] = walk.revisions;
    env[QStringLiteral("dialect")]         = dialect;   // ANTS-5405
    env[QStringLiteral("walk_ms")]         = double(walkMs);
    env[QStringLiteral("items")]           = int(targets->size());
    env[QStringLiteral("created_written")] = createdWrites;
    env[QStringLiteral("shipped_written")] = shippedWrites;
    env[QStringLiteral("undated_count")]   = int(undated.size());
    if (dryRun) env[QStringLiteral("dry_run")] = true;
    // The ids it could not date, capped. The COUNT above is the whole answer and
    // is never capped; the list is a sample to start from, and a truncated one
    // that did not say so would read as the complete set.
    constexpr int kUndatedSample = 50;
    QJsonArray sample;
    for (int i = 0; i < undated.size() && i < kUndatedSample; ++i)
        sample.append(undated.at(i));
    env[QStringLiteral("undated")] = sample;
    if (undated.size() > kUndatedSample)
        env[QStringLiteral("undated_truncated")] = true;
    env[QStringLiteral("shipped_undated_count")] = int(shippedUndated.size());
    if (!shippedUndated.isEmpty()) {
        QJsonArray shippedSample;
        for (int i = 0; i < shippedUndated.size() && i < kUndatedSample; ++i)
            shippedSample.append(shippedUndated.at(i));
        env[QStringLiteral("shipped_undated")] = shippedSample;
        if (shippedUndated.size() > kUndatedSample)
            env[QStringLiteral("shipped_undated_truncated")] = true;
    }
    // § 5 — `last_modified` is not backfilled at all. A commit touching a bullet
    // IS a modification, so every commit would rewrite it, and the column's only
    // consumer is "what changed recently", which `history` answers properly for
    // the window it covers.
    return QJsonDocument(env);
}

// § 2.3's walk is m_main-independent (caller_cwd, git and the store), so the
// test seam is the same shape as the section ops'.
QJsonDocument RemoteControl::cmdRoadmapLogBackfillDatesForTest(
        const QJsonObject &req) {
    return cmdRoadmapLogBackfillDates(req);
}
