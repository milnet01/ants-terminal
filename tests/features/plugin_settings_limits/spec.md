# Feature: plugin settings have size limits

ANTS-5419. A plugin with the `settings` permission stores strings with
`ants.settings.set(key, value)`. They persist in `config.json` under
`plugin_settings.<plugin>`. Without limits, one plugin can grow that file
without bound.

## Contract

The limits are `Config::kPluginSettingKeyMaxBytes` (256),
`Config::kPluginSettingValueMaxBytes` (64 KiB) and
`Config::kPluginSettingsTotalMaxBytes` (1 MiB). All three count UTF-8
bytes. The total is the sum of every key and value one plugin stores,
counted with the new value in place of the old one.

- **INV-1** — `Config::setPluginSetting` refuses a key over the key limit,
  a value over the value limit, and a write that would take the plugin's
  total over the total limit. A refusal returns a non-empty message and
  leaves the stored settings unchanged. An accepted write returns an empty
  string.
- **INV-2** — a write at exactly a limit is accepted. Replacing an existing
  key's value counts the new value, not both.
- **INV-3** — `ants.settings.set` raises a Lua error when the key or value
  is over its limit, before anything leaves the plugin's thread.
- **INV-4** — `ants.settings.set` raises a Lua error carrying the store's
  message when the store refuses the write. The plugin can catch it with
  `pcall`.

## Test

`test_plugin_settings_limits.cpp`, in the `test_lua` bundle. INV-1 and
INV-2 drive a `Config` in a temporary `XDG_CONFIG_HOME`. INV-3 and INV-4
run a script in a `LuaEngine` granted `settings`, with
`settingsSetRequested` answered by a lambda in the test.
