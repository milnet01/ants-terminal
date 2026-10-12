# ANTS-5667 — An outer project folder resolves to the repository its `repo_root` names

**Status:** accepted (2026-10-12), review-contract loops 1 + 2 folded, cap reached.
**Kind:** feature.
**Source:** ROADMAP.md ANTS-5667 (dantes-inferno-recomp feedback 2026-10-12; design is the user's decision relayed by that session the same day).
**Covers:** ANTS-5664 (rusty-psn's missing Roadmap button, the same layout), which is blocked by this item.
**Composes with:** ANTS-2160 (`.ants/project.json`), ANTS-4884 (`rcProjectRootFor`), ANTS-5594 (`RoadmapStore::projectIdContaining`), ANTS-1459 (button parity with `findRoadmapUnder`).

**Layman:** Some projects live in a folder of their own, with the git clone one level down. Writing the clone's folder name into the outer folder's settings file makes Ants treat the outer folder as that project: its roadmap, its mail and its Roadmap button all work there.

## 1. Problem

The user keeps a project in an outer folder that is not a git repository, with the registered clone one level down: `Dantes_Inferno_Recomp/hells-gate-recomp/`, and `rusty-psn/rusty-psn-source/`. Sessions start in the outer folder. Nothing that turns a folder into a project looks into a child folder:

1. `rcdetail::findRoadmapUnder` probes the folders `rcdetail::repoBoundedAncestors` returns. With no `.git` at or above the outer folder, that is the outer folder alone. `roadmap_query` refuses `no_roadmap_loaded`.
2. With `"roadmap": "hells-gate-recomp/ROADMAP.md"` in the outer folder's `.ants/project.json`, the file is found but `ownerDir` is the outer folder. `rcProjectRootFor` returns it, `RoadmapSource::migratedProject` finds no store row for it, and the verb reads and writes the markdown file the store renders over. A write is lost at the next render.
3. `RemoteControl::cmdSessionMessage` asks `RoadmapStore::projectIdContaining`, which walks up only, and refuses `no_project`. `hooks/ants-inbox-notice.sh` calls the same verb, and so does the status-bar mail chip (`claudestatuswidgets.cpp`, through `projectIdContaining`).
4. `MainWindow::refreshRoadmapButton` probes a fixed list of paths under the tab's folder. It neither reads `.ants/project.json` nor walks up, so it has drifted from `findRoadmapUnder` (ANTS-3350 and ANTS-2160 widened only the latter), and it hides the button in both layouts.

## 2. Surface

### 2.1 The key

`.ants/project.json` gains one optional string key:

```json
{ "repo_root": "hells-gate-recomp" }
```

It names, relative to the folder holding the file, the folder the roadmap store and git know this project by. The outer folder stays the project's home: its path keys (`source_roots`, `docs_dir`, …) are still resolved against it, so the outer folder's own files, such as `tools/`, stay in the project. The roadmap and `id_format` are the exceptions: both are taken from the repo root (§ 2.3), so every caller of one store row reads one roadmap and one prefix.

`ProjectSettings::Settings` gains `std::optional<QString> repoRoot`, parsed by the existing `validEntry`: a value whose path does not exist, or fails `PathValidation::isInsideProject`, is dropped, as every other path key is.

### 2.2 The resolver

```cpp
namespace ProjectSettings {
// The folder `dirCanonical` stands for in the roadmap store: the canonical
// directory its .ants/project.json `repo_root` names, or `dirCanonical`
// unchanged when the key is absent, dropped, or names something that is not a
// directory.
QString repoRootOf(const QString &dirCanonical);
}
```

It reads only the folder it is given. It does not walk: each caller passes the folder it has already resolved, so the rule each caller uses to reach that folder is unchanged.

### 2.3 Who calls it

- **`rcdetail::findRoadmapUnder`.** A probed folder whose `repoRootOf(dir)` differs from `dir` is probed through its repo root instead: the repo root's own `roadmap` key, then its candidate list. The folder's own `roadmap` key and candidates are not probed, and `ownerDir` is the repo root. `ownerDir` is the store key (ANTS-4882), and every caller that passes it is reached without a change of its own: `rcProjectRootFor`, `rcWorktreeMainCheckout` (the ANTS-4953 worktree-write refusal) and `roadmap_log`'s `projectRootDir`. A `workspace_search` for `findRoadmapUnder(` with a second argument lists them.
- **`roadmap_query`'s `check_sync`** passes `rcProjectRootFor(callerCanonical)` to `rcStampDriftFields`, not `callerCanonical`: `RoadmapWrite::measureDrift` renders the store against the root it is given.
- **`rcdetail::rlStoreCounterPrefix`** maps its caller folder through `repoRootOf` on entry. It hands that folder to `ProjectSettings::idFormatFor` and to its directory-default fallback, `rlResolveCounterPrefix`, so from the outer folder both see the repo root.
- **`ProjectSettings::idFormatFor(dir)`** reads the settings file at `repoRootOf(dir)`, for its other callers.
- **`RemoteControl::cmdSessionMessage`** passes `repoRootOf(rr.cwd)` to `projectIdContaining`.
- **`session_orient`'s `mail_pending` block** passes `repoRootOf(rootCanonical)` to `projectIdForRoot`.
- **The status-bar mail chip** (`ClaudeStatusBarController::refreshMailChip`) passes `repoRootOf(cwd)` to `projectIdContaining`. `ants_claude_lib` gains `PRIVATE ants_mcpcore_lib`, where `ProjectSettings` lives.
- **`hooks/ants-inbox-notice.sh`** is unchanged. A non-git outer folder skips its `git rev-parse` step and reaches `session_message`, which now resolves it.

### 2.4 The Roadmap button

`MainWindow::refreshRoadmapButton` calls `rcdetail::findRoadmapUnder` on the tab's canonical folder and drops its own candidate list. It then shows the button exactly where `roadmap_query` finds a roadmap, including a project subfolder and a `roadmap` key. This replaces ANTS-1459 RQ-4, which kept two copies of the list equal by a source check; the copies drifted anyway.

### 2.5 `project_settings`

`op:"set"` accepts `repo_root` and refuses `bad_path` when the value does not name an existing directory inside the folder. `op:"get"` reports it. `op:"detect"` and `op:"init"` never propose it: whether an outer folder stands for a child repository is the user's call.

### 2.6 Reload

`.ants/project.json` is read on every call, uncached, so adding the key takes effect on the next verb call. The verb changes ship in `ants-mcpd` (rebuild, then `/mcp`). The button and the mail chip are in the terminal and need a relaunch for the code. After that, the button re-reads the file on every status tick. The mail chip caches its project per folder and clears that cache when the store file changes, so a newly added key reaches it at the next store write.

### 2.7 Alternatives

- **Resolve a non-git folder holding exactly one registered child to that child.** Rejected by the user's decision for an explicit key. A guess also changes what an unrelated parent folder resolves to, and turns into a refusal the day a second clone lands beside the first.
- **Make the clone the project for everything.** Rejected: the outer folder's own paths (`tools/`) would leave the project, and the user wants the outer file to hold the project's settings.
- **Register the outer folder in the store instead.** Rejected: the store renders `ROADMAP.md` at the registered root, and the worktree and git logic (`rcMainCheckoutOf`) key on the repository.
- **Keep a separate button probe with a walk and the settings file added.** Rejected: that is the drift that caused ANTS-5664.

## 3. Invariants

- **INV-1** — `repoRootOf(dir)` returns the canonical directory `dir`'s `repo_root` names when it exists inside `dir`, and `dir` unchanged otherwise. Broken by following a value that escapes `dir` or names a file. *Test:* `ProjectSettings.RepoRootOf` over temporary folders: a child folder → the child; no file, no key, `"../x"`, an absolute path outside, a symlink pointing outside, a missing path and a regular file → `dir`.
- **INV-2** — `findRoadmapUnder` on a folder declaring `repo_root` returns the repo root's roadmap, with `ownerDir` the repo root, whether or not the folder holds a roadmap of its own. Broken by probing the folder's own roadmap first, or by leaving `ownerDir` at the folder. *Test:* `ProjectRepoRoot.FindRoadmapUnder` with an outer fixture holding only the key, then the same fixture with its own `ROADMAP.md`: both return the clone's file, with the clone as `ownerDir`.
- **INV-3** — From the outer folder, `rcProjectRootFor` returns the repo root, so `roadmap_query` answers with `source:"store"` and a `roadmap_log` append lands in the store row registered at the repo root. Broken by keying the store lookup on the outer folder. *Test:* `ProjectRepoRoot.RoadmapVerbsUseStore` with a throwaway store registered at the fixture clone: `roadmap_query` from the outer folder reports `source:"store"`, and an append from it is read back from the clone.
- **INV-4** — `session_message` from the outer folder reaches the repo root's mailbox: a message sent to the clone's slug is listed by `op:"inbox"` from the outer folder, and an ack there clears it for the clone. Broken by resolving the mailbox from the outer folder unmapped. *Test:* `ProjectRepoRoot.MailboxFromOuterFolder` with a throwaway store.
- **INV-5** — `session_orient`'s `mail_pending` and the status-bar mail chip key their store lookup on `repoRootOf` of their folder. Broken by a mail surface that disagrees with `session_message` from the outer folder. *Test:* `ProjectRepoRoot.MailPendingFromOuterFolder` for `session_orient`, and a source check that `ClaudeStatusBarController::refreshMailChip` passes its folder through `ProjectSettings::repoRootOf` before `projectIdContaining`.
- **INV-6** — `refreshRoadmapButton` decides visibility by `findRoadmapUnder` alone and holds no candidate list. Broken by a second probe that can drift. *Test:* `ProjectRepoRoot.ButtonUsesFindRoadmapUnder`, a source check on the function body; `tests/features/roadmap_viewer` INV-6 still passes.
- **INV-7** — `project_settings op:"set"` writes a valid `repo_root`, refuses `bad_path` for one that is missing, outside the folder or a file, and `op:"get"` reports it; `detect` never proposes it. Broken by accepting a value `repoRootOf` would ignore. *Test:* `ProjectRepoRoot.ProjectSettingsKey`.
- **INV-8** — A folder whose settings carry no `repo_root` resolves exactly as before through every surface in § 2.3. Broken by any mapping that fires without the key. *Test:* `ProjectRepoRoot.NoKeyUnchanged`: inside a fixture clone with no key, `rcProjectRootFor`, `findRoadmapUnder` and the mailbox lookup return what they return on the unchanged code; the existing `roadmap_subdir_dispatch` and `session_message` suites stay green.
- **INV-9** — From a folder declaring `repo_root`, `idFormatFor` returns the repo root's `id_format` and ignores the folder's own. Broken by reading the caller's folder, which lets two callers of one store row allocate under two prefixes. *Test:* `ProjectSettings.IdFormatFollowsRepoRoot`: an outer fixture declaring prefix `XX` and `repo_root` naming a clone that declares `HG` returns `HG`. A second case in `ProjectRepoRoot.CounterPrefixFromOuterFolder`: with no `id_format` in either file and no stored `id_prefix`, `rlStoreCounterPrefix` returns the same prefix from the outer folder as from the clone.

## 4. RAM / build cost

`repoRootOf` is one small file read per call. `ants_claude_lib` gains one link edge to `ants_mcpcore_lib`; no new translation unit beyond the test.

## 5. Out of scope

- A caller in a subfolder of the outer folder that is itself not a git repository (`Dantes_Inferno_Recomp/tools/`): no walk reaches the outer folder's settings from there.
- `git_state` and the other verbs that run git on the caller's folder. From the outer folder they still see no repository.
- Proposing `repo_root` automatically, in `detect` or in `align-project`.
- Removing the clone's own `.ants/project.json`. Its `roadmap` and `id_format` are what the outer folder reads (§ 2.3).

## 6. Tests

`tests/features/project_repo_root/` holds this spec's tests, label `features;fast`. INV-1 and INV-9 live with the existing `ProjectSettings` tests in `tests/features/project_settings/`. Each test except INV-8's fails on the pre-change code before the change is restored; INV-8 asserts that nothing changed.

| INV | Covered by |
|---|---|
| INV-1 | `ProjectSettings.RepoRootOf` |
| INV-2 | `ProjectRepoRoot.FindRoadmapUnder` |
| INV-3 | `ProjectRepoRoot.RoadmapVerbsUseStore` |
| INV-4 | `ProjectRepoRoot.MailboxFromOuterFolder` |
| INV-5 | `ProjectRepoRoot.MailPendingFromOuterFolder`, and its source check |
| INV-6 | `ProjectRepoRoot.ButtonUsesFindRoadmapUnder` |
| INV-7 | `ProjectRepoRoot.ProjectSettingsKey` |
| INV-8 | `ProjectRepoRoot.NoKeyUnchanged` |
| INV-9 | `ProjectSettings.IdFormatFollowsRepoRoot`, `ProjectRepoRoot.CounterPrefixFromOuterFolder` |

## 7. Cross-doc impact

- `docs/specs/ANTS-1459.md` RQ-4 is superseded by INV-6. `McpLastAuditSummary.Ants1459StatusBarRoadmapButtonWidened` becomes a check that the button calls `findRoadmapUnder`.
- `docs/specs/ANTS-2160.md` lists `repo_root` beside the other `.ants/project.json` keys. `docs/standards/mcp-config-keys.md` gains a `## Per-project repo_root` entry in the form of its `## Per-project id_format (ANTS-3771)` entry, and that entry says `id_format` is read from the repo root.
- The `project_settings` tool description names the key.
- `CLAUDE.md` § Conventions lists `repo_root` among the per-project layout keys.
- The CHANGELOG describes the outer-folder layout. ANTS-5664 closes with this item.

## Cold-eyes loop log

The rows are in [`../reviews/ANTS-5667-outer-folder-repo-root-loop-log.md`](../reviews/ANTS-5667-outer-folder-repo-root-loop-log.md).
