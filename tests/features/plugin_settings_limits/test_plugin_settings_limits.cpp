// Feature-conformance test for spec.md — ANTS-5419.
//
//   INV-1 — Config::setPluginSetting refuses over-limit key, value, total.
//   INV-2 — exactly-at-limit is accepted; a replacement counts once.
//   INV-3 — ants.settings.set raises on an over-limit key or value.
//   INV-4 — ants.settings.set raises with the store's refusal message.

#include "config.h"
#include "luaengine.h"

#include "../../_support/xdg_guard.h"

#include <QDir>
#include <QFile>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

const QString kPlugin = QStringLiteral("limits_probe");

QString repeated(char c, qsizetype n) { return QString(n, QLatin1Char(c)); }

}  // namespace

TEST(PluginSettingsLimits, Inv1Inv2ConfigEnforcesLimits) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ants_test::XdgGuard guard;
    guard.setEnv("XDG_CONFIG_HOME", tmp.path().toLocal8Bit());
    QDir().mkpath(tmp.path() + QStringLiteral("/ants-terminal"));

    Config cfg;
    const qsizetype keyMax = Config::kPluginSettingKeyMaxBytes;
    const qsizetype valueMax = Config::kPluginSettingValueMaxBytes;

    // INV-1 — key and value over their limits are refused, nothing stored.
    EXPECT_FALSE(cfg.setPluginSetting(kPlugin, repeated('k', keyMax + 1),
                                      QStringLiteral("v")).isEmpty());
    EXPECT_FALSE(cfg.setPluginSetting(kPlugin, QStringLiteral("big"),
                                      repeated('v', valueMax + 1)).isEmpty());
    EXPECT_TRUE(cfg.pluginSetting(kPlugin, QStringLiteral("big")).isNull());

    // INV-2 — exactly at the key and value limits is accepted.
    EXPECT_TRUE(cfg.setPluginSetting(kPlugin, repeated('k', keyMax),
                                     QStringLiteral("v")).isEmpty());
    EXPECT_TRUE(cfg.setPluginSetting(kPlugin, QStringLiteral("big"),
                                     repeated('v', valueMax)).isEmpty());

    // INV-1 — fill to just under the total, then one more full value
    // crosses it and is refused.
    int n = 0;
    QString refusal;
    while (refusal.isEmpty() && n < 64) {
        refusal = cfg.setPluginSetting(
            kPlugin, QStringLiteral("f%1").arg(n, 2, 10, QLatin1Char('0')),
            repeated('v', valueMax));
        ++n;
    }
    EXPECT_FALSE(refusal.isEmpty()) << "no write was ever refused";
    EXPECT_LT(n, 64);
    const QString refusedKey =
        QStringLiteral("f%1").arg(n - 1, 2, 10, QLatin1Char('0'));
    EXPECT_TRUE(cfg.pluginSetting(kPlugin, refusedKey).isNull())
        << "a refused write must not be stored";

    // INV-2 — at quota, replacing an existing value with one of the same
    // size is still accepted: the old value is not counted twice.
    EXPECT_TRUE(cfg.setPluginSetting(kPlugin, QStringLiteral("big"),
                                     repeated('w', valueMax)).isEmpty());
}

TEST(PluginSettingsLimits, Inv3Inv4LuaSetterRaises) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/limits.lua");
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QStringLiteral(
            "local ok, err = pcall(ants.settings.set, string.rep('k', %1), 'v')\n"
            "ants.log('key:' .. tostring(ok))\n"
            "ok, err = pcall(ants.settings.set, 'k', string.rep('v', %2))\n"
            "ants.log('value:' .. tostring(ok))\n"
            "ok, err = pcall(ants.settings.set, 'k', string.rep('v', %3))\n"
            "ants.log('valueok:' .. tostring(ok))\n"
            "ok, err = pcall(ants.settings.set, 'quota', 'x')\n"
            "ants.log('quota:' .. tostring(ok) .. ':' .. tostring(err))\n")
                    .arg(Config::kPluginSettingKeyMaxBytes + 1)
                    .arg(Config::kPluginSettingValueMaxBytes + 1)
                    .arg(Config::kPluginSettingValueMaxBytes)
                    .toUtf8());
    }

    LuaEngine engine;
    // The permission must be set first: registerApi exposes ants.settings
    // only to a plugin granted it.
    engine.setPluginName(kPlugin);
    engine.setPermissions({QStringLiteral("settings")});
    ASSERT_TRUE(engine.initialize());

    QStringList log;
    QStringList stored;
    QObject::connect(&engine, &LuaEngine::logMessage,
                     [&log](const QString &m) { log.append(m); });
    QObject::connect(&engine, &LuaEngine::settingsSetRequested,
                     [&stored](const QString &, const QString &key,
                               const QString &, QString &error) {
                         if (key == QLatin1String("quota"))
                             error = QStringLiteral("probe-refusal");
                         else
                             stored.append(key);
                     });

    ASSERT_TRUE(engine.loadScript(path)) << log.join('\n').toStdString();

    EXPECT_TRUE(log.contains(QStringLiteral("key:false"))) << log.join('\n').toStdString();
    EXPECT_TRUE(log.contains(QStringLiteral("value:false"))) << log.join('\n').toStdString();
    EXPECT_TRUE(log.contains(QStringLiteral("valueok:true"))) << log.join('\n').toStdString();
    bool quotaRaised = false;
    for (const QString &l : log)
        if (l.startsWith(QStringLiteral("quota:false:"))
            && l.contains(QStringLiteral("probe-refusal")))
            quotaRaised = true;
    EXPECT_TRUE(quotaRaised) << log.join('\n').toStdString();
    // INV-3 — over-limit writes never reached the store.
    EXPECT_EQ(stored, QStringList{QStringLiteral("k")});
}
