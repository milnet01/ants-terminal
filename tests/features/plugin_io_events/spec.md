# plugin_io_events — feature conformance

Contract: [`docs/specs/ANTS-4272-plugin-io-events.md`](../../../docs/specs/ANTS-4272-plugin-io-events.md)
§ 3, INV-1 to INV-9. This test holds no invariant of its own; each `TEST`
names the INV it checks.

Plugins can now see terminal output, finished lines, shell prompts and keys.
Output and keys need a permission the user grants once per plugin, and keys
are never passed on while a password is being typed.
