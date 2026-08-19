// ANTS-4512 — read_regions must not silently pick one of two alias arrays and
// then report a shape error against the array the caller got RIGHT.
//
// Drives RemoteControl::cmdReadRegions live against a seeded temp file, like
// roadmap_query_id_body_cap / roadmap_query_mode_hint. Behavioural on purpose:
// the sibling RR-7 alias test is a windowed source-grep, which can only prove
// the alias NAMES appear near the function — not which array a call resolved
// to, which is the entire defect here.
//
// See tests/features/read_regions_alias_conflict/spec.md.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

ANTS_TEST_SCOPE();

namespace {

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QString seed(const QTemporaryDir &tmp) {
    const QString p = QDir(tmp.path()).filePath(QStringLiteral("a.txt"));
    writeFile(p, QByteArray("one\ntwo\nthree\nfour\n"));
    return p;
}

QJsonObject wellShapedRegion() {
    QJsonObject r;
    r[QStringLiteral("path")]       = QStringLiteral("a.txt");
    r[QStringLiteral("start_line")] = 1;
    r[QStringLiteral("end_line")]   = 2;
    return r;
}

}  // namespace

// A1 — the reported call: `paths` as bare strings AND `regions` as properly
// shaped objects. Pre-fix, `paths` won on preference order and the reply was
// three copies of `item missing "path"` — true of the array the verb chose,
// false of the array the caller meant, and naming neither.
TEST(read_regions_alias_conflict, Ants4512TwoArraysRefuseInsteadOfPickingOne) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    seed(tmp);

    QJsonArray barePaths;
    barePaths.append(QStringLiteral("a.txt"));
    QJsonArray regions;
    regions.append(wellShapedRegion());

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("paths")]      = barePaths;
    req[QStringLiteral("regions")]    = regions;
    const QJsonObject resp = rc.cmdReadRegions(req).object();

    ASSERT_FALSE(resp.value(QStringLiteral("ok")).toBool())
        << "two arrays for one parameter must refuse, not pick one";
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString().toStdString(),
              std::string("bad_op_combo"));

    // The refusal must name BOTH keys — naming one repeats the original sin.
    const QString err = resp.value(QStringLiteral("error")).toString();
    EXPECT_TRUE(err.contains(QStringLiteral("paths")))
        << "refusal must name `paths`, got: " << err.toStdString();
    EXPECT_TRUE(err.contains(QStringLiteral("regions")))
        << "refusal must name `regions`, got: " << err.toStdString();
}

// A2 — one array alone still works, and the reply says which key it came from.
// `items_key` is what makes a later shape error interpretable at all.
TEST(read_regions_alias_conflict, Ants4512SingleAliasWorksAndEchoesItsKey) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    seed(tmp);

    QJsonArray regions;
    regions.append(wellShapedRegion());

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("regions")]    = regions;
    const QJsonObject resp = rc.cmdReadRegions(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("items_key")).toString().toStdString(),
              std::string("regions"))
        << "the reply must say which key the batch was read from";
}

// A3 — the canonical key keeps its name in the echo. A caller sending `items`
// must not be told the batch came from an alias.
TEST(read_regions_alias_conflict, Ants4512CanonicalKeyEchoesAsItems) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    seed(tmp);

    QJsonArray items;
    items.append(wellShapedRegion());

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("items")]      = items;
    const QJsonObject resp = rc.cmdReadRegions(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("items_key")).toString().toStdString(),
              std::string("items"));
}
