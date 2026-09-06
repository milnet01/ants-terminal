// Feature-conformance test for ANTS-4902 — the file_outline UnrealScript
// lane. See spec.md.
//
// Reported by UT_MonsterHunt: file_outline returns {language:"unknown"} and no
// symbols[] for `.uc`, though UnrealScript is brace-delimited and C-like and
// the generic rule set already carries `class`, `function` and `struct`. Their
// whole runtime surface is these files.
//
// The fixture is the shape the real corpus uses (190 files measured): the
// brace on its own line, `Super.` calls in the bodies, and a defaultproperties
// block at the end.

#include "../../_support/expect.h"
#include "fileoutline.h"

#include <gtest/gtest.h>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

ANTS_TEST_SCOPE();

namespace {

const char *kMutator = R"(//=============================================================================
// MonsterHuntMutator.
//=============================================================================
class MonsterHuntMutator extends Mutator;

var config int MaxMonsters;
var localized string StatusText;

const MAX_WAVES = 12;

struct WaveSpec
{
    var int Count;
};

function PostBeginPlay()
{
    Super.PostBeginPlay();
    Level.Game.RegisterDamageMutator(Self);
}

simulated function Tick(float DeltaTime)
{
    Super.Tick(DeltaTime);
}

final function bool CheckReplacement(Actor Other, out byte bSuperRelevant)
{
    return true;
}

static function int WaveCount()
{
    return MAX_WAVES;
}

singular function TakeDamage(int Damage)
{
    Health -= Damage;
}

defaultproperties
{
     MaxMonsters=16
}
)";

QString writeUc(const QTemporaryDir &dir, const QString &name) {
    const QString path =
        QFileInfo(dir.path()).canonicalFilePath() + QLatin1Char('/') + name;
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(kMutator);
    f.close();
    return path;
}

QStringList symbolNames(const QJsonObject &out) {
    QStringList names;
    for (const QJsonValue &v : out.value(QStringLiteral("symbols")).toArray())
        names << v.toObject().value(QStringLiteral("name")).toString();
    return names;
}

}  // namespace

// INV-1 — detected by extension and NAMED, so a caller can tell an outlined
// language from one that fell through to Mode::Auto and reported nothing.
TEST(FileOutlineUnrealScript, Inv1DetectedAndNamed) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeUc(dir, QStringLiteral("MonsterHuntMutator.uc"));

    const QJsonObject out =
        FileOutline::compute(path, FileOutline::Mode::Auto, true, 1000);
    EXPECT_EQ(out.value(QStringLiteral("language")).toString(),
              QStringLiteral("unrealscript"))
        << "pickModeByExt has no .uc branch, so the file falls through to "
           "Auto and compute() reports unknown";
}

// INV-2 — the class and every function spelling the real corpus uses.
TEST(FileOutlineUnrealScript, Inv2ExtractsClassAndEveryFunctionSpelling) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeUc(dir, QStringLiteral("MonsterHuntMutator.uc"));

    const QJsonObject out =
        FileOutline::compute(path, FileOutline::Mode::Auto, true, 1000);
    const QStringList syms = symbolNames(out);
    const std::string all = syms.join(QStringLiteral(",")).toStdString();

    EXPECT_TRUE(syms.contains(QStringLiteral("MonsterHuntMutator")))
        << "class declaration missing: " << all;
    EXPECT_TRUE(syms.contains(QStringLiteral("PostBeginPlay")))
        << "plain function missing: " << all;
    EXPECT_TRUE(syms.contains(QStringLiteral("CheckReplacement")))
        << "`final function` missing: " << all;
    EXPECT_TRUE(syms.contains(QStringLiteral("WaveCount")))
        << "`static function` missing: " << all;
    // The two modifiers that exist in no other brace-family language, and so
    // are the ones a shared rule set would not already carry.
    EXPECT_TRUE(syms.contains(QStringLiteral("Tick")))
        << "`simulated function` missing: " << all;
    EXPECT_TRUE(syms.contains(QStringLiteral("TakeDamage")))
        << "`singular function` missing: " << all;
    EXPECT_TRUE(syms.contains(QStringLiteral("WaveSpec")))
        << "struct missing: " << all;
}

// INV-3 — the shared rule set is not widened for the other languages. A
// modifier is consumed only when a declaration keyword follows, so a C# event
// field (the collision that kept `event` OUT of the keyword list) still
// outlines as it did.
TEST(FileOutlineUnrealScript, Inv3OtherBraceLanguagesUnchanged) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path =
        QFileInfo(dir.path()).canonicalFilePath()
        + QLatin1String("/Widget.cs");
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(R"(public class Widget
{
    public event EventHandler Changed;
    private static int simulated = 0;

    public void Redraw()
    {
        Changed(this, null);
    }
}
)");
    f.close();

    const QJsonObject out =
        FileOutline::compute(path, FileOutline::Mode::Auto, true, 1000);
    const QStringList syms = symbolNames(out);
    const std::string all = syms.join(QStringLiteral(",")).toStdString();
    EXPECT_EQ(out.value(QStringLiteral("language")).toString(),
              QStringLiteral("csharp"));
    EXPECT_TRUE(syms.contains(QStringLiteral("Widget"))) << all;
    // `event EventHandler Changed;` must NOT enter the outline as
    // "EventHandler" — the reason `event` is not a declaration keyword.
    EXPECT_FALSE(syms.contains(QStringLiteral("EventHandler")))
        << "a C# event field outlined as its TYPE: " << all;
}
