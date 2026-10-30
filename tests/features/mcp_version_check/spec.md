# version_check — feature contract (ANTS-5510)

Does every file `.claude/bump.json` lists hold the version its `version_source`
holds, and does a tracked file it does not name hold that version too?
`packaging/check-version-drift.sh` asks the first half for this project alone, from
its own list. This reads the recipe, so it serves any project that has one.

## Invariants

| INV | Statement |
|---|---|
| INV-1 | A listed file holding the source's version is `ok` and draws no finding. *Test:* `McpVersionCheck.Ants5510AgreeingFilesAreClean` |
| INV-2 | A listed file holding another version draws `version_drift` with its line, `found` and `expected`. The same version with a suffix draws `stray_suffix`. *Test:* `McpVersionCheck.Ants5510DriftAndStraySuffix` |
| INV-3 | A pattern matching nothing draws `pattern_not_found`, more than one place without `replace_all` draws `pattern_ambiguous`, and a listed file that cannot be read draws `file_missing`. A pattern is a literal: regex characters in it match themselves. *Test:* `McpVersionCheck.Ants5510PatternOutcomes` |
| INV-4 | A tracked file holding the version that the recipe names nowhere draws `unlisted_version_file` with `candidate:true`. A file the recipe names in a `$` note or a todo, a `.md`, `docs/`, `tests/`, a comment line and a longer version containing it draw nothing. *Test:* `McpVersionCheck.Ants5510UnlistedFiles` |
| INV-5 | No recipe is `ok:true`, `recipe_present:false`, with both checks in `skipped`. No tracked-file list names `unlisted` in `skipped`. Neither reads as clean. *Test:* `McpVersionCheck.Ants5510SilenceIsNamed` |
| INV-6 | A recipe outside the format refuses `bad_recipe`, including bump-recipe.md's foreign-dialect tells. A `version_source` that yields no version refuses `version_unreadable`. *Test:* `McpVersionCheck.Ants5510Refusals` |
| INV-7 | The verb is registered and declared, with `caller_cwd` required. *Test:* `McpVersionCheck.Ants5510VerbIsWired` |
