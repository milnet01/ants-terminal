# dependency_inventory — feature contract (ANTS-5515)

What this project pins and where, for the `check-dependencies` skill, which found
these with a hand grep. It reads tracked files and asks no network: finding the
latest version or an end-of-life date stays with the skill.

## Invariants

| INV | Statement |
|---|---|
| INV-1 | Every row carries `kind`, `file` and `line`. A workflow `uses:` is `action` (`repo`, `ref`, `subpath` for a reusable workflow, `version_comment` from a trailing comment), `local_action` (`path`) or `docker_action` (`image`), so third-party refs stay apart. A commented-out line draws no row. *Test:* `McpDependencyInventory.Ants5515UsesFormsStayApart` |
| INV-2 | `runs-on` yields one `runner` row per label, in its scalar, inline-list and block-list forms. *Test:* `McpDependencyInventory.Ants5515RunnerForms` |
| INV-3 | A workflow `container:` or `image:` and a Dockerfile `FROM` yield `container` rows with `image`, `tag` and `digest`. A registry port is not a tag; `scratch` and an earlier stage's alias draw no row. A value holding `$` is `unresolved:true`. *Test:* `McpDependencyInventory.Ants5515ContainerImages` |
| INV-4 | A runtime version names the file it came from: version files, `.tool-versions`, `go.mod`, `requires-python`, `rust-version`, `engines.node`, a workflow's `*-version:`, `cmake_minimum_required` and a Qt `find_package`. *Test:* `McpDependencyInventory.Ants5515RuntimeVersionsNameTheirFile` |
| INV-5 | A marker file with no version anywhere draws a `runtime` row with `version:null` and `no_authoritative_source:true`, never a guess. A version in a README draws nothing. *Test:* `McpDependencyInventory.Ants5515NoSourceIsSaid` |
| INV-6 | No tracked-file list names `tracked_files` in `skipped`, and every reply lists `not_covered`. Vendored folders draw no row. *Test:* `McpDependencyInventory.Ants5515SilenceIsNamed` |
| INV-7 | The verb is registered and declared, with `caller_cwd` required, and its code opens no network. *Test:* `McpDependencyInventory.Ants5515VerbIsWiredAndOffline` |
