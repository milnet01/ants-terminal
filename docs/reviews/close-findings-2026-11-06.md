# close-findings ledger — 2026-11-06 reviews

The disposition of every finding from two cold reviews of this tree on
2026-11-06: `review-code` (two passes) and `review-readability`. Each
finding is fixed, or queued on the roadmap under the id named here. This is
a dated record: line numbers are as the reviewers read them on that date.
A remedy is the reviewer's suggestion, not a verified fix; check both the
finding and the remedy against current code before building it.

## Fixed in this run

- ANTS-6432 — `versioncheck.cpp:211` file read outside the root (review-code lane 13).
- ANTS-6433 — `claudestatuswidgets.cpp:2290` auto-Enter on output (lane 30). Follow-up ANTS-6437.
- ANTS-6434 — `remotecontrol_feedback.cpp:1136`, `remotecontrol_changelog.cpp:118` and lane 11's
  `resolveSpecRelForId`: write through a symlink. The `.ants/project.json` override part was
  already contained by `ProjectSettings::load`.
- ANTS-6435 — `claudeallowlist.cpp:494` sudo -A rm generalisation (lane 30).
- ANTS-6436 — `terminalwidget.cpp:5844` tty_nr read as tpgid, and the readability comment at
  `:5839` (readability lane terminal-widget-b).

## Run record

- **swept:** `mcp-error-codes.md`, `mcp-config-keys.md`, `mcp-behavioural-notes.md` fixed;
  `doc_lint`'s `version_source` read agrees (it already runs `validatePath`); the tests'
  own contracts updated with each fix.
- **collateral:** `src/procstat.h` had no review lane; added to terminal-widget.
- **out of scope:** `mcp-config-keys.md` says the user-switch auto-confirm sends no
  continuation (ANTS-6438); three more `/proc` stat parsers (ANTS-6439).
- **surfaced, falsified:** none.

## Second run — roadmap section security-2026-11-06-review

- **Fixed:** ANTS-6440, 6442, 6443, 6445 (python3 fallback, user's choice),
  6446, 6447, 6448. Each commit names its red run.
- **Queued:** ANTS-6441 with ANTS-6444. The user chose a key file
  (2026-11-06); the design note is on ANTS-6441. It touches four subsystems,
  so it gets a short spec first.
- **Out of scope:** ANTS-6695 (compile-DB plugin flags), ANTS-6696 (checkov
  and luacheck config files).
- **Collateral:** `src/bracketedpaste.h` added to the terminal-widget lane.
- **swept:** not yet. The group's one sweep runs after ANTS-6441/6444 land.

## Security

### Row 1 — High — `vtparser.cpp:240-241`

review-code, lane 01-vt-parser-pty, dim 3. **Disposition:** queued, ANTS-6440.

One extra byte gets around the ANTS-1777 hardening against ESC sequences that end a string.

Quote: `if (m_state == OscString || m_state == DcsString || m_state == ApcString || m_state == IgnoreString) {`

Remedy: add the four `*Esc` states to the condition at line 240, so that an ESC in a peek state goes to `processString` and is discarded to Ground. Add `ESC ESC c` and `ESC ESC [2J` cases to the INV-5/INV-6 tests.

Also reported as: "readability lane 01-vt-parser-pty Info correctness lead src/vtparser.cpp:235-245"

### Row 8 — High — `terminalgrid.cpp:174–176`

review-code, lane 02-terminal-grid, dim 3. **Disposition:** queued, ANTS-6441.

The verifier's key is an exported environment variable. The README setup puts it in `~/.profile` / `~/.zshenv` as `export ANTS_OSC133_KEY=…`, and neither hook script un-exports it (searched `packaging/shell-integration/` for `export -n`, `typeset +x` and `unset`: no hits).

Quote: `if (const char *envKey = std::getenv("ANTS_OSC133_KEY")) m_osc133Key = QByteArray(envKey);`

Remedy: have the hook take the key into a shell-local variable and `export -n` / `typeset +x` it. Alternatively, hand the key to the shell through an inherited file descriptor rather than the environment.

### Row 31 — High — `terminalwidget.cpp:2996-3001`

review-code, lane 03-terminal-widget-a, dim 3. **Disposition:** queued, ANTS-6442.

the bracketed-paste sanitiser can be bypassed by nesting.

Quote: `sanitized.replace("\x1B[201~", "");`

Remedy: repeat the replacements until nothing changes, or strip every ESC, 0x9B and `C2 9B` byte from the payload before wrapping it.

### Row 238 — High — `sshdialog.cpp:94-105`

review-code, lane 22-app-entry-dialogs, dim 3. **Disposition:** queued, ANTS-6443.

the denylist leaves out `XAuthLocation`, and `-X`/`-Y` (or `-o ForwardX11=yes`) pass the filter.

Quote: `static const QStringList kDangerous = { … ProxyCommand, LocalCommand, … SecurityKeyProvider };`

Remedy: add `XAuthLocation` to `kDangerous`, and review the remaining path- or program-valued keywords as a class (or switch to an allowlist).

### Row 270 — Medium — `packaging/shell-integration/README.md:32-33`

review-code, lane 24-shell-surface, dim 3. **Disposition:** queued, ANTS-6444.

"Untrusted inner processes don't [know the key], so they cannot mint a valid HMAC."

### Row 271 — Medium — `packaging/shell-integration/ants-osc133.bash:68`

review-code, lane 24-shell-surface, dim 3. **Disposition:** queued, ANTS-6445.

The fallback signer puts the secret in argv, where any local user can read it with `ps` on every prompt draw. This is documented (`README:99-105`), but it is still a runtime secret leak on the AppImage and Flatpak installs, where the helper is absent.

Quote: `ants-osc133.zsh:59`

Remedy: Ship `ants-osc133-sign` in the AppImage and Flatpak bundles. Failing that, disable signing rather than fall back to argv.

### Row 290 — High — `auditdialog_catalogue.cpp:1349`

review-code, lane 26-audit-dialog, dim 3. **Disposition:** queued, ANTS-6446.

(pylint) and `hasCargo, hasCargo` (cargo_clippy) are auto-selected. `cargo clippy` runs the project's `build.rs` and proc-macros.

Quote: `:1416`

Remedy: default these lanes off, or gate every lane that runs project code behind the same per-project trust check.

### Row 308 — High — `auditrunner.cpp:373-376`

review-code, lane 27-audit-engine, dim 3. **Disposition:** queued, ANTS-6447.

the argv-injection guard only defuses a leading `-`. `isAuditArgSafe` (`:343`, `^[A-Za-z0-9._/=:,+@~\-]+$`) allows a leading `@`.

Quote: `return p.startsWith(QLatin1Char('-')) ? QStringLiteral("./") + p : p;`

Remedy: prefix `./` for a leading `@` as well (or reject `@` in `isAuditArgSafe`).

### Row 313 — Medium — `auditrunner.cpp:1378-1383`

review-code, lane 27-audit-engine, dim 3. **Disposition:** queued, ANTS-6448.

. The ANTS-1446 escape check misses `-imacros`, `-idirafter`, `--include=`/`--include`, `-include-pch`, `-iwithprefix` and `--sysroot`. A hostile compile DB line such as `-imacros /home/u/.ssh/id_rsa` passes validation. fix: extend the flag set, covering `--` and `=` forms.

Quote: `kFlags = {"-I","-isystem","-iquote","-include"}`

Remedy: extend the flag set, covering `--` and `=` forms.

## Path containment

### Row 136 — Low — `remotecontrol_docs.cpp:233-246`

review-code, lane 12-mcp-content-verbs-a, dim 3. **Disposition:** queued, ANTS-6449.

The doc walk used by `doc_integrity`, `doc_symbols`, `spec_lint`, `doc_dedup` and `doc_lint` has no `QDir::NoSymLinks`. A symlinked `*.md` therefore yields a file outside the root, and it is read; `doc_symbols`' locator returns lines from it.

Quote: `QDirIterator it(dirAbs, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, …)`

Remedy: add `QDir::NoSymLinks`, or check each yielded path with `isInsideProject`.

### Row 139 — Medium — `dependencyinventory.cpp:29-30`

review-code, lane 13-mcp-content-verbs-b, dim 3. **Disposition:** queued, ANTS-6450.

a tracked file that is a symlink is followed with no check that its target is under the root.

Quote: `QFile f(root + QLatin1Char('/') + rel); if (f.size() > kMaxFileBytes || !f.open(QIODevice::ReadOnly)) return {};`

Remedy: skip any tracked path whose canonical path is not under `root`.

### Row 150 — High — `roadmapstore.cpp:451`

review-code, lane 14-roadmap-store, dim 9. **Disposition:** queued, ANTS-6451.

with `remotecontrol_roadmap_migrate.cpp:253` `r.backupTo = req.value(QStringLiteral("backup_to")).toString();` (comment: "Not validated against the project root, deliberately"), and `roadmapmigrateverb.cpp:879-881` `backupPath = req.backupTo; … taken = store.snapshotTo(backupPath, &backupErr);`.

Quote: `if (QFile::exists(destPath) && !QFile::remove(destPath)) {`

Remedy: refuse a `destPath` whose canonical path equals `m_path` or its `-wal`/`-shm`. Refuse to replace an existing database that is not a previous `pre-migrate`/`backup_to` snapshot, for example by requiring the `.sqlite` name or a marker.

### Row 158 — Low — `roadmapmigrate.cpp:1014-1026`

review-code, lane 14-roadmap-store, dim 3. **Disposition:** queued, ANTS-6452.

the live roadmap (declared or root) is opened through `QFileInfo` and `QDir::Files`, which follow symlinks. Archives explicitly refuse symlinks (line 1078); the live file does not, so a symlinked `ROADMAP.md` can import a file from outside the project into the machine-global store. The comment at 998-1000 relies on that symlink behaviour, so this may be intended. Recorded as an open question, not…

Quote: `QFileInfo`

### Row 159 — Medium — `commentclaims.cpp:307`

review-code, lane 15-code-index-search-a, dim 3. **Disposition:** queued, ANTS-6453.

comment_claims can read files outside the project.

Quote: `if (QFileInfo::exists(root.filePath(p))) return QDir::cleanPath(p);`

Remedy: reject a `..`/absolute result, or a canonical path outside the root, in both branches of `resolve()`.

### Row 171 — Low — `functionsizes.cpp:166`

review-code, lane 15-code-index-search-a, dim 3. **Disposition:** queued, ANTS-6454.

Several files are opened again after `PathValidation`/walk time with no new containment check (`functionsizes.cpp:166`, `duplicateblocks.cpp:310`, `codebaseindex.cpp:205`). `security.md` § 3 asks for that check inside every open. The race needs a symlink swapped inside the user's own tree.

Quote: `functionsizes.cpp:166`

### Row 176 — Medium — `trace.cpp:336-342`

review-code, lane 16-code-index-search-b, dim 2. **Disposition:** queued, ANTS-6455.

Bug 1 — empty path accepted. `validatePath` returns `bad=false` for an empty path (`pathvalidation.cpp:223`). So `path:""`, or a non-string `path` (whose `toString()` is `""`), is accepted, which breaks INV-1's "exactly one of" rule.

Quote: `const QString abs = QDir::cleanPath(QDir(root).absoluteFilePath(raw)); req.pathRel = abs.mid(prefixLen);`

Remedy: refuse an empty or non-string `path` with `bad_args`. Derive `pathRel` from `check.resolved`.

### Row 177 — Low — `symbolquery.cpp:1016-1020`

review-code, lane 16-code-index-search-b, dim 3. **Disposition:** queued, ANTS-6456.

A `lane` that is a symlink to a directory outside the root passes the check (`isDir` follows the link), and the walk then runs outside the root.

Quote: `if ((cand == scanRoot || cand.startsWith(scanRoot + '/')) && QFileInfo(cand).isDir()) scanRoot = cand;`

Remedy: canonicalise `cand` and re-check it against the root, or refuse when `QFileInfo(cand).isSymLink()`.

### Row 191 — Low — `src/tokenusageengine.cpp:340-347`

review-code, lane 17-mcp-infra, dim 3. **Disposition:** queued, ANTS-6457.

The symlink check and the open are separate steps (a check-then-use race, TOCTOU).

Quote: `if (fi.isSymLink()) … QFile f(path); f.open(...)`

Remedy: `open(O_NOFOLLOW)`, then `fstat`.

### Row 216 — Medium — `pathvalidation.cpp:275`

review-code, lane 20-shared-utilities, dim 3. **Disposition:** queued, ANTS-6458.

A path to a missing file can still escape the root through a symlink.

Quote: `const QString cleaned = QDir::cleanPath(joined);`

Remedy: refuse `..` segments in the missing-path branch (or resolve prefixes one component at a time without the text cleanup), and return the landing path in `Check` for callers to write to.

### Row 269 — Medium — `hooks/ants-drift-check.sh:59`

review-code, lane 24-shell-surface, dim 3. **Disposition:** queued, ANTS-6459.

The temp file has a fixed name in a repository working tree, and the redirect follows a symlink.

Quote: `printf '%s\n' "$out" > "$marker.tmp"`

Remedy: Create the temp file with `mktemp "$project_root/.ants-audit-pending.XXXXXX"`, or refuse when `[ -L "$marker.tmp" ]` or `[ -L "$marker" ]`.

### Row 336 — Low — `roadmapfoldin.cpp:131`

review-code, lane 28-roadmap-parse-render, dim 3. **Disposition:** queued, ANTS-6460.

The symlink check (`counterStaysInProject`) runs before the open, and the open does not use `O_NOFOLLOW`. A symlink swapped in between is followed.

Quote: `::open(utf8.constData(), O_RDWR | O_CREAT, 0644)`

Remedy: add `O_NOFOLLOW`.

### Row 385 — Medium — `indiereviewengine.cpp:1537-1538`

review-code, lane 34-review-engines, dim 3. **Disposition:** queued, ANTS-6461.

`assembleThreatModelExtras` reads `CLAUDE.md`, `SECURITY.md` and `.semgrep.yml` with no check that their real location is inside the project. A repo whose `CLAUDE.md` is a symlink to any file the user can read (for example `~/.config/ants-terminal/config.json`) has that file's contents put in the synthesis prompt and sent to the AI endpoint.

Quote: `const QString abs = projectPath + QChar('/') + relPath; const QString body = slurpUtf8(abs);`

Remedy: run each path through the same `canonicalFilePath().startsWith(rootCanon + '/')` check.

### Row 407 — Low — `verifyengine.cpp:565-570`

review-code, lane 35-test-audit-verify, dim 11. **Disposition:** queued, ANTS-6462.

the walk follows directory symlinks. The `visited` set stops loops, but a link to `/` or `$HOME` still walks and reads all of it.

Quote: `QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)`

Remedy: add `QDir::NoSymLinks`, or require the canonical path to stay under the root.

### Row 1329 — Info — `src/auditengine.cpp:310`

review-readability, lane 11-audit-engine-b, correctness lead. **Disposition:** queued, ANTS-6463.

`resolveProjectPathLocal` has no project containment check. A tool output line like `./../../x.h:N:` makes `applyFilter` read a file outside the project (only to search it for words).

### Row 1402 — Info — `src/remotecontrol_workspace.cpp:4069`

review-readability, lane 26-mcp-state-workspace-a, correctness lead. **Disposition:** queued, ANTS-6464.

- `src/remotecontrol_workspace.cpp:4069`: `rel.startsWith(rootCanonical)` without a trailing `/` could strip a sibling-prefix path ("/a/b" vs "/a/bc"). It is probably unreachable after validatePath.

## Settings and records that a write can lose

### Row 60 — Medium — `mainwindow_tabs.cpp:554-558`

review-code, lane 05-mainwindow, dim 2. **Disposition:** queued, ANTS-6465.

this writes `config.json` through a second `Config` object. That breaks `docs/standards/config-hot-reload.md` C1/C2, which require writes to go through the same object the watcher reads. Two effects:

Quote: `Config cfg; cfg.setClaudeAutoModelNudgeShown(true); … cfg.setClaudeAutoModelSwitch(true);`

Remedy: use `m_config` setters.

### Row 99 — Medium — `remotecontrol_roadmap_query_verb.cpp:4072`

review-code, lane 09-mcp-roadmap-query-log, dim 8. **Disposition:** queued under ANTS-5420.

Markdown-path writes have no writer exclusion, so concurrent writes lose updates.

Remedy: take an exclusive per-root hold, or an `flock` on ROADMAP.md, around the read-modify-write on the markdown paths.

### Row 100 — Medium — `remotecontrol_roadmap_log.cpp:485-487`

review-code, lane 09-mcp-roadmap-query-log, dim 8. **Disposition:** queued, ANTS-6466.

Store append allocates its id and runs the duplicate check outside the transaction.

Remedy: re-read `allocationFloor` and redo the duplicate check inside `mutate`.

### Row 129 — Low — `apply_edits`

review-code, lane 11-mcp-state-workspace, dim 9. **Disposition:** queued, ANTS-6467.

has no check for a concurrent edit between the read (`:3328`) and `commit` (`:3404`). `mutation_probe` has one (RC-33). An edit made in that window is overwritten.

Quote: `:3328`

### Row 131 — Medium — `remotecontrol_changelog.cpp:1368-1421`

review-code, lane 12-mcp-content-verbs-a, dim 9. **Disposition:** queued, ANTS-6468.

(and `:1667`→`:1833`, `clAmend`, `release`, `normalize`) — `clReadFile(clPath, &clMarkdown)` … `clAtomicWrite(clPath, utf8)` every `changelog_log` op reads, edits and writes the file with no guard against a second writer. `remotecontrol_ci.cpp:122` itself says each Claude session runs its own `ants-mcpd`. So two sessions on one project can both append; the later rename silently drops the other en…

Quote: `:1667`

Remedy: take an `flock` on a sidecar file (the same shape as `ci`'s `LiveLock`) around read → write, and refuse `busy` when it cannot be taken.

### Row 145 — Medium — `runtraceverb.cpp:558-579`

review-code, lane 13-mcp-content-verbs-b, dim 9. **Disposition:** queued, ANTS-6469.

`finish` reads, changes and rewrites `gate-log.md` and `run-costs.tsv` (`:333-363`, `:574`) with no lock.

Quote: `ix.lines.insert(rowLine, row); ... if (!writeText(indexPath(p), indexText, &err))`

Remedy: take a `QLockFile` beside the index around the read-modify-write.

### Row 187 — Low — `src/mcpundeclaredargs.cpp:39-43`

review-code, lane 17-mcp-infra, dim 9. **Disposition:** queued, ANTS-6470.

After the timeout, the read-modify-write goes ahead without the lock. The header (`mcpundeclaredargs.h:33-34`) says the lock is held because the terminal and `ants-mcpd` both append. So when the two compete, records are lost.

Quote: `ConfigWriteLock lock(path, ConfigWriteLock::kGuiTimeoutMs);`

Remedy: skip the append when the lock is not acquired; tell "missing" apart from "unreadable" in `readLines`.

### Row 193 — Low — `src/mcporientation.cpp:249-381`

review-code, lane 17-mcp-infra, dim 9. **Disposition:** queued, ANTS-6471.

`~/.claude/settings.json` is read, changed and rewritten with no lock against a live Claude Code writing the same file.

### Row 204 — Medium — `secureio.h:147`

review-code, lane 19-config-session-project, dim 9. **Disposition:** queued, ANTS-6472.

the return value is ignored, and so are errors at close. The code then renames the temp file over the target anyway.

Quote: `::fsync(file.handle());`

Remedy: if `fsync` returns non-zero, take the `fail()` path and remove the temp file.

### Row 205 — Medium — `config.cpp:194-201`

review-code, lane 19-config-session-project, dim 9. **Disposition:** queued, ANTS-6473.

the comment says flock stops "last-rename-wins silently drops one process's … settings changes".

Remedy: inside the lock, re-read the file and merge only the keys this process changed. Otherwise, correct the comment and `configbackup.h:73-80`.

### Row 228 — Medium — `selfupdate.cpp:304-315`

review-code, lane 21-dialog-chrome-theme, dim 9. **Disposition:** queued, ANTS-6474.

there is no `fsync` of the temp file before the rename, and none of the directory after it. If the machine crashes or loses power after the rename, `$APPIMAGE` can be left empty or truncated on filesystems without ext4's rename heuristic. That file is the user's only copy of the program. `verifytrust.cpp:353` already does the parent-directory sync for a much smaller file. — fix: `::fsync(m_file->…

Quote: `if (!m_file->flush()) … std::rename(…m_tempPath…, …m_targetPath…)`

Remedy: `::fsync(m_file->handle())` before `close()`, and `fsyncParentDir(m_targetPath)` after the rename.

### Row 231 — Medium — `verifytrust.cpp:332-352`

review-code, lane 21-dialog-chrome-theme, dim 9. **Disposition:** queued, ANTS-6475.

every writer uses the same temp file name, opens it with `Truncate`, and runs reload → modify → save with no lock between processes. With two Ants instances (brief dim 16 case), one can rename the other's half-written temp file. The next load then sees corrupt JSON and moves the file aside, so every stored trust is dropped. Or one instance's save silently wipes the other's new entry. Both failure…

Quote: `const QString tmp = m_path + QStringLiteral(".tmp");`

Remedy: write through a unique temp file (`QSaveFile` or `QTemporaryFile` in the same directory), and take an `flock` on a lock file around reload-modify-save.

### Row 239 — Medium — `claudesetup.cpp:153-156`

review-code, lane 22-app-entry-dialogs, dim 9. **Disposition:** queued, ANTS-6476.

a file that exists but cannot be read looks the same as a missing file.

Quote: `return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();`

Remedy: have `readBytes` report failure separately from absence; refuse to write when the file exists but cannot be read.

### Row 258 — Medium — `modelswitchledger.cpp:158-161`

review-code, lane 23-model-switching, dim 9. **Disposition:** queued, ANTS-6477.

the comment's claim is false; another instance's append can be lost.

Quote: `// ANTS-1989 — same lock as appendRecord so a full rewrite (outcome backfill) can't interleave with a concurrent append's read-modify-write.`

Remedy: take `ConfigWriteLock` around the read as well (e.g. a locked `updateRecords(path, fn)` helper), or re-read and merge inside the lock before writing.

### Row 291 — Medium — `auditdialog.cpp:1133–1139`

review-code, lane 26-audit-dialog, dim 9. **Disposition:** queued, ANTS-6478.

If `.audit_allowlist.json` fails to parse, the code writes back only the one new entry. Every existing entry is silently lost.

Quote: `if (doc.isObject()) { … }`

Remedy: refuse the write on a parse failure, and hold the lock across the read and the write.

### Row 304 — Low — `auditdialog.cpp:1706`

review-code, lane 26-audit-dialog, dim 9. **Disposition:** queued, ANTS-6479.

a corrupt `trend.json` is replaced, losing its history. `sf.write` is unchecked.

Quote: `trend.json`

### Row 314 — Medium — `auditrulequality.cpp:234`

review-code, lane 27-audit-engine, dim 9. **Disposition:** queued, ANTS-6480.

. A whole-file rewrite with no lock. Two dialogs open on one project each load at construction and save on close, so the last writer wins and the other's suppression history is lost. fix: take `ConfigWriteLock` and merge on save, as `auditfpledger` does.

Quote: `QSaveFile f(m_path);`

Remedy: take `ConfigWriteLock` and merge on save, as `auditfpledger` does.

### Row 321 — Low — `auditcache.cpp:116-121`

review-code, lane 27-audit-engine, dim 17. **Disposition:** queued, ANTS-6481.

a newer-version `index.json` is treated as empty, then overwritten as v1. Its history is lost and its artefacts orphaned on downgrade. `auditrulequality.cpp:266` likewise ignores `schema_version` and rewrites the file as v2.

Quote: `:293`

### Row 326 — Medium — `roadmapfoldin.cpp:196-204`

review-code, lane 28-roadmap-parse-render, dim 8. **Disposition:** queued, ANTS-6482.

The stale-lock steal checks the lock's age, then renames whatever file is at that path. Those are two separate steps.

Quote: `if (mtime.secsTo(QDateTime::currentDateTime()) < kStaleLockSeconds) return false; … ::rename(lockPath…, aside…)`

Remedy: rename to a unique name (add pid and a random suffix). Re-check the moved file's age. If it is fresh, restore it and wait.

### Row 330 — Medium — `roadmapwrite.cpp:860-872`

review-code, lane 28-roadmap-parse-render, dim 8. **Disposition:** queued, ANTS-6483.

The publish re-reads the store after the commit, outside any lock. Nothing serialises the file write.

Quote: `if (!store.commit(error)) … opts.dryRun = false; const auto published = RoadmapRender::render(…)`

Remedy: hold a per-file advisory lock from step 0 through the publish, or re-check the store digest before committing the file and re-render if it changed.

### Row 345 — High — `claudeallowlist.cpp:327-335`

review-code, lane 30-claude-session-widgets, dim 9. **Disposition:** queued, ANTS-6484.

Saving deletes other permission settings.

Quote: `QJsonObject perms; … root["permissions"] = perms;`

Remedy: start `perms` from `root.value("permissions").toObject()` and overwrite or remove only the three arrays.

### Row 350 — Low — `claudestatuswidgets.cpp:2828-2921`

review-code, lane 30-claude-session-widgets, dim 9. **Disposition:** queued, ANTS-6485.

runs outside the ledger lock, and `writeRecords` takes the lock only for its own write. A record appended by another instance in between is overwritten by the stale list. This is rare while the actuator is parked. Fix: one locked read-modify-write helper in `ModelSwitchLedger`.

Quote: `readRecords`

Remedy: one locked read-modify-write helper in `ModelSwitchLedger`.

### Row 403 — Medium — `mutationjournal.cpp:85-86`

review-code, lane 35-test-audit-verify, dim 9. **Disposition:** queued, ANTS-6486.

any failure to open the file is treated as "file missing": permission denied (EACCES), too many open files (EMFILE) or an I/O error (EIO), as well as a file that is really gone. The journal is then deleted.

Quote: `:104`

Remedy: report `left_missing` only when `!QFileInfo::exists(path)`. In every other case keep the journal, as `restore_failed`.

Also reported as: "readability lane 34-test-audit-verify Info correctness lead src/mutationjournal.cpp:85-86,104"

### Row 429 — Low — `feedbackstore.cpp:192`

review-code, lane 36-changelog-feedback, dim 9. **Disposition:** queued, ANTS-6487.

a hand edit made between the drift check and `writeExport` is overwritten, against INV-6. The window is short.

Quote: `:239`

### Row 446 — High — `tools/install-hooks.sh:108`

review-code, lane 38-dev-shell-tools, dim 9. **Disposition:** queued, ANTS-6488.

an invalid `settings.json` is replaced with an empty file.

Quote: `current="$(read_settings)"`

Remedy: check `jq '.' "$target"` in the main shell and abort if it fails. Also refuse an empty `new_content` (`[ -n "$new_content" ] || abort`) before calling `atomic_write_settings`.

### Row 1307 — Info — `src/mainwindow.cpp:3114-3116`

review-readability, lane 05-mainwindow-a, correctness lead. **Disposition:** queued, ANTS-6489.

When an auto-profile rule matches, it calls `applyTheme`, which saves the theme to the config (`m_config.setTheme`, 1316). A profile switch therefore **permanently overwrites the user's saved theme**.

### Row 1372 — Info — `src/claudeallowlist.cpp:222`

review-readability, lane 21-claude-session-widgets-b, correctness lead. **Disposition:** queued, ANTS-6490.

If the settings file exists but cannot be opened, `loadSettings` returns silently with three empty lists and no hint. The parse-failure case does show a hint. `saveSettings` then refuses with a generic "check permissions".

## Memory caps applied too late, or missing

### Row 6 — Low — `vtstream.cpp:81,89`

review-code, lane 01-vt-parser-pty, dim 11. **Disposition:** queued under ANTS-5415.

has no cap. The whole paste is held until it is fed in full, plus every keystroke typed meanwhile. If the child stops reading, it grows without bound. This conflicts with the project's "no unbounded growth ships without a named cap" rule. My search for a documented cap returned `truncated: true` and I did not read the remaining rows, so a cap may be documented elsewhere.

Quote: `m_pasteBacklog.append(data)`

Remedy: name and enforce a cap, and on overflow drop with `writeLost`, as `Pty` does.

### Row 13 — Medium — `terminalgrid.cpp:2296`

review-code, lane 02-terminal-grid, dim 5. **Disposition:** queued, ANTS-6491.

A link that is opened, then followed by a cursor move (`CSI 10 C` or `CSI B`) with no text printed, then closed, still pushes spans. Nothing ever drops them, because only printing and clearing call `dropHyperlinkSpans`.

Quote: `2301–2315`

Remedy: call `dropHyperlinkSpans(row, sCol, eCol + 1)` before each push, or cap spans per row.

### Row 38 — Low — `terminalwidget.cpp:1020`

review-code, lane 03-terminal-widget-a, dim 11. **Disposition:** queued, ANTS-6492.

the URL span cache has no named cap.

Quote: `invalidateSpanCaches`

Remedy: give it an LRU cap, or drop entries outside the viewport ± N rows.

### Row 39 — Low — `terminalwidget.cpp:2431-2433`

review-code, lane 03-terminal-widget-a, dim 11. **Disposition:** queued, ANTS-6493.

the session log grows with no size limit. It may be by design as a user-requested capture; see Open questions.

Quote: `m_logFile->write(batch->rawBytes);`

### Row 48 — Medium — `terminalwidget.cpp:4780-4806`

review-code, lane 04-terminal-widget-b, dim 11. **Disposition:** queued, ANTS-6494.

the match list has no cap. A one-character query over the 1M-line maximum scrollback can store hundreds of millions of 12-byte matches, about 1–2 GB, and it is all built on the GUI thread.

Quote: `m_searchMatches.push_back({gl, startCol, lastCol - startCol + 1});`

Remedy: cap the match count (for example 100k) and show "N+" in the label.

### Row 68 — Low — `mainwindow_repo.cpp:180`

review-code, lane 05-mainwindow, dim 5. **Disposition:** queued, ANTS-6495.

and `m_repoVisibilityProbeInFlight` gain one entry per repo root visited and never evict. Their TTL is checked only on read. The spec standard requires a named cap.

Quote: `:223`

### Row 110 — Low — `remotecontrol_roadmap_query_verb.cpp:3958`

review-code, lane 09-mcp-roadmap-query-log, dim 5. **Disposition:** queued, ANTS-6496.

(`remotecontrol_roadmap_query_verb.cpp:3958`) is keyed on the raw, uncanonicalised `caller_cwd`, has no cap or eviction, and lives for the whole process. The shared context's spec-format rule says no unbounded growth ships without a named cap.

Quote: `remotecontrol_roadmap_query_verb.cpp:3958`

### Row 122 — Medium — `remotecontrol_workspace.cpp:2194-2202`

review-code, lane 11-mcp-state-workspace, dim 5. **Disposition:** queued, ANTS-6497.

`mutation_probe` keeps the child's merged output in QProcess's buffer for up to `timeoutSec` (default 300, maximum 1800). The 512 KiB cap only applies after the process has finished.

Quote: `if (!p.waitForFinished(timeoutSec * 1000)) {…} QByteArray out = p.readAllStandardOutput(); … if (out.size() > kCap) out = out.right(kCap);`

Remedy: drain in a `waitForReadyRead` loop, keep a rolling tail, and kill the child once its total output passes a ceiling.

### Row 128 — Low — `remotecontrol_state.cpp:2219`

review-code, lane 11-mcp-state-workspace, dim 11. **Disposition:** queued, ANTS-6498.

Whole-file `readAll()` with no size cap at `remotecontrol_state.cpp:2219`, `:2438`, `:2594`, `:3132` and `:3299`. a large `.md` file (or a symlink to a large file, see above) is read whole, up to four times per `invariant_check`.

Quote: `remotecontrol_state.cpp:2219`

Remedy: cap the read size, as `specListEnvelope` does with its 8 KiB read.

### Row 130 — Low — `remotecontrol_workspace.cpp:197`

review-code, lane 11-mcp-state-workspace, dim 11. **Disposition:** queued, ANTS-6499.

rg's stderr is only read after the run, so QProcess keeps it in memory for up to 30 s, for example when searching `/` produces a flood of "permission denied". This is a suspicion: I did not confirm how Qt buffers stderr during `waitForReadyRead`.

Quote: `/`

### Row 140 — Medium — `quotationcheckverb.cpp:424`

review-code, lane 13-mcp-content-verbs-b, dim 11. **Disposition:** queued, ANTS-6500.

and `:497` keeps every subject until the call ends. each subject holds its raw bytes plus a `Normalised`, which is the text plus an 8-byte offset for every byte. That is about 9× the file size, and the per-step intermediates are higher still while normalising.

Quote: `std::map<std::pair<QString, QString>, Subject> subjects;`

Remedy: cap the total bytes of subjects per call (reason `too_large` past it), or drop a subject after the last item that names it.

### Row 163 — Medium — `functionsizes.cpp:169`

review-code, lane 15-code-index-search-a, dim 11. **Disposition:** queued, ANTS-6501.

Whole-file reads with no size limit, held together in memory:

Remedy: a per-file byte cap, reported in `scanned`. In source_scrapes, analyse each file once and do not keep `texts`.

### Row 173 — Medium — `readregion.cpp:602`

review-code, lane 16-code-index-search-b, dim 11. **Disposition:** queued, ANTS-6502.

ANTS-2021 INV-9 and § 2.5 promise that peak heap stays within `max_bytes` (≤ 4 MiB) "regardless of the requested range or file size". The code breaks that in three places:

Quote: `QByteArray raw = f.readLine();`

Remedy: use `BoundedReadLine::read(f, maxBytes+…)` here, as `symbolquery.cpp:685` already does. Stream the brace scan and the heading scan rather than loading the file.

### Row 174 — Medium — `trace.cpp:851`

review-code, lane 16-code-index-search-b, dim 5. **Disposition:** queued, ANTS-6503.

every whole-word hit in every walked file (up to 5000 files × 2 MiB) becomes a `Row`. Each row is a `QJsonObject` carrying up to 200 bytes of line text (`:540-552`). All rows are built before `pageInto` applies `max_bytes`.

Quote: `const QVector<int> hits = pathMode ? pathHits(…) : wholeWordHits(text, req.needle);`

Remedy: cap the gathered rows (stop at `offset` + one page's worth and record `total` as a count only), or name a cap in the spec.

### Row 182 — Low — `queries.cpp:23`

review-code, lane 16-code-index-search-b, dim 11. **Disposition:** queued, ANTS-6504.

every distinct user `.scm` text compiles a new `TSQuery` that is never freed. Repeated edits grow the table without a cap. Fix: name a cap, or free superseded entries per language once no cursor holds them.

Quote: `:140`

Remedy: name a cap, or free superseded entries per language once no cursor holds them.

### Row 183 — Low — `trace.cpp:824-828`

review-code, lane 16-code-index-search-b, dim 5. **Disposition:** queued, ANTS-6505.

the 2 MiB check uses the size from the walk-time stat, then `f.readAll()` reads without a bound. A file that grew in between is read whole. Fix: use `f.read(kMaxFileBytes + 1 - text.size())` and treat any overflow as too large.

Quote: `f.readAll()`

Remedy: use `f.read(kMaxFileBytes + 1 - text.size())` and treat any overflow as too large.

### Row 194 — High — `luaengine.cpp:920-950`

review-code, lane 18-plugins-lua, dim 3. **Disposition:** queued, ANTS-6506.

`ants.palette.register` needs no permission. It only replaces an entry with the same title and action, so new names make new entries, and nothing caps how many. A plugin running `for i=1,1e6 do ants.palette.register{title=tostring(i), action=tostring(i), hotkey="Ctrl+F"..i} end` inside its 1.5 s budget fills `UiUpdates::palette`.

Quote: `mainwindow.cpp`

Remedy: cap entries per plugin (for example 64, like the handler cap) at `lua_ants_palette_register`, and raise a Lua error past the cap.

### Row 196 — Medium — `luaengine.cpp:669-676`

review-code, lane 18-plugins-lua, dim 5. **Disposition:** queued, ANTS-6507.

`ants.send` needs no permission and has neither a rate cap nor a size cap. Each call copies a string of up to about 10 MiB, which doubles to about 20 MiB once converted to Qt's two-byte text format, into the main thread's queue. A loop within one handler's budget can queue gigabytes.

Quote: `emit engine->sendToTerminal(QString::fromUtf8(text));`

Remedy: cap bytes per message and per second for every call that crosses to the main thread, and count what is dropped, as `admitMessage` does.

### Row 207 — Medium — `sessionmanager.cpp:594`

review-code, lane 19-config-session-project, dim 5. **Disposition:** queued, ANTS-6508.

`loadSession` copies each refused blob to `session_<id>.dat.corrupt-<ms>`. Each copy can be up to 100 MiB, and a file over the cap is copied too.

Quote: `:698`

Remedy: add `session_*.dat.corrupt-*` to the age sweep.

### Row 208 — Medium — `projectsettings.cpp:113-115`

review-code, lane 19-config-session-project, dim 3. **Disposition:** queued, ANTS-6509.

The file is committed in the repo, so a cloned repo controls it (ANTS-2160 § 8 calls this a trust boundary). There is no size cap.

Quote: `QFile f(rootCanonical + "/.ants/project.json"); … const QByteArray raw = f.readAll();`

Remedy: check `f.size()` against a small cap (for example 64 KiB) before reading, and treat a larger file as malformed.

### Row 209 — Medium — `projectlayoutengine.cpp:243`

review-code, lane 19-config-session-project, dim 11. **Disposition:** queued, ANTS-6510.

The whole roadmap is read and copied line by line, with no size bound. The roadmap path can be declared by a repo-committed file.

Quote: `:89`

Remedy: stream the file with `readLine`, or cap it.

### Row 215 — Low — `config.cpp:428-435`

review-code, lane 19-config-session-project, dim 5. **Disposition:** queued, ANTS-6511.

(`roadmap_view_state`), `:1085` (`audit_rule_pack_trust`), `:600` (`tokens_saved_by_project`) — per-path maps in `config.json` with no eviction. Entries for projects that no longer exist stay forever. I did not check whether ANTS-3579 or ANTS-1150 name a cap.

Quote: `roadmap_view_state`

### Row 218 — Medium — `gitwrap.cpp:29,39-44`

review-code, lane 20-shared-utilities, dim 5. **Disposition:** queued, ANTS-6512.

The 1 MiB stdout cap does not limit memory.

Quote: `p.waitForFinished(kHardKillMs); … r.stdoutBytes = p.readAllStandardOutput(); if (… > maxStdoutBytes) truncate`

Remedy: read in a loop with `waitForReadyRead`, and kill the process once the cap is reached.

### Row 229 — Medium — `updatedialog.cpp:207`

review-code, lane 21-dialog-chrome-theme, dim 11. **Disposition:** queued, ANTS-6513.

the `.manifest` and `.manifest.sig` downloads have no size limit. Spec § 4 says memory "holds one network buffer, not the file", but that is only true of the AppImage download. A wrong release asset served under the manifest's name (an AppImage uploaded by mistake, say) would be read into RAM whole. — fix: abort from `downloadProgress` above a small cap (for example 4 KiB for the manifest), and r…

Quote: `*into = reply->readAll();`

Remedy: abort from `downloadProgress` above a small cap (for example 4 KiB for the manifest), and require exactly 64 bytes for the signature.

### Row 241 — Medium — `diffviewer.cpp:676-733`

review-code, lane 22-app-entry-dialogs, dim 5. **Disposition:** queued, ANTS-6514.

every untracked file is read (up to 200 KB each) and turned into HTML on the GUI thread, with no limit on how many files.

Quote: `for (const QString &rel : newFiles) { … nf.read(kCap + 1) …`

Remedy: cap the number of files and the total bytes rendered in New files, and show a "… N more" line.

### Row 242 — Medium — `diffviewer.cpp:836`

review-code, lane 22-app-entry-dialogs, dim 11. **Disposition:** queued, ANTS-6515.

the whole output of `git diff --stat --patch HEAD` is buffered by QProcess and converted to UTF-16, and that copy is trimmed into a second one. The 1 MiB cap (`:433-438`) only applies after all of that.

Quote: `st_ptr->*slot = QString::fromUtf8(pg->readAllStandardOutput()).trimmed();`

Remedy: read incrementally (`readyReadStandardOutput`) and stop or kill the process once the cap is reached.

### Row 252 — Low — `claudesetup.cpp:480-488`

review-code, lane 22-app-entry-dialogs, dim 11. **Disposition:** queued, ANTS-6516.

. One state file per Claude session, never removed. On the `/tmp` fallback it persists across logins, which breaks the named-cap rule. fix: prune files older than N days on write.

Quote: `state="$state_dir/$sid"`

Remedy: prune files older than N days on write.

### Row 264 — Low — `aidialog.cpp:233`

review-code, lane 23-model-switching, dim 11. **Disposition:** queued, ANTS-6517.

the chat history is kept across re-opens on purpose and has no cap. Each reply adds up to 64 K characters. The project's spec standard says "no unbounded growth ships without a named cap". fix: cap the number of document blocks, or set `QTextDocument::setMaximumBlockCount`.

Quote: `m_chatHistory->append(html)`

Remedy: cap the number of document blocks, or set `QTextDocument::setMaximumBlockCount`.

### Row 273 — Medium — `hooks/ants-drift-check.sh:52,59,63,65-66`

review-code, lane 24-shell-surface, dim 11. **Disposition:** queued, ANTS-6518.

`drift-check.log` is appended on every Stop event (every turn) that has helper stderr or an error rc. It is never capped or rotated.

Quote: `2>>"$logfile"`

Remedy: Before appending, cap it the way `ants_grep_nudge_record` does (keep the last N lines).

### Row 274 — Low — `_common.sh:256,268`

review-code, lane 24-shell-surface, dim 11. **Disposition:** queued, ANTS-6519.

Per-session files, one per session or process, with no eviction anywhere:

Remedy: Prune files older than N days on write.

### Row 307 — Low — `auditdialog.cpp:609–623`

review-code, lane 26-audit-dialog, dim 11. **Disposition:** queued, ANTS-6520.

limits each file to 4 MB but has no cap on how many files it holds during a run, which happens before `capFindings`. `spec-format`'s "named cap" rule asks for one.

Quote: `m_fileLineCache`

### Row 351 — Low — `claudestatuswidgets.h:462-473`

review-code, lane 30-claude-session-widgets, dim 11. **Disposition:** queued, ANTS-6521.

the near-miss and conservatism hashes, and `m_lastOverrideMsByProject`, are keyed by `shellCwd()`, which changes on every `cd`. "Bounded by distinct shellCwd() values" is not the named cap that `spec-format.md` requires. Fix: cap the hashes, or key them on the repository root.

Quote: `m_lastOverrideMsByProject`

Remedy: cap the hashes, or key them on the repository root.

### Row 357 — Medium — `remotecontrol_review.cpp:1022-1030`

review-code, lane 31-mcp-review-verbs, dim 11. **Disposition:** queued, ANTS-6522.

Every untracked, non-ignored directory is walked recursively with no file cap, no byte budget and no time limit. This happens twice per `verify_changes` call (before and after the run), and on `cache_only` probes too.

Quote: `QDirIterator it(abs, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories); while (it.hasNext()) inner.append(it.next());`

Remedy: cap the entries walked. When the cap trips, mark the snapshot invalid (uncacheable), the same way ANTS-5097 handles a failed git status.

### Row 379 — Low — `featurecoverage.cpp:933-946`

review-code, lane 33-spec-engines, dim 11. **Disposition:** queued, ANTS-6523.

Every file in every `tests/features/*` directory, binary fixtures included, is read whole with no cap. So are the spec and doc bodies and the allowlist.

Quote: `776`

Remedy: apply `maxFileBytes`.

### Row 386 — Medium — `indiereviewdialog.cpp:206-207`

review-code, lane 34-review-engines, dim 11. **Disposition:** queued, ANTS-6524.

the synthesis prompt has no size cap. It concatenates every lane report (each can be up to 10 MiB per ANTS-1258 §4, across about 30 lanes) plus three whole project files. Lane prompts are capped at 200 KiB (INV-3); this one is not. The spec-format rule "no unbounded growth ships without a named cap" is broken.

Quote: `job.request.userPrompt = IndieReviewEngine::synthesisPrompt(m_lastReports, extras);`

Remedy: pass it through `clipToCapClosingFence` against `kPromptCapBytes`.

### Row 387 — Medium — `debtsweepengine.cpp:85-86`

review-code, lane 34-review-engines, dim 5. **Disposition:** queued, ANTS-6525.

the "stdout cap" is applied after QProcess has buffered the whole output. The comment above it says the cap prevents "a massive diff or log [pinning] RAM", and it does not. Both `git log --all --format=%s` (`:973`) and `git diff --unified=0` over a tag window are fully buffered first. The same pattern is at `:1340`.

Quote: `QByteArray out = p.readAllStandardOutput(); if (out.size() > stdoutCap) out.truncate(stdoutCap);`

Remedy: read in chunks on `readyReadStandardOutput`, and kill the process once the cap is reached.

### Row 406 — Low — `testauditengine.cpp:234`

review-code, lane 35-test-audit-verify, dim 5. **Disposition:** queued, ANTS-6526.

this map gains one entry per token briefed and never evicts. The partition cache it sits beside is capped at 16. It breaks the "no unbounded growth" rule.

Quote: `:1555`

Remedy: evict inside `cachePartition`'s LRU loop.

### Row 430 — Low — `feedbackstore.cpp:53`

review-code, lane 36-changelog-feedback, dim 11. **Disposition:** queued, ANTS-6527.

has no size cap, and the store keeps each reporter's full text. With the compaction ops retired, the corpus only grows. Spec § 13 gives today's sizes but names no cap or eviction, which the project's "named cap" rule requires.

Quote: `readAll()`

### Row 434 — Medium — `buildcache.cpp:175`

review-code, lane 37-diagnostics-logging, dim 11. **Disposition:** queued, ANTS-6528.

and `fromJson` at `:292` (`for (const auto &v2 : errs)`) is not capped either. loading applies no size cap, no 50-entry cap and no 240-character cap. Spec § 5 says the cache file is "≤ ~50 KiB in the worst case", but only the writer enforces that.

Quote: `const QByteArray bytes = f.readAll();`

Remedy: refuse a file over about 64 KiB, and re-apply `kMaxErrors` and `kMessageCharCap` in `fromJson`.

## High bugs

### Row 7 — High — `terminalgrid.cpp:3649–3650`

review-code, lane 02-terminal-grid, dim 3. **Disposition:** queued, ANTS-6529.

, then `placeInlineImage` adds the same cost again, and `canFit` is `return bytes <= MAX_BYTES - used;`. A Kitty `a=T,i=N` image is checked once with `canFit(c)` but charged twice (once for the cache, once for the inline copy).

Quote: `:2543`

Remedy: guard `used <= MAX_BYTES` inside `canFit`, and charge a shared pixel buffer once on the incremental path, as `recomputeImageBudget` already does.

### Row 9 — High — `terminalgrid.cpp:2956–2976`

review-code, lane 02-terminal-grid, dim 2. **Disposition:** queued, ANTS-6530.

A height-only shrink (cols unchanged) keeps the top rows and throws away the bottom `m_rows - rows` rows. Those rows hold the cursor line and the newest output, and none of them goes to scrollback. The cursor is then clamped onto unrelated content.

Quote: `int copyRows = std::min(rows, m_rows); for (int r = 0; r < copyRows; ++r) …`

Remedy: when rows shrink, push the top `max(0, cursorRow - rows + 1)` rows into scrollback (with `++m_scrollbackPushed` and hyperlink lockstep) before copying.

### Row 30 — High — `terminalwidget.cpp:1910-1916`

review-code, lane 03-terminal-widget-a, dim 8. **Disposition:** queued under ANTS-5826 (lead at same lines; no claim).

use-after-free.

Quote: `for (const auto &ql : m_quickSelectLabels) { if (ql.label == m_quickSelectInput) { exitUrlQuickSelect(); if (ql.isFilePath) { openFileAtPath(ql.url);`

Remedy: copy `url` and `isFilePath` into locals before calling `exitUrlQuickSelect()`.

### Row 32 — High — `terminalwidget.cpp:3085-3118`

review-code, lane 03-terminal-widget-a, dim 2. **Disposition:** queued, ANTS-6531.

legacy key encoding ignores Alt everywhere, and Shift on cursor and function keys.

Quote: `:3069`

Remedy: add an ESC prefix for Alt on text keys and Ctrl chords, and modifier parameters on arrow, Home/End and F-keys. Intent is under Open questions.

### Row 42 — High — `terminalwidget.cpp:3895`

review-code, lane 04-terminal-widget-b, dim 2. **Disposition:** queued, ANTS-6532.

Qt delivers the second press of a double-click as a double-click event. The base `QWidget::mouseDoubleClickEvent` would forward it to `mousePressEvent`, but this override doesn't, and it never checks `mouseReportingActive()`.

Quote: `void TerminalWidget::mouseDoubleClickEvent(QMouseEvent *event) {`

Remedy: start the function with `if (mouseReportingActive() && !(event->modifiers() & Qt::ShiftModifier)) { sendMouseEvent(event, true, false); return; }`.

### Row 43 — High — `terminalwidget.cpp:5059-5060`

review-code, lane 04-terminal-widget-b, dim 2. **Disposition:** queued under ANTS-5824.

the loop walks forward and stops at the first region whose start is at or above the cursor. The cursor is normally at the newest prompt, so that is almost always `regions[0]`, the oldest finished command.

Quote: `for (size_t i = 0; i < regions.size(); ++i) { if (globalLine >= regions[i].startLine && regions[i].hasOutput && regions[i].commandEndMs > 0) {`

Remedy: use `promptRegionIndexAtLine(globalLine)`, or walk backwards to the nearest finished region.

Also reported as: "readability lane 04-terminal-widget-b Info correctness lead src/terminalwidget.cpp:5059"

### Row 82 — High — `src/claudeintegration.cpp:312-314`

review-code, lane 07-claude-integration-a, dim 2. **Disposition:** queued, ANTS-6533.

together with `src/claudeintegration_sessions.cpp:276` `m_transcriptPath.clear();` (the branch for "no Claude under the focused shell").

Quote: `const bool coldStart = m_transcriptPath.isEmpty(); … if (coldStart && isColdStartDroppable) { … return; }`

Remedy: tell the transient window apart from "no Claude here". For example, set a `m_coldStartPending` flag in `setShellPid` and clear it on the first poll. Drop `PermissionRequest` only while that flag is set; otherwise route it by `session_id` alone.

### Row 121 — High — `remotecontrol_workspace.cpp:3328`

review-code, lane 11-mcp-state-workspace, dim 9. **Disposition:** queued, ANTS-6534.

`apply_edits` decodes the whole file as UTF-8 and writes the whole buffer back. Qt replaces every invalid UTF-8 byte with U+FFFD, which is written as `EF BF BD`. So editing one line of a Latin-1 or partly binary file silently corrupts every non-UTF-8 byte in it, and the reply says `ok:true`.

Quote: `QString working = QString::fromUtf8(in.readAll());`

Remedy: decode with `QStringDecoder` and check `hasError()`. On error, skip the edit with reason `not_utf8` and never write.

### Row 195 — High — `pluginmanager.cpp:186-191`

review-code, lane 18-plugins-lua, dim 8. **Disposition:** queued, ANTS-6535.

The chain: the plugin manager is created as `new PluginManager(this)`, a child of the main window, and `~MainWindow` neither unloads nor deletes it. So it is destroyed during `~QWidget`'s cleanup of children, after `MainWindow`'s own members, such as `m_config`, have already been destroyed.

Quote: `:101-103`

Remedy: in `~MainWindow` (or `closeEvent`), call an explicit `m_pluginManager->shutdown()` (it would need to be made public) or `delete m_pluginManager` before the members die.

### Row 203 — High — `projectlayoutengine.cpp:203-208`

review-code, lane 19-config-session-project, dim 2. **Disposition:** queued under ANTS-6021.

What goes wrong: when a roadmap is declared in `.ants/project.json`, nothing is added to `probedPaths`. The declared file isn't, the candidate paths aren't, and `.ants/project.json` itself isn't either. The same happens for the declared changelog (`:272-280`) and the declared `specs_dir` (`:498-500`).

Quote: `if (const auto rm = ProjectSettings::load(cwd).roadmap) { … discovered.append(*rm); }`

Remedy: append the declared paths and `.ants/project.json` to `probed`.

Also reported as: "readability lane 39-config-session-project Info correctness lead src/projectlayoutengine.cpp:203"

### Row 227 — High — `dialogfocus.h:100`

review-code, lane 21-dialog-chrome-theme, dim 16. **Disposition:** queued, ANTS-6536.

Qt counts a minimised (iconified) window as visible. Every chromed dialog has a working Minimise button: `dialogchrome.cpp:203-204` connects `minimizeRequested` to `showMinimized`. So once the user minimises any dialog, `shouldSuppressEventForDialog` keeps returning true, and MainWindow's filter (`mainwindow.cpp:2098`) swallows every click, key and wheel event sent to the terminal. A frameless di…

Quote: `if (!d || !d->isVisible()) continue;`

Remedy: skip dialogs where `d->isMinimized()`, here and in `shouldRefocusOnDialogClose` (line 38). Or hide the chrome's minimise button.

### Row 254 — High — `modelswitchledger.cpp:239-244`

review-code, lane 23-model-switching, dim 2. **Disposition:** queued under ANTS-6012.

the user-override detector can never match the transcript format this same lane documents.

Quote: `QStringLiteral("^\\s*/model\\s+(haiku|sonnet|opus)\\b")`

Remedy: project the `local_command` `/model` events into `TranscriptTurn` (reusing `modelTierFromLocalCommand`'s parse), carrying a timestamp, and feed those to `computeOutcome`.

Also reported as: "readability lane 38-model-switching Info correctness lead src/modelswitchledger.cpp:239-245"

### Row 266 — High — `hooks/ants-session-preamble.sh:32`

review-code, lane 24-shell-surface, dim 2b. **Disposition:** queued, ANTS-6537.

The resume line that ANTS-1252 §1.1 promises ("even when the resume snapshot from PreCompact is present") keys on `CLAUDE_SESSION_ID`. Nothing in the tree sets that variable.

Quote: `sid="$(printf '%s' "${CLAUDE_SESSION_ID:-}" | …)"`

Remedy: Read `.session_id` from the SessionStart stdin payload with `jq`, as precompact already does. Fall back to `CLAUDE_CODE_SESSION_ID`.

### Row 267 — High — `packaging/cc-job/cc-job:113-114`

review-code, lane 24-shell-surface, dim 9. **Disposition:** queued, ANTS-6538.

Starting a job under a reused name truncates the previous job's log without any check.

Quote: `rm -f "$rec" "$STATE/$name.read"`

Remedy: Before truncating, refuse when `-s log && ! -e .read`, with the same message as `clean` and a `--force` flag.

### Row 289 — High — `auditdialog.cpp:184`

review-code, lane 26-audit-dialog, dim 8. **Disposition:** queued, ANTS-6539.

the dialog is created with `WA_DeleteOnClose` (`mainwindow_menus.cpp:472`), so closing it mid-check destroys it. The members (`m_checks`, `m_completedResults` and so on) are destroyed first; the child `QProcess` only later, in `~QWidget`. `~QProcess` kills the running child, waits for it, and emits `finished` and `readyRead*` along the way.

Quote: `:246`

Remedy: in `~AuditDialog`, before the members go, `disconnect(m_process, nullptr, this, nullptr)` and do the same for every blame `QProcess`, then stop them with `releaseProcess`.

### Row 309 — High — `auditrunner.cpp:925-976`

review-code, lane 27-audit-engine, dim 2. **Disposition:** queued under ANTS-5857.

the field extraction reads only `filename|path|File`, `message|Description`, `check_id`, `line_number|start.line` and top-level `severity`. From what I know of each tool's JSON format (not checked here), most tools do not use those names:

Quote: `filename|path|File`

Remedy: map each tool's own JSON keys.

### Row 310 — High — `auditdelta.cpp:56-57`

review-code, lane 27-audit-engine, dim 2. **Disposition:** queued, ANTS-6540.

, fed from `auditrunner.cpp:2645`. Any earlier finding in a changed file whose tool did not run this time is classed as removed and dropped from the recorded baseline (`d.merged`). That covers: gitleaks and trivy, which are always skipped under narrowing (`not_file_scoped`)

Quote: `if (!currentFps.contains(fpOf(v))) d.removed.append(v);`

Remedy: limit `priorOnChanged` to tools that ran and finished "ok". Carry the rest forward.

### Row 337 — High — `roadmapdialog.cpp:2588-2598`

review-code, lane 29-roadmap-dialog, dim 2. **Disposition:** queued, ANTS-6541.

the open cards and sections, and the scroll position, are saved only in `closeEvent`. Closing with Esc or the title-bar × goes through `QDialog::reject`, and `dialogchrome.cpp`'s ChromeGuard says *"reject(), accept() and done() hide the dialog without a Close reaching this filter"* (ANTS-5037). `install` also wires `TitleBar::closeRequested` to `QDialog::reject`.

Quote: `if (m_config) { captureScrollAnchor(); saveViewState(); }`

Remedy: do the capture and save in `done()` or `hideEvent`, or call `saveViewState()` from `handleAnchorClicked`.

### Row 365 — High — `doclint.cpp:294`

review-code, lane 32-doc-engines, dim 9. **Disposition:** queued, ANTS-6542.

What goes wrong. The duplicate-TOC repair deletes the whole physical line at the walk-time line number. It never re-locates the entry.

Quote: `deletions.insert(g.line);`

Remedy: re-locate the duplicate's entry from the freshly detected `toc.entries` (the second entry with that slug). Remove only that link's list item. Refuse when that item shares its line with another entry.

### Row 398 — High — `testauditengine.cpp:1484-1485`

review-code, lane 35-test-audit-verify, dim 2. **Disposition:** queued, ANTS-6543.

a paginated partition caches only the page it was asked for. The pre-pass map is also cut to that page at `:1494`. Every page has the same token (`computeToken` takes no offset), so each page call overwrites the cache entry.

Quote: `:1510`

Remedy: cache the full result before slicing, and slice a copy for the envelope.

Also reported as: "readability lane 34-test-audit-verify Info correctness lead src/testauditengine.cpp:1480-1510"

### Row 399 — High — `testauditengine.cpp:437-439`

review-code, lane 35-test-audit-verify, dim 2. **Disposition:** queued, ANTS-6544.

this excludes every directory whose name starts with `build`, not just build trees.

Quote: `"/(node_modules|\\.venv|__pycache__|build[^/]*|dist|_deps"`

Remedy: match `build`, `build-*` and `cmake-build-*` only, or exclude a directory only when it contains `CMakeCache.txt`.

### Row 415 — High — `changelogquery.cpp:106`

review-code, lane 36-changelog-feedback, dim 2. **Disposition:** queued under ANTS-6029.

changelog_query reports every entry of a project with no version numbers as Unreleased.

Quote: `if (!line.startsWith(QLatin1String("## ["))) return false;`

Remedy: accept `^## \d{4}-\d{2}-\d{2}\s*$` in `parseVersionHeading` as a version whose name is the date.

## One-edit comment fixes

### Row 71 — Low — `docs/standards/menus.md (docs/standards/menus.md)`

review-code, lane 05-mainwindow. **Disposition:** queued, ANTS-6560.

M1 says `StayOpenOnToggleFilter` lives in "anonymous namespace in `mainwindow.cpp`". It is in `mainwindow_internal.h`, `namespace mainwindowdetail`. This is `review-contract`'s to fix.

Quote: `StayOpenOnToggleFilter`

### Row 80 — Medium — `src/remotecontrolgate.h:28`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

The code (remotecontrolgate.cpp:15) stores `focusedRaw` unchanged, and lines 32-35 of the same comment say it arrives already canonical. A caller trusting line 28 could pass a non-canonical focused path and get a false `cwd_mismatch`.

Quote: `// Read caller_cwd from req, canonicalise both it and focusedRaw,`

Remedy: say only `caller_cwd` is canonicalised.

Also reported as: "code lane 06-mcp-transport Low None remotecontrolgate.h:28"

### Row 81 — Low — `mcpdsocket.cpp:20`

review-code, lane 06-mcp-transport. **Disposition:** queued, ANTS-6567.

ANTS-4932 § 2.1 says the picker also reads "the legacy `/tmp` name". The code (`mcpdsocket.cpp:20`) says ANTS-5587 removed that. The spec text is stale.

Quote: `mcpdsocket.cpp:20`

### Row 90 — Medium — `claudeintegration_store.cpp:1912-1915`

review-code, lane 08-claude-integration-b, dim 2. **Disposition:** queued, ANTS-6551.

This contradicts the same tool's wire text and detail.

Quote: `"\"deregister\" (ANTS-4617) is the INVERSE: it DELETES the project and every row that hangs off it"`

Remedy: reword the `op` description to match the detail.

### Row 94 — Low — `claudeintegration_docs.cpp:761`

review-code, lane 08-claude-integration-b, dim 2. **Disposition:** queued, ANTS-6551.

The count is stale.

Quote: `"cheaper than the five calls it replaces"`

Remedy: change both to "six".

Also reported as: "readability lane 19-claude-integration-c Low Q1 src/claudeintegration_docs.cpp:761"

### Row 95 — Low — `orientation.cpp:342-345`

review-code, lane 08-claude-integration-b, dim 2. **Disposition:** queued, ANTS-6551.

Some wire descriptions list fewer ops than the schema enum

Remedy: name the ops; the details can carry the prose.

### Row 120 — Low — `migrate.cpp:301-308`

review-code, lane 10-mcp-roadmap-batch. **Disposition:** queued, ANTS-6564.

ANTS-2132 spec lines 621-622 — "A second Ants process sharing the machine-global store is not covered."

### Row 201 — Low — `PLUGINS.md (PLUGINS.md)`

review-code, lane 18-plugins-lua. **Disposition:** queued, ANTS-6570.

§ Resource Limits says each "loadScript / fireEvent starts a 1500 ms deadline". The code restarts the budget for each handler (`timedPcall`), so one event with 64 handlers can run about 96 s. The fix belongs to `review-contract`.

Quote: `timedPcall`

### Row 202 — Low — `pluginmanager.cpp:208`

review-code, lane 18-plugins-lua. **Disposition:** queued, ANTS-6570.

ANTS-1750 INV-11/INV-12 say a cleanly unloaded engine is destroyed with `deleteLater` posted to the worker. The code deletes it on the main thread after the worker has finished (`pluginmanager.cpp:208`), which is safe because `m_state` is already null. The fix belongs to `review-contract`.

Quote: `pluginmanager.cpp:208`

### Row 210 — High — `src/projectlayoutengine.cpp:363`

review-readability, lane 39-config-session-project, Q4. **Disposition:** queued, ANTS-6554.

This comment is false. `isStale` (691–711) stats each probed path, and the string `docs/*STANDARD*|*DESIGN*|*STYLE*|*GUIDE*.md` never exists. `caseInsensitiveResolve` also returns empty for it, because no directory entry has that name. So the marker never invalidates the cache. The comment at 329–330 also says the code records "the docs/ directory", but it records the glob string. A stranger trus…

Quote: `// Marker so isStale picks up future additions / standards-name renames.`

Remedy: correct both comments to say the marker is a label `isStale` cannot stat (or probe `docs/` itself).

Also reported as: "code lane 19-config-session-project Low dim 2 projectlayoutengine.cpp:364; readability lane 39-config-session-project Info correctness lead src/projectlayoutengine.cpp:364"

### Row 225 — Low — `pathvalidation.cpp:252`

review-code, lane 20-shared-utilities, dim 2. **Disposition:** queued, ANTS-6575.

Stale documents, where the doc side is wrong:

### Row 265 — Low — `docs/subsystems.md (docs/subsystems.md)`

review-code, lane 23-model-switching. **Disposition:** queued, ANTS-6568.

The `docs/subsystems.md` entry for `modelnearmissledger` still says "7-token `blocked_by` taxonomy". The code (`modelnearmissledger.cpp:26-36`, `modelautoswitch.cpp:106-217`) has 9 tokens. The `modelautoswitch` entry's gate list also omits `idle_end_of_session` and `downgrade_requires_active_work`. The document is the wrong side; this belongs to `review-contract`.

Quote: `modelnearmissledger`

### Row 282 — Low — ``

review-code, lane 24-shell-surface. **Disposition:** queued, ANTS-6576.

ANTS-1252 is wrong about the code in two places:

### Row 325 — Low — `auditdelta.cpp:52-53`

review-code, lane 27-audit-engine. **Disposition:** queued, ANTS-6547.

ANTS-1870 § 2.4 step 2 says `added = current ∉ priorOnChanged`. The code (`auditdelta.cpp:52-53`, ANTS-5085) uses all prior findings, so the spec text is stale. This is for `review-contract`.

Quote: `auditdelta.cpp:52-53`

### Row 335 — Low — `roadmaprender.cpp:56-63`

review-code, lane 28-roadmap-parse-render. **Disposition:** queued, ANTS-6573.

These comments still say `dropped` items are excluded or have no marker. ANTS-4977 changed the code to render them as 🚫 (`roadmaprender.cpp:56-63`, `roadmapparse.cpp:260`):

Quote: `roadmaprender.cpp:56-63`

### Row 352 — Low — `docs/subsystems.md:616`

review-code, lane 30-claude-session-widgets. **Disposition:** queued, ANTS-6552.

says the trackers "reset on `isCompactSummary`". The code skips those events (`claudetranscriptwalker.h:144`), as specs ANTS-1341 and ANTS-1407 also describe. The document is wrong; this goes to `review-contract`.

Quote: `isCompactSummary`

### Row 353 — Low — `claudestateresolver.cpp:39-42`

review-code, lane 30-claude-session-widgets. **Disposition:** queued, ANTS-6552.

The `claudestateresolver` module-map ladder (`awaitingInput → planMode → auditing → base`) leaves out the ANTS-5718 rung (`claudestateresolver.cpp:39-42`) and the Background state. The document is incomplete.

Quote: `awaitingInput → planMode → auditing → base`

### Row 364 — Low — `remotecontrol_review.cpp:1279-1280`

review-code, lane 31-mcp-review-verbs. **Disposition:** queued, ANTS-6563.

"runVerify takes (root, opts) and re-calls loadGateConfig internally".

### Row 423 — High — `src/changelogquery.h:109`

review-readability, lane 41-changelog-feedback, Q4. **Disposition:** queued, ANTS-6549.

This is false since ANTS-6282. `releaseLint` now counts ids in a bullet's indented continuation lines (changelogquery.cpp:527 and 545–546), and spec ANTS-3533 line 17 records that change. Someone trusting the header, which is the API contract, would think wrapped-bullet ids are ignored, and could "restore" that behaviour. That would bring back the false `shipped_not_cited` findings ANTS-6282 fixe…

Quote: `// any section. An id in continuation prose is a cross-reference, not a claim`

Remedy: say that a bullet's indented continuation lines cite too, as `parse()` reads them.

Also reported as: "code lane 36-changelog-feedback Low dim 2 changelogquery.h:108–110"

### Row 468 — Low — `flatpak/…AntsTerminal.yml:12`

review-code, lane 39-dev-python-ci-packaging. **Disposition:** queued, ANTS-6555.

"Targets org.kde.Platform//6.10", while `:41` is `runtime-version: '6.11'`.

Quote: `:41`

### Row 469 — Low — `ci.yml:109`

review-code, lane 39-dev-python-ci-packaging. **Disposition:** queued, ANTS-6555.

"no step in this workflow runs git", but `:217` runs `git ls-files`. This is harmless, because `ls-files` needs no credentials.

Quote: `:217`

### Row 470 — Low — `tools/measure/filemap_prototype.py:356`

review-code, lane 39-dev-python-ci-packaging, dim 2. **Disposition:** queued, ANTS-6555.

"run tools/filemap.py --write". That path does not exist; the script is `tools/measure/filemap_prototype.py`.

Quote: `tools/measure/filemap_prototype.py`

Remedy: name the real path in both.

Also reported as: "readability lane 47-tools-roadmap-surveys Low Q4 tools/measure/filemap_prototype.py:356"

### Row 474 — Medium — `src/vtstream.cpp:20-21`

review-readability, lane 01-vt-parser-pty, Q4. **Disposition:** queued, ANTS-6583.

This comment says the Pty destructor "has no thread affinity (it just closes an FD and waitpid's a child)". In fact `~Pty` blocks for up to about 1.5 s. Its own comment (ptyhandler.cpp:132–134) says blocking is safe only because it runs on the worker thread. `terminalwidget.cpp:723–725` says Pty and its notifier must be torn down on the thread that created them. A stranger trusting this comment c…

Quote: `// the existing Pty dtor's SIGHUP/SIGTERM/SIGKILL ladder, which has`

Remedy: state that destruction must run on the worker, and why.

### Row 475 — Low — `src/vtstream.cpp:35-36`

review-readability, lane 01-vt-parser-pty, Q4. **Disposition:** queued, ANTS-6583.

`writeLost` also fires on a fatal write or drain error (a dead master; ptyhandler.cpp:492 and :548, ANTS-4456). The `qWarning` text "on write overflow" misreports those cases. A stranger judging whether a UI banner is needed is told the loss only happens past the cap.

Quote: `// not silent. Rare (only past the 4 MiB EAGAIN queue cap), hence a log`

Remedy: say that both causes emit it.

### Row 476 — Low — `src/vtparser.cpp:481-483`

review-readability, lane 01-vt-parser-pty, Q4. **Disposition:** queued, ANTS-6583.

"Every other byte … is discarded to Ground" is not true for ESC, CAN or SUB. `processChar` intercepts those at 233–258 before `processString` sees them. A stranger reasoning about the peek states from this comment alone would think a second ESC is swallowed. It is not (see Correctness leads).

Quote: `// Kitty sequences survive. Every other byte — the dangerous ESC`

Remedy: name the C0 bytes that never reach this branch.

### Row 477 — Low — `src/ptyhandler.cpp:659-670`

review-readability, lane 01-vt-parser-pty, Q4. **Disposition:** queued, ANTS-6583.

The current rule is: keep `m_childPid` if the child is not reaped yet, and `finished(-1)` means it was not reaped. That rule is buried under the history of pre-fix code and of a removed signal (`childUnreapedAtEof`) that no longer exists. A stranger has to read two tickets' worth of past behaviour to find the one invariant.

Quote: `// ANTS-1131 — when waitpid(WNOHANG) returns 0, the child`

Remedy: state the rule in two lines and move the history to the commit log.

### Row 478 — Low — `src/vtparser.h:52-53`

review-readability, lane 01-vt-parser-pty, Q4. **Disposition:** queued, ANTS-6583.

These comments repeat constant values: `kMaxParams` (32, also at vtparser.h:138–140) and `kMaxStringBytes` (10 MiB, vtparser.h:47 and vtparser.cpp:192). `vtstream.h:290` repeats Pty's 16 KB read buffer, and `vtstream.cpp:35` repeats the 4 MiB queue cap. A stranger who changes a constant in its .cpp file leaves these lying. The vtstream.h:290 one feeds an in-flight memory bound.

Quote: `// True when a CSI's parameter list hit the 32-param DoS cap and one or`

Remedy: name the constant instead of its value.

### Row 479 — Low — `src/ptyhandler.cpp:357-358`

review-readability, lane 01-vt-parser-pty, Q4. **Disposition:** queued, ANTS-6583.

The code calls `execvpe`, not `execvp`. Line 186's "(same reason TERM* + workDir are forwarded explicitly above)" also points up the file, but the TERM forwarding is below, at 371. The ANTS-1994 comment at 378–387 corrects the first, so the reader recovers.

Quote: `// those buffers and calls execvp — no std::string or`

Remedy: say `execvpe`, and "below".

### Row 483 — High — `src/terminalgrid.h:787`

review-readability, lane 02-terminal-grid, Q4. **Disposition:** queued, ANTS-6578.

The member comment starting at 786 says the flag will "Pause scrollback insertion while the user is scrolled up". - That is false. The setter's comment (348-354) and `scrollUp` (2074-2077) show paused lines are still pushed into history. The flag only turns off the doubling guard. - A stranger reading the member would think lines are dropped while paused, and "fix" a non-bug or rely on behaviour …

Quote: `// setScrollbackInsertPaused). False = normal terminal behaviour.`

### Row 485 — Medium — `src/terminalgrid.cpp:2285`

review-readability, lane 02-terminal-grid, Q4. **Disposition:** queued, ANTS-6578.

The comment says the `startRow > endRow` case is caused by a resize or reflow. - That cause no longer exists: `resize` now calls `dropActiveHyperlink()` (3082), so no open link survives a resize. - What still reaches this branch is the cursor moving up while a link is open (cursor-up, cursor-position, reverse index). A `scrollDown` that does not move the start row also reaches it. - A stranger mi…

Quote: `// hyperlink's text into scrollback while leaving startRow`

### Row 490 — Medium — `src/terminalgrid.cpp:1016`

review-readability, lane 02-terminal-grid, Q4. **Disposition:** queued, ANTS-6578.

Reset (RIS, `ESC c`) keeps a hand-written list of ten callbacks, then rebuilds the whole object. - Nothing where the callbacks are declared (`terminalgrid.h` 715-730, 778-784) says a new callback must be added to that list. - A stranger adding a callback would see it silently wiped by `reset`. - The quotas solved the same problem by grouping into `MinuteQuotas`, with the note "a quota added here …

Quote: `auto responseCb = std::move(m_responseCallback);`

### Row 492 — Low — `src/terminalgrid.cpp:1466`

review-readability, lane 02-terminal-grid, Q4. **Disposition:** queued, ANTS-6578.

`vtparser.cpp:282` is the Escape-state handler, not the cap. The cap is `kMaxStringBytes` at `vtparser.cpp:187`, so a stranger following the pointer lands in the wrong place. - Fix: name `kMaxStringBytes` instead of a line number. - Cost: one edit (comment only). - Searched: src for kMaxString, and read vtparser.cpp 270-295.

Quote: `// The VT parser caps OSC payloads at 10MB (vtparser.cpp:282) to cover`

### Row 493 — Low — `src/terminalgrid.h:59`

review-readability, lane 02-terminal-grid, Q4. **Disposition:** queued, ANTS-6578.

This comment sits above `class CombiningMap` but describes `struct TermLine`, 50 lines below. A stranger reads it as describing `CombiningMap`. - Fix: move it to just above `struct TermLine`. - Cost: one edit (comment only).

Quote: `// A line stored in scrollback or screen, with wrap metadata`

### Row 494 — Low — `src/terminalgrid.cpp:3074`

review-readability, lane 02-terminal-grid, Q4. **Disposition:** queued, ANTS-6578.

Eight lines of history about an old wrong comment bury the current rule, which is "close the open link on resize". - Fix: keep the rule and move the history to the commit body. - Cost: one edit (comment only).

Quote: `// Indie-review-2026-05-14 lane-1 H2: the comment used to claim "close`

### Row 498 — Low — `src/terminalgrid.cpp:2153`

review-readability, lane 02-terminal-grid, Q4. **Disposition:** queued, ANTS-6578.

Insert-lines (IL) does not go through `scrollDown`; `insertLines` rotates by itself. Reverse-index and RI are the same command. Scroll-down (CSI T) is missing from the list. - A stranger would wrongly expect an IL fix to land here. - Fix: list RI and CSI T. - Cost: one edit (comment only).

Quote: `// reverse-index / RI / IL-driven downward scrolls (mirrors scrollUp,`

### Row 499 — Medium — `src/terminalwidget.h:1005`

review-readability, lane 03-terminal-widget-a, Q4. **Disposition:** queued, ANTS-6579.

The header gives the sign backwards. In the code, `1` means scroll up (`terminalwidget.cpp:3833`, `m_selectionScrollDirection = 1; // scroll up (increase offset)`), and the timer at cpp:306-308 says the same. Someone trusting the header would flip the direction of drag-to-scroll.

Quote: `int m_selectionScrollDirection = 0; // -1=up, 0=none, 1=down`

Remedy: change the comment to `1=up (older), -1=down (newer)`.

### Row 500 — Medium — `src/terminalwidget.cpp:2645-2647`

review-readability, lane 03-terminal-widget-a, Q4. **Disposition:** queued, ANTS-6579.

This points at code that is no longer there. The saved-image path now lives in `pasteFromClipboard` (moved by ANTS-5580). It also says that path is "safe by construction". But `pasteFromClipboard` (cpp:2783-2785) quotes it, because the configured folder "may hold a space or `$(`". Someone believing this comment could remove the `shellQuote` call on the saved-image path.

Quote: `// Shell safety. Unlike the saved-image path in keyPressEvent — whose`

Remedy: point the comment at `pasteFromClipboard` and drop "safe by construction".

### Row 501 — Low — `src/terminalwidget.cpp:1486`

review-readability, lane 03-terminal-widget-a, Q4. **Disposition:** queued, ANTS-6579.

This is backwards. The code draws a right-pointing triangle when `pr.folded` (collapsed), and the comments on lines 1488 and 1515 say so. Someone editing just this line could swap the fold icons.

Quote: `// Draw a small triangle (right = expanded, down = collapsed)`

Remedy: change it to "right = collapsed, down = expanded".

### Row 502 — Low — `src/terminalwidget.cpp:890-891`

review-readability, lane 03-terminal-widget-a, Q4. **Disposition:** queued, ANTS-6579.

The example on the next line says `"3m07s"`, but `.arg((ms % 60000) / 1000)` adds no leading zero, so the code produces `3m7s`. A reader trusts the wrong format, or a test is written against it.

Quote: `// A finished command's run time as the block chrome shows it: "850ms",`

Remedy: make the comment match the code, or pad the seconds (see Correctness leads).

### Row 503 — Low — `src/terminalwidget.cpp:2227`

review-readability, lane 03-terminal-widget-a, Q4. **Disposition:** queued, ANTS-6579.

There is no Shift+Up scroll path. `keyPressEvent` scrolls only on Shift+Home/End/PageUp/PageDown (lines 2111-2113), and Shift+Up goes to the shell as an arrow key (`encodeLegacyKey`). A reader would look for a scroll path that doesn't exist.

Quote: `// Other scroll-up paths (Shift+PageUp, Shift+Home, Shift+Up)`

Remedy: remove "Shift+Up".

### Row 504 — Low — `src/terminalwidget.cpp:2846`

review-readability, lane 03-terminal-widget-a, Q4. **Disposition:** queued, ANTS-6579.

`MainWindow::showDiffViewer` is now a thin entry point. Its own comment says "Dialog body lives in diffviewer.{cpp,h}" (mainwindow.cpp:2847). Someone following the pointer finds no dialog code there.

Quote: `// Fully async confirmation dialog, matching mainwindow.cpp::showDiffViewer's`

Remedy: point the comment at `diffviewer.cpp`.

### Row 505 — Low — `src/terminalwidget.cpp:2715-2746`

review-readability, lane 03-terminal-widget-a, Q4. **Disposition:** queued, ANTS-6579.

Thirty lines of review history (two past bugs, (a) and (b)) bury the current rule: "a configured image folder outside $HOME is replaced by the default, and checked before it is created". The same thing happens at cpp:93-113 (the retired OpenGL story above the constructor) and at cpp:2988-2994 ("The 7-bit sanitiser above missed…", where "above" now means the past, not the code).

Quote: `// 0.7.53 (2026-04-27 indie-review HIGH) — path`

Remedy: keep one line of rule per site and move the history to the commit log.

### Row 519 — Medium — `src/terminalwidget.cpp:5968`

review-readability, lane 04-terminal-widget-b, Q4. **Disposition:** queued, ANTS-6579.

The header comment says `onGridLineCompleted` runs only the grid-changing triggers. In fact it also emits the plugin `line` event (5982–5990) and fires every non-instant notify/sound/command trigger (6050–6069). Someone looking for where non-instant triggers fire would skip this function.

Quote: `// cursor was leaving. Runs the subset of trigger rules that mutate the grid`

Remedy: make the header list all three jobs.

### Row 521 — Medium — `src/terminalwidget.cpp:6231`

review-readability, lane 04-terminal-widget-b, Q4. **Disposition:** queued, ANTS-6579.

This comment presents call order as enough to keep per-style fonts. But `updateFontMetrics()` (541–560) rebuilds `m_fontBold`, `m_fontItalic` and `m_fontBoldItalic` from `m_font` on every call, and it has other callers (128, 605). So any later call drops a per-style family. It also means the `setFamily` calls at 6233–6235 are overwritten one line later, by line 6236. A reader would trust the orde…

Quote: `// customization MUST call setFontFamily FIRST, then the per-style`

Remedy: state that `updateFontMetrics` resets every style variant.

### Row 527 — Low — `src/terminalwidget.cpp:5582`

review-readability, lane 04-terminal-widget-b, Q4. **Disposition:** queued, ANTS-6579.

"this" refers to a comment that is no longer there. The current reason for the method sits under two paragraphs of history.

Quote: `// ANTS-3364 — this described the defect it was written against and not`

Remedy: keep only the final paragraph's reason.

### Row 528 — Low — `src/terminalwidget.cpp:6441`

review-readability, lane 04-terminal-widget-b, Q4. **Disposition:** queued, ANTS-6579.

`pasteToTerminal` can also run a command: `sendScratchpad` calls it with `submitAfter=true` (5157). So the reason given does not explain the choice.

Quote: `// is not used here because we want the command executed, not pasted.`

Remedy: state the real reason, or drop the sentence.

### Row 533 — High — `src/mainwindow.h:403`

review-readability, lane 05-mainwindow-a, Q4. **Disposition:** queued, ANTS-6560.

This comment is false. There are three different desktop-notification paths: - `showDesktopNotification`: uses the tray icon, else `notify-send`. - `onTriggerFired` "notify" (`mainwindow.cpp:2603-2619`): calls D-Bus `org.freedesktop.Notifications` directly, with no tray fallback. - `terminalwidget.cpp:2630`: runs `notify-send` directly with its own app name and icon. **What a stranger gets wrong:…

Quote: `// ANTS-4273 — one desktop-notification path for every caller. Returns`

### Row 534 — Medium — `src/mainwindow.cpp:2345`

review-readability, lane 05-mainwindow-a, Q4. **Disposition:** queued, ANTS-6560.

The comment goes on to list "fileChanged, post-approve/-decline on allowlist, etc.". `mainwindow.h:576-577` adds "after any async probe completes". In fact the only caller is `mainwindow_tabs.cpp:1035`. - **What a stranger gets wrong:** they will assume a file change or a finished probe already refreshes the whole status bar through this function, and will not add the call. - Fix: state the singl…

Quote: `// Called from: onTabChanged, plus any place that wants to force a`

### Row 537 — Low — `src/mainwindow.cpp:1657`

review-readability, lane 05-mainwindow-a, Q4. **Disposition:** queued, ANTS-6560.

The comment lists three items: the Roadmap button, the update action and the update check. The function also builds the version chip (1762–1781) and the peer-usage watcher (1722–1735). `mainwindow.h:590-593` repeats the "three". - Fix: drop the count and list what is built.

Quote: `// existing slots, then constructs the three orphan chrome items`

### Row 540 — Low — `src/mainwindow.cpp:1857`

review-readability, lane 05-mainwindow-a, Q4. **Disposition:** queued, ANTS-6560.

The comment points to a comment that no longer exists. `showDiffViewer` (2846–2888) was carved out into `diffviewer.cpp` and has no raise/activate comment at its end. - Fix: drop the cross-reference. The paragraph already explains the constraint on its own.

Quote: `// constraint as showDiffViewer (see the comment at the end of that`

### Row 541 — Low — `src/mainwindow.cpp:2408`

review-readability, lane 05-mainwindow-a, Q4. **Disposition:** queued, ANTS-6560.

Lines 58–71 of `claudeintegration.cpp` now hold `tabSpecificAcceptsTabIndex` and the start of the constructor, not the `setShellPid` behaviour the comment means. - Fix: cite `ClaudeIntegration::setShellPid` by name instead.

Quote: `// claudeintegration.cpp:58-71.`

### Row 542 — Low — `src/mainwindow.h:711`

review-readability, lane 05-mainwindow-a, Q4. **Disposition:** queued, ANTS-6560.

`kwinpositiontracker.h:40-41` says the class was renamed from `XcbPositionTracker` because "the class doesn't use XCB at all". - Fix: change the comment to say "KWin D-Bus position tracker".

Quote: `// XCB position tracker (Qt's pos()/moveEvent broken for frameless windows on KWin)`

### Row 544 — Medium — `src/mainwindow_menus.cpp:955`

review-readability, lane 06-mainwindow-b, Q4. **Disposition:** queued, ANTS-6560.

This is false whenever the theme has not changed. `applyTheme` returns early for the same theme (mainwindow.cpp:1289), and the ANTS-5034 comment at menus:253-255 says so too. Opacity actually reaches the terminals through `applyConfigToTerminal` → `setWindowOpacityLevel` (tabs.cpp:59). A reader who trusts this comment could delete that call as redundant, and opacity changes made in Preferences wo…

Quote: `// Opacity is now applied via per-pixel alpha in applyTheme() above`

Remedy: say that opacity reaches the terminals through `applyConfigToTerminal`. Cost: one edit (comment only). Searched: anchors for `Opacity is now` (no hit); read `applyTheme` at mainwindow.cpp:1281-1289.

### Row 545 — Medium — `src/mainwindow_repo.cpp:174`

review-readability, lane 06-mainwindow-b, Q4. **Disposition:** queued, ANTS-6560.

No 60 s timeout exists. Every cache entry, negative ones included, uses `kCacheTtlMs` (10 minutes, line 135). A reader would hunt for a 60 s constant, or tune 10 minutes thinking it affects only positive results.

Quote: `// Cache both hits and negative results — a 60 s`

Remedy: state that negative results share the 10-minute TTL, or add the separate negative TTL the comment describes. Cost: one edit (comment only). Searched: read the whole file; anchors for `negative` (no hit).

Also reported as: "readability lane 06-mainwindow-b Info correctness lead src/mainwindow_repo.cpp:180"

### Row 546 — Medium — `src/mainwindow_tabs.cpp:1061`

review-readability, lane 06-mainwindow-b, Q4. **Disposition:** queued, ANTS-6560.

This 21-line block documents `terminalForCaller`, but it sits above the unrelated `MainWindowRootProvider` class (1085) and runs straight into the ANTS-4932 comment for that class. It says "Three cases", while line 1145 says "four-case". It claims "the four terminalForCaller call sites in this file". There are none in this file: four are in mainwindow_mcp.cpp and three in remotecontrol_terminal.c…

Quote: `// ANTS-1392 — caller_cwd-anchored terminal lookup. Walks every tab`

Remedy: move the block above `terminalForCaller` and drop the call-site count. Cost: one edit. Moving a comment moves no pinned text; anchor 282 pins only the `MainWindow::terminalForCaller` signature. Searched: `terminalForCaller(` across *.cpp/*.h.

### Row 553 — Low — `src/mainwindow_mcp.cpp:357`

review-readability, lane 06-mainwindow-b, Q4. **Disposition:** queued, ANTS-6560.

Both line-number references in this file are stale. remotecontrol.cpp:347 is now `kUnrecognisedFormatExpected()`. The `ptyhandler.cpp:171` reference at line 83 now lands on a comment inside `Pty::start`, not the `environ`-copy loop.

Quote: `// (matches the IPC verb at remotecontrol.cpp:347).`

Remedy: name the function, not the line. Cost: one edit (comment only).

### Row 554 — Low — `src/mainwindow_menus.cpp:847`

review-readability, lane 06-mainwindow-b, Q4. **Disposition:** queued, ANTS-6560.

Nothing called `setupPromptNav` exists.

Quote: `// QShortcuts (see view-menu comment near setupPromptNav). The`

Remedy: point at the "Previous Prompt" comment in `setupViewMenu`. Cost: one edit (comment only). Searched: src for `setupPromptNav` (only this line).

### Row 555 — Low — `src/mainwindow_menus.cpp:981`

review-readability, lane 06-mainwindow-b, Q4. **Disposition:** queued, ANTS-6560.

This sends the reader to `setupStatusBarChrome`. The binding is actually in `setupClaudeMcpProviders` (mcp.cpp:73-91), which `setupStatusBarChrome` calls at mainwindow.cpp:1737.

Quote: `// socket itself is bound/unbound only at launch (see`

Remedy: name `setupClaudeMcpProviders`. Cost: one edit (comment only).

### Row 556 — Low — `src/mainwindow_menus.cpp:45`

review-readability, lane 06-mainwindow-b, Q4. **Disposition:** queued, ANTS-6560.

This is history, and its "the helpers contain the per-menu body verbatim (no behaviour change)" has not been true since many later ANTS edits. A reader might think the helpers are frozen copies.

Quote: `// ANTS-1181 — setupMenus() was historically a 947-line block stuffed`

Remedy: drop it to the commit history. Cost: one edit (comment only). census_number:0 at menus:45 confirmed.

### Row 557 — Low — `src/mainwindow_tabs.cpp:1205`

review-readability, lane 06-mainwindow-b, Q4. **Disposition:** queued, ANTS-6560.

No truncation happens here. `setTabLabel` does it (mainwindow.cpp:2588), so a reader changing the cap would look here and find nothing.

Quote: `// refresh tick. Truncated to the same 30-char ceiling the`

Remedy: say that `setTabLabel` truncates. Cost: one edit (comment only).

### Row 558 — Low — `src/mainwindow_sessions.cpp:217`

review-readability, lane 06-mainwindow-b, Q4. **Disposition:** queued, ANTS-6560.

The comment argues "can't", then ends by calling direct writing "equivalent" and merely "one lookup" cheaper. A reader cannot tell whether calling `setTabTitleForRemote` would be wrong or just slower.

Quote: `// honor it. Can't call setTabTitleForRemote here because`

Remedy: state the one real reason. Cost: one edit (comment only).

### Row 559 — Low — `src/mainwindow_tabcolors.cpp:87`

review-readability, lane 06-mainwindow-b, Q4. **Disposition:** queued, ANTS-6560.

The rule for adding a palette colour comes after a paragraph about a withdrawn note. The last sentence (ANTS-4690 caps the wash at 4.5:1) may make the "no worse than the worst" bar unnecessary, so a reader adding a colour cannot tell which test binds.

Quote: `// ON CONTRAST, because the previous note here was reassuring and wrong.`

Remedy: state the current entry test first and move the history out. Cost: one edit (comment only).

### Row 560 — Low — `src/mainwindow_tabcolors.cpp:262`

review-readability, lane 06-mainwindow-b, Q4. **Disposition:** queued, ANTS-6560.

The custom colour picker has already shipped (lines 200-229, ANTS-1374), so this "future" reason is stale.

Quote: `// keeps the format future-proof if a custom-colour entry lands`

Remedy: say alpha is stored because custom picks can carry it. Cost: one edit (comment only).

### Row 561 — Low — `src/mainwindow_repo.cpp:207`

review-readability, lane 06-mainwindow-b, Q4. **Disposition:** queued, ANTS-6560.

The ANTS-5080 `errorOccurred` block separates this comment from the line it describes (223).

Quote: `// ANTS-1137 — mark in-flight before start so a re-entry within`

Remedy: move it above line 223. Cost: one edit (comment only).

### Row 562 — Low — `src/mainwindow_quake.cpp:162`

review-readability, lane 06-mainwindow-b, Q4. **Disposition:** queued, ANTS-6560.

The code calls `create()`, not `winId()`.

Quote: `// handshake. winId() on a QWidget forces a native window backing.`

Remedy: name `create()`. Cost: one edit (comment only).

### Row 565 — Medium — `src/globalshortcutsportal.cpp:111`

review-readability, lane 07-chrome-widgets, Q4. **Disposition:** queued, ANTS-6550.

Three comments disagree about what happens to the portal session when the app goes away: - Lines 96-97 say orphan sessions are "released only when xdg-desktop-portal itself restarts". - Lines 111-112 say a dropped connection "has the same effect" as Close. - Lines 21-22 say the session lives for the D-Bus client's lifetime unless Close is called. The comment also credits this destructor with fixi…

Quote: `// before it completes, the kernel-level connection drop has the`

Remedy: state once what the portal does when the connection drops, and drop the history.

### Row 569 — Medium — `src/titlebar.h:44`

review-readability, lane 07-chrome-widgets, Q4. **Disposition:** queued under ANTS-5840.

The comment promises the full title in a tooltip. But titlebar.cpp:38 makes the title label transparent to mouse events, and Qt's hover lookup skips such widgets, so the tooltip most likely never appears. A stranger would trust that long titles are recoverable on hover.

Quote: `// buttons. ElidedLabel truncates to fit; full title stays readable in`

Remedy: correct the comment, or change the attribute (see Correctness leads).

### Row 570 — Low — `src/coloredtabbar.cpp:184`

review-readability, lane 07-chrome-widgets, Q4. **Disposition:** queued, ANTS-6550.

The tab colour has been a full-height wash since 0.6.26 (lines 368-369), not a bottom strip. The header (line 91) still says "colour strip". A stranger narrowing `update()` to the strip would leave stale wash on screen.

Quote: `// Bottom-strip area of the changed tab is the only dirty region;`

Remedy: say the whole tab rect is dirty.

### Row 571 — Low — `src/coloredtabbar.cpp:368`

review-readability, lane 07-chrome-widgets, Q4. **Disposition:** queued, ANTS-6550.

The comment opens with release history and a quoted user request. The rule that now governs the wash, the cap at the name (ANTS-4690, 386-389), comes last. A stranger reads two pages of history before reaching the constraint they must keep.

Quote: `// 0.6.26 — replace the old 3px bottom strip with a full vertical`

Remedy: lead with the current rule and move the history to the changelog.

### Row 573 — Low — `src/coloredtabbar.cpp:108`

review-readability, lane 07-chrome-widgets, Q4. **Disposition:** queued, ANTS-6550.

User themes loaded from JSON set `bgSecondary` freely (themes.cpp:395, `isUserTheme`). So the mid-luminance case can arrive today from user input, not only from "a future theme". A stranger would treat the one-direction search as safe.

Quote: `// Precondition (true for all 11 shipped themes): every theme's`

Remedy: name user themes in the precondition.

### Row 574 — Low — `src/coloredtabbar.h:157`

review-readability, lane 07-chrome-widgets, Q4. **Disposition:** queued, ANTS-6550.

That file is listed in `.gitignore` (line 151), so a stranger working from a clone will not have it. themedstylesheet.cpp:90 points at it too.

Quote: `// INVESTIGATION_tab_close_x.md). normal tints the resting glyph,`

Remedy: cite the ticket, or a tracked document.

### Row 575 — Low — `src/coloredtabbar.h:190`

review-readability, lane 07-chrome-widgets, Q4. **Disposition:** queued, ANTS-6550.

It also emits `tabsChanged` (ANTS-5645, lines 203-209), so it is no longer its sole purpose. A stranger might delete or bypass the class thinking it only installs the bar.

Quote: `// Trivial QTabWidget subclass whose sole purpose is to install a`

Remedy: mention the signal.

### Row 576 — Low — `src/kwinpositiontracker.h:8`

review-readability, lane 07-chrome-widgets, Q4. **Disposition:** queued, ANTS-6550.

This is history, and it is now misleading. It says `moveViaKWin` and `centerWindow` gate on `kwinPresent()`, but the only caller is `runKWinScript` (kwinpositiontracker.cpp:51). A stranger may add redundant checks at callers, or think the callers do the gating.

Quote: `// ANTS-1142 — kwinPresent() lifted from`

Remedy: say `runKWinScript` gates on it. Searched: whole-tree workspace_search for `kwinPresent` — src/ shows only kwinpositiontracker.cpp:51 and the definition.

### Row 577 — High — `src/verifytrust.cpp:223`

review-readability, lane 08-dialog-chrome-theme, Q4. **Disposition:** queued, ANTS-6557.

This comment is false. `config.cpp` does not rename the bad file. It calls `rotateCorruptFileAside` (`configbackup.h:41`), which *copies* it to `<file>.corrupt-<milliseconds>`, retries if the name is taken, and keeps only the newest 5. This file renames to `<file>.corrupt.<seconds>`, with no retry and no pruning. A stranger who trusts the comment and writes cleanup or recovery code for one patter…

Quote: `// Same naming as the config.cpp / themes.cpp rotation (ANTS-1179).`

Remedy: correct the comment to name the actual naming, or switch to the shared helper.

### Row 579 — Medium — `src/verifytrust.h:97`

review-readability, lane 08-dialog-chrome-theme, Q4. **Disposition:** queued, ANTS-6557.

This path is wrong. `defaultTrustFilePath()` uses `AppConfigLocation`. Both binaries set the application name to "Ants Terminal" (`main.cpp:219`, `mcpdmain.cpp:173`) and set no organisation name, so the real file is `~/.config/Ants Terminal/verify-trust.json`. The test `tests/features/mcpd_trust_prompt/test_mcpd_trust_prompt.cpp:78` uses that real path. The same wrong path is at `verifytrust.h:87…

Quote: `// Default ctor uses ~/.config/ants-terminal/verify-trust.json.`

Remedy: state the real directory in all three comments.

### Row 581 — Low — `src/updatedialog.h:4`

review-readability, lane 08-dialog-chrome-theme, Q4. **Disposition:** queued, ANTS-6557.

Not every dialog here is non-modal. `ModalClient::showPrompt` runs `box.exec()` (`verifytrustmodal.cpp:176`), and `reviewdialogbase.cpp:433` uses `QMessageBox::warning`. A stranger who trusts this line will assume there is no modal path.

Quote: `// § 2.4 steps 2–6). Non-modal, like every dialog here (dialogs.md). It offers`

Remedy: limit the claim to this dialog, or name the exceptions.

### Row 583 — Low — `src/reviewdialogbase.cpp:428-430`

review-readability, lane 08-dialog-chrome-theme, Q4. **Disposition:** queued, ANTS-6557.

"The four performFoldIn callsites" is history, and the header (`reviewdialogbase.h:4-6`) names three dialogs. The current rule ("surface a failure here") is buried under it.

Quote: `// contention) must not silently look like success. The four performFoldIn`

Remedy: keep the one-line why and drop the count.

### Row 584 — Low — `src/verifytrustmodal.h:1`

review-readability, lane 08-dialog-chrome-theme, Q4. **Disposition:** queued, ANTS-6557.

No class has this name; the class is `VerifyTrust::ModalClient`. The same wrong name is at `verifytrustmodal.cpp:1`, `mainwindow.cpp:1182` and `remotecontrol.h:108`. A stranger who searches for the name finds only comments.

Quote: `// VerifyTrustModalClient — chrome-layer subclass of`

Remedy: use `ModalClient`.

### Row 590 — Medium — `src/auditdialog_catalogue.cpp:858`

review-readability, lane 09-audit-dialog, Q4. **Disposition:** queued, ANTS-6546.

The code says otherwise. `clang_tidy` registers with autoSelect `false` (line 887); only `available` depends on the build dir. A stranger reading the comment would expect clang-tidy to run by default once a compile database exists.

Quote: `// 1. Auto-select is gated on presence of compile_commands.json`

Remedy: say that availability, not auto-select, is gated.

### Row 596 — Low — `src/auditdialog_triage.cpp:170`

review-readability, lane 09-audit-dialog, Q4. **Disposition:** queued, ANTS-6546.

The comment says `endpointEgressError` is "checked above", but it runs below, at 185-191. Someone reordering this function would be misled about when the check happens.

Quote: `// finding that snippet IS the credential. endpointEgressError, checked`

Remedy: change "above" to "below".

### Row 597 — Low — `src/auditdialog.h:38`

review-readability, lane 09-audit-dialog, Q4. **Disposition:** queued, ANTS-6546.

The next line names `mainwindow.cpp:1351`. The only call is at `src/mainwindow_menus.cpp:471`.

Quote: `// nullptr default per cold-eyes HIGH #3 — single call site`

Remedy: drop the location, or name it without a line number.

### Row 598 — Low — `src/auditdialog.cpp:3395`

review-readability, lane 09-audit-dialog, Q4. **Disposition:** queued, ANTS-6546.

No `.audit_history` file exists. The snapshot goes to `.audit_cache/trend.json` (`trendPath`).

Quote: `// fresh appendSnapshot per filter click before — the .audit_history`

Remedy: name trend.json.

### Row 599 — Low — `src/auditdialog_catalogue.cpp:1182`

review-readability, lane 09-audit-dialog, Q4. **Disposition:** queued, ANTS-6546.

Nothing reads `audit_rules.semgrep.yaml`; the semgrep command passes only `--config=p/…`.

Quote: `// audit_rules.semgrep.yaml (or .semgrep.yml) and it's picked up`

Remedy: drop the claim, or wire the file in.

### Row 600 — Low — `src/auditdialog.cpp:447`

review-readability, lane 09-audit-dialog, Q4. **Disposition:** queued, ANTS-6546.

The call is in `handleCheckOutput` (2971). The section banners from 423 to 462 describe filtering, parsing and a "tiny state machine", none of which lives in this file any more. A stranger hunting for the parser lands on empty sections.

Quote: `// here. The single call site at onCheckFinished now uses the`

Remedy: cut those banners down to one pointer to AuditEngine / AuditHygiene.

### Row 601 — Low — `src/auditdialog_catalogue.cpp:103`

review-readability, lane 09-audit-dialog, Q4. **Disposition:** queued, ANTS-6546.

Five checks in four lanes follow (1, 1b twice, 2, 3).

Quote: `// Each of the three lanes below addresses a distinct "feature-e2e`

Remedy: drop the count.

### Row 611 — Medium — `src/auditrunner.cpp:1506`

review-readability, lane 10-audit-engine-a, Q4. **Disposition:** queued, ANTS-6547.

The comment says a parameter `*offending` receives the escape in that braces form. There is no `offending`: the parameter is `errReason`, and the text written is `"include path %1 (entry %2): %3"` (1586–1588). The comment also sits above the two size constants, not above the function. Anyone matching on the error text by this form gets it wrong.

Quote: `// "{file: …, include: …, reason: …}") and the function short-circuits.`

Remedy: name `errReason` and state the real format, directly above `validateCompileCommandsImpl`.

### Row 612 — Low — `src/auditrunner.cpp:666`

review-readability, lane 10-audit-engine-a, Q4. **Disposition:** queued, ANTS-6547.

This comment describes `allocSarifPath` (712) but sits above the unrelated reaper block, so it reads as documenting `reapStaleFallbackArtifacts`. It also claims a "16-byte random suffix". The code keeps 16 hex characters (`.toHex().left(16)`, 717), which is 8 bytes of randomness. A stranger judging the anti-squatting strength trusts the wrong figure.

Quote: `// Fallback SARIF path when .audit_cache is read-only.`

Remedy: move it above `allocSarifPath` and say 16 hex chars (8 bytes).

### Row 613 — Low — `src/auditrunner.cpp:2467`

review-readability, lane 10-audit-engine-a, Q4. **Disposition:** queued, ANTS-6547.

This list calls mypy a JSON tool and leaves out gitleaks. `isJsonFindingTool` (777–781) has gitleaks and not mypy, and mypy is parsed line by line. A stranger adding a tool trusts the wrong list.

Quote: `// STDERR (only the JSON tools — ruff/bandit/semgrep/trivy/mypy/`

Remedy: correct the list, or point at `isJsonFindingTool`.

### Row 614 — Low — `src/auditrunner.cpp:39`

review-readability, lane 10-audit-engine-a, Q4. **Disposition:** queued, ANTS-6547.

The file header claims tests cover "pool isolation", but no pool exists (`auditrunner.h:13`: "There is no `m_auditPool` thread pool"). Lines 22–23 also refer to two "v2 will parse…" deferrals "below", which no longer appear anywhere in the file (I read all of it). A stranger goes looking for both.

Quote: `// pool isolation.`

Remedy: drop the pool clause and the "deferrals below" sentence.

### Row 615 — Low — `src/auditrunner.cpp:2489`

review-readability, lane 10-audit-engine-a, Q4. **Disposition:** queued, ANTS-6547.

This names the difference from the finished handler but not the reason. The two handlers' opening steps (elapsed time, `drain()`, the `outputCapped` check) are also copied, and ANTS-5085 patched both. A stranger tidying the inconsistency by merging channels here cannot tell whether stderr-only is deliberate.

Quote: `// Stderr only, unlike the finished handler's merge (ANTS-6198).`

Remedy: one line saying why this path takes stderr only.

### Row 616 — Low — `src/auditrulequality.h:106`

review-readability, lane 10-audit-engine-a, Q4. **Disposition:** queued, ANTS-6547.

`save()` returns early when nothing has changed (`auditrulequality.cpp:192`), so it does not force anything. A caller expecting a write after `reload()` or on a clean tracker gets none.

Quote: `// Force-flush to disk. Called automatically on destruction; exposed`

Remedy: say "saves if changed".

### Row 618 — Medium — `src/auditscope.h:47`

review-readability, lane 11-audit-engine-b, Q4. **Disposition:** queued, ANTS-6547.

A newcomer would believe a change to `runGit` covers every git call in this file. Two more private runners sit in auditscope.cpp.

Quote: `// Impure: the audit code's one git runner (cache, scope and SARIF provenance).`

Remedy: name `runGitRaw` and `runGitSucceeds` as siblings that need the same change.

### Row 622 — Medium — `src/auditcache.h:55`

review-readability, lane 11-audit-engine-b, Q4. **Disposition:** queued, ANTS-6547.

`Manifest` has no `history` member. History is only in `raw["history"]`. The note at 61–63 about a "QJsonArray-shaped value" sits above `raw` and names no field. A newcomer would look for `m.history`.

Quote: ` // lastRun and history mirror the JSON shape verbatim; loaders `

Remedy: say that history is read from `raw["history"]`.

### Row 623 — Low — `src/auditcache.h:23`

review-readability, lane 11-audit-engine-b, Q4. **Disposition:** queued, ANTS-6547.

"The manifest fields" invites passing the manifest's `iso_timestamp`, which contains colons. The function needs `IsoNow::forFilename`. The `IsoNow` comment ten lines down partly corrects this.

Quote: ` // gitShaShort are the manifest fields (§ 2.1 of spec). Returns `

Remedy: say `isoBasename` must be `IsoNow::forFilename`.

### Row 624 — Medium — `src/auditengine.h:514`

review-readability, lane 11-audit-engine-b, Q4. **Disposition:** queued, ANTS-6547.

The documented format is not what the code produces: - **Location line:** the code writes ``At `FILE:LINE` (rule `id`).`` on its own continuation line, or ``Rule `id`.`` when there is no location. - **Title:** the first sentence of the message, cut to 80 characters and markdown-escaped, not MESSAGE. - **ID:** comes from `renderId(idPrefix, …)`, not a fixed `ANTS-`. The `idPrefix` parameter is not…

Quote: `// to one bullet - 📋 [ANTS-N] **MESSAGE.** at FILE:LINE (rule).`

Remedy: rewrite the format block from the code at auditengine.cpp:1465–1497 and document `idPrefix`.

### Row 631 — Low — `src/auditengine.h:133`

review-readability, lane 11-audit-engine-b, Q4. **Disposition:** queued, ANTS-6547.

`AuditDialog::applyFilter`, `parseFindings` and `capFindings` no longer exist (auditdialog.h:249–255 says they moved). "Behaviour byte-identical to the prior method" (line 136) stopped being true after the ANTS-2118 carriage-return strip and the ANTS-5085 size cap. "Pure" misleads, because the function reads source files. Lines 143 and 161 have the same stale counterpart wording.

Quote: `// Pure-function counterpart of AuditDialog::applyFilter. Takes`

Remedy: drop the counterpart and byte-identical history, and say it reads project files for the context filter.

### Row 632 — Low — `src/auditengine.h:411`

review-readability, lane 11-audit-engine-b, Q4. **Disposition:** queued, ANTS-6547.

`summariseSarif` also checks whether a file exists and lists the cache directory to find an html sibling (auditengine.cpp:1035–1067).

Quote: `// no I/O outside the read.`

Remedy: mention the html sibling probe.

### Row 636 — Low — `src/audithygiene.h:1`

review-readability, lane 11-audit-engine-b, Q4. **Disposition:** queued, ANTS-6547.

The file header describes only the two config parsers. The file also holds framework detection, which reads files, and the `lineHasCode` lexer. A newcomer would not expect the lexer here.

Quote: `// Pure parsers that read project-local tool-calibration files and return`

Remedy: widen the header to list all four functions.

### Row 637 — High — `src/roadmapstore.cpp:2280`

review-readability, lane 12-roadmap-store-a, Q4. **Disposition:** queued, ANTS-6574.

The header says this lookup returns nothing for a project that is "not registered" (`roadmapstore.h:290`). The sibling `projectIdForSlug` says nothing comes back when there is "no such project" (`.h:281-282`). Neither query filters out `deregistered_at`, so both return the id of a deregistered project, and so does `projectIdContaining`, which calls `projectIdForRoot`. Two comments elsewhere say t…

Quote: `q.prepare(QStringLiteral("SELECT project_id FROM project WHERE root = ?"));`

Remedy: in the three `projectIdFor*` doc comments, say that deregistered rows are returned on purpose (mail reaches them) and that `readProject*` is the call that hides them.

### Row 641 — Medium — `src/roadmapstore.cpp:3894`

review-readability, lane 12-roadmap-store-a, Q4. **Disposition:** queued, ANTS-6574.

The stated reason is wrong. `ANTSX-0001` can never match `ANTS-[0-9]*`, because the hyphen already excludes it. What the `[0-9]` actually keeps out is synthesised `ANTS-S0001` ids (the header says so at `.h:1237-1239`) and hyphenated sibling prefixes such as `ANTS-X`. The twin comment at 3918-3920 (`ANTSX-S0001`) has the same error. A stranger who simplifies the pattern and checks the stated reas…

Quote: `// sibling prefix sharing a leading run (ANTS / ANTSX) cannot contribute.`

Remedy: restate both comments as "keeps `-S` synthesised ids and hyphen-extended prefixes out", matching the header.

### Row 645 — High — `src/roadmapmigrateload.h:60`

review-readability, lane 13-roadmap-store-b, Q4. **Disposition:** queued, ANTS-6574.

The comment says "Both match passes in matchItems() require `idFromMigration`", so `matchedOrigin` could only ever hold one value. That is false: - The first pass (roadmapmigrateload.cpp:540–553) matches any stored row by its folded id, with no `idFromMigration` test. A hand-filed row matched by id is reported `matched:true` (689–701). - Only the two weak-key passes (via `weakKeyCandidates`, 566)…

Quote: `// matched row is migration-allocated BY CONSTRUCTION — a field for it`

Remedy: correct the comment (three passes; only the two weak ones are migration-only), or add the field.

### Row 647 — Medium — `src/roadmapmigrateverb.h:96`

review-readability, lane 13-roadmap-store-b, Q4. **Disposition:** queued, ANTS-6574.

This 17-line comment documents `loadInOpenTransaction` (line 188), but it sits directly above `struct PlannedId`'s own comment (113) with no break. Two structs then separate it from its function. A reader takes it for the start of `PlannedId`'s description. Someone changing `loadInOpenTransaction` never sees its contract ("the store must be Access::Bulk and already in a transaction; on failure NO…

Quote: `// ANTS-4491 § 4.3 — the migration half of a dialect convert, run inside a`

Remedy: move lines 96–112 to just above line 188.

### Row 658 — Low — `src/roadmapmigrateverb.cpp:393`

review-readability, lane 13-roadmap-store-b, Q4. **Disposition:** queued, ANTS-6574.

The ANTS-5335 history is spliced into the middle of a sentence. "Vestige could detect that" now refers back past it, to the github-task-list behaviour two sentences earlier.

Quote: `// through the same predicate, which this used to restate as ants-v1 only. Vestige could detect that only by`

Remedy: move the ANTS-5335 sentence after the Vestige one, or into the commit history.

### Row 660 — High — `src/roadmapparse.h:475`

review-readability, lane 14-roadmap-parse-render-a, Q4. **Disposition:** queued, ANTS-6573.

**This comment says a `dropped` item renders with an empty marker and so produces no record. That is false.** `roadmaprender.cpp:44` now renders `dropped` as 🚫 (ANTS-4977), and `stripInlineEmoji` accepts 🚫. The same false claim is at `roadmapparse.cpp:2438-2442` (*"whose emoji is the empty string by design"*). A stranger would trust that `parseAntsV1Bullet` filters out dropped items, and build on…

Quote: `// § 3.11 makes a fifth glyph an anti-pattern), so it produces no record here,`

### Row 671 — Low — `src/roadmapwrite.cpp:86`

review-readability, lane 14-roadmap-parse-render-a, Q4. **Disposition:** queued, ANTS-6573.

**This comment ties `kNameCap` to the divergence guard alone, but the Layman-gate message reuses it at 774.** A stranger who changes the cap for one refusal changes it for both. Fix: say in the comment that both refusals use it, or give each refusal its own constant.

Quote: `// Named in the refusal. Capped because the divergence this guard was written`

### Row 672 — Low — `src/roadmapwrite.cpp:765`

review-readability, lane 14-roadmap-parse-render-a, Q4. **Disposition:** queued, ANTS-6573.

**Two layers of history disagree here.** ANTS-4628 says the gate names only what this write touched, so the list is always within reach. ANTS-5267 then says a whole-project write broke that. The current rule (cap at `kNameCap`, state the total, point at `gate_failures`) is buried under both. Fix: state the current rule in one line and move the history to the commit or spec.

Quote: `// the remedy. A whole-project write broke that assumption —`

### Row 673 — Low — `src/roadmapparse.cpp:7`

review-readability, lane 14-roadmap-parse-render-a, Q4. **Disposition:** queued, ANTS-6573.

**The file header says the bodies were "moved verbatim" and no behaviour changed.** Dozens of later ANTS changes are in the file now. Line 280's *"Every comment below is the pattern's own, moved verbatim with it"* is false the same way: the ANTS-4077, 4597 and 4608 comments were added later. Fix: drop the "verbatim / no behaviour changed" claims.

Quote: `// in namespace RoadmapParse. No parsing behaviour changed.`

### Row 674 — Low — `src/roadmapparse.cpp:36`

review-readability, lane 14-roadmap-parse-render-a, Q4. **Disposition:** queued, ANTS-6573.

**This comment heads an anonymous namespace that is empty (34-39).** The helpers it describes live elsewhere. Fix: delete the empty namespace and its comment.

Quote: `// ANTS-1428 — GFM-task-list adapter helpers. All operate on`

### Row 675 — Low — `src/roadmapparse.cpp:430`

review-readability, lane 14-roadmap-parse-render-a, Q4. **Disposition:** queued, ANTS-6573.

**`rxKind` is no longer case-insensitive or anchored.** ANTS-4065 dropped both, as its own comment at 293 says. Line 348 makes the same stale claim (*"unlike the anchored Kind:/Layman:/Evidence: labels"*). A stranger could "restore parity" on `rxKind`. Fix: name only `rxLayman` as the case-insensitive sibling, in both comments.

Quote: `// ANTS-3407 — CaseInsensitiveOption for parity with rxLayman/rxKind, so a`

### Row 676 — Low — `src/roadmapparse.cpp:1935`

review-readability, lane 14-roadmap-parse-render-a, Q4. **Disposition:** queued, ANTS-6573.

**There are now three compound entries.** ANTS-4065 added `"process + tooling"` → `chore`, and its first term, `process`, is not a kind. Fix: correct the count and state how the third entry maps.

Quote: `// than restated. The two compound entries map to their FIRST term, per that`

### Row 677 — Low — `src/roadmapparse.h:385`

review-readability, lane 14-roadmap-parse-render-a, Q4. **Disposition:** queued, ANTS-6573.

**This comment describes `authoredLinkTypes` / `linkKeyForType`, but it sits above `stripTrailingCaretAnchor`.** A stranger reads the wrong comment for each declaration. Fix: move the comment down to the two declarations at 391-392.

Quote: `// The four link types in the order the render writes them, and each one's`

### Row 678 — Low — `src/roadmapparse.h:12`

review-readability, lane 14-roadmap-parse-render-a, Q4. **Disposition:** queued, ANTS-6573.

**Wrong library.** `CMakeLists.txt:453-478` builds `roadmapparse.cpp`, `roadmapindex.cpp` and `passheadingwrite.cpp` into `ants_roadmapparse_lib`. `passheadingwrite.h:6` and `roadmapindex.h:2` make the same stale claim. A stranger adding a dependency would edit the wrong target. Fix: name `ants_roadmapparse_lib` in all three headers.

Quote: `// Qt6::Core only; lives in ants_core_lib so RoadmapDialog, the remotecontrol`

### Row 682 — Low — `src/roadmapclock.h:1`

review-readability, lane 14-roadmap-parse-render-a, Q4. **Disposition:** queued, ANTS-6573.

**Not every path does.** `PassHeadingWrite::flipPassStatus` (`passheadingwrite.cpp:269`) and `annotatePass` (`:331`) read `QDate::currentDate()`. A stranger who sets `setTodayForTest` would expect pass-heading dates to follow, and they do not. Fix: route those two calls through `RoadmapClock::today()`, or narrow the claim.

Quote: `// ANTS-4501 § 2.2 — the one seam every path reads "today" through.`

### Row 683 — Medium — `src/roadmaprender.h:8-9`

review-readability, lane 15-roadmap-parse-render-b, Q4. **Disposition:** queued, ANTS-6573.

This header opens the render's main file. It tells a stranger that dropped items are never published. That stopped being true at ANTS-4977: `roadmaprender.cpp:56-57` publishes `dropped` with 🚫, and `roadmap-data-model.md:97` now says § 7.5 "excludes `internal` ones". A stranger trusting this header would add a dropped filter, or debug a "missing" dropped item that is not missing.

Quote: `// roadmap-data-model.md excludes internal and dropped), never in detail.`

Remedy: change the parenthesis to "excludes `internal`" and point to ANTS-4977.

### Row 684 — Medium — `src/roadmapfoldin.cpp:593`

review-readability, lane 15-roadmap-parse-render-b, Q4. **Disposition:** queued, ANTS-6573.

The same stale claim, used here to justify the store floor. A stranger asking "is the store floor still needed now that dropped is published?" could decide it is not. In fact `internal` items and deleted ids still justify it, but the comment no longer says so.

Quote: `// by design (roadmap-data-model.md § 7.5 excludes internal and dropped),`

Remedy: name `internal` and deleted ids as the reasons, and drop `dropped`.

### Row 685 — Medium — `src/roadmapsource.h:296-297`

review-readability, lane 15-roadmap-parse-render-b, Q4. **Disposition:** queued, ANTS-6573.

This says every store record comes from `bulletText()`, which emits the ants-v1 bullet form. Since ANTS-5087, `recordFor()` (roadmapsource.cpp:127-137) builds pass-headings records from `passBlockText()` and `parsePassHeadingBlock()`. A stranger reasoning about what `fmt` (the project's id format) can affect would conclude no dialect but ants-v1 ever reaches this path. That is false, and nothing …

Quote: `// RoadmapRender::bulletText(), which emits ants-v1, so the GFM bold branch the`

Remedy: say "every ants-v1 record", and note that pass-headings records ignore `fmt`.

### Row 686 — Low — `src/roadmapsource.h:176-177`

review-readability, lane 15-roadmap-parse-render-b, Q4. **Disposition:** queued, ANTS-6573.

`storeFor()` has five production callers, not one: remotecontrol_roadmap_query.cpp:1044, roadmapdialog.cpp:707, roadmapfoldin.cpp:418 and :974, and claudestatuswidgets.cpp:1520. A stranger changing its contract would think there was one call site to check.

Quote: `// wrapper still OWNS the decision — it is the only caller — but the decision is`

Remedy: drop "it is the only caller".

### Row 688 — Low — `src/roadmaprender.cpp:138-161`

review-readability, lane 15-roadmap-parse-render-b, Q4. **Disposition:** queued, ANTS-6573.

This long rationale for suppression by presence, `anchored` and `offset >= 0` sits above the one-line `bulletText(it)` wrapper. The code it explains is in `trailerLines()` at 227. A stranger editing `shadows` in `trailerLines()` does not see the base rule's reasoning, and the wrapper's own comment (line 164) sends them back to `trailerLines()`.

Quote: `// ANTS-3808 § 2.3, amended by ANTS-4505 — each trailer key is emitted from its`

Remedy: move the block above `trailerLines()`.

### Row 693 — Low — `src/roadmapfoldin.cpp:106-107`

review-readability, lane 15-roadmap-parse-render-b, Q4. **Disposition:** queued, ANTS-6573.

The budget is now the `attempts` parameter, which is 20 for `LockWait::Interactive` (about 1 s). The "(5 s budget)" in this comment, and in `acquireRenameLock`'s comment at 211-212, describes only the default. A stranger tuning the interactive wait would read 5 s here.

Quote: `// Acquire ::flock(LOCK_EX|LOCK_NB) on path, polling 50 ms × 100`

Remedy: say "polling 50 ms × `attempts`".

### Row 694 — Low — `src/roadmapfoldin.h:140-141`

review-readability, lane 15-roadmap-parse-render-b, Q4. **Disposition:** queued, ANTS-6573.

`cmdRoadmapLogAppend` is defined in src/remotecontrol_roadmap_log.cpp:151, not in remotecontrol.cpp. A stranger following the pointer opens the wrong file.

Quote: `// (remotecontrol.cpp cmdRoadmapLogAppend: .arg(n, 4, 10, QLatin1Char('0'))),`

Remedy: name remotecontrol_roadmap_log.cpp.

### Row 701 — Medium — `src/roadmapdialog.h:422`

review-readability, lane 16-roadmap-dialog, Q4. **Disposition:** queued, ANTS-6572.

This comment, and the one at `roadmapdialog.cpp:3297-3299` ("filter entries through the case-sensitive <MAJOR>.<MINOR>.md regex"), leave out the `P<N>[.<sub>].md` phase archives that ANTS-4073 added (3238-3255). Those are loaded too, after every version archive. A stranger trusting the header would think phase archives are skipped.

Quote: ` // appends each *.md archive matching ^[0-9]+\.[0-9]+\.md$ `

Remedy: name both filename classes and their sort order in both comments. Cost: comment only.

### Row 702 — Low — `src/roadmapdialog.cpp:1146`

review-readability, lane 16-roadmap-dialog, Q4. **Disposition:** queued, ANTS-6572.

Since ANTS-5604 the date has its own column, and `.rm-id` takes the density tier's `codePx` (10, 12 or 14 px), not a fixed 12 px (1148).

Quote: `// ANTS-1241 — id + shipped-date are now inline on the`

Remedy: cut the comment to what is still true. Cost: comment only. The anchor on the `.rm-id` rule below it is not moved.

### Row 703 — Low — `src/roadmapdialog.h:14`

review-readability, lane 16-roadmap-dialog, Q4. **Disposition:** queued, ANTS-6572.

The header still describes a row of checkboxes. ANTS-4412 folded them into a Status button and a Kind button, and added a Contents toggle and a Reset button.

Quote: `// peer category checkboxes (✅ Done / 📋 Planned / 🚧 In progress /`

Remedy: describe the current filter row. Cost: comment only.

### Row 704 — Low — `src/roadmapdialog.h:656`

review-readability, lane 16-roadmap-dialog, Q4. **Disposition:** queued, ANTS-6572.

There is no `refresh()` function; the render runs in `rebuild()`.

Quote: `// checkboxes; passed through to renderCardsHtml on every refresh().`

Remedy: say `rebuild()`. Cost: comment only.

### Row 705 — Low — `src/roadmapdialog.cpp:1493`

review-readability, lane 16-roadmap-dialog, Q4. **Disposition:** queued, ANTS-6572.

The banner's border is `currentColor` (the ToolUse "current work" colour), not the theme's `accent`. A stranger changing the theme accent would expect the banner to follow it.

Quote: `// placeholder chain. Reuses the accent border + secondary`

Remedy: name the colour the code actually uses. Cost: comment only.

### Row 706 — Low — `src/roadmapdialog.cpp:3422`

review-readability, lane 16-roadmap-dialog, Q4. **Disposition:** queued, ANTS-6572.

The body is emitted as one escaped `<p>` per line (`applyInline`, 1453-1454). So no table can appear inside a body, which agrees with 1101-1102 ("the cards path emits no other table"). The two comments contradict each other. I am fairly sure 3422 is the stale one, but not certain.

Quote: `// A markdown table inside an expanded body has its own column count and`

Remedy: drop or correct one of them. Cost: comment only.

### Row 707 — Low — `src/roadmapdialog.cpp:173`

review-readability, lane 16-roadmap-dialog, Q4. **Disposition:** queued, ANTS-6572.

The table does not follow this exactly. Compact `h4Px` is 10 against Cozy's 11 (−1), and Comfortable `metaPx` is 13 (+1). The header's tier notes (h:168-170) say the same thing. A stranger tuning a tier would "fix" the table to match the comment.

Quote: `// Comfortable are -2 / +2 shifts on`

Remedy: describe the shifts as they are. Cost: comment only.

### Row 713 — Medium — `src/claudeintegration.cpp:505`

review-readability, lane 17-claude-integration-a, Q4. **Disposition:** queued, ANTS-6551.

The comment, and the name `RcHandler`, say the off-thread flag records where a handler came from: built by `rcDelegate`, so it only forwards to a `cmd*()`. In practice four hand-written handlers are wrapped in `RcHandler{...}` directly: `test_audit_partition`, `test_audit_brief`, `project_query` and `caller_cwd_info` (mcptoolregistry.cpp:652, 760, 881, 1161). `offThreadEligible` defaults to true …

Quote: `// factory forwards to a RemoteControl cmd*() and nothing else, so it touches`

Remedy: say at the struct and here that the flag is the author's own declaration that the code is thread-safe, and that it defaults to true.

### Row 719 — Low — `src/claudeintegration.cpp:1117`

review-readability, lane 17-claude-integration-a, Q4. **Disposition:** queued, ANTS-6551.

The comment lists the `raw`-eligible tools as read_region, read_regions and workspace_search. `isRawEligible` (mcpprojection.cpp:193) also includes `file_outline` (ANTS-4365). `makeRawProp`'s comment (1629–1630) has the same stale list.

Quote: `// workspace_search); see the wrap branch below.`

Remedy: name the predicate, not its members.

### Row 720 — Low — `src/claudeintegration.cpp:71`

review-readability, lane 17-claude-integration-a, Q4. **Disposition:** queued, ANTS-6551.

One sentence says `start()` is idempotent and also that it resets. It also calls `ClaudeIntegration` a singleton, but File → New Window makes a second one (comment at 891–893). The clock is shared static state, and the `isValid()` guard is what protects the second window.

Quote: `// QElapsedTimer::start() is idempotent; calling it on already-`

Remedy: say the guard stops a second window resetting the shared clock.

### Row 721 — Low — `src/claudeintegration.h:554`

review-readability, lane 17-claude-integration-a, Q4. **Disposition:** queued, ANTS-6551.

This comment describes `wrapMcpData` but sits above `dispatchResultIsSuccess`, `handlerRefusalCode` and `toolCallResult`. Its subject is 20 lines lower, at 575.

Quote: `// ANTS-1294 — frame user-supplied MCP content as data. Wraps a`

Remedy: move it down to `wrapMcpData`.

### Row 722 — Low — `src/claudeintegration.h:482`

review-readability, lane 17-claude-integration-a, Q4. **Disposition:** queued, ANTS-6551.

There are three capped tiers now; BriefAssembly is 30.

Quote: `// restore the 60/10 defaults.`

Remedy: point to the `kRateLimit*Cap` constants instead of quoting values.

### Row 723 — Low — `src/mcptoolregistry.cpp:1073`

review-readability, lane 17-claude-integration-a, Q4. **Disposition:** queued, ANTS-6551.

The cited lines in indiereviewdispatcher.cpp are now `inFlightCountForTest()`, not the network manager and event loop.

Quote: `// dispatcher.cpp:223-224); on the main thread that nested loop`

Remedy: cite `dispatchLanes` by name instead of by line.

### Row 724 — Medium — `src/claudeintegration_codenav.cpp:237`

review-readability, lane 18-claude-integration-b, Q4. **Disposition:** queued, ANTS-6551.

The same comment appears at codenav 888, codenav 1434 and orientation 1257. A sibling comment at orientation 1833 reads "The tools/list pass below appends a sentence to this description." Nothing "below" exists in these files. `kEagerDescription` is at `claudeintegration.cpp:2531`, and the appended sentence is added at `claudeintegration.cpp:2252`; the ANTS-1677 split left these comments pointing…

Quote: `// On the wire, kEagerDescription below replaces this text.`

Remedy: name the file and symbol, e.g. "replaced by kEagerDescription in claudeintegration.cpp".

### Row 728 — Medium — `src/claudeintegration_reports.cpp:577`

review-readability, lane 18-claude-integration-b, Q4. **Disposition:** queued, ANTS-6551.

This return shape (`order_before`, `order_after`, `prose_moved`, `moves`) belongs to op normalize. When the amend paragraph (ANTS-5246, 566–576) was inserted, it ended up directly after amend's own "Returns {…}". The `detail` text now shows two Returns clauses back to back. A reader, or someone editing amend, will take the second one as amend's.

Quote: `"Returns {ok, op, file, changed, "`

Remedy: move the normalize Returns sentence to the end of the normalize paragraph (after line 565).

### Row 733 — Low — `src/claudeintegration_orientation.cpp:1076`

review-readability, lane 18-claude-integration-b, Q4. **Disposition:** queued, ANTS-6551.

This is the exact byte count of another file's text, and it goes stale on any edit to that prelude. A stranger may trust that only 10 bytes are free.

Quote: `// ANTS-4472 — the SessionStart prelude is at 1390 of its`

Remedy: drop the figure and keep "near its 1400-byte cap".

### Row 734 — Low — `src/claudeintegration_orientation.cpp:288`

review-readability, lane 18-claude-integration-b, Q4. **Disposition:** queued, ANTS-6551.

Fifteen lines of incident history, the same at session_orient 88–99. The rule a stranger needs comes last: declare every argument the handler reads, or ANTS-2175's `ignored_args` flags it. similar_code's comment (1520–1527) states the same rule in a few lines.

Quote: `// ANTS-4468 — \mode\ was HONOURED by the handler and never`

Remedy: lead with that rule in one line and keep the ticket id as the label.

### Row 741 — Medium — `src/claudeintegration_docs.cpp:639-644`

review-readability, lane 19-claude-integration-c, Q4. **Disposition:** queued, ANTS-6551.

`spec_conformance` offers `etag_match` and promises an "unchanged" short-circuit, yet it is not in `isEtagSupportedTool`. A few hundred lines later, docs.cpp:866-868 states the opposite rule for `doc_lint`: "absent from isEtagSupportedTool (INV-13) and offering the argument would advertise a short-circuit it never performs". A stranger who trusts that comment would take `spec_conformance` for the…

Quote: `QJsonObject scEtag; scEtag["type"] = "string";`

Remedy: a one-line comment at `scEtag`, and at `isEtagSupportedTool`, saying `spec_conformance` does its own "unchanged" check in `remotecontrol_docs.cpp`.

### Row 743 — Low — `src/claudeintegration_sessions.cpp:88-89`

review-readability, lane 19-claude-integration-c, Q4. **Disposition:** queued, ANTS-6551.

The poll timer runs every 2000 ms (`m_pollTimer.setInterval(2000)` in claudeintegration.cpp, line 81). Line 138 of this same function says "up to 2 s", so the reader is corrected nearby.

Quote: `// from the previous tab persisted until the next poll tick (~1s`

Remedy: change "~1s" to "~2 s".

### Row 744 — Low — `src/claudeintegration_sessions.cpp:209`

review-readability, lane 19-claude-integration-c, Q4. **Disposition:** queued, ANTS-6551.

Since ANTS-5089 the code does not always fall through: line 227 returns before the scan when `scanOrphans` is false. The comment at 226 corrects it nearby.

Quote: `// skip the scan (ANTS-1867). So always fall through to the ppid`

Remedy: say "falls through when scanOrphans is set".

### Row 745 — Low — `src/claudeintegration_sessions.cpp:828`

review-readability, lane 19-claude-integration-c, Q4. **Disposition:** queued, ANTS-6551.

`claudebgtasks.cpp` no longer contains any sidechain filter. ANTS-1261 moved it into the shared walker in `src/claudetranscriptwalker.h` (lines 138 and 217). A stranger opening `claudebgtasks.cpp` to compare finds nothing.

Quote: `// in flight. Mirror the filter already present in claudebgtasks.cpp.`

Remedy: point the comment at `claudetranscriptwalker.h`.

### Row 746 — Low — `src/claudeintegration_sessions.cpp:389-391`

review-readability, lane 19-claude-integration-c, Q4. **Disposition:** queued, ANTS-6551.

"Cheap (~32KB read)" was true of the first window. Since ANTS-5731 and ANTS-5779, the context and state searches (lines 896-904 and 945-951) can each widen to the 16 MiB cap. Someone tuning the backstop would trust this cost figure.

Quote: `// every 10 poll cycles (~20s) we re-parse unconditionally — parse is`

Remedy: say the read starts at 32 KiB and can widen to the record cap.

### Row 747 — Low — `src/claudeintegration_callpolicy.cpp:549-550`

review-readability, lane 19-claude-integration-c, Q4. **Disposition:** queued, ANTS-6551.

The list has grown by about twenty additions, each with its own ticket number, so the order no longer follows any roadmap list. A stranger would go looking for a list that does not exist.

Quote: `// for the etag short-circuit to be worth it. Order mirrors the`

Remedy: drop the order claim.

### Row 749 — High — `src/claudestatuswidgets.h:297`

review-readability, lane 20-claude-session-widgets-a, Q4. **Disposition:** queued, ANTS-6552.

A stranger reading the header would think Undo submits `/model` with `\n`. ANTS-1912 established that `\n` does not submit in Claude Code, and the code at `claudestatuswidgets.cpp:2775-2778` sends `\r`. Someone "aligning" code to the header would bring that bug back, and the correction sits about 2,500 lines away in another file.

Quote: `// /model <fromTier>\n and seeds m_lastOverrideMsByProject so`

Remedy: change `\n` to `\r` in the header comment.

### Row 753 — Medium — `src/claudestatuswidgets.cpp:1887-1888`

review-readability, lane 20-claude-session-widgets-a, Q4. **Disposition:** queued, ANTS-6552.

It says the model pill's slot is sized without the "(auto → X)" suffix (pinSlots). Since ANTS-5762, pinSlots deliberately does not pin `m_modelStateBtn`: it fits its text (1082-1086). The test `Inv6OccasionalLeftSteadyInOrder` asserts the same. A stranger would keep this clearing code for a layout reason that no longer exists, or wrongly add the suffix to pinSlots.

Quote: `// A queued switch dies with the setting: the model pill's slot is sized`

Remedy: restate the real reason for clearing the queued tier when auto-switch is off, or drop the pinSlots clause.

### Row 754 — Medium — `src/claudestatuswidgets.cpp:2460`

review-readability, lane 20-claude-session-widgets-a, Q4. **Disposition:** queued, ANTS-6552.

The chip-click-while-idle path (the lambda at 341-405) never seeds `m_lastOverrideMsByProject`. Only the deferred path (2461) and undo (2784) write it, plus the ledger fill and seed paths. A stranger would believe all three manual-switch paths start the cool-down.

Quote: `// (mirrors the undo + chip-click-while-idle paths).`

Remedy: drop "chip-click-while-idle", or say why that path does not seed.

### Row 762 — Low — `src/claudestatuswidgets.cpp:2508`

review-readability, lane 20-claude-session-widgets-a, Q4. **Disposition:** queued, ANTS-6552.

The labels are translated inside shortenReason (`QObject::tr`, 2511-2522). The call site at 2654 passes the result to `.arg()` and does not translate it. A stranger following the comment would add a second translation at the call site.

Quote: `// are wrapped in tr() at the call site (Qt i18n discipline).`

Remedy: say "wrapped in tr() here".

### Row 763 — Low — `src/claudestatuswidgets.h:114-115`

review-readability, lane 20-claude-session-widgets-a, Q4. **Disposition:** queued, ANTS-6552.

The comment names `m_claudeIntegration` and `focusedTerminal()`. The class has `m_integration` and `m_focusedTerminalProvider`. A stranger searching for those names finds nothing.

Quote: `// showBgTasksDialog dismissal). Body reads focusedTerminal()`

Remedy: use the real member names.

### Row 764 — Low — `src/claudestatuswidgets.cpp:108-114`

review-readability, lane 20-claude-session-widgets-a, Q4. **Disposition:** queued, ANTS-6552.

It says "GUI-thread only — all three refreshers run on the status-bar timer". cachedConfig() is now also called from pinSlots, refreshTokensSavedChip, the auto-confirm burst, the handshake and emitSwitchSurfacing. A stranger judging whether a new caller is safe would read the "three refreshers" list as complete.

Quote: `// 2 s status-bar tick calls refreshModelChip / refreshModelStateChip /`

Remedy: state the rule ("call only on the GUI thread") without the stale list of callers.

### Row 765 — Low — `src/claudestatuswidgets.cpp:1318-1329`

review-readability, lane 20-claude-session-widgets-a, Q4. **Disposition:** queued, ANTS-6552.

The current rule (show the chip while 0 ≤ done < total) is buried under three tickets of history across two comment blocks (1318-1340). A stranger has to read all of it to learn the one-line rule.

Quote: `// ANTS-1219-INV-3 / ANTS-1216 / ANTS-1246: hide branch covers`

Remedy: state the rule in one line and leave the history to the commits.

### Row 767 — High — `src/claudetabtracker.cpp:126`

review-readability, lane 21-claude-session-widgets-b, Q4. **Disposition:** queued, ANTS-6552.

The comment at 119–123 says the sentinel stops `detectClaudeChild` from wiping the path on the next poll. In `detectClaudeChild` (177–184) the opposite happens. When `found == 0` and `claudePid != 0`, which the sentinel makes true, the code releases the watch and clears `transcriptPath`. With `claudePid == 0` the path would have been kept. A stranger trusting the comment would expect a seeded tes…

Quote: `if (entry.claudePid == 0) entry.claudePid = -1; // sentinel`

Remedy: correct the comment to say what the sentinel actually does, or drop the sentinel if the intent was to keep the path.

### Row 768 — Medium — `src/claudetabtracker.h:120-121`

review-readability, lane 21-claude-session-widgets-b, Q4. **Disposition:** queued, ANTS-6552.

The signal comment lists state, plan mode, tool and awaitingInput. `maybeEmit` (`claudetabtracker.cpp:334-341`) also compares `auditing` and the three background counts. It deliberately skips `contextTokens`, `contextEstimated`, `idleSinceMs`, `awaitingRule` and `claudeStartMs`, and nothing says the skip is deliberate. Only `awaitingRule`'s exclusion is mentioned, at `.cpp:75`. A stranger adding …

Quote: `// Emitted whenever a tracked shell's state changes (transcript-`

Remedy: give `maybeEmit` a comment naming which fields trigger the signal and why the others do not, and point the header at it.

### Row 769 — Medium — `src/claudepromptdetect.h:11-13`

review-readability, lane 21-claude-session-widgets-b, Q4. **Disposition:** queued, ANTS-6552.

The threat-model comment says a single hostile line can no longer fire the detector. But "Tab to accept" is both an anchor (`.cpp:46`) and a strong signal (`.cpp:30-32`). So one line containing it gives `hasAnchor && hasStrong`, and the detector fires. The test INV-1 confirms this is intended ("footer corroborates alone"), which means the paragraph overstates the security guarantee. A stranger wh…

Quote: `// of those, so requiring co-occurrence raises the bar from "any one`

Remedy: say in the header that "Tab to accept" alone is enough, and why that is acceptable.

### Row 781 — Low — `src/claudeallowlist.cpp:257-263`

review-readability, lane 21-claude-session-widgets-b, Q4. **Disposition:** queued, ANTS-6552.

This paragraph describes the refuse-on-parse-failure block at 283–317, but it sits above the mkdir and lock code, followed by another comment. It also says "Rotate the corrupt file aside", which reads as a move. `rotateCorruptFileAside` *copies* the file (`configbackup.h`, `QFile::copy`), so the original stays and every refused Save adds another `.corrupt-*` copy.

Quote: `// Read existing file to preserve non-permission keys (model, editor`

Remedy: move the comment onto the parse block and say "copy".

### Row 782 — Low — `src/claudebgtasks.h:154-158`

review-readability, lane 21-claude-session-widgets-b, Q4. **Disposition:** queued, ANTS-6552.

"Reparses only when mtime advanced" is out of date. `poll()` reparses when mtime *or* size moved, *or* when `walkHelperTranscripts` reports that a helper moved. That last call is a side-effecting function inside the `if` condition at `.cpp:226-228`, and no comment there says it folds events into `m_acc`. A stranger reordering that condition changes behaviour.

Quote: `// the 2 s status-timer cycles a cheap mtime check; reparses only`

Remedy: correct the header and add a comment at the condition.

### Row 783 — Low — `src/claudebgtasks.cpp:120-121`

review-readability, lane 21-claude-session-widgets-b, Q4. **Disposition:** queued, ANTS-6552.

"Same rule as parseTranscript's liveness pass": that pass now lives in `finalize`. The header at `.h:139-141` likewise says `rescan` is "driven by the QFileSystemWatcher" and is "one transcript walk capped at 16 MiB". It is now driven by the debounced `ClaudeTranscriptWatch` and resumes from a cursor.

Quote: `// Walks m_tasks and flips finished when the output file is gone. Same`

Remedy: point at `finalize` and update the `rescan` comment.

### Row 784 — Low — `src/claudebgtasks.h:104`

review-readability, lane 21-claude-session-widgets-b, Q4. **Disposition:** queued, ANTS-6552.

`ClaudeTaskListTracker::parseTranscript`, over the same walker, is documented as "Stateless, GUI-thread-only" (`claudetasklist.h:85-86`). One of the two threading claims is wrong, and a stranger moving a parse off the GUI thread cannot tell which.

Quote: `// transcript. Stateless and safe to call concurrently.`

Remedy: make the two agree after checking.

### Row 785 — Low — `src/claudebgtasksdialog.cpp:26-30`

review-readability, lane 21-claude-session-widgets-b, Q4. **Disposition:** queued, ANTS-6552.

This is `tailFile`'s comment, but it sits above `stripAnsi`. It also cites "CLAUDE.md rule 3", which is a retired rule number.

Quote: `// Read up to maxBytes from the tail of path and return as UTF-8. We`

Remedy: move the paragraph to `tailFile` and drop the rule reference.

### Row 786 — Low — `src/claudebgtasksdialog.cpp:60-61`

review-readability, lane 21-claude-session-widgets-b, Q4. **Disposition:** queued, ANTS-6552.

"lstat avoids the symlink trap": `lstat` runs on the *canonical* path, and `canonicalFilePath()` has already resolved every symlink. So `lstat` adds nothing over `stat` here, and a stranger may think it is the symlink defence.

Quote: `// by uid) and have its contents rendered into the dialog. lstat`

Remedy: say the canonicalisation is the defence, and that `lstat` only checks for a regular file.

### Row 787 — Low — `src/claudebgtasksdialog.cpp:285-287`

review-readability, lane 21-claude-session-widgets-b, Q4. **Disposition:** queued, ANTS-6552.

It says "restore after with std::min clamp", and so does `claudebgtasksdialog.h:24`, but the code uses `qMin`. A stranger grepping for `std::min` finds nothing.

Quote: `// 0.7.37 scroll-preservation pattern: skip identical renders, then`

Remedy: say `qMin`.

### Row 788 — Low — `src/claudetranscriptwalker.h:77-79`

review-readability, lane 21-claude-session-widgets-b, Q4. **Disposition:** queued, ANTS-6552.

The gating is documented *below*, at `walk()` (211–228). The file header (16–17) also still names the two `parseTranscript` functions as the walker's users, though they now go through `parseIncremental` / `walkFrom`.

Quote: `// event and advancing cur over what it consumed. The gating above applies`

Remedy: say "below, at walk()".

### Row 790 — Low — `src/claudetasklist.cpp:108-119`

review-readability, lane 21-claude-session-widgets-b, Q4. **Disposition:** queued, ANTS-6552.

Twelve lines of report history (a dated screenshot, the earlier ANTS-1216 widening, ANTS-1218) bury a one-line rule: pending only, because running tasks are not something the user can act on. The same pattern appears at 318–330 and `claudetabtracker.cpp:161-164, 214-227`.

Quote: `// ANTS-1221 — pending only. The chip surfaces user-actionable`

Remedy: keep the rule line and move the history to the commit or changelog.

### Row 797 — Medium — `src/remotecontrol.cpp:198`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

The ANTS-4887 block (183-201) sits directly above `rcGitDirOf`. That function returns `dotGit` for a directory and an empty string on failure, never `dir`. The block actually describes `rcMainCheckoutOf` (216). The ANTS-4884 block (174-182) describes `rcProjectRootFor` (251). A stranger would read the wrong contract for `rcGitDirOf`.

Quote: `// Returns dir unchanged unless every step holds: a .git FILE, a parsable`

Remedy: move each block to sit on its own function.

### Row 799 — Medium — `src/remotecontrol_internal.h:188`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

This block now sits above `rlNotStoreServedRefusal` (208). Its subject, `rcRoadmapWriteFields`, is at 213, after an inserted ANTS-5161 block. A stranger reads the dry-run policy as describing the store refusal.

Quote: `// ANTS-4463 — the dry-run field policy, in ONE place because nine sites emit`

Remedy: move the block down to sit above `rcRoadmapWriteFields`.

### Row 800 — Medium — `src/remotecontrol_internal.h:553`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

The ANTS-4849 block for `rlStableIdShape` interrupts this one, and only three of the declarations that follow take `fmt`. A stranger can't tell which four are meant, or that the warning doesn't cover `rlStableIdShape`.

Quote: `// ANTS-3771 — each of these four reads BulletRecord::id out of a markdown`

Remedy: say "three", and move the ANTS-4849 block next to `rlStableIdShape`.

### Row 801 — Medium — `src/remotecontrol_internal.h:7`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

The header defines six structs, not three: GfmBullet, AntsV1Bullet, VerifyGitSnapshot, HistoryContext (with inline `record`/`pendingBytes` bodies), RlFlipLinks and LocateOutcome. It also defines `inline constexpr kPlanApplyKey`. This is the rule the header says it exists to hold, so a stranger deciding where a new definition may go is misled.

Quote: `// 1. A definition stays in a .cpp. Only the three structs and nothing else`

Remedy: state the rule without a count, and name the exceptions.

### Row 802 — Medium — `src/remotecontrol.cpp:1925`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

`RoadmapDialog::parseBullets` is now a one-line forwarder (roadmapdialog.cpp:647-648). The GFM branch and `splitOnEmDash` are in roadmapparse.cpp, and `rxTrailAnchor` exists in neither file. The same stale pointer is at `remotecontrol_internal.h:69` and `remotecontrol.cpp:1920`. Someone keeping the write form in step with the read form is sent to the wrong file.

Quote: `// rxTrailAnchor strip in roadmapdialog.cpp's GFM branch.`

Remedy: point at roadmapparse.cpp's `splitOnEmDash` and `stripTrailingCaretAnchor`.

### Row 804 — Low — `src/mcpdmain.cpp:275`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

The code (282) displaces the smallest only when the new root has more bytes; otherwise the new root is dropped.

Quote: `// a new root displaces the one with the fewest bytes.`

Remedy: add "when it holds more".

### Row 805 — Low — `src/remotecontrol.cpp:2137`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

`applyGfmFlip` returns `void`.

Quote: `// place. Returns the byte count written so callers can surface`

Remedy: delete that sentence.

### Row 806 — Low — `src/remotecontrol.cpp:666`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

The caller, `rlResolveCounterPrefix` (743), falls back to the declared prefix and then to `rlLeafDirPrefix`, never to "ANTS".

Quote: `// falls back to "ANTS" (back-compat for a fresh/id-less roadmap). The`

Remedy: name the real fallback.

### Row 807 — Low — `src/remotecontrol.cpp:609`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

This is the pre-ANTS-4849 pattern; `rlStableIdShape` now allows a leading digit. The note at 615-618 partly corrects it.

Quote: `// ^[A-Za-z][A-Za-z0-9_-]+$ that is NOT the canonical ^ANTS-[0-9]+$`

Remedy: refer to `rlStableIdShape` instead of quoting a pattern.

### Row 808 — Low — `src/remotecontrol.cpp:501`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

The single-ID cap (413, 1 MiB) is larger than the store cap (405, 16384), and since ANTS-4630 the cache keeps bodies whole.

Quote: `// cached body is safe: the emission cap is <= the store cap, so`

Remedy: restate why re-eliding is safe now.

### Row 809 — Low — `src/remotecontrol.cpp:1307`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

This says the walkers toggle on any indent. ANTS-4404 (1963-1969) replaced that with MarkdownScan.

Quote: `// fence at ^ {0,3}(|~~~), and this file's own walkGfmBullets /`

Remedy: drop the walker claim.

### Row 810 — Low — `src/remotecontrol.cpp:7`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

The file now includes 16 project headers. Lines 60-62 also describe "the file-scope static here", which no longer exists.

Quote: `// nothing this TU uses, leaving the 13 that do. Add an`

Remedy: keep only the "add an include only when THIS TU names the symbol" rule.

### Row 811 — Low — `src/remotecontrol.h:1350`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

Op dispatch is in `cmdRoadmapLogDispatch` (remotecontrol_roadmap_query_verb.cpp:4190, 4344). The same stale wording is at 1366, 1383 and 1387-1388.

Quote: `// Dispatched from cmdRoadmapLog when req["op"] == "flip". See`

Remedy: name `cmdRoadmapLogDispatch`.

### Row 812 — Low — `src/remotecontrolgate.h:47`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

`QJsonObject` serialises keys in sorted order, so the assignment order in the .cpp has no effect. A stranger may preserve or "fix" an order that does nothing.

Quote: `// verdict. Key order matches ANTS-1295: {ok, error, code}.`

Remedy: drop the key-order claim.

### Row 813 — Low — `src/remotecontrol_internal.h:144`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

Lines 135-147 are history of how the decomposition was cut, and they bury the one current rule: a declaration is here because another TU uses it.

Quote: `// Nineteen of the file's 36 file-scope statics are referenced from a slice`

Remedy: keep that one sentence and move the history to the spec.

### Row 814 — Low — `src/remotecontrol.cpp:2290`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6567.

Since ANTS-2059 the bracket ID is optional (2235-2244).

Quote: `// bullets already have the canonical [PREFIX-NNNN] id), no counter`

Remedy: say no anchor is injected, without claiming an ID exists.

### Row 817 — Medium — `src/remotecontrol_roadmap_log.cpp:855`

review-readability, lane 23-mcp-roadmap-query-log-a, Q4. **Disposition:** queued, ANTS-6565.

This is false on two counts: - the seed is `corpusHighWater(...)` (815), which can be any value, not 0; - on a dry run, `counterResolved` is true and the shared read block is skipped, so there is no fall-through. A stranger debugging a wrong first id would trust "counter=0 → newId=1".

Quote: `// cf now reads "0"; fall through to the shared`

Remedy: say the file now holds `seed` and that a dry run skips the read.

### Row 818 — Medium — `src/remotecontrol_roadmap_log.cpp:3720`

review-readability, lane 23-mcp-roadmap-query-log-a, Q4. **Disposition:** queued, ANTS-6565.

Failure mode (b) no longer exists. Since ANTS-4550, a phrase that spans a hard-wrapped line break matches, and the second hint a few lines down says exactly that (3743–3746). A stranger reads (b) as a case the code still handles.

Quote: `// (bodies wrap at ~70 cols) so no single physical line contains it.`

Remedy: drop (b) from the 3716–3720 header, or restate it as the folded-whitespace case.

### Row 819 — Medium — `src/remotecontrol_roadmap_log.cpp:2282`

review-readability, lane 23-mcp-roadmap-query-log-a, Q4. **Disposition:** queued, ANTS-6565.

No `format_mismatch` gate exists in flip. The only `format_mismatch` hit in this file is this comment. Pass-headings is routed at 1710–1732 (ANTS-2126/5334), before the file read, not after it. A stranger will look for a gate that is not there.

Quote: `// (Pass-headings is caught earlier; see the format_mismatch`

Remedy: point at the ANTS-2126 early route.

### Row 820 — Medium — `src/remotecontrol_roadmap_log.cpp:2791`

review-readability, lane 23-mcp-roadmap-query-log-a, Q4. **Disposition:** queued, ANTS-6565.

This is amend_body's doc block, but it sits above `namespace {` / `rlSetBodyStore`, about 90 lines before `cmdRoadmapLogAmendBody`. It also says full-body replace is out of scope, yet set_body (a full-body replace) is a mode of the same handler. The ANTS-4808 paragraph just after it partly corrects this.

Quote: `// untouched. Exact-match patch only this pass; full-body-replace is out of`

Remedy: move the ANTS-3406 block onto `cmdRoadmapLogAmendBody` and drop the "out of scope" sentence.

### Row 821 — Low — `src/remotecontrol_roadmap_log.cpp:33`

review-readability, lane 23-mcp-roadmap-query-log-a, Q4. **Disposition:** queued, ANTS-6565.

This comment documents `rlStampToday` (line 78), but it sits on `rlAddScrubWarning`, with `rlUnrecognisedFormat` in between. A stranger reads a date rule on a scrub-warning function.

Quote: `// The local calendar date, formatted to satisfy the item table's GLOB CHECK.`

Remedy: move lines 33–34 to above `rlStampToday`.

### Row 828 — Medium — `src/remotecontrol_roadmap_log.cpp:2041`

review-readability, lane 23-mcp-roadmap-query-log-a, Q4. **Disposition:** queued, ANTS-6565.

This calls the lambda's block *the* store path. Since ANTS-4485, the store is reached first at 1952–2010 for every ants-v1 project without an anchor. A stranger changing store behaviour edits this block and misses the earlier one, or the reverse.

Quote: `// ANTS-3809 § 2.2 — the store path, at the TOP of this lambda and so`

Remedy: say which inputs still reach this block after the 1952 dispatch.

### Row 836 — Low — `src/remotecontrol_roadmap_log.cpp:1557`

review-readability, lane 23-mcp-roadmap-query-log-a, Q4. **Disposition:** queued, ANTS-6565.

The current rule (the full bullet is opt-in via `return:"full"`, and compact `post_bullets` is the default) comes last, after 40 lines of history. This line reads as though `headline_only` is the switch.

Quote: `// ANTS-5263 — SUPPRESSED under return:"headline_only". That`

Remedy: lead with the current rule in one line and move the history to the changelog.

### Row 851 — Medium — `src/remotecontrol_roadmap_query_verb.cpp:4168`

review-readability, lane 24-mcp-roadmap-query-log-b, Q4. **Disposition:** queued under ANTS-6129.

The guard at line 4339 checks `m_roots`, not `m_main`. The same stale name appears at lines 4271-4277 and in the test-seam comments (4379, 4386, 4405-4418). A stranger who searches for `m_main` finds the `MainWindow` pointer (`remotecontrol.h:1734`) and wrongly concludes that append needs a main window.

Quote: `// m_main guard only fires on the append path.`

Remedy: name the root provider (`m_roots`) in these comments. Cost: comment only.

### Row 852 — Low — `src/remotecontrol_roadmap_query_verb.cpp:292`

review-readability, lane 24-mcp-roadmap-query-log-b, Q4. **Disposition:** queued, ANTS-6565.

The rounding it points to is at `remotecontrol.cpp:1868`; line 1002 is now an unrelated helper.

Quote: `// Canonical dup-detector rounding (remotecontrol.cpp ~1002)`

Remedy: name the function instead of a line number. Searched: `\* 100\.0 \+ 0\.5` across the tree.

### Row 853 — Low — `src/remotecontrol_roadmap_query_verb.cpp:2472`

review-readability, lane 24-mcp-roadmap-query-log-b, Q4. **Disposition:** queued, ANTS-6565.

The index is filled 20 lines above, at 2448-2458. Line 1410 is unrelated.

Quote: `// (line ~1410); reuse it for the fallback.`

Remedy: drop the line reference.

### Row 854 — Low — `src/remotecontrol_roadmap_query.cpp:759`

review-readability, lane 24-mcp-roadmap-query-log-b, Q4. **Disposition:** queued under ANTS-6129.

This contradicts lines 754-756, which say `keyword` is empty under `annotate_batch`.

Quote: `// to_status is validated canonical at the gate, so keyword is non-empty.`

Remedy: delete it or narrow it to `flip_batch`.

### Row 855 — Low — `src/remotecontrol_roadmap_query.cpp:58`

review-readability, lane 24-mcp-roadmap-query-log-b, Q4. **Disposition:** queued, ANTS-6565.

These functions write ROADMAP.md (`rcAtomicWriteRoadmap`) and are exported in `rcdetail`, not file-static. A stranger trusting "pure" might call one to preview and write the file instead. The code nearby shows the write.

Quote: `// functions: each is pure given (req, roadmapPath, markdown) — invoked`

Remedy: say they write the file unless `dry_run` is set.

### Row 856 — Low — `src/remotecontrol_roadmap_query_verb.cpp:506`

review-readability, lane 24-mcp-roadmap-query-log-b, Q4. **Disposition:** queued under ANTS-6129.

This header for `cmdRoadmapQuery` sits above the report-mode helpers, about 360 lines before the function (line 870). The `roadmap_log` header at 3933-3941 likewise sits above the batch-hint and plan helpers, not above `cmdRoadmapLog`.

Quote: `// ANTS-1117 v1: roadmap-query — parse the active tab's ROADMAP.md`

Remedy: move each header to its function.

### Row 863 — Low — `src/remotecontrol_roadmap_log_batch.cpp:3368-3373`

review-readability, lane 25-mcp-roadmap-batch, Q4. **Disposition:** queued, ANTS-6564.

The comment says "Two callers (`repair_trailers`, `backfill_dates`) remap this function's refusals and interpolate `root.isEmpty() ? roadmapPath : root`". Four callers remap now (render and place_by_release as well), and the ternary lives in `rlNotStoreServedRefusal` (`publish.cpp:56`), not in the callers. A reader would look in the wrong place for the interpolation.

Quote: `// into the message; assigning at the end left both empty on every refusal,`

Remedy: point the comment at `rlNotStoreServedRefusal` and drop the caller count.

### Row 877 — Low — `src/remotecontrol_roadmap_log_batch.cpp:1158-1161`

review-readability, lane 25-mcp-roadmap-batch, Q4. **Disposition:** queued, ANTS-6564.

The line numbers point at nothing relevant: `remotecontrol_roadmap_log.cpp:3290-3346` is now amend_body's database route. A reader following the pointer lands on unrelated code.

Quote: `// cmdRoadmapLogAppend's :3293-3344 block so cmdRoadmapLogAppendBatch`

Remedy: drop the line numbers.

### Row 878 — Low — `src/remotecontrol_roadmap_log_batch.cpp:4422-4432`

review-readability, lane 25-mcp-roadmap-batch, Q4. **Disposition:** queued, ANTS-6564.

This block describes workspace_search's process handling and kill sequence, but `cmdWorkspaceSearch` is in `remotecontrol_workspace.cpp:263`. Only constants and pattern helpers sit here, inside a file whose header (line 1) says "Roadmap batch and section write ops". A newcomer looking for the kill logic will search this file for it.

Quote: `// Process model: QProcess::start("rg", argv) — argv-only, no shell`

Remedy: point the comment at `remotecontrol_workspace.cpp` and name these helpers in the file header.

### Row 879 — Low — `src/remotecontrol_roadmap_log_batch.cpp:245-260`

review-readability, lane 25-mcp-roadmap-batch, Q4. **Disposition:** queued, ANTS-6564.

The step title says GFM is checked first. The paragraph right after it says pass-headings is checked first, then tells the story of "the OLD code". The current order is buried under its history.

Quote: `// 5. format detect (GFM first, then ants-v1 — mirrors single-flip).`

Remedy: retitle the step "pass-headings, then GFM, then ants-v1" and move the history to the commit record.

### Row 880 — Low — `src/remotecontrol_roadmap_migrate.cpp:105-113`

review-readability, lane 25-mcp-roadmap-batch, Q4. **Disposition:** queued, ANTS-6564.

This comment is about the `deregister` branch at 127, but it sits on top of the `ExclusiveHold` struct and the ANTS-5086 hold comment. A reader takes it as describing the hold.

Quote: `// ANTS-4617 — the inverse. Branches BEFORE the migrate request is built,`

Remedy: move it down to the `deregister` branch.

### Row 892 — Medium — `src/remotecontrol_workspace.cpp:1247`

review-readability, lane 26-mcp-state-workspace-a, Q4. **Disposition:** queued, ANTS-6566.

Not true now: the ANTS-4753 hidden-glob branch at 1260 comes first, and only line 1258 says so. The current order is buried under three layers of history (2045, 4420, 4753). A newcomer reordering hints would trust "FIRST".

Quote: `// ANTS-4420 — ordered FIRST among the zero-match hints, and deliberately.`

Remedy: replace 1241–1259 with one comment listing the current order and why.

### Row 893 — Low — `src/remotecontrol_workspace.cpp:83`

review-readability, lane 26-mcp-state-workspace-a, Q4. **Disposition:** queued, ANTS-6566.

`rcRunRg` now has three callers: `cmdCoChangeFamily` at 4230 has a third classification, where a hard kill is `truncated` rather than a failure. The cited_by comment at 1622–1623 also names a `matches.isEmpty()` guard that ANTS-5711 replaced with `nothingFound` (885). A newcomer would assume two callers and look for a condition that is no longer there.

Quote: `// even in principle. And the two verbs classify differently on purpose:`

Remedy: name all three callers and the `nothingFound` guard.

### Row 894 — Low — `src/remotecontrol_workspace.cpp:1852`

review-readability, lane 26-mcp-state-workspace-a, Q4. **Disposition:** queued, ANTS-6566.

The guard is now `PathValidation::validatePath` (1745), not a startsWith check.

Quote: `// token orientation envelope. Path-escape guarded by canonical-path`

Remedy: point at PathValidation.

### Row 895 — Low — `src/remotecontrol_workspace.cpp:3493`

review-readability, lane 26-mcp-state-workspace-a, Q4. **Disposition:** queued, ANTS-6566.

`cmdSpecLog` is not in this file; the definition search found it in remotecontrol_feedback.cpp. The comment looks left over from the file split, and a reader looks here for it in vain.

Quote: `// cmdSpecLog reuses isValidSpecId and resolveSpecRelForId (ANTS-3356) from`

Remedy: move or delete it.

### Row 896 — Low — `src/remotecontrol_workspace.cpp:1331`

review-readability, lane 26-mcp-state-workspace-a, Q4. **Disposition:** queued, ANTS-6566.

It omits matches_only, which also returns earlier (996).

Quote: `// honest truncated. count_only / files_only returned earlier — they carry`

Remedy: add it.

### Row 909 — Medium — `src/remotecontrol_state.cpp:2023`

review-readability, lane 27-mcp-state-workspace-b, Q4. **Disposition:** queued under ANTS-6131.

This comment is false. The pattern now has three branches, and since ANTS-4810 branch 1 accepts a topic suffix, so it need not end in digits. `phase_22_foo-12` matches both branch 1 and the phase branch. Routing is safe only because 2412 checks `startsWith("phase_")`. The comment at 2377–2378 ("the two regexes are disjoint (isValidSpecId enforces this)") repeats the claim. A stranger adding a bra…

Quote: `// disjoint (arm 1 ends in -<digits>, the phase arm never does), so`

Remedy: say routing is decided by the `phase_` prefix, not by the branches being disjoint. Cost: comment-only.

### Row 910 — Low — `src/remotecontrol_state.cpp:814`

review-readability, lane 27-mcp-state-workspace-b, Q4. **Disposition:** queued under ANTS-6131.

The next line returns `parse_failed`. `docs/specs/ANTS-1254.md` INV-10 also says `not_audited`. A reader can't tell whether the comment or the code is wrong.

Quote: `// INV-10: empty results / no runs[] → not_audited.`

Remedy: reconcile against the spec, then make the comment state what the code does. Cost: comment-only for the comment. The code side is under Correctness leads.

### Row 911 — Low — `src/remotecontrol_state.cpp:1394`

review-readability, lane 27-mcp-state-workspace-b, Q4. **Disposition:** queued under ANTS-6131.

This header for `session_orient` sits above `buildFeedbackPendingBlock`, about 235 lines from the function it describes. It says the response's version tag (ETag) changes "when any of the three upstreams' payload changes", but the bundle now merges about ten blocks. Line 1407 points to "server_build / codebase_index above", which are now below, inside another function.

Quote: `// ANTS-1883 — session_orient: bundle of current_state + project_layout`

Remedy: move and refresh the header beside `cmdSessionOrient`. Keep it free of the function's literal name, per the warning at 1418–1420. Cost: comment-only.

### Row 912 — Low — `src/remotecontrol_state.cpp:109`

review-readability, lane 27-mcp-state-workspace-b, Q4. **Disposition:** queued, ANTS-6566.

The comment points at a line number in an unnamed file. The server-side check is in `remotecontrol_terminal.cpp` near line 210.

Quote: `// SO_PEERCRED (line ~1296), but the client never checked the server's:`

Remedy: name the file and function. Cost: comment-only. Searched: `SO_PEERCRED` in `src/`.

### Row 913 — Low — `src/remotecontrol_state.cpp:1865`

review-readability, lane 27-mcp-state-workspace-b, Q4. **Disposition:** queued, ANTS-6566.

Since ANTS-3353, `detect()` also runs when the layout differs from the defaults (`auxOffDefault`, 1904). The following paragraph explains that, so a careful reader recovers.

Quote: `// declare source_roots. Cheap gate: the detect() walk runs ONLY`

Remedy: drop "ONLY … below the low-water mark". Cost: comment-only.

### Row 921 — Medium — `src/remotecontrol_review.cpp:159`

review-readability, lane 28-mcp-review-verbs, Q4. **Disposition:** queued, ANTS-6563.

This 14-line comment explains the `sparse_partition_hint` at lines 252–279, but it sits above `if (derived)` at line 170, with the unassigned section in between. Someone editing the sparse hint will not find its reasoning, and someone reading line 170 will take the comment as the reason for `derived_from`.

Quote: `// ANTS-3567 — symmetry with cmdColdEyesPartition's sparse_partition_hint`

Remedy: move the ANTS-3567 paragraph down to line 252.

### Row 922 — Low — `src/remotecontrol_review.cpp:137`

review-readability, lane 28-mcp-review-verbs, Q4. **Disposition:** queued under ANTS-5944.

This is false when the partition is derived. In that case the lanes come from a directory walk, but `path` still names the module-map file (lines 151–157). A stranger would trust `path` as the lanes' origin.

Quote: `// ANTS-4846 — path names the source the lanes came from. An override the`

Remedy: say `path` names the declared source, and that this is where to declare one when `derived` is true.

### Row 923 — Low — `src/remotecontrol_review.cpp:666`

review-readability, lane 28-mcp-review-verbs, Q4. **Disposition:** queued, ANTS-6563.

This is false. `citations_seen` and `citations_resolved` (521–522) are now always sent, so the happy-path reply is no longer v1's. A stranger relying on "byte-identical" would wrongly assume the reply never changed.

Quote: `// (envelope stays byte-identical to v1 on the happy path).`

Remedy: drop the byte-identical claim.

### Row 924 — Low — `src/remotecontrol_review.cpp:775`

review-readability, lane 28-mcp-review-verbs, Q4. **Disposition:** queued, ANTS-6563.

The pointer has gone stale. `Config::aiModel()` and its `"llama3"` default are now at config.cpp:1124–1125, and lines 716–717 are something else.

Quote: `// config.cpp:716-717 — § 3.3 footgun.`

Remedy: name `Config::aiModel()` instead of a line range.

### Row 926 — Low — `src/remotecontrol_coldeyes.cpp:1`

review-readability, lane 28-mcp-review-verbs, Q4. **Disposition:** queued, ANTS-6563.

The header lists only part of what the file holds: - This file also holds `session_memory`, `workflow_state`, `project_layout`, `build_status` and `enrichLikelyFixes`. - `remotecontrol_review.cpp:1` ("Indie review verbs") also holds `verify_changes`. Its banner at 1397–1399 ("cold_eyes_* MCP tools") sits over helpers only; the cold_eyes verbs themselves live in this file. A stranger looking for `…

Quote: `// ANTS-3833 TU 13/25 — Cold eyes and test verbs.`

Remedy: make each header list the verb families the file holds, and relabel the review.cpp banner as shared helpers.

### Row 928 — High — `src/remotecontrol_docs.cpp:973-974`

review-readability, lane 29-mcp-content-verbs-a, Q4. **Disposition:** queued, ANTS-6561.

The comment says no standard in this project has the required-sections block. `docs/standards/specs.md` does have it, as does `docs/standards/spec-format.md`. A stranger would believe `missing_section` never fires here and would misread the check's output. Fix: delete the "shipping default … yet" clause.

Quote: `// means the check is skipped — which is this verb's shipping default, since`

### Row 933 — Medium — `src/remotecontrol_terminal.cpp:188-190`

review-readability, lane 29-mcp-content-verbs-a, Q4. **Disposition:** queued, ANTS-6561.

The comment points at a trust-model comment "at the top of this file". This file (TU 2 of the ANTS-3833 split) has none; the phrase survives only in CHANGELOG and metainfo. A stranger hunting for the trust model would not find it. Fix: quote the claim inline, or point at the ADR (design decision record) that holds it.

Quote: `// at the top of this file claims "UID-scoped + 0700 perms +`

### Row 942 — Low — `src/cihistory.h:41`

review-readability, lane 29-mcp-content-verbs-a, Q4. **Disposition:** queued, ANTS-6561.

Rows also carry `reason` (`not_started`, `timeout`, `incomplete_pages`: `cihistory.cpp` lines 179-180 and 347). Likewise, `cireport.h:28` says `unread` is `[{file, job, field}]`, but `matchJobs` adds `workflow` (`cireport.cpp:208`). A stranger reading the shape from the header would miss a field. Fix: list the optional keys.

Quote: `QJsonArray fetchErrors; // [{call, exit_code}]`

### Row 943 — Low — `src/remotecontrol_docs.cpp:102-111`

review-readability, lane 29-mcp-content-verbs-a, Q4. **Disposition:** queued, ANTS-6561.

`cmdDocIntegrity`'s whole contract comment (102-108) sits above the shared `docPathNotFound` helper, not above `cmdDocIntegrity` (120). A stranger reading either function gets the wrong description. Fix: move 102-108 down to `cmdDocIntegrity`.

Quote: `static QJsonObject docPathNotFound(const QString &verb, const QString &rawPath) {`

### Row 944 — High — `src/remotecontrol_feedback.cpp:1639-1644`

review-readability, lane 30-mcp-content-verbs-b, Q4. **Disposition:** queued, ANTS-6561.

The comment says `untracked[]` carries a DEPRECATED marker and was removed in 0.7.93. The code still emits `untracked[]` with no marker (line 1666), and the build is 0.7.115 (`CMakeLists.txt:2`). A stranger trusting it would either think the field is dead and delete it, breaking callers, or hunt for a marker that does not exist. Nothing in this file corrects it; the tool description in `claudeint…

Quote: `// for one release with a DEPRECATED marker (removed in`

Remedy: rewrite the comment to say `untracked[]` is still emitted for compatibility, with no removal date.

### Row 952 — Low — `src/remotecontrol_feedback.cpp:354-364`

review-readability, lane 30-mcp-content-verbs-b, Q4. **Disposition:** queued, ANTS-6561.

`compact_resolved` is retired (line 728; remotecontrol.h:1575-1576). This comment, and the one at 342-343 (`shared extractor with compact_resolved`), point at a caller that no longer exists. Lines 354-361 also restate the helper's own header (44-47). The header at 38-39 puts a careful reader right.

Quote: `// ANTS-3802 — now a shared helper, so compact_resolved resolves ids the`

Remedy: delete 354-364's restatement and the compact_resolved mentions.

### Row 953 — Low — `src/remotecontrol_feedback.cpp:597-602`

review-readability, lane 30-mcp-content-verbs-b, Q4. **Disposition:** queued, ANTS-6561.

The first three lines describe the ANTS-id gate in `fbIsAntsId` (633). They are merged into the comment above `fbAssignRefusal`, so a reader of either function gets the wrong one's rationale.

Quote: `// The ids these ops write are the ones the Ants MCP maintainer assigns at`

Remedy: move lines 597-599 above `fbIsAntsId`.

### Row 969 — Medium — `src/doclint.cpp:390`

review-readability, lane 31-doc-engines, Q4. **Disposition:** queued, ANTS-6558.

This is now false. DocIntegrity skips an oversize document whole (docintegrity.cpp:858-869, ANTS-5055: "skipped whole, as doc_lint skips it, never read cut short"). A stranger trusting this comment would think the two engines handle the cap differently and could "fix" one to match.

Quote: `// maxDocBytes TRUNCATES in DocIntegrity; here it SKIPS, at every checker`

Remedy: delete the contrast and say both skip.

### Row 970 — Low — `src/doccitations.cpp:1053`

review-readability, lane 31-doc-engines, Q4. **Disposition:** queued under ANTS-6135.

This three-line comment belongs to `dcResolveAttribution` (1082). Inserting `dcEmitCandidates` stranded it on top of the ANTS-4939 paragraph. Now `dcEmitCandidates` opens with an unrelated sentence and `dcResolveAttribution` has no comment.

Quote: `// ANTS-4639 (1) — the scanned document's own directory, ahead of the repo root`

Remedy: move lines 1053-1055 to just above `dcResolveAttribution`.

### Row 971 — Low — `src/doccitations.h:49`

review-readability, lane 31-doc-engines, Q4. **Disposition:** queued, ANTS-6558.

It says only five fields are request-driven and that "the rest … have no request argument". But `quotes`, `quoteMinChars` and `paths` are also read from the request (remotecontrol_docs.cpp:1735-1738). A stranger changing a default would wrongly assume no caller can set it.

Quote: `// expressible against a hard-coded number. The five request-driven fields are`

Remedy: mark those three `// request arg` and drop "five".

### Row 972 — Low — `src/docsindex.h:4`

review-readability, lane 31-doc-engines, Q4. **Disposition:** queued, ANTS-6558.

Both the header and `build()`'s doc comment (docsindex.h:104) name a fixed `docs/` subtree. `walkDocs` honours the `.ants/project.json` `docs_dir` override (docsindex.cpp:254-263, ANTS-2160). The same stale wording is at docsindex.cpp:223.

Quote: `// <root>/*.md (non-recursive) + <root>/docs/**/*.md (recursive), scans each`

Remedy: say "`<root>/docs` or the configured `docs_dir`".

### Row 973 — Low — `src/docfacts.cpp:144`

review-readability, lane 31-doc-engines, Q4. **Disposition:** queued, ANTS-6558.

The comment above (131-132) says a line "indented deeper than the first item" is nested. The code treats one extra space as the same level, and nothing says why. A stranger "fixing" it to `> base` changes the counts.

Quote: `if (ind > base + 1) continue;`

Remedy: one line on why one space of slack is allowed (or align the comment).

### Row 974 — Low — `src/docfacts.cpp:71`

review-readability, lane 31-doc-engines, Q4. **Disposition:** queued, ANTS-6558.

The comment above describes a parameter called `claim`. The parameter is `masked`, and the caller passes the last-sentence tail of the code-masked line.

Quote: `bool singleCardinal(const QString &masked, int *value) {`

Remedy: rename the comment's word to the parameter (or vice versa).

### Row 975 — Low — `src/docsymbols.cpp:279`

review-readability, lane 31-doc-engines, Q4. **Disposition:** queued, ANTS-6558.

A 22-line incident story (`isControlPlaneTool()`, "two full loops") sits inside the `Unresolved` switch case. The current rule (call / qualified / bare by span shape) is three lines of code under it. The header (docsymbols.h:116-120) already states the rule.

Quote: `// ANTS-4359 — the SHAPE of the span the document wrote, so the`

Remedy: cut to one line naming the rule and move the history to the spec.

### Row 985 — Medium — `src/speclog.h:40`

review-readability, lane 32-spec-engines, Q4. **Disposition:** queued, ANTS-6577.

The code also emits `"heading"` (speclog.cpp:442), and test_mcp_spec_log.cpp:704 asserts it. `createLoopLog` leaves `rowOrder` empty, although the comment lists only three values for it. A caller writing a switch on these values from the header gets it wrong.

Quote: `// ANTS-4353/4364 — append_loop only. rowShape is "table" or "bullet",`

Remedy: list `"heading"`, and say `rowOrder` is empty when a new log is created.

### Row 990 — Medium — `src/specparse.cpp:164`

review-readability, lane 32-spec-engines, Q4. **Disposition:** queued, ANTS-6577.

This paragraph, and lines 161-162 ("Accept … case-insensitive"), describe heading regexes that now live in `invariantsSection` (lines 102-127). `parseSpecBody` has none of them. "case-insensitive" is also false: `[Ii]nvariants` folds only the first letter, so `## INVARIANTS` is not found. A stranger edits here and finds nothing, or trusts the case claim.

Quote: `// ANTS-4115 — the heading's TEXT must CONTAIN the word "invariants"; it need`

Remedy: move the paragraph to `invariantsSection`, and correct or drop "case-insensitive".

### Row 991 — Low — `src/specparse.cpp:118`

review-readability, lane 32-spec-engines, Q4. **Disposition:** queued, ANTS-6577.

The code ends the section at any `## ` (line 121). A `### Invariants` section is not ended by the next `###`, so "equal-or-lower depth" is false.

Quote: `// Section ends at the next ##  heading of equal-or-lower depth (treat`

Remedy: say "the next `## ` heading only".

### Row 992 — Low — `src/specparse.cpp:1`

review-readability, lane 32-spec-engines, Q4. **Disposition:** queued, ANTS-6577.

The file has since grown many changes (ANTS-3676, 3799, 4115, 5100, 6046…). "Verbatim" sends a stranger to compare against remotecontrol.cpp.

Quote: `// ANTS-3665 — see specparse.h. Moved verbatim from remotecontrol.cpp's`

Remedy: drop "verbatim".

### Row 993 — Low — `src/specconformance.h:37`

review-readability, lane 32-spec-engines, Q4. **Disposition:** queued, ANTS-6577.

The envelope also carries `executable_fences`, `skipped_fences` and `hint` (specconformance.cpp:320-331).

Quote: `// {ok, path, cases_run, findings[], candidates[], refusals[],`

Remedy: list them.

### Row 994 — Low — `src/specconformance.cpp:177`

review-readability, lane 32-spec-engines, Q4. **Disposition:** queued, ANTS-6577.

This omits the `bad_pattern` compile refusal (lines 235-247), which sits between the table step and the input cap.

Quote: `// ---- precedence: engine -> pattern cap -> table -> input cap -------`

Remedy: add it to the chain.

### Row 995 — Low — `src/featurecoverage.h:104`

review-readability, lane 32-spec-engines, Q4. **Disposition:** queued, ANTS-6577.

Two errors: - The regex is `{2,80}`, i.e. length 2 to 80. The .cpp says it exists to catch `ls` and `ps`. - Only lane 2 calls it: `bulletMatchesAnyTitle`, featurecoverage.cpp:335 and 339. - Fix: say length 2–80 and lane 2 only.

Quote: `// identifier-shaped (no whitespace, len ≥ 3) entries. Shared by both`

### Row 996 — Low — `src/featurecoverage.h:183`

review-readability, lane 32-spec-engines, Q4. **Disposition:** queued, ANTS-6577.

The code tests length ≥ 4 with a leading letter, not four letters. It also omits the ANTS-6389 rule: no fallback for a path or a known file extension (featurecoverage.cpp:600-612).

Quote: `// tail is identifier-shaped (≥4 alpha chars) — prevents matching file`

Remedy: restate both conditions.

### Row 997 — Low — `src/featurecoverage.cpp:144`

review-readability, lane 32-spec-engines, Q4. **Disposition:** queued, ANTS-6577.

The comment describes no code; it sits above the dedup check and reads as explaining it.

Quote: `// Pure numeric tail → noise (e.g. 1024). The regex already`

Remedy: delete it.

### Row 998 — Low — `src/featurecoverage.cpp:464`

review-readability, lane 32-spec-engines, Q4. **Disposition:** queued, ANTS-6577.

The code skips a file that would overflow the cap and keeps adding later, smaller files (lines 555-558, `continue`). featurecoverage.h:147-148 says the same ("Content stops at").

Quote: `// ANTS-5100 — bounded: the content half stops at opts.maxBlobBytes (UTF-8`

Remedy: say "a file that would overflow is skipped".

### Row 999 — Low — `src/featurecoverage.h:141`

review-readability, lane 32-spec-engines, Q4. **Disposition:** queued, ANTS-6577.

The contract omits ANTS-5623: inside a git work tree, only files git lists are read (featurecoverage.cpp:475-478). A caller such as DebtSweepEngine would expect ignored files to be included.

Quote: `// Concatenate every source/config/doc file in the project tree (per the`

Remedy: add one line.

### Row 1000 — Low — `src/featurecoverage.cpp:29`

review-readability, lane 32-spec-engines, Q4. **Disposition:** queued, ANTS-6577.

It is defined at line 733, mid-file.

Quote: `// FeatureCoverage::specStopwords() (defined at end of file) wraps it`

Remedy: drop "at end of file".

### Row 1001 — Low — `src/speclint.h:28`

review-readability, lane 32-spec-engines, Q4. **Disposition:** queued, ANTS-6577.

The overview's counts are stale: - `check()` now emits about 17 kinds, including `header_*`, `filename_id_mismatch`, `duplicate_invariant`, `test_coverage_*` and `invariants_unparsed`. - "TWO CHECKS SHIP GATED" (line 14) omits the test-surface, test-coverage and Layman gates. - Fix: drop the counts, or list the kinds.

Quote: `// ANTS-4127 adds a sixth check to the same walk: a *Test:* clause naming a`

### Row 1002 — Low — `src/speclint.cpp:193`

review-readability, lane 32-spec-engines, Q4. **Disposition:** queued, ANTS-6577.

The next line, "an absent Status, which is 53 specs of this corpus", states a current count with no date, and it goes stale.

Quote: `// The only two vocabularies § 2.5's table names. Everything else — including an`

Remedy: date it or drop the number.

### Row 1007 — Medium — `src/indiereviewengine.cpp:976`

review-readability, lane 33-review-engines, Q4. **Disposition:** queued, ANTS-6571.

This comment is false in two ways: - `deriveComputedPartition` drops git-ignored directories (`underIgnoredDir`, ANTS-4809, line 475). `buildBasenameIndex` does not. - The partition uses `isIndexableFile`, which admits extensionless `#!` scripts. This uses `isIndexableSuffix`. The reason for the suffix difference is recorded only in ROADMAP (the 2026-11-03 "Decide per site" note), not in the code…

Quote: `// The same filters as deriveComputedPartition (indexable suffixes, no`

Remedy: state which filters differ and why, in the comment.

### Row 1014 — Low — `src/coldeyesengine.h:4`

review-readability, lane 33-review-engines, Q4. **Disposition:** queued under ANTS-6137.

Four are listed. The list leaves out `crossDocDiffFromReports`, `parseScope` and `activeSpecIds`. The default-lane list (8–13) also leaves out the `audit-infra` lane and the community docs (CONTRIBUTING, SECURITY, LEGAL, CODE_OF_CONDUCT) that `derivePartition` adds. A stranger will trust the header about which lanes exist.

Quote: `// Five operations:`

Remedy: drop the count, and add `audit-infra` and the community docs to the lane list.

### Row 1015 — Low — `src/debtsweepengine.cpp:1421`

review-readability, lane 33-review-engines, Q4. **Disposition:** queued under ANTS-6137.

This is false. `kFixable` (1435) and the branches cover `duplicate_include` and `obsolete_qstring_idiom` too. The table a few lines below shows the truth.

Quote: `// applyMechanicalFix — orphan_q_unused only in v1`

Remedy: drop "orphan_q_unused only in v1" from the banner.

### Row 1016 — Low — `src/debtsweepengine.cpp:1060`

review-readability, lane 33-review-engines, Q4. **Disposition:** queued, ANTS-6571.

Label (h) is already used at 709 for the dead-branch detector. The header calls this one (i) at `debtsweepengine.h:180`.

Quote: `// Code drift (h) — dead suppression directive [ANTS-3743]`

Remedy: change (h) to (i).

### Row 1017 — Low — `src/indiereviewdialog.cpp:150`

review-readability, lane 33-review-engines, Q4. **Disposition:** queued, ANTS-6571.

Since ANTS-1648, each inlined file is clipped at `kMaxSourceBodyBytes` (`indiereviewengine.h:335–338`). Only the total is uncapped, and no file is inlined "whole" past 1 MiB.

Quote: `// assembleBriefForDispatch is uncapped (inlines whole source + standards`

Remedy: say the total is uncapped, not each file.

### Row 1025 — Medium — `src/mutationprobe.cpp:167`

review-readability, lane 34-test-audit-verify, Q4. **Disposition:** queued, ANTS-6580.

This comment describes how `judgeBaseline` orders its checks, but it sits above `budgetExhausted`. Someone reading `budgetExhausted` looks for "non-zero exit is red" logic that is not there. The comment is also wrong about `judgeBaseline`: exit 4/5 with no counts returns `DidNotRun` before the "non-zero exit is red" check.

Quote: `// ANTS-4401 — see mutationprobe.h. Ordered so the loudest evidence wins: a`

Remedy: move the comment to `judgeBaseline` and add the pytest-4/5 exception.

### Row 1028 — Medium — `src/verifyengine.h:79`

review-readability, lane 34-test-audit-verify, Q4. **Disposition:** queued, ANTS-6580.

The list of `configSource` values leaves out `"bad_config"`, which `loadGateConfig` sets at `verifyengine.cpp:410`. Line 106 mentions only "the parse-error sentinel" and never names it. A caller branching on this list would never handle a malformed config.

Quote: `// one of ".ants/verify.json" | "auto-detected" | "none" |`

Remedy: add `"bad_config"` to the list at line 79 and name it at line 106.

### Row 1033 — Low — `src/testauditengine.cpp:2168`

review-readability, lane 34-test-audit-verify, Q4. **Disposition:** queued, ANTS-6580.

The comment and the returned hint say "after the 5 s budget". `foldIn` now waits about 1 s when `interactive` (ANTS-5102, which is how the dialog calls it).

Quote: `// rename-lock fallback (ANTS-1490) both gave up after the`

Remedy: say "after the lock wait".

### Row 1034 — Low — `src/testauditengine.cpp:2258`

review-readability, lane 34-test-audit-verify, Q4. **Disposition:** queued, ANTS-6580.

`trailingCountRx` strips only the trailing count. Emoji are never removed; they survive into `curDim` when no known dimension matches.

Quote: `// Trailing \(N)\ count and any leading emoji in \## \U0001F9EA Naming (7)\.`

Remedy: drop "and any leading emoji".

### Row 1037 — Low — `src/testauditengine.h:53`

review-readability, lane 34-test-audit-verify, Q4. **Disposition:** queued, ANTS-6580.

This is a hand-kept count of a list. The same count appears at `testauditengine.cpp:67` ("returning all 18"). It goes stale the next time a dimension is added.

Quote: `// 18 dimensions — engine canonical (INV-6). Skill markdown`

Remedy: drop the number.

### Row 1038 — Low — `src/testrescache.h:53`

review-readability, lane 34-test-audit-verify, Q4. **Disposition:** queued, ANTS-6580.

The cap values (50, 8000, 1000, 4000, 20 lines) are repeated in comments at h:21, h:45-46 and h:53-55, and at cpp:218 and cpp:223. "20" is a bare literal at `testrescache.cpp:226` with no named constant. Changing `kMaxFailures` or the tail length leaves up to five comments wrong.

Quote: `// function. Caps failingTests[] at 50; per-failure full body at`

Remedy: name the 20-line constant, and have the comments name the constants instead of their values.

### Row 1039 — Low — `src/mutationprobe.h:86`

review-readability, lane 34-test-audit-verify, Q4. **Disposition:** queued, ANTS-6580.

`parseCounts` also returns 0/0 for ctest's "No tests were found" and Catch2's "No tests ran" (`mutationprobe.cpp:150-159`). The format list above leaves them out, so someone trusting it would treat 0/0 as impossible.

Quote: `// Anything else leaves both at -1 rather than guessing.`

Remedy: add the two empty-run forms to the list.

### Row 1040 — Low — `src/testauditdialog.cpp:438`

review-readability, lane 34-test-audit-verify, Q4. **Disposition:** queued, ANTS-6580.

Line 21 of `testauditengine.h` is about chunk-report fencing. The fold_in ownership text is at lines 22-23, and a line pointer drifts with every edit anyway.

Quote: `// The engine owns ID allocation + insert (testauditengine.h:21) — do`

Remedy: cite "INV-3" instead of a line.

### Row 1043 — Low — `src/verifyengine.cpp:439`

review-readability, lane 34-test-audit-verify, Q4. **Disposition:** queued, ANTS-6580.

"(Phase 1; Phase 2 wires the client through cmdVerifyChanges)" reads as future work. Phase 2 has shipped: `remotecontrol_review.cpp:1225,1237` pass a client. The same wording is at `verifyengine.h:115-116`.

Quote: `// Back-compat: no client → honour unconditionally`

Remedy: say the null-client path is kept for callers without a trust client.

### Row 1044 — Low — `src/testauditdialog.cpp:209`

review-readability, lane 34-test-audit-verify, Q4. **Disposition:** queued, ANTS-6580.

The comment runs to 10 lines of history ("the H1 zombie", "the H2 staleness gate, previously decorative", "is now a live production caller"). It buries the current rule, which is: reload resume state, refresh the partition once, and dispatch only unreviewed chunks when the token matches.

Quote: `// ANTS-1843 — re-derive the partition once up-front so m_token + m_chunks`

Remedy: state that rule in two lines and move the history to the commit.

### Row 1063 — Medium — `src/readregion.cpp:575`

review-readability, lane 35-code-index-search-a, Q4. **Disposition:** queued, ANTS-6553.

the code below this comment only clamps `startLine` to 1. Nothing is refused, so the comment is false.

Quote: `// as a defensive bad_args rather than reading garbage.`

Remedy: make the comment say it clamps.

### Row 1064 — Medium — `src/applyedits.cpp:79`

review-readability, lane 35-code-index-search-a, Q4. **Disposition:** queued, ANTS-6553.

the function's opening comment states a scope the code no longer has. The first branch (lines 83-110) handles multi-line misses. The fix is noted four lines lower, but the opening comment is the first thing a reader sees.

Quote: `// A multi-line miss therefore reports no near miss, which is honest; the`

Remedy: rewrite the opening comment to state the current rule and drop the old history.

### Row 1065 — Low — `src/applyedits.cpp:16`

review-readability, lane 35-code-index-search-a, Q4. **Disposition:** queued, ANTS-6553.

the comment says trailing whitespace is dropped. Leading whitespace is dropped too (`pendingSpace = !out.isEmpty()`), which makes near-miss matching ignore indentation.

Quote: `// ANTS-4418 — collapse runs of horizontal whitespace to one space and drop`

Remedy: say "leading and trailing".

### Row 1066 — Low — `src/codebaseindex.cpp:30`

review-readability, lane 35-code-index-search-a, Q4. **Disposition:** queued, ANTS-6553.

not true today. `langForExt` maps `.lua` and `.pyi` (symbolquery.cpp:49-51), and this gate admits neither. Someone trusting "in step" would think Lua projects are indexed.

Quote: `// kept in step with SymbolQuery::langForExt. Every family is delegated`

Remedy: name the exceptions, or reword the rule as "outline → index".

### Row 1067 — Low — `src/fileoutline.cpp:436`

review-readability, lane 35-code-index-search-a, Q4. **Disposition:** queued, ANTS-6553.

this comment explains rxPyConst, but it sits above rxHtmlId (lines 439-449), with rxPyConst after that. A reader of rxPyConst never sees why it is anchored at column 0.

Quote: `// Anchored at column 0 — an indented assignment is a local or a class`

Remedy: move the ANTS-4379 comment down to sit directly above rxPyConst.

### Row 1068 — Low — `src/fileoutline.cpp:576`

review-readability, lane 35-code-index-search-a, Q4. **Disposition:** queued, ANTS-6553.

the brace-family rules are numbered (1)…(4), but the shell rules, numbered (1)…(3), sit in the middle of them. That makes two "(3)"s, and rxGenericBinding's "unlike rules 1–3" is unclear.

Quote: `// (3) JS/TS arrow-function assignments: export const foo = (…) => ….`

Remedy: give the shell set its own prefix, or move it after the brace set.

### Row 1069 — Low — `src/fileoutline.h:7`

review-readability, lane 35-code-index-search-a, Q4. **Disposition:** queued, ANTS-6553.

out of date. The file now builds about 30 patterns for eight modes. Lines 2-3 still say "C++ / Python / Markdown", and the `Generic` list at line 34 leaves out Ruby and UnrealScript.

Quote: `// No clangd / no tree-sitter — a 6-regex set covers the 90% case`

Remedy: update the summary or drop the counts.

### Row 1070 — Low — `src/readregion.cpp:162`

review-readability, lane 35-code-index-search-a, Q4. **Disposition:** queued, ANTS-6553.

this describes the end of a symbol's range as it was before brace-matching (ANTS-2222/2224) existed. It also describes `resolveSymbol`, which is 70 lines further down.

Quote: `// the next outline entry, or INT_MAX (to EOF) when the match is last.`

Remedy: move it to `resolveSymbol` and mention brace-matching.

### Row 1071 — Low — `src/findsources.cpp:306`

review-readability, lane 35-code-index-search-a, Q4. **Disposition:** queued, ANTS-6553.

wrong: the filename may not have matched. findsources.h:57-60 also gives `variantsForToken` as for filename matching, but the variants are also the needles searched for in file contents (line 264).

Quote: `// already know the filename matched; even files that didn't`

Remedy: correct both comments.

### Row 1072 — Low — `src/similarcode.h:40`

review-readability, lane 35-code-index-search-a, Q4. **Disposition:** queued, ANTS-6553.

FileOutline also gives "namespace" (cpp) and "const" (py). Line 39's "trimmed by FileOutline" is false for C++ function lines, which are passed through untrimmed (fileoutline.cpp:958).

Quote: `QString kind; // FileOutline kind: "class" / "func" / "qt"`

Remedy: refer to FileOutline's kinds instead of listing them.

### Row 1082 — Medium — `src/paginationengine.h:58-60`

review-readability, lane 36-code-index-search-b, Q4. **Disposition:** queued, ANTS-6553.

The comment says the measure-then-cut happens "when `limit < 0`". Since ANTS-5651 (paginationengine.cpp:78-90), an explicit `limit` is also cut to the byte budget, and `limit == 0` takes the auto path too. A caller relying on getting exactly `limit` rows from an explicit page would be wrong. The parameter note at 68-71 repeats the omission.

Quote: `// Slice filtered per offset + limit, applying auto-truncate`

Remedy: say that both paths are cut to the byte budget, keeping at least one row.

### Row 1083 — Low — `src/paginationengine.h:1`

review-readability, lane 36-code-index-search-b, Q4. **Disposition:** queued, ANTS-6553.

It is also used by changelog (remotecontrol_changelog.cpp:756, 837). Someone tuning it for roadmap needs alone would change changelog paging without knowing.

Quote: `// ANTS-1436 — pagination + auto-truncate helper for cmdRoadmapQuery.`

Remedy: drop "for cmdRoadmapQuery" or list the callers' shape generically.

### Row 1084 — Low — `src/symbolquery.cpp:437`

review-readability, lane 36-code-index-search-b, Q4. **Disposition:** queued, ANTS-6553.

A stray, repeated "holds." left over from an edit; the reader stops to look for a missing clause.

Quote: `// holds. bySymbol keys on views into the caller's stable needle list.`

Remedy: delete the stray word.

### Row 1085 — Low — `src/symbolquery.cpp:196`

review-readability, lane 36-code-index-search-b, Q4. **Disposition:** queued, ANTS-6553.

C++ now pushes six `def` patterns (lines 253-316), and Generic pushes three.

Quote: `// Per-call compiled anchors for one language. def holds 1-2 patterns`

Remedy: drop the count.

### Row 1086 — Low — `src/trace/extract.h:73`

review-readability, lane 36-code-index-search-b, Q4. **Disposition:** queued, ANTS-6553.

`rowCacheBytes()` returns current use only; nothing exposes the cap (`kCacheCapBytes` is file-local). A test author would look for a cap accessor that does not exist.

Quote: `// The cache's byte cap and current use, for tests.`

Remedy: "current use, and a clear, for tests".

### Row 1087 — Medium — `src/mcpprojection.h:75`

review-readability, lane 37-mcp-infra, Q4. **Disposition:** queued, ANTS-6562.

The header says non-string and empty names are listed in `fields_unmatched`. The code does not do that: the loop at mcpprojection.cpp:253-257 skips non-strings and requires `!name.isEmpty()`. It also leaves out the whole list on a refusal (`!refusal`, line 264), which the header never mentions. A stranger writing a test, or a caller, from this contract would expect `fields:[""]` or `fields:[3]` t…

Quote: `// - field absent / non-string / empty -> omitted, and NAMED in`

Remedy: say that only absent, non-empty string names are listed, and never on `ok:false`.

### Row 1090 — Medium — `src/jsonlfile.cpp:36`

review-readability, lane 37-mcp-infra, Q4. **Disposition:** queued, ANTS-6562.

This is false. `McpUndeclaredArgs::appendRecord` (mcpundeclaredargs.cpp:42) also calls `writeLinesAtomic`, and its lines hold tool names and argument names chosen by the caller. The comment is the only stated reason why a permissions failure may be allowed to continue. A stranger deciding whether that is still safe would rely on a premise that no longer holds.

Quote: `// ANTS-5151 — warns and writes: every caller today is a model-switch`

Remedy: name all three callers and say why each is acceptable.

### Row 1091 — Low — `src/jsonlfile.h:29`

review-readability, lane 37-mcp-infra, Q4. **Disposition:** queued, ANTS-6562.

The header promises the file "is never briefly group/world-readable", and that the function "Returns false on any I/O failure". The code does something different when `setOwnerOnlyPerms` fails: it warns and commits anyway (jsonlfile.cpp:39-41).

Quote: `// tightens the temp file to owner-only (0600) before the rename, so the final`

Remedy: state the warn-and-write exception in the header.

### Row 1092 — Low — `src/mcpprojection.h:236`

review-readability, lane 37-mcp-infra, Q4. **Disposition:** queued, ANTS-6562.

There are two problems. First, this block documents `bulletMatchesQuery`, but it sits directly on `enum class QueryMode` (line 248) and is separated from the function at line 259. Second, it lists "the three text surfaces" and says "case-insensitive substring". Since ANTS-5257 the match also covers `id` when it contains a space (mcpprojection.cpp:765-772), and the mode can be whole-word or regex.

Quote: `// ANTS-3391 — true when the roadmap bullet object \bullet\ contains \needle\`

Remedy: move the block to line 259 and add `id` and the mode.

### Row 1093 — Low — `src/mcpprojection.h:200`

review-readability, lane 37-mcp-infra, Q4. **Disposition:** queued, ANTS-6562.

This is history that now contradicts the current rule. `fields` is one of the three arguments every verb honours (`isUniversalDispatchArg`), so it is no longer a "conditional" argument. Lines 17-23 do the same thing: a paragraph about the removed `fields=` allowlist sits on `isDefaultCompactTool`. A stranger reading them could think `fields` is still conditional.

Quote: `// Before this the four conditional args were exempt EVERYWHERE, so passing`

Remedy: delete both history paragraphs.

### Row 1094 — Low — `src/mcpspill.h:77`

review-readability, lane 37-mcp-infra, Q4. **Disposition:** queued, ANTS-6562.

The header says only `read_regions` uses the ceiling. The code applies the ceiling to `read_region` as well (mcpspill.cpp:161-163, ANTS-4954). A stranger reading the header would think `read_region` spills at the base threshold.

Quote: `// it inline. It never lowers the configured threshold. read_regions`

Remedy: add `read_region`.

### Row 1095 — Low — `src/mcpspill.cpp:657`

review-readability, lane 37-mcp-infra, Q4. **Disposition:** queued, ANTS-6562.

`readSpill` no longer reads the whole file: since ANTS-5104 it seeks and reads one page (lines 624-630). The comparison now points at something that is gone.

Quote: `// (§ 4 — do not mirror readSpill's unconditional readAll on the row path).`

Remedy: drop the comparison.

### Row 1096 — Low — `src/mcpspill.cpp:593`

review-readability, lane 37-mcp-infra, Q4. **Disposition:** queued, ANTS-6562.

This figure is probably stale. The no-array hint alone (lines 329-333) is about 300 characters before the 64-character handle is added, and the `handle` field adds 64 more. I did not measure it. A stranger reasoning about INV-9's margin would start from a number that is too small.

Quote: `// head+pointer envelope's fixed overhead (handle + hint + keys ≈ 330 B)`

Remedy: drop the figure, or say what it measured.

### Row 1098 — Low — `src/tokenusageengine.h:18`

review-readability, lane 37-mcp-infra, Q4. **Disposition:** queued, ANTS-6562.

The list leaves out `Tracker::recordCall`, which is where a success or failure counter is actually added up. A stranger who follows the list exactly would add a field that is never filled in.

Quote: `// Adding a field means updating every list of them in tokenusageengine.cpp:`

Remedy: add `recordCall` to the list.

### Row 1101 — Low — `src/mcporientation.cpp:87`

review-readability, lane 37-mcp-infra, Q4. **Disposition:** queued, ANTS-6562.

This `//` comment sits between string literals that are joined into one, and it ends in `\n"`, left over from when it was part of the string. A stranger cannot tell at a glance whether this text reaches the shipped script.

Quote: `// INV-10 cap — when no such file is present (the test case).\n"`

Remedy: drop the trailing `\n"`.

### Row 1102 — Low — `src/secretredact.h:72`

review-readability, lane 37-mcp-infra, Q4. **Disposition:** queued, ANTS-6562.

It says rule order lets the specific shapes "win on the narrow value range". But `scrub` settles overlaps by start position first (lines 216-231). `Bearer …` starts before the token it wraps, so it always wins whatever the order — which is what lines 165-168 intend. Order decides only when two matches start at the same place, as with `generic_secret`'s capture group. A stranger reordering rules t…

Quote: `// 4. Context-wrapping patterns (Bearer header, generic secret`

Remedy: say priority applies only at equal starts, and that Bearer wins by starting earlier.

### Row 1103 — Low — `src/resolvedroot.h:8`

review-readability, lane 37-mcp-infra, Q4. **Disposition:** queued, ANTS-6562.

The comment says the function is declared in remotecontrol.h. It is declared in this header, at line 67. remotecontrol.h:904 only mentions it in a comment.

Quote: `// All three now route through \resolveCallerCwdRoot()\ (declared in`

Remedy: say it is declared below.

### Row 1104 — Low — `src/mcpdeprecation.cpp:52`

review-readability, lane 37-mcp-infra, Q4. **Disposition:** queued, ANTS-6562.

Every entry in the table gets an error naming ANTS-5485, and the comment at line 9 counts "the twenty verbs". A stranger adding a verb removed later, under another ticket, would make it cite the wrong ticket and leave the count wrong.

Quote: `QStringLiteral("Tool %1 was removed (ANTS-5485); use %2 instead.")`

Remedy: say beside the table that the message names ANTS-5485 for every entry, and drop the count.

### Row 1105 — Low — `src/jsoncanonical.h:9`

review-readability, lane 37-mcp-infra, Q4. **Disposition:** queued, ANTS-6562.

The .cpp is already 196 lines.

Quote: `// exponent. So the divergence is numbers alone — which is why this is ~120`

Remedy: drop the line count.

### Row 1106 — High — `src/modelrecommender.h:34`

review-readability, lane 38-model-switching, Q4. **Disposition:** queued, ANTS-6568.

`score()` now keeps a function-level `static QHash` memo cache (modelrecommender.cpp:169-196, ANTS-1927). That cache is "GUI-thread only … deliberately unsynchronised", which is said only inside the .cpp body. A stranger who trusts the header (and line 2, "Stateless free function") would call `score()` from a worker thread, for example in a review dialog, and race on the cache.

Quote: `// Stateless: no per-call mutable state.`

Remedy: replace both header lines with "memoised per (path, mtime, size, homeTier); GUI-thread only, not thread-safe".

### Row 1112 — Medium — `src/modelautoswitch.cpp:108-122`

review-readability, lane 38-model-switching, Q2,Q4. **Disposition:** queued, ANTS-6568.

In `decide()`, about 60 of the roughly 125 lines are ticket history: - :108-122 interleaves the focused-state gate (ANTS-1939) and the long-ToolUse computation (ANTS-1959), then the code computes both. - :138-160 is 23 lines spanning ANTS-1908, 1914, 1973 and 1931. - :198-213 is 16 lines. The stranger loses the thread at :123-137, where `longToolUse` and `composerStaleThreshold` are shared by two…

Quote: `// ANTS-1939 — focused_state_not_idle soft-veto. Mirrors the ANTS-1908`

Remedy: one or two lines per gate stating the current yield condition; move the history to the specs or the commit body.

### Row 1113 — Medium — `src/llmclient.h:65-66`

review-readability, lane 38-model-switching, Q4. **Disposition:** queued, ANTS-6568.

The implementation (cpp:74) writes `*schemeOut` on false as well, and both callers depend on that: aidialog.cpp:177 and `endpointEgressError` at cpp:222 print the rejected scheme. A stranger "matching the doc" by moving the assignment into the true branch would blank both rejection messages.

Quote: `// True iff endpoint parses with an http/https scheme. On true,`

Remedy: say "receives the lowercased scheme whenever endpoint is non-empty (callers print it on rejection)".

### Row 1117 — Low — `src/modelautoswitch.h:144`

review-readability, lane 38-model-switching, Q4. **Disposition:** queued, ANTS-6568.

h:281 (and cpp:153, "Vestige: 67%") say `composer_not_empty` is "the dominant near-miss blocker (~67%)". The two comments give two different tokens as the dominant blocker at the same figure, with no date. A stranger cannot tell which is current.

Quote: `// (ticks_target_stable_insufficient, ~67% of blocks) is score oscillation at`

Remedy: date each measurement or drop the figures.

### Row 1118 — Low — `src/modelautoswitch.cpp:198-199`

review-readability, lane 38-model-switching, Q4. **Disposition:** queued, ANTS-6568.

The next line says "to preserve the v1 8-token ordering". v1 had 7 tokens (cpp:103-104); 8 is the post-ANTS-1917 count.

Quote: `// ANTS-1959 — downgrades are unsafe while the shell is idle (9th token,`

Remedy: say "the existing 8-token ordering".

### Row 1120 — Low — `src/llmclient.h:57`

review-readability, lane 38-model-switching, Q4. **Disposition:** queued under ANTS-6142.

aidialog.cpp no longer has a cap; since ANTS-1727 it reads `LlmClient::kMaxBytes` (aidialog.h:63). The comment points at something that is not there.

Quote: `// Accumulated-content + SSE-line-buffer cap (mirrors aidialog.cpp).`

Remedy: drop "(mirrors aidialog.cpp)" and the superseded ANTS-1753 history.

### Row 1121 — Low — `src/modelswitchledger.h:10`

review-readability, lane 38-model-switching, Q4. **Disposition:** queued under ANTS-6142.

`model_switch_stats` is dispatched from RemoteControl (`remotecontrol_state.cpp`, which builds the `StatsConfig` at :1320-1335). The sibling header modelnearmissledger.h:9 says so. A stranger looking for the caller would search the wrong file.

Quote: `// (dispatched from mainwindow.cpp / ants_chrome_lib) can reach it. Tiers are`

Remedy: name RemoteControl.

### Row 1122 — Low — `src/modelswitchledger.h:256-258`

review-readability, lane 38-model-switching, Q4. **Disposition:** queued, ANTS-6568.

It continues "disabled switcher (no config surfaced)". In fact `statsEnvelope` always writes `auto_model_switch_enabled`, `floor_tier`, `min_dwell_sec` and an off-reason (cpp:570-594), and the wrapper applies a 30-day window that only the .cpp comment (:644-647) mentions.

Quote: `// Back-compat thin wrapper — defaults to scope:"project" with a`

Remedy: say "default StatsConfig: project scope, 30-day window, no epoch filter".

### Row 1123 — Low — `src/aidialog.h:42`

review-readability, lane 38-model-switching, Q4. **Disposition:** queued, ANTS-6568.

The cap is 4096 UTF-16 characters, not bytes. Lines 56-58 of the same header correct this (ANTS-2205), and :51-52 say "bytes" again.

Quote: `// the body), and cap at 4 KiB. Defense against OWASP LLM02 —`

Remedy: say "4096 chars".

### Row 1124 — Low — `src/aidialog.cpp:160-163`

review-readability, lane 38-model-switching, Q4. **Disposition:** queued, ANTS-6568.

The rationale says a scheme "with no transport encryption would leak that traffic, so reject anything other than http/https". Plain http is unencrypted and is allowed. The next lines and the status text correct it.

Quote: `// context (potentially carrying secrets, file paths, command output)`

Remedy: state the actual reason (only network schemes Qt can POST to are allowed; http's risk is handled by `plaintextPromptWarning`).

### Row 1133 — Medium — `src/projectlayoutengine.h:141`

review-readability, lane 39-config-session-project, Q4. **Disposition:** queued, ANTS-6554.

The comment continues "1:1, no renames or reshaping", which the code contradicts in four ways: - `snifferTrace` is written as `sniffer_branches_tried`. - `roadmap_found` and `changelog_found` are derived extras with no field behind them. - `standards_files_truncated` and `standards_files_total` are emitted only when the list was cut. - The trace object is emitted only when a roadmap was found. A …

Quote: `// Mechanical case-mapping: camelCase struct fields ↔`

Remedy: drop "1:1, no renames" and name the exceptions.

### Row 1134 — Medium — `src/projectlayoutengine.cpp:154`

review-readability, lane 39-config-session-project, Q4. **Disposition:** queued, ANTS-6554.

`remotecontrol.cpp:301–309` keeps a hand-copied YAML subset of this list, with the comment "so reader/writer discovery stay in lockstep". The six entries match today. This side has no pointer back, so a stranger adding a YAML candidate here would miss the writer's copy.

Quote: `const QStringList kChangelogCandidates = {`

Remedy: a one-line comment on `kChangelogCandidates` naming `findYamlChangelogUnder` in remotecontrol.cpp.

### Row 1135 — Medium — `src/config.h:103`

review-readability, lane 39-config-session-project, Q4. **Disposition:** queued, ANTS-6554.

Two hand-kept lists of this object's keys disagree with the code: - The header lists 4 keys. It omits `composer_stale_ms`, `idle_ceiling_sec` and `debug`. - config.cpp:441–444 lists on-disk keys but omits `auto_model_home_tier` and `auto_model_debug`. The .cpp comment also says the object is a WHITELIST that a new knob must join. A stranger taking the header as the full contract would think a new…

Quote: `// claudeAutoModel() returns a QJsonObject keyed by:`

Remedy: list all seven keys once, in the header, and point the .cpp comment at it.

### Row 1136 — Low — `src/config.h:78`

review-readability, lane 39-config-session-project, Q4. **Disposition:** queued under ANTS-6143.

The shape listed on line 80 has six keys, and line 90 says "the six keys above".

Quote: `// RoadmapDialog 5 status-emoji checkboxes — applies only when`

Remedy: "6", or drop the count.

### Row 1137 — Low — `src/config.cpp:109`

review-readability, lane 39-config-session-project, Q4. **Disposition:** queued under ANTS-6143.

`rotateCorruptFileAside` is in configbackup.h. Both headers say it moved there in 0.7.31.

Quote: `// secureio.h — millisecond timestamp + retry-on-collision).`

Remedy: say configbackup.h.

### Row 1138 — Low — `src/configbackup.h:21`

review-readability, lane 39-config-session-project, Q4. **Disposition:** queued, ANTS-6554.

The list of callers is stale. The real callers are config.cpp, claudeallowlist.cpp, sessionmanager.cpp and claudesetup.cpp. SettingsDialog is not one. Lines 15–16 repeat the same list.

Quote: `// clobber it. Used by Config, Claude allowlist, SessionManager, and`

Remedy: drop the caller lists (§3).

### Row 1139 — Low — `src/sessionmanager.cpp:199`

review-readability, lane 39-config-session-project, Q4. **Disposition:** queued, ANTS-6554.

No function called `ensureSessionsDir` exists. The 0700 directory comes from `sessionDir()` → `ensurePrivateDir`.

Quote: `// see ensureSessionsDir above). Real tamper resistance would require`

Remedy: name `sessionDir()`.

### Row 1140 — Low — `src/sessionmanager.cpp:705`

review-readability, lane 39-config-session-project, Q4. **Disposition:** queued, ANTS-6554.

Since ANTS-5106, temp files are `<name>.<pid>.tmp`. `saveSessionAsync` writes them too. The "two temp names we actually own" in lines 710–713 are now four patterns, and only the trailing tag "pre-ANTS-5106 names" hints at this.

Quote: `// saveSession + saveTabOrder both write to <name>.tmp and`

Remedy: describe the current per-pid names first and the legacy pair second.

### Row 1141 — Low — `src/config.cpp:1030`

review-readability, lane 39-config-session-project, Q4. **Disposition:** queued, ANTS-6554.

Session files are capped at 100 MB (`MAX_RESTORE_FILE_BYTES`), and the default is 50,000 lines of scrollback. A cost comment (§3) that understates the cost misleads anyone deciding whether persistence should stay on by default.

Quote: `// without hunting through Settings. The only cost is ~a few KB of`

Remedy: state the real bound.

### Row 1142 — Low — `src/projectsettings.cpp:256`

review-readability, lane 39-config-session-project, Q4. **Disposition:** queued, ANTS-6554.

This 14-line block describes `gitIgnoredDirs`. That function moved out of the namespace (see line 272), so the block now sits on top of `} // namespace` and describes nothing.

Quote: `// ANTS-4092 — the candidate dirs git itself ignores. workspace_search`

Remedy: move it onto `gitIgnoredPaths`, or delete it, since the header already covers it.

### Row 1143 — Low — `src/projectsettings.h:157`

review-readability, lane 39-config-session-project, Q4. **Disposition:** queued, ANTS-6554.

`detect` calls `CodebaseIndex::isIndexableFile` (projectsettings.cpp:201, 398). That function delegates to `isIndexableSuffix` only for files that have a suffix (codebaseindex.cpp:74). A stranger changing what `detect` counts could edit the wrong function.

Quote: `// non-empty). Else counts source files (CodebaseIndex::isIndexableSuffix)`

Remedy: name `isIndexableFile`.

### Row 1146 — Medium — `src/luaengine.cpp:1165`

review-readability, lane 40-plugins-lua, Q4. **Disposition:** queued, ANTS-6570.

The cost estimate (`64 × 512 KiB ≈ 32 MiB`) counts only the thread stack. A wedged query worker also keeps its `LuaEngine` alive, with a Lua heap of up to `MAX_LUA_MEMORY` (10 MiB, `luaengine.h:64`). The comment at lines 1157–1158 adds a result slot of up to `result_cap_bytes`. The real ceiling is in the hundreds of MiB, not 32 MiB. A stranger tuning `kMaxQueryZombies` would trust the false figur…

Quote: `// while it is wedged, so an unbounded wedge-loop would leak ~512 KiB/thread. Past`

Remedy: restate the bound per zombie as stack + Lua heap cap + result slot.

### Row 1148 — Medium — `src/pluginmanager.cpp:463`

review-readability, lane 40-plugins-lua, Q4. **Disposition:** queued, ANTS-6570.

This comment and the one at lines 445–447 (*"bind mounts, hardlinks under different names"*) say the canonical-path check catches bind mounts and hardlinks. `canonicalFilePath` follows symlinks only: a bind mount is invisible to it, and directories cannot be hardlinked. A stranger judging this security guard would think it covers cases it does not.

Quote: `// (entry that exists at scan time but resolves outside via a bind mount, a`

Remedy: say the check catches symlinks only, as a backstop behind `NoSymLinks`.

### Row 1152 — Low — `src/pluginmanager.cpp:572`

review-readability, lane 40-plugins-lua, Q4. **Disposition:** queued, ANTS-6570.

`info.name` is a directory name from a single `entryList` call (line 498), so two entries cannot share it, and "symlink shenanigans" cannot change it. A stranger would hunt for a collision path that does not exist.

Quote: `// after the name-spoofing fix (lane-6 C-1), two plugin directories that`

Remedy: say the guard is defensive only.

### Row 1153 — Low — `src/pluginmanager.h:92`

review-readability, lane 40-plugins-lua, Q4. **Disposition:** queued, ANTS-6570.

The same comment calls `dispatchTo` "non-blocking" two lines earlier, and it is queued. It also claims to "share" `healthyEngines()`, but it calls its own copy of the filter (finding above).

Quote: `// healthyEngines() filter). The only supported synchronous-from-GUI`

Remedy: say "the only supported GUI-side entry", and drop "sharing".

### Row 1154 — Low — `src/pluginmanager.cpp:64`

review-readability, lane 40-plugins-lua, Q4. **Disposition:** queued, ANTS-6570.

There is no `watchPaths()`. The code calls `addPath`.

Quote: `// zero because we never call watchPaths() in the non-dev path.`

Remedy: name `addPath`.

### Row 1155 — Low — `src/luaengine.cpp:276`

review-readability, lane 40-plugins-lua, Q4. **Disposition:** queued, ANTS-6570.

No branch is labelled "first expiry". The comment means the `wallExpired` branch at line 291.

Quote: `// threshold dropped to 1 below (see "first expiry" branch) the`

Remedy: name `wallExpired`.

### Row 1157 — Low — `src/luaengine.cpp:314`

review-readability, lane 40-plugins-lua, Q4. **Disposition:** queued, ANTS-6570.

The rationale for the default budget sits in `startPcallBudget`, which does not set the budget. The default is `m_pcallBudgetMs = 1500` at `luaengine.h:322`. A stranger changing the default edits the header and never sees the reasoning.

Quote: `// The default budget is big enough to let a normal plugin finish`

Remedy: move the comment to the member.

### Row 1158 — Low — `src/luaengine.cpp:41`

review-readability, lane 40-plugins-lua, Q4. **Disposition:** queued, ANTS-6570.

The panic-handler comment says unprotected allocation sites are hypothetical. The ANTS-4442 comment at lines 56–58 says that was false. That correction sits right below, so a reader recovers quickly.

Quote: `// Lua error escaping a pcall (today: rare; tomorrow: a regression`

Remedy: trim the stale "today: rare" clause.

### Row 1160 — Low — `src/luaengine.h:298`

review-readability, lane 40-plugins-lua, Q4. **Disposition:** queued, ANTS-6570.

The assert is valid only while `PaletteAction` is the last value in the list. A stranger who appends a new event after it gets no protection from the guard. Today there are 16 values, so the 32-bit limit is far off.

Quote: `static_assert(static_cast<int>(PluginEvent::PaletteAction) < 32,`

Remedy: comment that `PaletteAction` must stay last, or assert on a final count value.

### Row 1167 — Low — `src/changelogquery.cpp:23`

review-readability, lane 41-changelog-feedback, Q4. **Disposition:** queued under ANTS-6145.

`FenceInfo` has no info-string field. The rule moved to `MarkdownScan::fenceCloses`, as lines 40–42 say. A reader would look for a field that does not exist.

Quote: `// an info string follows (CommonMark: a close fence carries no info).`

Remedy: drop the clause.

### Row 1168 — Low — `src/changeloglog.h:16`

review-readability, lane 41-changelog-feedback, Q4. **Disposition:** queued under ANTS-6145.

This sits above `kindToCategory`, but it describes `canonicalCategories`, which has its own comment at line 29.

Quote: `// The six canonical Keep-a-Changelog categories, in spec order.`

Remedy: delete it here.

### Row 1169 — Low — `src/changeloglog.cpp:556`

review-readability, lane 41-changelog-feedback, Q4. **Disposition:** queued, ANTS-6549.

This sits above `isReleasableLine`, not above `closeUnreleased`, which is the function it describes.

Quote: `// ANTS-4363 — see the header. Close [Unreleased] into a version block.`

Remedy: move it to `closeUnreleased`.

### Row 1170 — Low — `src/changeloglog.cpp:504`

review-readability, lane 41-changelog-feedback, Q4. **Disposition:** queued, ANTS-6549.

Steps "3." and "4." (line 540) point back to steps 1 and 2, which are no longer numbered after the ANTS-6145 refactor.

Quote: `// 3. Find the ### <category> heading within the section, and`

Remedy: drop the numbers, or restore steps 1 and 2.

### Row 1171 — Low — `src/changeloglog.cpp:822`

review-readability, lane 41-changelog-feedback, Q4. **Disposition:** queued, ANTS-6549.

About 24 lines of incident history (Vestige's line ranges, the self-perpetuating guard) come before the two-line current rule ("Position settles it"). `classifyUnreleased`'s header already states that rule. Lines 848–854 then copy every shape field into a local. A reader has to dig through the history to find the rule.

Quote: `// ANTS-4489 (reported by Vestige) — classify by POSITION, not by`

Remedy: keep the rule and a one-line pointer, and move the history out.

### Row 1174 — Medium — `src/diffviewer.cpp:788`

review-readability, lane 42-app-entry-dialogs, Q4. **Disposition:** queued under ANTS-6146.

False. `finalize` reaches this point on every refresh round whose output changed, against the same Copy button. So the disconnect runs on every live update; it is not insurance. A stranger who trusts the comment and drops the disconnect will stack one clipboard handler per refresh. Fix: say the disconnect replaces the previous round's handler on every re-render.

Quote: `// invoked twice (shouldn't happen, but cheap insurance).`

### Row 1175 — Medium — `src/diffviewer.cpp:675`

review-readability, lane 42-app-entry-dialogs, Q4. **Disposition:** queued under ANTS-6146.

Only binary files get a one-line summary. A file over 200 KB is still rendered, cut at 200 KB with a "(truncated at 200 KB)" note (lines 701-726). A stranger changing the size handling will look for a summary branch that does not exist. Fix: say that files over 200 KB are shown up to 200 KB and marked truncated.

Quote: `// Binary files and files > 200 KB get a one-line summary instead.`

### Row 1179 — Medium — `src/settingsdialog.cpp:641-643`

review-readability, lane 42-app-entry-dialogs, Q4. **Disposition:** queued, ANTS-6545.

The comment argues that the portal-status label's diagnostic value does not depend on Quake mode. Yet `syncQuake` greys it with `portalStatus->setEnabled(on)` (647). A stranger cannot tell whether that line is a bug or the comment is stale. Fix: decide which is meant, then either drop the label from `syncQuake` or reword the comment to say it is greyed but kept.

Quote: `// mode is off. Portal-status label stays visible — its`

### Row 1180 — Low — `src/settingsdialog.cpp:441-443`

review-readability, lane 42-app-entry-dialogs, Q4. **Disposition:** queued, ANTS-6545.

This list of what the MCP master switch greys out leaves out `m_claudeMcpOffload`, which `mirrorMcpMaster` also disables (455). The same stale list appears at 262-263 and in `settingsdialog.h:105-107`. Someone adding a new MCP toggle who goes by these lists will not know offload is gated. Fix: name `mirrorMcpMaster` as the one list and drop the copies in prose.

Quote: `// them. When off, the auto-switch checkbox, orientation hook, and`

### Row 1181 — Low — `src/settingsdialog.cpp:1112`

review-readability, lane 42-app-entry-dialogs, Q4. **Disposition:** queued, ANTS-6545.

The comment sits above the home-tier block (ANTS-1974), not above the three toggles. The same misplacement is at 1244. Fix: move both comments down to the toast/pulse/undo lines.

Quote: `// ANTS-1893 — load the three surfacing mute toggles (default TRUE).`

### Row 1184 — Low — `src/diffviewer.cpp:346`

review-readability, lane 42-app-entry-dialogs, Q4. **Disposition:** queued, ANTS-6545.

The round state is held by a `std::shared_ptr` plus a generation counter (ANTS-5059), not by anything like QPointer. A stranger looking for a QPointer guard on `state` will not find one. Fix: say "a shared_ptr per round; the generation counter discards older rounds".

Quote: `// QPointer-style ownership inside the runProbes lambda.`

### Row 1185 — Low — `src/diffviewer.cpp:102`

review-readability, lane 42-app-entry-dialogs, Q4. **Disposition:** queued, ANTS-6545.

A size that changes and has probably gone stale: `ROADMAP.md` now has matches past line 94,800. I did not measure its bytes. Fix: drop the figure ("multi-MiB files such as ROADMAP.md").

Quote: `// includes multi-MiB files (this repo's own ROADMAP.md is ~2.4 MiB), and the`

### Row 1186 — Low — `src/antshelper.cpp:81-82`

review-readability, lane 42-app-entry-dialogs, Q4. **Disposition:** queued under ANTS-6146.

This says traversal *substrings* are refused. The ANTS-1649 paragraph six lines below, and the code, refuse `..` only as a whole path component. The next paragraph corrects it, which is why this is LOW. Fix: change "substrings" to "a `..` path component".

Quote: `// not a co-UID local attacker. Refuse traversal substrings and`

### Row 1187 — Low — `src/antshelpermain.cpp:8`

review-readability, lane 42-app-entry-dialogs, Q4. **Disposition:** queued, ANTS-6545.

The parser (130-136) takes the first argument other than `--repo-root <x>`, flags included. A stray `--foo`, or a trailing `--repo-root` with no value, becomes the JSON body. Fix: say "the first argument that is not `--repo-root <path>`".

Quote: `// Reads optional JSON request from argv (first non-flag positional`

### Row 1192 — Low — `src/buildcache.h:22`

review-readability, lane 43-diagnostics-logging, Q4. **Disposition:** queued under ANTS-6147.

`appendOrFoldError` (buildcache.cpp:75-90) only adds errors to `errors[]`. Warnings are only counted. A stranger writing a consumer would filter `errors[]` for warnings that are never there.

Quote: `QString severity; // "error" or "warning" (note continuations folded into parent error)`

Remedy: say that `errors[]` entries are always "error".

### Row 1193 — Low — `src/buildcache.h:61`

review-readability, lane 43-diagnostics-logging, Q4. **Disposition:** queued, ANTS-6556.

docs/specs/ANTS-1299.md has no § 3.3.2. Its headings are § 3.3 with `op=read` subsections, and the probe set is described at about lines 163-168 and in § 4 row 16. A stranger following the pointer gets stuck.

Quote: `// instead of stale:false. The probed set is per § 3.3.2 step 3`

Remedy: point at § 3.3 `op=read` step 3, or § 4 invariant 16.

### Row 1196 — Low — `src/readlog.h:42-44`

review-readability, lane 43-diagnostics-logging, Q4. **Disposition:** queued, ANTS-6556.

The envelope list leaves out `lines_oversize_skipped`, which readlog.cpp:343 emits. ROADMAP shows the spec's own copy of this list was fixed for the same omission on 2026-10-19; this one was not. A caller written from this header misses a field.

Quote: `// {ok:true, path, lines[], matched, scanned, returned, truncated,`

Remedy: add `lines_oversize_skipped?` to the list.

### Row 1197 — Low — `src/debuglog.h:57`

review-readability, lane 43-diagnostics-logging, Q4. **Disposition:** queued, ANTS-6556.

The file is created when a category is switched on (`setActive` → `openLogFileLocked`), not on first write.

Quote: `// Log file path (created on first write). Fixed location:`

Remedy: "created when a category is first enabled".

### Row 1198 — Low — `src/debuglog.h:64`

review-readability, lane 43-diagnostics-logging, Q4. **Disposition:** queued under ANTS-6147.

`StallGuard` in the same header (line 118), `ANTS_LOG_ALWAYS`, and dialogshowtracer.cpp all call `write` directly. A stranger would think those sites are bugs.

Quote: `// Write one line. Do not call directly; use ANTS_LOG.`

Remedy: say direct calls must check `enabled()` first, or name the allowed callers.

### Row 1200 — Low — `src/debuglog.cpp:17-18`

review-readability, lane 43-diagnostics-logging, Q4. **Disposition:** queued, ANTS-6556.

This says the cap is checked when the file opens. Since ANTS-5110 it is also checked after every write (line 200). The comment at line 197 puts a reader right, hence LOW.

Quote: `// ANTS-1170: cap on-disk debug log size. When a fresh open() finds an`

Remedy: state both triggers at the constant.

### Row 1201 — Low — `src/debuglog.cpp:47`

review-readability, lane 43-diagnostics-logging, Q4. **Disposition:** queued, ANTS-6556.

This table and the `Category` enum in debuglog.h are two hand-kept lists of one set. Both hold the same 17 entries today. Nothing tells someone adding a category that it needs a table row, and without one the category is missing from the menu and logs as "?".

Quote: `} kCategoryTable[] = {`

Remedy: a comment on the enum naming the table.

### Row 1202 — Low — `src/pathvalidation.cpp:249`

review-readability, lane 44-shared-utilities, Q4. **Disposition:** queued under ANTS-6148.

A stranger who trusts "~14 call-sites" and "all call-sites pass non-empty roots" (line 251) would think they could audit every caller by hand. There are now about 45.

Quote: `// would silently accept /etc/passwd if any one of its ~14 call-sites`

Remedy: drop the count and say "any call-site".

### Row 1203 — Low — `src/pathvalidation.cpp:44-58`

review-readability, lane 44-shared-utilities, Q4. **Disposition:** queued, ANTS-6575.

`makeErr` is mostly comment, and lines 44-58 and 99-117 mix history with the rule ("measured 2026-08-19", "found the working form … by guessing", "the existing ANTS-4421 contract test is what caught this"). Nowhere does it say that the three hint branches have a fixed order: lane first, then the absolute-path handover, then the feedback file. A stranger adding a fourth hint, or reordering one, ha…

Quote: `// ANTS-4569 — the \lane\ case, and only it. A lane is a search SCOPE`

Remedy: add one line stating the order the hints are tried in, and move the history paragraphs to the tickets.

### Row 1205 — Low — `src/gitwrap.cpp:28`

review-readability, lane 44-shared-utilities, Q4. **Disposition:** queued, ANTS-6575.

This comment, and `gitwrap.h:4` ("5 s SIGTERM + 200 ms grace SIGKILL … 4 KiB stderr cap"), repeat the constants' values. Whoever changes `kHardKillMs`, `kKillGraceMs` or `kStderrCapBytes` leaves two false comments behind.

Quote: `// ANTS-1250-INV-12: 5 s wall-clock kill, 200 ms grace then SIGKILL.`

Remedy: refer to the constants by name, not by value.

### Row 1206 — Low — `src/gitwrap.h:8`

review-readability, lane 44-shared-utilities, Q4. **Disposition:** queued under ANTS-6148.

`cmdGitState` is in `remotecontrol_state.cpp:43`, so a stranger following this pointer opens the wrong file. Line 2 also says the wrapper calls `QProcess::start("git", argv)`; since ANTS-5527 it goes through `HostExec::start`, which starts `flatpak-spawn` inside a Flatpak.

Quote: `// JSON. cmdGitState (in remotecontrol.cpp) builds the argv, calls`

Remedy: name `remotecontrol_state.cpp` and mention the HostExec route.

### Row 1207 — Low — `src/gitwrap.h:80`

review-readability, lane 44-shared-utilities, Q4. **Disposition:** queued under ANTS-6148.

For a deleted file, `+++ /dev/null` is skipped (`gitwrap.cpp:108`), so `path` holds the pre-image path from `--- a/…`. A caller trusting "post-image" would look up a file that no longer exists.

Quote: `QString path; // repo-relative post-image path`

Remedy: say "post-image path, or the pre-image path for a deletion".

### Row 1209 — Low — `src/subsystemmap.h:3-4`

review-readability, lane 44-shared-utilities, Q4. **Disposition:** queued under ANTS-6148.

The cache is a process-wide static (`subsystemmap.cpp:27`), not a member of RemoteControl, and codebaseindex.cpp, indiereviewengine.cpp and remotecontrol_review.cpp use the same functions. A stranger would assume each RemoteControl has its own cache. The cpp comment at lines 23-26 corrects this, hence LOW.

Quote: `// file's mtime (no wall-clock TTL — INV-2). One copy lives on`

Remedy: say "one process-wide cache, keyed by path".

### Row 1210 — Low — `src/subsystemmap.h:7`

review-readability, lane 44-shared-utilities, Q4. **Disposition:** queued, ANTS-6575.

docs/subsystems.md is now 616 lines. I did not measure how much of that is the catalogue.

Quote: `// docs/subsystems.md so the ~130-line lane catalogue is not reloaded`

Remedy: drop the count.

### Row 1212 — Low — `src/treewatcher.h:45`

review-readability, lane 44-shared-utilities, Q4. **Disposition:** queued under ANTS-6148.

There is no `maxWatches` parameter or setter. The cap is the private `m_maxWatches = 20000` (line 91). A stranger looks for a knob to set and finds none.

Quote: `// changed(). Silently stops adding once \maxWatches\ is reached (degrade,`

Remedy: name `m_maxWatches` and say it is fixed.

### Row 1213 — Low — `src/guithread.h:106-107`

review-readability, lane 44-shared-utilities, Q4. **Disposition:** queued, ANTS-6575.

It continues "refused, its own release it without running their callables". The sentence is garbled, and it is the one that explains why the join loop cannot deadlock. A stranger changing this loop has to reverse-engineer what it meant.

Quote: `// returns. Between short waits, deliver the posted marshals: with \worker\`

Remedy: rewrite it as "with `worker` refused, its marshals return without running their callables".

### Row 1217 — Medium — `CMakeLists.txt:1670`

review-readability, lane 45-build-system, Q4. **Disposition:** queued, ANTS-6548.

Both copies of this comment say a single test cannot have its own timeout. They do not mention that `tests/slow_test_timeouts.cmake` (ANTS-3658, wired in at 4764–4769) is exactly that mechanism. Someone with one slow test would raise the bundle-wide `TIMEOUT 60` for every test.

Quote: `# being sanitized. Per-test overrides are not available here —`

Remedy: point the comment at `tests/slow_test_timeouts.cmake`.

### Row 1218 — Medium — `CMakeLists.txt:30`

review-readability, lane 45-build-system, Q4. **Disposition:** queued, ANTS-6548.

The build-info values compile into `ants_mcpcore_lib` (line 868), not core. More comments still describe the pre-ANTS-4932 layout: - 964–965, 1016 and 1122–1123 say "compiled into core". - 1013–1017 sits under `ants_claude_lib` and talks about `claudeintegration.cpp`, but that class's sources are in mcpcore (line 798). - 701, 748, 3474, 4305, 4319, 4346, 4351 and 4355 put `specparse`, `buildcache…

Quote: `# generated build_info_values.cpp compiled into ants_core_lib. Previously`

Remedy: replace "ants_core_lib" / "core" with `ants_mcpcore_lib` where the source now lives.

### Row 1219 — Medium — `CMakeLists.txt:420`

review-readability, lane 45-build-system, Q4. **Disposition:** queued, ANTS-6548.

This overview and the layering map below it (427–435) leave out `ants_mcpcore_lib`, `ants_roadmapparse_lib`, `ants_roadmapstore_lib`, `ants_treesitter_lib` and `ants_trace_lib`. It says core holds "config / debuglog", but `config.cpp` and `debuglog.cpp` are in mcpcore (599–600). It says audit "depends on core", but it links mcpcore (1048). This is the map a newcomer reads first to decide where a …

Quote: `# Six mandatory + two conditional STATIC libraries. Each src/*.cpp compiles`

Remedy: redraw the map from the current `add_library` blocks, without the count.

### Row 1224 — Low — `CMakeLists.txt:4172`

review-readability, lane 45-build-system, Q4. **Disposition:** queued, ANTS-6548.

The bundle timeout is 60 s (1681), not 10 s. The comment also sits orphaned and indented after test_lua's `set_property`.

Quote: `# the bundle's gtest_discover_tests default TIMEOUT 10s. Pre-fix`

Remedy: correct it to 60 s, or delete it.

### Row 1225 — Low — `CMakeLists.txt:3463`

review-readability, lane 45-build-system, Q4. **Disposition:** queued, ANTS-6548.

The `ANTS_RC_SOURCES_REL` list holds 25 TUs. The same paragraph is pasted at 3872, 4080 and 4548, and "eleven"/"twelfth" recurs at 4220–4222 and "TU N/12" at 519. Fixing one copy leaves three stale.

Quote: `# ANTS-3833 — the RemoteControl implementation is eleven TUs. The two`

Remedy: drop the count, since the list is the count.

### Row 1226 — Low — `CMakeLists.txt:1640`

review-readability, lane 45-build-system, Q4. **Disposition:** queued, ANTS-6548.

This contradicts line 1657 in the same function ("The bundles now hold hundreds of TUs").

Quote: `# errors. Test bundles compile fast already (≤ 4 TUs each) so`

Remedy: delete the size claim and keep the redefinition reason.

### Row 1227 — Low — `CMakeLists.txt:973`

review-readability, lane 45-build-system, Q4. **Disposition:** queued, ANTS-6548.

`rebuildCommandPalette` is now defined at `mainwindow.cpp:3298` and called at 484, 490 and 640. The paragraph is history ("Phase 1 scope"). Searched: `src/` for `rebuildCommandPalette`.

Quote: `# (e.g. rebuildCommandPalette() at line 411). Pre-1217 the define was`

Remedy: drop the line number and the history; keep the reason the define is PUBLIC.

### Row 1228 — Low — `CMakeLists.txt:2320`

review-readability, lane 45-build-system, Q4. **Disposition:** queued, ANTS-6548.

This invites target-wide defines, which the ANTS-4797 rule 13 lines below forbids.

Quote: `# had it. Apply target-wide so any future bundle source can reuse.`

Remedy: delete the second sentence.

### Row 1229 — Low — `CMakeLists.txt:3941`

review-readability, lane 45-build-system, Q4. **Disposition:** queued, ANTS-6548.

test_dialogs now has about 50 sources. Other stale counts: - 341: "EIGHT executables"; there are now far more linked targets, including about 14 benches. - 1632: "(≤ 7)" archives; test_claude passes 9. - 1169: "opt these four OUT"; the list at 1176 has five or six. Fix: drop the counts.

Quote: `# roadmapdialog + diffviewer (the dialogs hosted by chrome). 11 tests`

### Row 1230 — Low — `CMakeLists.txt:1598`

review-readability, lane 45-build-system, Q4. **Disposition:** queued, ANTS-6548.

The JOB_POOLS cap is set above, at 347.

Quote: `# compile_pool=max(2, nproc/4) JOB_POOLS cap below, a default`

Remedy: "above".

### Row 1232 — Low — `cmake/GenerateBuildInfoValues.cmake:23`

review-readability, lane 45-build-system, Q4. **Disposition:** queued, ANTS-6548.

`string(TIMESTAMP)` reads SOURCE_DATE_EPOCH itself, in both branches. This branch only switches to UTC, so a reader would wrongly think the `else()` ignores the variable. Line 11 also says SOURCE_DIR is used for the "template path", but that comes from TEMPLATE_FILE.

Quote: `# Honor SOURCE_DATE_EPOCH for reproducible builds (distro packagers).`

Remedy: "Use UTC when SOURCE_DATE_EPOCH is set", and drop "+ template path".

### Row 1233 — Low — `cmake/build_info_values.cpp.in:4`

review-readability, lane 45-build-system, Q4. **Disposition:** queued, ANTS-6548.

The comment names `remotecontrol.cpp` as a reader. The readers are now `remotecontrol_state.cpp`, `remotecontrol_feedback.cpp`, `claudeintegration.cpp`, `mainwindow.cpp`, `aboutdialogs.cpp` and `mcpdversion.cpp`; `remotecontrol.cpp` reads none of them. Searched: `src/` for `ANTS_BUILD_(DATE|TIME|COMMIT|TYPE)`.

Quote: `// changed, so ONLY this object file recompiles — never the ~1 MB TUs`

Remedy: say "the TUs that read these symbols" without naming or sizing them.

### Row 1242 — Medium — `tools/perf-report.sh:23`

review-readability, lane 46-tools-ci-gate, Q4. **Disposition:** queued under ANTS-6150.

The code says the opposite. Lines 91-100 find benchmarks from the source tree (`tests/perf/bench_*.cpp`, top level only), "not from ctest". The `--help` output (`sed -n '2,24p'`) prints this false line, cut off mid-paragraph. A stranger adding a benchmark would label it in ctest and expect it to be found. It is not found unless the file is `tests/perf/bench_<name>.cpp` with an executable at `$BUI…

Quote: `# HOW IT FINDS WORK. Benchmarks are discovered from ctest's perf label, and`

Remedy: rewrite the header to say discovery is by source file, and end the help range on a paragraph boundary. Cost: one edit (comment only).

### Row 1243 — Medium — `tools/check-roadmap.sh:15`

review-readability, lane 46-tools-ci-gate, Q4. **Disposition:** queued, ANTS-6581.

The `--allow` IDs are matched against the full bracket-stripped ID (for example `ANTS-1118`, line 91). "No `ANTS-` prefix expected" reads as "pass `1118`", and that silently fails to exempt anything.

Quote: ` # ANTS- prefix expected — pass them verbatim). The default allowlist `

Remedy: say the IDs are full IDs without brackets, for example `ANTS-1118`. Cost: one edit (comment only).

### Row 1244 — Medium — `tools/check-standard-mirrors.sh:213`

review-readability, lane 46-tools-ci-gate, Q4. **Disposition:** queued, ANTS-6581.

Lines 129-132 set the current rule: `owner_body` now turns such links into plain text, so a non-zero count "means owner_body needs widening". The message still tells the reader this result is expected and correct, which was the earlier ANTS-4825 behaviour. A stranger seeing it would accept a real gap in `owner_body`.

Quote: ` echo " They do NOT resolve for a reader on GitHub — doc_integrity reports them, correctly:$unmirrorable_list" `

Remedy: change the message to say the de-link missed a link shape and `owner_body` needs widening. Cost: one edit (message text only).

### Row 1246 — Low — `tools/ci_workflow.py:11`

review-readability, lane 46-tools-ci-gate, Q4. **Disposition:** queued, ANTS-6581.

There are eight tables below (`ACTIONS`, `RUNNER_ENV`, `RUNNER_UNSET`, `WORKFLOW_KEYS`, `JOB_KEYS`, `JOB_CONDITIONS`, `STEP_KEYS`, `STEP_CONDITIONS`). A stranger looking for "the three" cannot tell which ones the refusal rule depends on.

Quote: ` provides and this machine does not is mapped by the three tables below, and `

Remedy: drop the count, or name the tables. Cost: one edit (comment only).

### Row 1248 — Low — `tools/qt62-guard.sh:318`

review-readability, lane 46-tools-ci-gate, Q4. **Disposition:** queued, ANTS-6581.

This comment heads both branches, but the `--run-job` branch mounts the source read-write (line 356, `-v "$PWD:/src"`). Only the header, about 240 lines up, says so.

Quote: `# Source is bind-mounted READ-ONLY, so a stray write fails loudly and the host`

Remedy: scope the comment to the compile branch. Cost: one edit (comment only).

### Row 1249 — Low — `tools/local-ci.sh:8`

review-readability, lane 46-tools-ci-gate, Q4. **Disposition:** queued, ANTS-6581.

`setup-git-hooks.sh` now sets only `core.hooksPath` and deletes any per-repo `ants.gate.*` keys. Its own comment says the options are committed in `.ants/gate.conf`, and `tools/hooks/pre-push:4` agrees. A stranger would look for gate options in the wrong file.

Quote: `# tools/setup-git-hooks.sh sets the hook's per-repo options.`

Remedy: point the comment at `.ants/gate.conf`. Cost: one edit (comment only).

### Row 1250 — Low — `tools/check-standard-mirrors.sh:7`

review-readability, lane 46-tools-ci-gate, Q4. **Disposition:** queued, ANTS-6581.

13 files under docs/standards/ now carry `MIRROR BEGIN` (searched; `README.md`'s single hit is not counted). "Four" is out of date.

Quote: `# WHY a mirror exists at all: this repo is PUBLIC, and the four delta standards`

Remedy: drop the count. Cost: one edit (comment only).

### Row 1251 — Low — `tools/hooks/pre-commit:10`

review-readability, lane 46-tools-ci-gate, Q4. **Disposition:** queued, ANTS-6581.

The script it describes says it "SKIPS loudly" (`check-standard-mirrors.sh:25`) and prints a `SKIP` line per file (line 87).

Quote: ` # this repo does not carry. It skips silently where there is no owner tree to `

Remedy: change "silently" to "loudly". Cost: one edit (comment only).

### Row 1252 — Low — `tools/e2e/run.sh:2`

review-readability, lane 46-tools-ci-gate, Q4. **Disposition:** queued under ANTS-6155.

`cases.sh:23` sources it too. A stranger changing the harness would check only smoke.sh's use.

Quote: `# ANTS-2049 — e2e harness driver (sourced by smoke.sh, not executed directly).`

Remedy: name both callers, or say "sourced by the e2e suites". Cost: one edit (comment only).

### Row 1253 — Low — `tools/install-hooks.sh:184`

review-readability, lane 46-tools-ci-gate, Q4. **Disposition:** queued, ANTS-6581.

The dry run announces the very wildcard that lines 122-124 say uninstall must not use. The real removal (line 191) deletes only names taken from the source directory. A reader of the dry run would think other tools' `ants-*` scripts are at risk.

Quote: `echo "[dry-run] would unlink hook scripts in: $hooks_dir/ants-*.sh"`

Remedy: list the exact file names, as the install dry run does at line 222. Cost: one edit (message only).

### Row 1258 — Low — `tools/roadmap-backup-lib.sh:7`

review-readability, lane 47-tools-roadmap-surveys, Q4. **Disposition:** queued, ANTS-6582.

The list below names five keys, and line 43 writes five for the snapshot job. A stranger writing a reader would trust "always all three". The "snapshot job only" notes just below partly correct it.

Quote: `# The record is three key=value lines, always all three:`

Remedy: "three lines; five for the snapshot job".

### Row 1259 — Low — `tools/roadmap-corpus-survey.py:304`

review-readability, lane 47-tools-roadmap-surveys, Q4. **Disposition:** queued, ANTS-6582.

An empty value is rejected as prose at 171-175 and never reaches `kinds`, so this label can never print. It also suggests empty values are tallied here. The rejection line (306-307) says "(over N words or N chars)", but that count also includes the empty captures.

Quote: `print(f" {k or '(empty — value ran onto the next line)':22s} {v}")`

Remedy: drop the empty label, and word the rejection line as "empty or over …".

### Row 1260 — Low — `tools/roadmap-conformance.py:130`

review-readability, lane 47-tools-roadmap-surveys, Q4. **Disposition:** queued, ANTS-6582.

Seven lines of history about removed code sit inside the live loop, and `bold_headline`'s docstring repeats it. A stranger has to read past history to find the current rule.

Quote: `# A parser_truncates finding lived here: it compared a naive match`

Remedy: move it to the commit body or a history note.

### Row 1261 — Low — `tools/mcp-arg-usage-survey.py:13`

review-readability, lane 47-tools-roadmap-surveys, Q4. **Disposition:** queued, ANTS-6582.

The code (line 33) globs `**/*.jsonl` recursively. A stranger narrowing the scan would trust the one-level glob.

Quote: `Transcripts are read from ~/.claude/projects/*/*.jsonl (subagent transcripts`

Remedy: state the recursive glob.

### Row 1268 — Low — `tools/roadmap-roundtrip-diff.py:37`

review-readability, lane 47-tools-roadmap-surveys, Q4. **Disposition:** queued, ANTS-6582.

Correction history sits in the column comment, and the docstring repeats it (lines 7-8).

Quote: `# Corrected 2026-08-15: this cited "ANTS-3765 § 2.4", which is that spec's`

Remedy: move it to the commit body.

### Row 1269 — Low — `tools/quote-displacement-survey.py:10`

review-readability, lane 47-tools-roadmap-surveys, Q4. **Disposition:** queued, ANTS-6582.

Line 48 does normalise: it cuts the quotation at `…`/`...`, and a 12-character minimum appears twice (26, 49) with no reason given.

Quote: `Single lines only, with no normalisation, so a quotation that wraps or carries`

Remedy: state the ellipsis cut and the minimum in the docstring.

### Row 1271 — Medium — `packaging/flatpak/za.co.antsprojectshub.AntsTerminal.yml:12`

review-readability, lane 48-hooks-packaging, Q4. **Disposition:** queued, ANTS-6559.

The real runtime is `runtime-version: '6.11'` (line 41). Lines 14-15 ("Qt6.10 also clears…", "only 6.9/6.10 remain") are stale as well. Someone bumping or checking the runtime would trust 6.10, or "fix" line 41 back to it. Line 41 is close enough to correct them, so this is not HIGH.

Quote: `# Targets org.kde.Platform//6.10 — the KDE SDK brings cmake, ninja, and all`

Remedy: say 6.11 and drop the bump-history sentence.

### Row 1276 — Medium — `packaging/opensuse/ants-terminal.spec:317`

review-readability, lane 48-hooks-packaging, Q4. **Disposition:** queued, ANTS-6559.

The code at line 363 runs `ctest … -LE perf`, so perf is excluded. This paragraph, and the `%%ctest cannot take -LE` reasoning at 321-324, are history from before ANTS-4720. The current rule only appears 23 lines later (line 340). The header at lines 21-22 also lists `%%ctest` among the macros used, but it is no longer used. Someone reading top-down would believe perf runs here.

Quote: `# The whole suite, perf and e2e lanes included. Two things had to be checked`

Remedy: lead with the ANTS-4720 rule and move the superseded paragraph to history or delete it.

### Row 1277 — Medium — `packaging/opensuse/ants-terminal.spec:139`

review-readability, lane 48-hooks-packaging, Q4. **Disposition:** queued, ANTS-6559.

Line-number pointers into other files have gone stale. CMakeLists.txt lines 84-93 are a comment about the Qt floor and Sql, not a `find_package` or `pkg_check_modules(LUA…)`. Lines 775-779, 804-808 and 989-993 are unrelated source lists. So `:92`, `:91` (line 167), `:86-91` (line 218), `:777` (line 400), `:806` (line 144) and `:991` (line 291) all point at the wrong code. Someone following them t…

Quote: `# CMakeLists.txt:92 — unconditionally, not gated on -DANTS_TESTS. Omitting it`

Remedy: name the `find_package`/`install()` call instead of a line number.

### Row 1278 — Medium — `hooks/ants-precompact-snapshot.sh:6`

review-readability, lane 48-hooks-packaging, Q4. **Disposition:** queued, ANTS-6559.

The script never reads `$CLAUDE_TRANSCRIPT_PATH`; it uses only the payload's `transcript_path` (line 31). It also extracts no "in-progress spec path" (line 8), only todos. Someone testing or debugging by setting that variable would lose time.

Quote: `# Reads transcript via $CLAUDE_TRANSCRIPT_PATH (or falls back to the`

Remedy: describe only the payload field and the todos.

### Row 1283 — Low — `hooks/_common.sh:6`

review-readability, lane 48-hooks-packaging, Q4. **Disposition:** queued, ANTS-6559.

The file now defines eight functions; the header lists two. Someone looking for the shared helpers would stop at the header.

Quote: `# Two surfaces:`

Remedy: list all of them, or drop the count.

### Row 1284 — Low — `hooks/_common.sh:45`

review-readability, lane 48-hooks-packaging, Q4. **Disposition:** queued, ANTS-6559.

The code is a `grep -E` regex, not a bash glob. The "second test" that enforces length does not exist; `{1,64}` in the same regex does it.

Quote: `# POSIX-ish bracket pattern; bash glob is good enough here. Length`

Remedy: say the regex enforces both charset and length.

### Row 1285 — Low — `hooks/ants-bash-veto.sh:30`

review-readability, lane 48-hooks-packaging, Q4. **Disposition:** queued, ANTS-6559.

Nothing is stripped; line 34 `exit 0`s on the token anywhere in the command, not just "trailing". Someone looking for the strip would not find it.

Quote: `# the override from hooks/README.md. Strip the comment before any`

Remedy: say the hook exits on the token anywhere.

### Row 1286 — Low — `hooks/ants-drift-check.sh:37`

review-readability, lane 48-hooks-packaging, Q4. **Disposition:** queued, ANTS-6559.

A string `case` on `$project_root` cannot detect a symlink moved on disk; the variable cannot change between the gate and here. The comment claims a protection the code does not give.

Quote: `# the destructive step — re-checking here closes the race where a`

Remedy: call it a defensive re-check of the same rule.

### Row 1287 — Low — `launch.sh:17`

review-readability, lane 48-hooks-packaging, Q4. **Disposition:** queued, ANTS-6559.

`src/remotecontrol.cpp` ends before line 8124 (a read from 8124 returned no lines), so the pointer leads nowhere.

Quote: `# cwd, not from the binary's location (see remotecontrol.cpp:8129); all`

Remedy: name the function instead of a line.

### Row 1288 — Low — `packaging/check-version-drift.sh:57`

review-readability, lane 48-hooks-packaging, Q4. **Disposition:** queued, ANTS-6559.

It points at `CLAUDE.md "Key Design Decisions"`, but a search finds that phrase only in this file. Someone told to keep two lists in sync cannot find the other. Line 12's `memory: audit_2026_04_15.md` also names a file that is not in the repo.

Quote: `# Packaging files — keep in sync with the release checklist in`

Remedy: point at the checklist's real home, or drop the pointer.

### Row 1289 — Low — `packaging/debian/rules:6`

review-readability, lane 48-hooks-packaging, Q4. **Disposition:** queued, ANTS-6559.

`override_dh_auto_test` (line 21) is a second override.

Quote: `# via the CMake buildsystem. We only override the configure step to`

Remedy: drop "only".

### Row 1290 — Low — `packaging/obs/obs-status.sh:5`

review-readability, lane 48-hooks-packaging, Q4. **Disposition:** queued, ANTS-6559.

The exit-code table leaves out two cases. Usage errors exit 2 (line 37), the same code as "still building". "osc not installed" exits 1, the same code as "a build failed". A caller branching on these codes would misread both.

Quote: `# Run after obs-submit.sh. Exit 0 = every repository built it. 1 = one failed,`

Remedy: list the usage and missing-osc codes, or give them distinct codes.

### Row 1487 — Low — `CMakeLists.txt:252`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1217 is not in the roadmap

Quote: `ANTS-1217`

### Row 1488 — Low — `CMakeLists.txt:374`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1217 is not in the roadmap

Quote: `ANTS-1217`

### Row 1489 — Low — `CMakeLists.txt:418`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1217 is not in the roadmap

Quote: `ANTS-1217`

### Row 1490 — Low — `CMakeLists.txt:440`

review-readability, lane comment_claims, comment_claims:missing_quotation. **Disposition:** queued, ANTS-6569.

src/roadmaprender.h does not contain this text

Quote: `because ANTS-3794 will call it from a headless publish path`

### Row 1491 — Low — `CMakeLists.txt:1190`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1217 is not in the roadmap

Quote: `ANTS-1217`

### Row 1492 — Low — `CMakeLists.txt:1594`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1217 is not in the roadmap

Quote: `ANTS-1217`

### Row 1493 — Low — `CMakeLists.txt:1606`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1217 is not in the roadmap

Quote: `ANTS-1217`

### Row 1494 — Low — `CMakeLists.txt:1653`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1373 is not in the roadmap

Quote: `ANTS-1373`

### Row 1495 — Low — `CMakeLists.txt:2228`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1217 is not in the roadmap

Quote: `ANTS-1217`

### Row 1496 — Low — `CMakeLists.txt:2363`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1217 is not in the roadmap

Quote: `ANTS-1217`

### Row 1497 — Low — `CMakeLists.txt:2667`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1217 is not in the roadmap

Quote: `ANTS-1217`

### Row 1498 — Low — `CMakeLists.txt:3722`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1217 is not in the roadmap

Quote: `ANTS-1217`

### Row 1499 — Low — `CMakeLists.txt:3870`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1372 is not in the roadmap

Quote: `ANTS-1372`

### Row 1500 — Low — `CMakeLists.txt:3940`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1217 is not in the roadmap

Quote: `ANTS-1217`

### Row 1501 — Low — `CMakeLists.txt:4129`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1217 is not in the roadmap

Quote: `ANTS-1217`

### Row 1502 — Low — `CMakeLists.txt:4170`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1332 is not in the roadmap

Quote: `ANTS-1332`

### Row 1503 — Low — `CMakeLists.txt:4177`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1217 is not in the roadmap

Quote: `ANTS-1217`

### Row 1504 — Low — `CMakeLists.txt:4373`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1217 is not in the roadmap

Quote: `ANTS-1217`

### Row 1505 — Low — `packaging/check-version-drift.sh:58`

review-readability, lane comment_claims, comment_claims:missing_quotation. **Disposition:** queued, ANTS-6569.

CLAUDE.md does not contain this text

Quote: `Key Design Decisions`

### Row 1506 — Low — `src/aboutdialogs.cpp:1`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6557.

ANTS-1181 is not in the roadmap

Quote: `ANTS-1181`

### Row 1507 — Low — `src/aboutdialogs.h:1`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6557.

ANTS-1181 is not in the roadmap

Quote: `ANTS-1181`

### Row 1508 — Low — `src/aidialog.cpp:139`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6568.

ANTS-1168 is not in the roadmap

Quote: `ANTS-1168`

### Row 1509 — Low — `src/aidialog.h:27`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6568.

ANTS-1168 is not in the roadmap

Quote: `ANTS-1168`

### Row 1510 — Low — `src/claudeallowlist.cpp:164`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6552.

ANTS-1168 is not in the roadmap

Quote: `ANTS-1168`

### Row 1511 — Low — `src/claudebgtasksdialog.cpp:56`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6552.

ANTS-1169 is not in the roadmap

Quote: `ANTS-1169`

### Row 1512 — Low — `src/claudeintegration.cpp:159`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6551.

ANTS-1151 is not in the roadmap

Quote: `ANTS-1151`

### Row 1513 — Low — `src/claudeintegration.cpp:189`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6551.

ANTS-1151 is not in the roadmap

Quote: `ANTS-1151`

### Row 1514 — Low — `src/claudeintegration.cpp:1370`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6551.

ANTS-1151 is not in the roadmap

Quote: `ANTS-1151`

### Row 1515 — Low — `src/claudeintegration.h:240`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6551.

ANTS-1168 is not in the roadmap

Quote: `ANTS-1168`

### Row 1516 — Low — `src/claudeintegration_sessions.cpp:125`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6551.

ANTS-1168 is not in the roadmap

Quote: `ANTS-1168`

### Row 1517 — Low — `src/claudeintegration_sessions.cpp:331`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6551.

ANTS-1168 is not in the roadmap

Quote: `ANTS-1168`

### Row 1518 — Low — `src/claudeintegration_sessions.cpp:804`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6551.

ANTS-1169 is not in the roadmap

Quote: `ANTS-1169`

### Row 1519 — Low — `src/claudetranscript.h:18`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6552.

ANTS-1168 is not in the roadmap

Quote: `ANTS-1168`

### Row 1520 — Low — `src/coloredtabbar.cpp:437`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6550.

ANTS-1185 is not in the roadmap

Quote: `ANTS-1185`

### Row 1521 — Low — `src/commentclaims.cpp:522`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6553.

ANTS-1 is not in the roadmap

Quote: `ANTS-1`

### Row 1522 — Low — `src/commentclaims.cpp:522`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6553.

ANTS-1 is not in the roadmap

Quote: `ANTS-1`

### Row 1523 — Low — `src/config.cpp:22`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6554.

ANTS-1169 is not in the roadmap

Quote: `ANTS-1169`

### Row 1524 — Low — `src/config.cpp:42`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6554.

ANTS-1183 is not in the roadmap

Quote: `ANTS-1183`

### Row 1525 — Low — `src/config.cpp:88`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6554.

ANTS-1183 is not in the roadmap

Quote: `ANTS-1183`

### Row 1526 — Low — `src/config.cpp:216`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6554.

ANTS-1183 is not in the roadmap

Quote: `ANTS-1183`

### Row 1527 — Low — `src/config.cpp:252`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6554.

ANTS-1179 is not in the roadmap

Quote: `ANTS-1179`

### Row 1528 — Low — `src/config.cpp:394`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6554.

ANTS-1179 is not in the roadmap

Quote: `ANTS-1179`

### Row 1529 — Low — `src/config.cpp:985`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6554.

ANTS-1165 is not in the roadmap

Quote: `ANTS-1165`

### Row 1530 — Low — `src/config.h:599`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6554.

ANTS-1183 is not in the roadmap

Quote: `ANTS-1183`

### Row 1531 — Low — `src/debuglog.cpp:17`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6556.

ANTS-1170 is not in the roadmap

Quote: `ANTS-1170`

### Row 1532 — Low — `src/debuglog.cpp:95`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6556.

ANTS-1170 is not in the roadmap

Quote: `ANTS-1170`

### Row 1533 — Low — `src/debuglog.h:145`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6556.

ANTS-1367 is not in the roadmap

Quote: `ANTS-1367`

### Row 1534 — Low — `src/featurecoverage.h:123`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6577.

FIBR-0042 is not in the roadmap

Quote: `FIBR-0042`

### Row 1535 — Low — `src/featurecoverage.h:123`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6577.

FIBR-0042 is not in the roadmap

Quote: `FIBR-0042`

### Row 1536 — Low — `src/globalshortcutsportal.cpp:77`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6550.

ANTS-1152 is not in the roadmap

Quote: `ANTS-1152`

### Row 1537 — Low — `src/globalshortcutsportal.cpp:85`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6550.

ANTS-1152 is not in the roadmap

Quote: `ANTS-1152`

### Row 1538 — Low — `src/globalshortcutsportal.cpp:119`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6550.

ANTS-1152 is not in the roadmap

Quote: `ANTS-1152`

### Row 1539 — Low — `src/globalshortcutsportal.cpp:144`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6550.

ANTS-1152 is not in the roadmap

Quote: `ANTS-1152`

### Row 1540 — Low — `src/globalshortcutsportal.cpp:223`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6550.

ANTS-1152 is not in the roadmap

Quote: `ANTS-1152`

### Row 1541 — Low — `src/globalshortcutsportal.cpp:238`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6550.

ANTS-1152 is not in the roadmap

Quote: `ANTS-1152`

### Row 1542 — Low — `src/globalshortcutsportal.cpp:244`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6550.

ANTS-1152 is not in the roadmap

Quote: `ANTS-1152`

### Row 1543 — Low — `src/globalshortcutsportal.cpp:325`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6550.

ANTS-1152 is not in the roadmap

Quote: `ANTS-1152`

### Row 1544 — Low — `src/globalshortcutsportal.cpp:346`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6550.

ANTS-1152 is not in the roadmap

Quote: `ANTS-1152`

### Row 1545 — Low — `src/globalshortcutsportal.h:117`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6550.

ANTS-1152 is not in the roadmap

Quote: `ANTS-1152`

### Row 1546 — Low — `src/luaengine.cpp:228`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6570.

ANTS-1172 is not in the roadmap

Quote: `ANTS-1172`

### Row 1547 — Low — `src/luaengine.cpp:240`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6570.

ANTS-1332 is not in the roadmap

Quote: `ANTS-1332`

### Row 1548 — Low — `src/luaengine.cpp:272`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6570.

ANTS-1332 is not in the roadmap

Quote: `ANTS-1332`

### Row 1549 — Low — `src/luaengine.cpp:295`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6570.

ANTS-1332 is not in the roadmap

Quote: `ANTS-1332`

### Row 1550 — Low — `src/luaengine.cpp:307`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6570.

ANTS-1332 is not in the roadmap

Quote: `ANTS-1332`

### Row 1551 — Low — `src/luaengine.cpp:540`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6570.

ANTS-1332 is not in the roadmap

Quote: `ANTS-1332`

### Row 1552 — Low — `src/luaengine.h:26`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6570.

ANTS-1736 is not in the roadmap

Quote: `ANTS-1736`

### Row 1553 — Low — `src/luaengine.h:67`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6570.

ANTS-1736 is not in the roadmap

Quote: `ANTS-1736`

### Row 1554 — Low — `src/luaengine.h:236`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6570.

ANTS-1332 is not in the roadmap

Quote: `ANTS-1332`

### Row 1555 — Low — `src/luaengine.h:275`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6570.

ANTS-1172 is not in the roadmap

Quote: `ANTS-1172`

### Row 1556 — Low — `src/luaengine.h:306`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6570.

ANTS-1332 is not in the roadmap

Quote: `ANTS-1332`

### Row 1557 — Low — `src/main.cpp:257`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6545.

ANTS-1170 is not in the roadmap

Quote: `ANTS-1170`

### Row 1558 — Low — `src/main.cpp:425`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6545.

ANTS-1169 is not in the roadmap

Quote: `ANTS-1169`

### Row 1559 — Low — `src/mainwindow.cpp:9`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1325 is not in the roadmap

Quote: `ANTS-1325`

### Row 1560 — Low — `src/mainwindow.cpp:56`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1181 is not in the roadmap

Quote: `ANTS-1181`

### Row 1561 — Low — `src/mainwindow.cpp:1615`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1174 is not in the roadmap

Quote: `ANTS-1174`

### Row 1562 — Low — `src/mainwindow.cpp:1922`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1168 is not in the roadmap

Quote: `ANTS-1168`

### Row 1563 — Low — `src/mainwindow.cpp:3300`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1174 is not in the roadmap

Quote: `ANTS-1174`

### Row 1564 — Low — `src/mainwindow.h:122`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1181 is not in the roadmap

Quote: `ANTS-1181`

### Row 1565 — Low — `src/mainwindow.h:315`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1182 is not in the roadmap

Quote: `ANTS-1182`

### Row 1566 — Low — `src/mainwindow.h:333`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1174 is not in the roadmap

Quote: `ANTS-1174`

### Row 1567 — Low — `src/mainwindow_menus.cpp:10`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1325 is not in the roadmap

Quote: `ANTS-1325`

### Row 1568 — Low — `src/mainwindow_menus.cpp:20`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1181 is not in the roadmap

Quote: `ANTS-1181`

### Row 1569 — Low — `src/mainwindow_menus.cpp:45`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1181 is not in the roadmap

Quote: `ANTS-1181`

### Row 1570 — Low — `src/mainwindow_menus.cpp:452`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1168 is not in the roadmap

Quote: `ANTS-1168`

### Row 1571 — Low — `src/mainwindow_menus.cpp:538`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1168 is not in the roadmap

Quote: `ANTS-1168`

### Row 1572 — Low — `src/mainwindow_menus.cpp:844`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1165 is not in the roadmap

Quote: `ANTS-1165`

### Row 1573 — Low — `src/mainwindow_repo.cpp:6`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1325 is not in the roadmap

Quote: `ANTS-1325`

### Row 1574 — Low — `src/mainwindow_tabs.cpp:5`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1325 is not in the roadmap

Quote: `ANTS-1325`

### Row 1575 — Low — `src/mainwindow_tabs.cpp:81`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1182 is not in the roadmap

Quote: `ANTS-1182`

### Row 1576 — Low — `src/mainwindow_tabs.cpp:173`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1174 is not in the roadmap

Quote: `ANTS-1174`

### Row 1577 — Low — `src/mainwindow_tabs.cpp:190`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1174 is not in the roadmap

Quote: `ANTS-1174`

### Row 1578 — Low — `src/mainwindow_tabs.cpp:201`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6560.

ANTS-1182 is not in the roadmap

Quote: `ANTS-1182`

### Row 1579 — Low — `src/pluginmanager.cpp:115`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6570.

ANTS-1173 is not in the roadmap

Quote: `ANTS-1173`

### Row 1580 — Low — `src/pluginmanager.cpp:676`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6570.

ANTS-1736 is not in the roadmap

Quote: `ANTS-1736`

### Row 1581 — Low — `src/pluginmanager.h:57`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6570.

ANTS-1736 is not in the roadmap

Quote: `ANTS-1736`

### Row 1582 — Low — `src/ptyhandler.cpp:440`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6583.

ANTS-1164 is not in the roadmap

Quote: `ANTS-1164`

### Row 1583 — Low — `src/ptyhandler.cpp:667`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6583.

ANTS-1175 is not in the roadmap

Quote: `ANTS-1175`

### Row 1584 — Low — `src/remotecontrol.h:145`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6567.

ANTS-0 is not in the roadmap

Quote: `ANTS-0`

### Row 1585 — Low — `src/remotecontrol.h:1291`

review-readability, lane comment_claims, comment_claims:dead_reference. **Disposition:** queued, ANTS-6567.

no file or directory tests/features/roadmap_backfill_dates/spec.md under the project

Quote: `See tests/features/roadmap_backfill_dates/spec.md.`

### Row 1586 — Low — `src/remotecontrol_coldeyes.cpp:399`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6563.

ANTS-1372 is not in the roadmap

Quote: `ANTS-1372`

### Row 1587 — Low — `src/remotecontrol_feedback.cpp:30`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6561.

ANTS-12 is not in the roadmap

Quote: `ANTS-12`

### Row 1588 — Low — `src/remotecontrol_feedback.cpp:1418`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6561.

ANTS-1372 is not in the roadmap

Quote: `ANTS-1372`

### Row 1589 — Low — `src/remotecontrol_internal.h:423`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6567.

ANTS-119 is not in the roadmap

Quote: `ANTS-119`

### Row 1590 — Low — `src/remotecontrol_internal.h:443`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6567.

ANTS-0042 is not in the roadmap

Quote: `ANTS-0042`

### Row 1591 — Low — `src/remotecontrol_review.cpp:1106`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6563.

ANTS-1372 is not in the roadmap

Quote: `ANTS-1372`

### Row 1592 — Low — `src/remotecontrol_review.cpp:1122`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6563.

ANTS-1372 is not in the roadmap

Quote: `ANTS-1372`

### Row 1593 — Low — `src/remotecontrol_roadmap_log_batch.cpp:95`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6564.

ANTS-9999 is not in the roadmap

Quote: `ANTS-9999`

### Row 1594 — Low — `src/remotecontrol_roadmap_query.cpp:887`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6565.

ANTS-999 is not in the roadmap

Quote: `ANTS-999`

### Row 1595 — Low — `src/remotecontrol_roadmap_query.cpp:887`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6565.

ANTS-1000 is not in the roadmap

Quote: `ANTS-1000`

### Row 1596 — Low — `src/remotecontrol_roadmap_query.cpp:2284`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6565.

ANTS-9999 is not in the roadmap

Quote: `ANTS-9999`

### Row 1597 — Low — `src/remotecontrol_roadmap_query.cpp:2414`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6565.

ANTS-0042 is not in the roadmap

Quote: `ANTS-0042`

### Row 1598 — Low — `src/remotecontrol_state.cpp:135`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6566.

ANTS-1169 is not in the roadmap

Quote: `ANTS-1169`

### Row 1599 — Low — `src/remotecontrol_terminal.cpp:382`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6561.

ANTS-1176 is not in the roadmap

Quote: `ANTS-1176`

### Row 1600 — Low — `src/remotecontrolgate.cpp:1`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6567.

ANTS-1372 is not in the roadmap

Quote: `ANTS-1372`

### Row 1601 — Low — `src/remotecontrolgate.cpp:2`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6567.

ANTS-1372 is not in the roadmap

Quote: `ANTS-1372`

### Row 1602 — Low — `src/remotecontrolgate.h:1`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6567.

ANTS-1372 is not in the roadmap

Quote: `ANTS-1372`

### Row 1603 — Low — `src/remotecontrolgate.h:11`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6567.

ANTS-1372 is not in the roadmap

Quote: `ANTS-1372`

### Row 1604 — Low — `src/roadmapfoldin.cpp:49`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6573.

ANTS-1342 is not in the roadmap

Quote: `ANTS-1342`

### Row 1605 — Low — `src/roadmapfoldin.cpp:82`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6573.

ANTS-1342 is not in the roadmap

Quote: `ANTS-1342`

### Row 1606 — Low — `src/roadmapfoldin.cpp:517`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6573.

ANTS-406 is not in the roadmap

Quote: `ANTS-406`

### Row 1607 — Low — `src/roadmapfoldin.cpp:617`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6573.

ANTS-1342 is not in the roadmap

Quote: `ANTS-1342`

### Row 1608 — Low — `src/roadmapfoldin.cpp:1022`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6573.

ANTS-1342 is not in the roadmap

Quote: `ANTS-1342`

### Row 1609 — Low — `src/roadmapfoldin.h:77`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6573.

ANTS-9999 is not in the roadmap

Quote: `ANTS-9999`

### Row 1610 — Low — `src/roadmapmigrate.cpp:187`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6574.

ANTS-119 is not in the roadmap

Quote: `ANTS-119`

### Row 1611 — Low — `src/roadmapmigrate.cpp:206`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6574.

ANTS-0042 is not in the roadmap

Quote: `ANTS-0042`

### Row 1612 — Low — `src/roadmapmigrate.cpp:1307`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6574.

DEMO-0057 is not in the roadmap

Quote: `DEMO-0057`

### Row 1613 — Low — `src/roadmapparse.cpp:1377`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6573.

ANTS-9999 is not in the roadmap

Quote: `ANTS-9999`

### Row 1614 — Low — `src/roadmapparse.cpp:1401`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6573.

ANTS-9999 is not in the roadmap

Quote: `ANTS-9999`

### Row 1615 — Low — `src/roadmapstore.h:1123`

review-readability, lane comment_claims, comment_claims:dead_reference. **Disposition:** queued, ANTS-6574.

no definition of acked_at found

Quote: `see acked_at`

### Row 1616 — Low — `src/secretredact.h:144`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6562.

ANTS-1184 is not in the roadmap

Quote: `ANTS-1184`

### Row 1617 — Low — `src/secretredact.h:150`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6562.

ANTS-1184 is not in the roadmap

Quote: `ANTS-1184`

### Row 1618 — Low — `src/settingsdialog.cpp:676`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6545.

ANTS-1178 is not in the roadmap

Quote: `ANTS-1178`

### Row 1619 — Low — `src/settingsdialog.cpp:1320`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6545.

ANTS-1178 is not in the roadmap

Quote: `ANTS-1178`

### Row 1620 — Low — `src/speclint.cpp:667`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6577.

ANTS-1 is not in the roadmap

Quote: `ANTS-1`

### Row 1621 — Low — `src/speclint.cpp:668`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6577.

ANTS-12 is not in the roadmap

Quote: `ANTS-12`

### Row 1622 — Low — `src/sshdialog.cpp:284`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6545.

ANTS-1168 is not in the roadmap

Quote: `ANTS-1168`

### Row 1623 — Low — `src/sshdialog.h:76`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6545.

ANTS-1168 is not in the roadmap

Quote: `ANTS-1168`

### Row 1624 — Low — `src/terminalgrid.cpp:772`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6578.

ANTS-1196 is not in the roadmap

Quote: `ANTS-1196`

### Row 1625 — Low — `src/terminalgrid.cpp:855`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6578.

ANTS-1210 is not in the roadmap

Quote: `ANTS-1210`

### Row 1626 — Low — `src/terminalgrid.cpp:1430`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6578.

ANTS-1197 is not in the roadmap

Quote: `ANTS-1197`

### Row 1627 — Low — `src/terminalgrid.cpp:1809`

review-readability, lane comment_claims, comment_claims:dead_reference. **Disposition:** queued, ANTS-6578.

no file or directory ink/build/ink.js under the project

Quote: `see ink/build/ink.js:705).`

### Row 1628 — Low — `src/terminalgrid.cpp:2379`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6578.

ANTS-1169 is not in the roadmap

Quote: `ANTS-1169`

### Row 1629 — Low — `src/terminalgrid.cpp:3512`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6578.

ANTS-1166 is not in the roadmap

Quote: `ANTS-1166`

### Row 1630 — Low — `src/terminalgrid.cpp:3671`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6578.

ANTS-1169 is not in the roadmap

Quote: `ANTS-1169`

### Row 1631 — Low — `src/terminalgrid.h:138`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6578.

ANTS-1166 is not in the roadmap

Quote: `ANTS-1166`

### Row 1632 — Low — `src/terminalgrid.h:178`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6578.

ANTS-1169 is not in the roadmap

Quote: `ANTS-1169`

### Row 1633 — Low — `src/terminalgrid.h:203`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6578.

ANTS-1196 is not in the roadmap

Quote: `ANTS-1196`

### Row 1634 — Low — `src/terminalgrid.h:809`

review-readability, lane comment_claims, comment_claims:dead_reference. **Disposition:** queued, ANTS-6578.

no file or directory ink/build/ink.js under the project

Quote: `see ink/build/ink.js:700`

### Row 1635 — Low — `src/terminalgrid.h:968`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6578.

ANTS-1169 is not in the roadmap

Quote: `ANTS-1169`

### Row 1636 — Low — `src/terminalwidget.cpp:160`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6579.

ANTS-1195 is not in the roadmap

Quote: `ANTS-1195`

### Row 1637 — Low — `src/terminalwidget.cpp:1085`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6579.

ANTS-1180 is not in the roadmap

Quote: `ANTS-1180`

### Row 1638 — Low — `src/terminalwidget.cpp:1170`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6579.

ANTS-1180 is not in the roadmap

Quote: `ANTS-1180`

### Row 1639 — Low — `src/terminalwidget.cpp:1348`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6579.

ANTS-1180 is not in the roadmap

Quote: `ANTS-1180`

### Row 1640 — Low — `src/terminalwidget.cpp:3458`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6579.

ANTS-1199 is not in the roadmap

Quote: `ANTS-1199`

### Row 1641 — Low — `src/terminalwidget.cpp:6224`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6579.

ANTS-1211 is not in the roadmap

Quote: `ANTS-1211`

### Row 1642 — Low — `src/terminalwidget.cpp:6281`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6579.

ANTS-1198 is not in the roadmap

Quote: `ANTS-1198`

### Row 1643 — Low — `src/terminalwidget.cpp:6300`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6579.

ANTS-1198 is not in the roadmap

Quote: `ANTS-1198`

### Row 1644 — Low — `src/terminalwidget.cpp:6312`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6579.

ANTS-1198 is not in the roadmap

Quote: `ANTS-1198`

### Row 1645 — Low — `src/terminalwidget.cpp:6324`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6579.

ANTS-1169 is not in the roadmap

Quote: `ANTS-1169`

### Row 1646 — Low — `src/themes.cpp:361`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6557.

ANTS-1179 is not in the roadmap

Quote: `ANTS-1179`

### Row 1647 — Low — `src/verifytrust.cpp:223`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6557.

ANTS-1179 is not in the roadmap

Quote: `ANTS-1179`

### Row 1648 — Low — `tools/check-shipped-coverage.sh:214`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1 is not in the roadmap

Quote: `ANTS-1`

### Row 1649 — Low — `tools/check-shipped-coverage.sh:214`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-2 is not in the roadmap

Quote: `ANTS-2`

### Row 1650 — Low — `tools/check-shipped-coverage.sh:214`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-12 is not in the roadmap

Quote: `ANTS-12`

### Row 1651 — Low — `tools/check-shipped-coverage.sh:215`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-123 is not in the roadmap

Quote: `ANTS-123`

### Row 1652 — Low — `tools/safe-build.sh:2`

review-readability, lane comment_claims, comment_claims:unknown_ticket. **Disposition:** queued, ANTS-6569.

ANTS-1217 is not in the roadmap

Quote: `ANTS-1217`

## Duplicate folding

### Row 91 — Medium — `src/claudeintegration_terminal.cpp:1691`

review-readability, lane 17-claude-integration-a, Q1. **Disposition:** queued, ANTS-6590.

`last_selection`'s description points to "get_text's max_bytes". `get_text`'s schema (1569–1584) declares no `max_bytes`, but its handler reads one (remotecontrol_terminal.cpp:632, in `cmdGetText`). Anyone deciding from the schema what `get_text` accepts will conclude it has no byte cap.

Quote: `"after secret redaction, as get_text's max_bytes. "`

Remedy: declare `max_bytes` in `get_text`'s inputSchema.

Also reported as: "code lane 08-claude-integration-b Low dim 2 claudeintegration_terminal.cpp:1690-1693"

### Row 144 — Medium — `runtraceverb.cpp:246-263`

review-code, lane 13-mcp-content-verbs-b, dim 4. **Disposition:** queued under ANTS-5954/ANTS-6272.

against `sessioncostverb.cpp:119,139` — two token-summing copies of the same transcript walk have drifted apart:

Quote: `sessioncostverb.cpp:119,139`

Remedy: share one usage-summing function. Which sidechain rule is right is under Open questions.

Also reported as: "readability lane 30-mcp-content-verbs-b Medium Q1 src/runtraceverb.cpp:239-274; readability lane 30-mcp-content-verbs-b Info correctness lead src/runtraceverb.cpp:249-271"

### Row 199 — High — `src/luaengine.cpp:719`

review-readability, lane 40-plugins-lua, Q1. **Disposition:** queued, ANTS-6608.

ANTS-5070 changed `get_output` and `settings.get` to push their results inside a protected call, so that running out of memory cannot skip a live C++ buffer's cleanup. It missed `get_cwd` (line 750, through `pushQString`) and `project.root` (lines 1357–1358, an inline copy that keeps the `QByteArray r` alive across `lua_pushlstring`). A stranger adding a new string getter will copy `pushQString`:…

Quote: `static void pushQString(lua_State *L, const QString &s) {`

Remedy: send `get_cwd` and `project.root` through `pushBytesProtected` with the buffer scoped, then delete `pushQString`.

Also reported as: "code lane 18-plugins-lua Low dim 4 luaengine.cpp:719-722; readability lane 40-plugins-lua Info correctness lead src/luaengine.cpp:719-722"

### Row 212 — Medium — `src/sessionmemoryengine.cpp:128`

review-readability, lane 39-config-session-project, Q1. **Disposition:** queued under ANTS-6019.

This is a hand-written copy of the 0600 permission mask, and its result is thrown away. secureio.h:20–26 says every permission set goes through `setOwnerOnlyPerms`, which is marked `[[nodiscard]]` (ANTS-5151), and this file includes secureio.h. `saveStore` is also a second write path beside `writeFileAtomicOwnerOnly`, which secureio.h calls "the safe write every owner-only file shares", and it la…

Quote: `QFile::setPermissions(path,`

Remedy: call `setOwnerOnlyPerms` and handle its result (or route through `writeFileAtomicOwnerOnly`).

Also reported as: "code lane 19-config-session-project Low dim 3 sessionmemoryengine.cpp:128; readability lane 39-config-session-project Info correctness lead src/sessionmemoryengine.cpp:114-129"

### Row 263 — Medium — `src/modelnearmissledger.cpp:141`

review-readability, lane 38-model-switching, Q1. **Disposition:** queued under ANTS-6016.

The ledger record filter (epoch, project, time window) has three copies, and they have diverged: - `filterWindow` (near-miss :131-147) **drops** a record with a malformed timestamp. - `statsForScope` (`modelswitchledger.cpp:620-625`, `if (tsMs > 0 && nowMs - tsMs > windowMs)`) **keeps** it. - `filterProject` (:149-159) repeats filterWindow's first two checks. Line 129 claims the near-miss filter …

Quote: `if (tsMs <= 0) continue; // malformed ts → skip`

Remedy: have `filterWindow` call `filterProject`, and decide and comment one rule for malformed timestamps in both ledgers.

Also reported as: "code lane 23-model-switching Low dim 4 modelswitchledger.cpp:621-622; readability lane 38-model-switching Info correctness lead src/modelnearmissledger.cpp:141"

### Row 318 — Low — `auditrunner.cpp:1007-1008`

review-code, lane 27-audit-engine, dim 4. **Disposition:** queued, ANTS-6586.

(`^([^:]+):…`) and `auditengine.cpp:573-576` (`^([^\s:]+):…`, applied to trimmed lines) parse the same tool lines differently. For a path containing whitespace the fingerprints differ, so a false positive learned in the GUI does not suppress in the headless runner. fix: share one parser.

Quote: `^([^:]+):…`

Remedy: share one parser.

### Row 341 — Medium — `src/roadmapdialog.cpp:1848`

review-readability, lane 16-roadmap-dialog, Q1. **Disposition:** queued, ANTS-6610.

The "how far does a bullet's text run" walk in `lastTouchFromBlame` stops at the first blank line. Its twin `skipBulletBlockAt` (1464-1488) and `RoadmapParse` (roadmapparse.cpp:1077-1083, ANTS-1426) both carry on past a blank line that is followed by an indented line. No comment says the blame walk differs on purpose. A stranger who fixes block rules in one place will assume the blame walk matche…

Quote: `if (cont.trimmed().isEmpty()) break;`

Remedy: share one block-extent helper, or comment why blame differs. Cost: pinned — `docs/specs/ANTS-1237.md` names `lastTouchFromBlame` (line 41) and defines "until blank" (lines 281, 455); anchor `lastTouchFromBlame(` (line 37). Searched: `src` for `INV-31`; `roadmapparse.cpp` for its blank-line rule; th…

Also reported as: "code lane 29-roadmap-dialog Low dim 4 roadmapdialog.cpp:1848; readability lane 16-roadmap-dialog Info correctness lead src/roadmapdialog.cpp:1835-1858"

### Row 358 — Medium — `src/remotecontrol_review.cpp:779`

review-readability, lane 28-mcp-review-verbs, Q1. **Disposition:** queued under ANTS-5943.

The partition verb works out lanes in two steps: the declared partition, then a computed fallback when that yields ≤1 lane (lines 41–58). The dispatch verb has its own copy of lane resolution without the fallback. So a lane name that `indie_review_partition` returns with `derived:true` is refused here as `unknown lane … (not in partition)`. A stranger changing how lanes are resolved in one verb w…

Quote: `const auto allLanes = IndieReviewEngine::derivePartition(root);`

Remedy: have both verbs get their lanes from one shared resolver.

Also reported as: "code lane 31-mcp-review-verbs Low dim 2 remotecontrol_review.cpp:779-784"

### Row 366 — Medium — `doclint.cpp:567`

review-code, lane 32-doc-engines, dim 4. **Disposition:** queued, ANTS-6598.

What goes wrong. The doc_lint adapter's "is this a finding" test has drifted from doc_citations' own stale test (`doccitations.cpp:1914-1919`). Doc_citations excludes `foreign_path` (ANTS-4085) and any `loop_log_row` citation (ANTS-4918 INV-49), because both can never be cleared. Doc_lint turns both into findings.

Quote: `if (status.isEmpty() || status == QStringLiteral("ok")) continue;`

Remedy: skip `foreign_path` and any entry with `loop_log_row:true` in the adapter, or reuse one shared predicate.

### Row 367 — Medium — `docfacts.cpp:136-150`

review-code, lane 32-doc-engines, dim 4. **Disposition:** queued, ANTS-6598.

(`countListItems`) vs `docintegrity.cpp:777-807` (`countItems`), plus `statedCount` / `singleCardinal` What goes wrong. Two engines both run under doc_lint and check the same thing: does a lead-in's count match the list beneath it? Their rules have diverged.

Quote: `countListItems`

Remedy: one shared item counter in MarkdownScan, used by both checks. Or drop one check and record the decision in the spec.

Also reported as: "readability lane 31-doc-engines Info correctness lead src/docintegrity.cpp:781"

### Row 368 — Medium — `docintegrity.cpp:33`

review-code, lane 32-doc-engines, dim 4. **Disposition:** queued under ANTS-6275.

Heading detection has five diverged copies: `docintegrity.cpp:33`: `^(#{1,6})[ \t]+(.*)$`. No indent allowed, no blockquote.

Remedy: build both heading lists from `MarkdownScan::headings`, slugged with `gfmSlug`, with trailing hashes stripped in one place.

### Row 374 — Medium — `specconformance.cpp:45-52`

review-code, lane 33-spec-engines, dim 2. **Disposition:** queued, ANTS-6613.

`cellsOf` splits on every pipe and ignores GFM's `\|` escape. Spec § 2.2 calls the expectation block a "GFM table", and GFM requires `\|` for a pipe inside a cell, code spans included.

Quote: `for (const QString &c : s.split(QLatin1Char('|'))) out << c.trimmed();`

Remedy: use the escape-aware splitter, then unescape `\|` in each cell.

### Row 376 — Low — `speclog.cpp:626`

review-code, lane 33-spec-engines, dim 9. **Disposition:** queued, ANTS-6613.

`append_inv` inserts under one definition of the section (the first `## ` heading containing the word, by substring) but checks for duplicates under another (strict `##`/`###` first, then loose). The code comment at 632–634 admits this.

Quote: `646`

Remedy: insert at `invariantsSection().end`.

### Row 381 — Low — `specparse.cpp:191-194`

review-code, lane 33-spec-engines, dim 4. **Disposition:** queued, ANTS-6613.

's lazy `(.+?)` cells ignore `\|`. In a row like `| INV-1 | a \| b | t |`, `body` becomes `a \` and `test_surface` becomes `b | t`. Same divergence from `tableCells` as the Medium above.

Quote: `tableRe`

### Row 419 — Low — `changeloglog.cpp:514`

review-code, lane 36-changelog-feedback, dim 4. **Disposition:** queued, ANTS-6588.

the writer and the reader treat category case differently.

Quote: `changelogquery.cpp:142`

Also reported as: "readability lane 41-changelog-feedback Info correctness lead src/changeloglog.cpp:514"

### Row 420 — Low — `changeloglog.cpp:629–632`

review-code, lane 36-changelog-feedback, dim 4. **Disposition:** queued under ANTS-6030.

accepts any `date`, including one with a newline, while it refuses a bad `version` (`:582`). `closeUnreleasedDated` validates its date (`:667`), so the two release paths have drifted apart. Fix: apply the same YYYY-MM-DD check.

Quote: `closeUnreleased`

Remedy: apply the same YYYY-MM-DD check.

### Row 431 — High — `buildcache.cpp:43`

review-code, lane 37-diagnostics-logging, dim 4. **Disposition:** queued, ANTS-6596.

this does not match `fatal error:`. The other copy of the same GCC line parser, at `scrollbackerrors.cpp:19`, does: `(?:fatal\s+)?error:`. The two copies have drifted apart.

Quote: `R"(^([^:\n]+):(\d+)(?::(\d+))?:[ \t]+(error|warning|note):…`

Remedy: use `(?:fatal[ \t]+)?(error|warning|note)` in the regex.

Also reported as: "readability lane 43-diagnostics-logging Medium Q1 src/buildcache.cpp:43; readability lane 43-diagnostics-logging Info correctness lead src/buildcache.cpp:43"

### Row 448 — Medium — `tools/local-ci.sh:364-369`

review-code, lane 38-dev-shell-tools, dim 4. **Disposition:** queued, ANTS-6595.

the "interrupted build" marker works three different ways, and only one is robust.

Quote: `tools/asan-nightly.sh:84-88`

Remedy: in both scripts, write the marker before the build and remove it on success, as qt62-guard does. Add `HUP`, and run the job in the background followed by `wait`.

Also reported as: "readability lane 46-tools-ci-gate Medium Q1 tools/local-ci.sh:364"

### Row 458 — Medium — `packaging/debian/rules:21-25`

review-code, lane 39-dev-python-ci-packaging, dim 4. **Disposition:** queued under ANTS-6104.

Both recipes say in their own comments that they lack the RPM `%check` test settings. The RPM side (`ants-terminal.spec:295`, `:316`, `:363`) records why each setting is needed:

Quote: `packaging/archlinux/PKGBUILD:65-70`

Remedy: export the same two variables and pass `-LE perf` in both.

Also reported as: "readability lane 48-hooks-packaging Info correctness lead packaging/archlinux/PKGBUILD:69"

### Row 464 — Low — `tools/rc-namespace-scan.py:107-114`

review-code, lane 39-dev-python-ci-packaging, dim 4. **Disposition:** queued, ANTS-6594.

the char-literal skip does not handle C++14 digit separators (`1'000`). `split-motion-check.py:297` does handle them.

Quote: `1'000`

Also reported as: "readability lane 47-tools-roadmap-surveys Info correctness lead tools/rc-namespace-scan.py:107"

### Row 465 — Low — `tools/roadmap-archive-survey.py:65-71`

review-code, lane 39-dev-python-ci-packaging, dim 4. **Disposition:** queued, ANTS-6594.

does not skip fenced blocks. Its sibling `roadmap-slug-oracle.py:75-79` does, and says that is the rule. So the collision counts at sections C and D include headings quoted inside fences.

Quote: `HEADING_RE.match(line)`

Remedy: skip fences as the oracle does, or say why not.

Also reported as: "readability lane 47-tools-roadmap-surveys Low Q1 tools/roadmap-archive-survey.py:65; readability lane 47-tools-roadmap-surveys Info correctness lead tools/roadmap-archive-survey.py:65"

### Row 473 — Medium — `src/vtparser.cpp:240-241`

review-readability, lane 01-vt-parser-pty, Q1. **Disposition:** queued, ANTS-6619.

The set of string-collecting states is kept by hand in five places: - the ESC exemption here - the CAN/SUB buffer release at 251–253 - the dispatch case list at 331–338 - `transition`'s truncation reset at 568–569 - `reArmStringIntroducer` at 546–548, where each case clears its buffer They agree today. A stranger adding a string state (for example, collecting PM instead of ignoring it) who misses…

Quote: `if (m_state == OscString || m_state == DcsString ||`

Remedy: keep one table per string state (its buffer, end action and peek state) and read every site from it.

### Row 482 — High — `src/terminalgrid.cpp:2152`

review-readability, lane 02-terminal-grid, Q1. **Disposition:** queued under ANTS-6177.

Four functions each hand-copy the same steps: rotate a block of rows, keep the link table in step, then blank the rows that came in. They are `insertLines` (1907-1922), `deleteLines` (1934-1950), `scrollUp` (2106-2125) and `scrollDown` (2149-2167). - A past fix had to be applied to more than one copy: ANTS-1668 added the link-table rotate to `scrollDown` "mirrors scrollUp". - The copies have sinc…

Quote: `// ANTS-1668 — keep OSC 8 hyperlink spans aligned with their rows on`

### Row 486 — Medium — `src/terminalgrid.cpp:477`

review-readability, lane 02-terminal-grid, Q1. **Disposition:** queued, ANTS-6614.

`handleAsciiPrintRun` copies `handlePrint`'s wrap step, wide-pair cleanup and link drop. - The wrap block appears at 456-461 and again at 535-541. - ANTS-5076 had to add `dropHyperlinkSpans` to both (477 and 569), so a past fix touched both copies. - The fast path says it "mirrors handlePrint", but `handlePrint` has no pointer back. A stranger fixing printing in `handlePrint` would not know the p…

Quote: `dropHyperlinkSpans(row, startCol, startCol + span); // ANTS-5076`

### Row 487 — Medium — `src/terminalgrid.cpp:2993`

review-readability, lane 02-terminal-grid, Q1. **Disposition:** queued, ANTS-6614.

`resize` has two copies of the copy-without-reflow step: one for the live screen (2962-2976) and one for the saved main screen (2996-3010). - The comment shows the 0.6.28 combining fix had to be applied to both. - They have drifted: the saved-screen copy checks `m_altScreen[r].cells.size()` and the live copy does not. - A stranger fixing one will miss the other. - Fix: one copy step inside `resiz…

Quote: `// Same combining-map preservation as the main-screen simple-copy`

### Row 506 — Medium — `src/terminalwidget.cpp:1913-1916`

review-readability, lane 03-terminal-widget-a, Q1. **Disposition:** queued, ANTS-6615.

Quick-select keyboard mode opens a link straight through `QDesktopServices::openUrl`. A mouse click goes through `openHyperlink`, which runs the OSC 8 phishing check (`if (span.isOsc8)` at cpp:4497). OSC 8 links are hidden links that programs print, where the visible text can differ from the real address. `enterUrlQuickSelect` builds its labels from `urlSpansForLine` but does not copy `isOsc8` (`…

Quote: `} else if (ql.url.startsWith("http://") || ql.url.startsWith("https://")) {`

Remedy: route quick-select's URL branch through `openHyperlink`.

Also reported as: "readability lane 03-terminal-widget-a Info correctness lead src/terminalwidget.cpp:1913-1916"

### Row 507 — Low — `src/terminalwidget.cpp:1472-1474`

review-readability, lane 03-terminal-widget-a, Q1. **Disposition:** queued, ANTS-6615.

The success/failure colours are written out twice in `paintEvent`, and the copies differ: - Status stripe: `0x9E,0xCE,0x6A` / `0xF7,0x76,0x8E`. - Command-mark gutter (cpp:1846-1847): `0x9F,0xE0,0x64` / `0xE0,0x64,0x64`. Someone retheming "success green" changes one and leaves the other.

Quote: `? QColor(0x9E, 0xCE, 0x6A) // green (success)`

Remedy: one pair of named colour constants used by both.

### Row 508 — Low — `src/terminalwidget.cpp:2443-2454`

review-readability, lane 03-terminal-widget-a, Q1. **Disposition:** queued, ANTS-6615.

The asciicast recording escapes its JSON by hand. `escapeJson` in `scrollbackexporter.cpp:47-66` does the same job for the Cast export, and the two differ: `\b` and `\f` come out as `\u0008` and `\u000c` here, but as `\b` and `\f` there. Both are valid JSON. A fix to how recordings are escaped would land in only one copy.

Quote: `escaped.reserve(raw.size() + raw.size() / 4);`

Remedy: have the recording call `escapeJson`.

### Row 509 — Low — `src/terminalwidget.cpp:1965-1974`

review-readability, lane 03-terminal-widget-a, Q1. **Disposition:** queued, ANTS-6615.

These two lists name the keys that must not snap the view back to the bottom. They must stay in step with the handlers further down: the Shift+Home/End/PageUp/PageDown list is repeated at 2111-2113, and Ctrl+Shift Up/Down/J/K/C/F/G match their handlers. Someone adding a Ctrl+Shift shortcut that acts on the visible screen would add the handler and miss the list. The view would then jump to the bot…

Quote: `const bool shiftOnlyScroll = (mods & Qt::ShiftModifier)`

Remedy: one named key-set used by both the snap check and the Shift-scroll handler.

### Row 513 — Medium — `src/terminalwidget.cpp:4026`

review-readability, lane 04-terminal-widget-b, Q1. **Disposition:** queued, ANTS-6615.

There are five copies of the "code point to UTF-16" step, and they disagree: - `a11yComposeCols`' `put` (4024–4033) swaps invalid code points for U+FFFD. - `lineText`'s `appendCp` (4224–4232), `lineText`'s `put` (4252–4260) and `scrollbackexporter.cpp`'s `appendCodepoint` (15–23) emit raw surrogate math. - `lineColumns`' `units` (4299) counts by `lineText`'s rule, and its comment says it "walks t…

Quote: `if (cp > 0x10FFFF || QChar::isSurrogate(cp)) cp = QChar::ReplacementCharacter;`

Remedy: one shared encoder, plus one unit-count helper that uses the same rule.

### Row 514 — Medium — `src/terminalwidget.cpp:3287`

review-readability, lane 04-terminal-widget-b, Q1. **Disposition:** queued, ANTS-6615.

`copySelectionRich` builds the HTML half of a copy cell by cell. It drops combining marks and skips wide-character continuation cells. The plain half (`selectedText` → `a11yComposeCols`, 3976–3988) keeps both, and it works out its column range from its own copy (3978–3984 against 3269–3271). The RC-44 comment at 3250 records a fix that had to bring this copy back into line with `selectedText`. Th…

Quote: `uint32_t cp = c.codepoint;`

Remedy: build the column range and the cell text from one helper shared by both halves.

### Row 515 — Medium — `src/terminalwidget.cpp:5949`

review-readability, lane 04-terminal-widget-b, Q1. **Disposition:** queued, ANTS-6615.

The list of grid-changing trigger kinds is kept by hand in `checkTriggers` (5949–5951) and in `onGridLineCompleted` (6047–6049). The dispatch code that sends `run_script` or `triggerFired` is copied too (5958–5962 against 6061–6065). The `// ANTS-5079` tags at 5955 and 6060 show that a past fix had to patch both. If someone adds a new grid-changing kind to only one list, `checkTriggers` will send…

Quote: `if (rule.actionType == QLatin1String("highlight_line")`

Remedy: one predicate for "is this a grid-changing kind" and one dispatch helper.

### Row 516 — Medium — `src/terminalwidget.cpp:5256`

review-readability, lane 04-terminal-widget-b, Q1. **Disposition:** queued under ANTS-6178.

The steps "scroll to global line X, clamp, update the scroll bar, repaint" are copied eight times: - `navigatePrompt` at 3372–3376, 3380–3382, 3387–3391 and 3395–3397. - `nextBookmark`/`prevBookmark` at 5254–5257, 5262–5265, 5274–5277 and 5282–5285. - `scrollToMatch` has a related ninth copy at 4836–4838. ANTS-5078 had to add `updateScrollBar()` to the bookmark copies and to `scrollToMatch` one b…

Quote: `updateScrollBar(); // ANTS-5078`

Remedy: extract a `scrollToGlobalLine(gl)` helper.

### Row 517 — Medium — `src/terminalwidget.cpp:6469`

review-readability, lane 04-terminal-widget-b, Q1. **Disposition:** queued, ANTS-6615.

The rule for "can this block fold?" is copied at 3761, 5060, 5730, 5739 and 6469. The paint copy (1480) also requires a following region (`std::next(it) != regions.end()`); the others don't. So the right-click menu offers "Fold Output" on the newest block, which never shows a fold triangle. Someone changing which blocks can fold has six places to edit, and one of them already differs.

Quote: `if (!pr.hasOutput || pr.commandEndMs == 0) return; // mirrors toggleFoldAtCursor gating`

Remedy: one `isFoldable(index)` predicate used everywhere.

### Row 518 — Medium — `src/terminalwidget.cpp:5408`

review-readability, lane 04-terminal-widget-b, Q1. **Disposition:** queued, ANTS-6615.

The "12-line lookback window" (the comment at 5476) is a bare literal at 5408, 5458, 5459 and 5466. Separately, the question detector's debounce is a literal `3` (5425), while the permission detector uses `kMissedScansToRetract`, which is local to its block (5481). Someone widening the window, or changing the debounce, in the permission path will leave the question detector and the structure chec…

Quote: `for (int i = total - 1; i >= std::max(0, total - 12); --i) {`

Remedy: two named constants at function scope, shared by both detectors.

### Row 524 — Medium — `src/terminalaccessible.cpp:92`

review-readability, lane 04-terminal-widget-b, Q1. **Disposition:** queued, ANTS-6615.

Three copies of the boundary lookup (64–79, 83–98, 102–117) have diverged: - `textAtOffset` handles an offset that sits on a boundary (`isAtBoundary`). - `textBeforeOffset` does not, so at a word start it skips the previous word. Fixing ANTS-5831's "line means a terminal row, not a wrap point" needs the same special case in all three.

Quote: `int end = f.toPreviousBoundary();`

Remedy: one shared helper returning the unit `[start, end)` that contains an offset, with Before/After defined from it.

### Row 525 — Low — `src/terminalwidget.cpp:6409`

review-readability, lane 04-terminal-widget-b, Q1. **Disposition:** queued, ANTS-6615.

Trailing blanks are trimmed in five places with two different rules: - `isSpace()`, which also strips tabs and non-breaking spaces: 6409 and 6429. - `' '` only: 3986, 4787 and 5986. The `!= '\n'` test also implies that `lineText` can contain a newline, which it cannot.

Quote: `while (!line.isEmpty() && line.back().isSpace() && line.back() != '\n')`

Remedy: one trim helper, with the rule chosen once.

### Row 526 — Low — `src/terminalwidget.cpp:4595`

review-readability, lane 04-terminal-widget-b, Q1. **Disposition:** queued, ANTS-6615.

The editor list in the comment does not match the code. The comment lists `sublime`; the code matches `subl` and `sublime_text`. The code also accepts `vscodium`, `vi` and `ex`, which the comment omits. A user, or a stranger following the "keep this list in sync" note (4586), would set `sublime` and land in the unknown-editor branch.

Quote: `// sublime/subl <path>:<line>[:<col>]`

Remedy: make the comment list the names the code checks.

### Row 532 — High — `src/mainwindow.cpp:2893`

review-readability, lane 05-mainwindow-a, Q1. **Disposition:** queued under ANTS-6173.

Two separate copies apply settings to a running window, and they now behave differently. - **Copy 1:** this function, which runs when someone edits `config.json` by hand. - **Copy 2:** the Preferences "settings changed" handler, `src/mainwindow_menus.cpp:936-990`. - **Only the Preferences copy:** sets up Quake mode and its hotkey (ANTS-1738). It also applies the MCP on/off switch and removes the …

Quote: `void MainWindow::onConfigFileChanged(const QString &path) {`

### Row 538 — Low — `src/mainwindow.cpp:2374-2376`

review-readability, lane 05-mainwindow-a, Q1. **Disposition:** queued, ANTS-6600.

The steps that clear and hide the branch chip, with its separator, appear three times: - `refreshStatusBarForActiveTab`, 2374–2376 - `updateStatusBar`, 2462–2464 - `updateStatusBar`, 2525–2527 The separator was a later addition (0.6.26). A further widget tied to the branch chip would have to be added in all three places. - Fix: in `refreshStatusBarForActiveTab`'s no-terminal branch, call `updateS…

Quote: `m_statusGitBranch->clear();`

### Row 539 — Low — `src/mainwindow.cpp:1467`

review-readability, lane 05-mainwindow-a, Q1. **Disposition:** queued, ANTS-6600.

The branch chip's colour rule (green for main/master/trunk, amber otherwise) is written twice: in `applyTheme` and in `updateStatusBar` (2511–2516). The ANTS-1147 ticket is cited beside both, which shows one past change had to touch both. `applyTheme` also sets `m_lastBranchChipValid = false`, so the next timer tick redraws the chip anyway. - **What a stranger gets wrong:** a colour change made i…

Quote: `const QColor &col = primary ? theme.ansi[2] : theme.ansi[3];`

### Row 543 — High — `src/mainwindow_tabs.cpp:1229`

review-readability, lane 06-mainwindow-b, Q1. **Disposition:** queued under ANTS-6172.

There are three copies of "make a shell tab": `newTab` (478-517), `newTabForRemote` (1227-1280), and the first part of `restoreSessions`' loop (sessions.cpp:194-198). The comment says the remote copy mirrors `newTab`, but they have drifted apart. `newTab` ends with `saveTabOrderOnly()` (ANTS-1159, line 516); `newTabForRemote` does not. Someone changing tab creation in one copy would trust the "mi…

Quote: `// Mirror of the newTab() slot but with explicit cwd/command`

Remedy: extract one "add a new shell tab" helper and call it from both. Cost: pinned. These ANTS-1159 anchors pin it: `MainWindow::newTabForRemote` (remote_control_new_tab:94/99, remote_control_launch:92), `saveTabOrderOnly` (crash_safe_session_persist:119/131) and the welcome_dialog `newTabForRemote(QStri…

### Row 548 — Medium — `src/mainwindow_menus.cpp:382`

review-readability, lane 06-mainwindow-b, Q1. **Disposition:** queued, ANTS-6600.

The themes submenu is built twice: initially (201-218) and again in "Reload User Themes" (376-387). ANTS-3556 had to patch both copies. They have also diverged: the first ticks `m_config.theme()`, the reload ticks `m_currentTheme`. The next change to theme actions will be needed twice.

Quote: `// ANTS-3556 — same deferral as the initial theme actions`

Remedy: one helper that (re)builds the themes submenu, called from both places. Cost: pinned. Anchor `applyTheme(name)` (theme_switch_popup_defer:100, 104). dup:8eec6ae0c6eb.

### Row 550 — Low — `src/mainwindow_sessions.cpp:300`

review-readability, lane 06-mainwindow-b, Q1. **Disposition:** queued, ANTS-6600.

Lines 300-303 are an inline copy of `trackTerminalShell` (tabs.cpp:627-632). The comment above admits ANTS-1375 fixed a path that "forgot" this step. A future change to shell tracking would go into `trackTerminalShell` and miss this copy.

Quote: `if (m_claudeTabTracker && tab.terminal->shellPid() > 0)`

Remedy: call `trackTerminalShell(tab.terminal)`. Cost: pinned. Anchors `trackShell(` (claude_dot_restored_tabs:34) and `MainWindow::restoreSessions` (claude_dot_restored_tabs:25).

### Row 551 — Low — `src/mainwindow_tabs.cpp:904`

review-readability, lane 06-mainwindow-b, Q1. **Disposition:** queued, ANTS-6600.

The close-tab and close-window confirm dialogs (895-959, 963-1018) are near-copies. This comment ("as the window-close dialog has") records a fix that had to bring one copy in line with the other.

Quote: `// ANTS-5123 — dialogs.md D1–D4: theme chrome, resizable, size persisted,`

Remedy: one builder that takes title, body, object-name prefix and the accept action. Cost: pinned. Anchors `showCloseTabConfirmDialog(` / `void MainWindow::showCloseTabConfirmDialog` (confirm_close_with_processes:121/143/192) and `void MainWindow::showCloseWindowConfirmDialog(` (window_close_confirm:105/1…

### Row 552 — Low — `src/mainwindow_menus.cpp:93`

review-readability, lane 06-mainwindow-b, Q1. **Disposition:** queued, ANTS-6600.

This hand-rolls shell single-quoting, although `shellQuote` exists in shellutils.h, which this file already includes (line 9). There is a third copy at mcporientation.cpp:190. A fix to quoting made in `shellQuote` would not reach Undo Close Tab's `cd`.

Quote: `escaped.replace("'", "'\\''");`

Remedy: use `shellQuote(info.cwd)`. Cost: one edit. Searched: src for `'\\''` (3 copies); anchors for `escaped.replace` (no hit).

### Row 564 — Medium — `src/globalshortcutsportal.cpp:325`

review-readability, lane 07-chrome-widgets, Q1. **Disposition:** queued under ANTS-6190.

"Fail the session for good" is written out at seven sites: 85, 144, 223, 238, 244, 325 and 346. The copies already differ. The two BindShortcuts sites (322-324, 344-345) empty `m_pending` and clear `m_sessionHandle`. The CreateSession sites and the `isAvailable` site (144, which has just added to `m_pending`) do neither. The comment at 312-314 ("fail the same way (ANTS-1142: drain and stay failed…

Quote: `m_permanentlyFailed = true; // ANTS-1152`

Remedy: one private helper that sets the latch, drains the queue and emits `sessionFailed`.

### Row 566 — Medium — `src/kwinpositiontracker.cpp:25`

review-readability, lane 07-chrome-widgets, Q1. **Disposition:** queued, ANTS-6589.

`MainWindow::centerWindow` (mainwindow.cpp:1533-1549) builds its own copy of the same "find this pid's window, set frameGeometry" KWin script. It fills in `pid` with `.arg` and has neither the ANTS-1768 integral-only guard nor the comment explaining it. Both live only here. A stranger adding a value to the centring script, such as a screen name, gets no compile-time stop against script injection.

Quote: `static_assert(std::is_integral_v<decltype(pid)> &&`

Remedy: put a centring-script builder beside `kwinMoveScript`, under the same guard.

### Row 567 — Low — `src/opaquemenubar.h:58`

review-readability, lane 07-chrome-widgets, Q1. **Disposition:** queued, ANTS-6589.

The same opaque-background fill exists three times: - opaquemenubar.h:46-61 - opaquestatusbar.h:31-46 - coloredtabbar.cpp:165-170 with 344-348 The menu-bar copy is where the rationale lives, and it names neither sibling. A stranger fixing the fill there (its composition mode, say) would miss the status bar and tab bar.

Quote: `p.fillRect(rect(), m_bg);`

Remedy: one shared fill helper, or at least cross-references in the menu-bar copy.

### Row 568 — Low — `src/titlebar.cpp:95`

review-readability, lane 07-chrome-widgets, Q1. **Disposition:** queued, ANTS-6589.

The title-bar button setup is written out four times (57-64, 67-75, 78-86, 89-97): text, object name, accessible name and description, autoRaise, size 32×28. A stranger resizing the buttons or adding a fifth must edit every block alike.

Quote: `m_closeBtn->setFixedSize(32, 28);`

Remedy: a local lambda that builds one button.

### Row 578 — Medium — `src/themes.cpp:374`

review-readability, lane 08-dialog-chrome-theme, Q1. **Disposition:** queued, ANTS-6597.

This is a hand-written copy of the shared `rotateCorruptFileAside` helper, and so is `verifytrust.cpp:224-226`. The helper is used by `config.cpp:110` and `claudesetup.cpp:89`. The copies have drifted from it in four ways: - they rename instead of copying; - they use seconds instead of milliseconds; - they don't retry when the name is taken; - they don't prune old files. The two copies also diffe…

Quote: `QFile::rename(filePath, rotatedPath);`

Remedy: route both sites through `rotateCorruptFileAside`, or name in each comment why they differ.

### Row 580 — Medium — `src/dialogchrome.cpp:169-176`

review-readability, lane 08-dialog-chrome-theme, Q1. **Disposition:** queued under ANTS-6216.

The same seven palette-role assignments are repeated at `roadmapdialog.cpp:2217-2225` and `roadmapshortcutsdialog.cpp:59-66`. `roadmapdialog.cpp:2242-2246` also repeats `applyTheme`'s title-bar `setThemeColors` call. The copies have drifted: only `dialogchrome` sets `PlaceholderText` (the D6 fix), and the shortcuts copy adds `Highlight` roles. Today the drift does no harm, because both roadmap di…

Quote: `QPalette dp = dlg->palette();`

Remedy: have the roadmap sites call `DialogChrome::applyTheme(this, m_titleBar, theme)`, then set only their extra roles.

### Row 586 — High — `src/auditdialog.cpp:1213`

review-readability, lane 09-audit-dialog, Q1. **Disposition:** queued under ANTS-6191.

The rule for "is this finding visible right now?" is written out four times: - `actionableFindings` (auditdialog.cpp:1209-1225) - `visibleUntriagedKeys` (auditdialog_triage.cpp:285-300) - the row loop in `renderResults` (auditdialog.cpp:3471-3483) - the count loop in `renderResults` (auditdialog.cpp:3221-3230) The count loop already differs: it skips the severity-pill and text filters, and no com…

Quote: `if (m_sinceBaseline) {`

Remedy: one member predicate used by all three lists, with a comment at the count loop saying what it leaves out on purpose.

### Row 587 — High — `src/auditdialog_triage.cpp:477`

review-readability, lane 09-audit-dialog, Q1. **Disposition:** queued under ANTS-6174.

`requestAiTriage` and `requestAiTriageBatch` each carry their own copy of the same steps: - the snippet fallback (117-121 / 394-398) - reading the config and picking the default model (123-132 / 403-408) - the egress check, request headers, redirect policy, post and reply cap (179-206 / 455-485) - writing the verdict back (244-252 / 522-530) Comments show past fixes had to touch both copies: ANTS…

Quote: `// ANTS-1798/2121 — refuse redirects so a hostile 3xx can't bounce the POST`

Remedy: one helper that builds and sends the triage request, and one that applies a verdict to a key.

### Row 592 — Medium — `src/auditdialog_catalogue.cpp:1507`

review-readability, lane 09-audit-dialog, Q1. **Disposition:** queued under ANTS-6197.

Timeouts are set after all checks are registered, matched by id strings far from where the checks are registered. Meanwhile `compiler_warnings` sets its own timeout at registration (line 927). If a stranger renames one of these ids, or adds a slow tool, the check silently drops back to the 30 s default and reports tool-health timeouts.

Quote: `if (c.id == "cppcheck" || c.id == "cppcheck_unused" ||`

Remedy: set `timeoutMs` at each registration, as compiler_warnings does.

### Row 593 — Medium — `src/auditdialog.cpp:3036`

review-readability, lane 09-audit-dialog, Q1. **Disposition:** queued under ANTS-6196.

This is a hand-kept list of check ids that are registered in another file (the catalogue). The comment explaining it is here, but nothing at the registrations points back. All 13 ids exist today; I checked them against the catalogue. Renaming an id in the catalogue silently turns off the comment/string filter for that check.

Quote: `static const QSet<QString> kSourceScannedChecks = {`

Remedy: a flag on AuditCheck, set where the check is registered.

### Row 594 — Medium — `src/auditdialog_debtsweep.cpp:243`

review-readability, lane 09-audit-dialog, Q1. **Disposition:** queued, ANTS-6585.

This hand-kept category list mirrors the categories debtsweepengine.cpp sets. They match today; I searched `category = ` and `run("…")` in src/debtsweep*.cpp. If the engine gains a category, its findings never render, but "Defer all" still folds them into ROADMAP.

Quote: `static const QStringList catOrder = {`

Remedy: render the known categories first, then any others that are left.

### Row 595 — Medium — `src/auditdialog.cpp:2173`

review-readability, lane 09-audit-dialog, Q1. **Disposition:** queued, ANTS-6585.

The SARIF and HTML save lambdas (2171-2205 and 2212-2243) are the same code apart from a label. Comments show past fixes had to touch both: 0.7.52 ("same atomic + 0600 treatment as SARIF"), ANTS-5084 and ANTS-5151 each appear twice.

Quote: `ensurePrivateDir(m_projectPath + "/.audit_cache"); // ANTS-5085 — 0700 from the start`

Remedy: one `saveReport(extension, label, body)` member.

### Row 602 — Low — `src/auditdialog.cpp:1227`

review-readability, lane 09-audit-dialog, Q1. **Disposition:** queued, ANTS-6585.

The "actionable ≥ 70" threshold is also typed by hand into the empty-state message (1284), the tooltip (2440) and the header doc (auditdialog.h:160). Change the code and the user-facing text is wrong.

Quote: `f.aiVerdict == QStringLiteral("TRUE_POSITIVE") || f.confidence >= 70;`

Remedy: one named constant, used in all three strings.

### Row 603 — Low — `src/auditdialog_catalogue.cpp:115`

review-readability, lane 09-audit-dialog, Q1. **Disposition:** queued, ANTS-6585.

Four nearly identical in-process check blocks (114-187). They differ only in id, name, description, severity and the deliberate `maxLines = 0`.

Quote: `AuditCheck c;`

Remedy: an `addInProcessCheck` helper, like the existing `addGrepCheck`.

### Row 605 — Medium — `src/auditrunner.cpp:1614`

review-readability, lane 10-audit-engine-a, Q1. **Disposition:** queued, ANTS-6586.

Lines 1621–1626 copy the reply's `by_tool` shape from `mcptoolregistry.cpp:471-476`, but the two have diverged. Since ANTS-4371 the real reply also carries `paths_given`, `scanned_whole_project` and `no_files` (`mcptoolregistry.cpp` ~489–492), and this copy does not. A stranger adding a field to the reply will not know this size-measuring copy exists. The header (`auditrunner.h:234-236`) also cal…

Quote: `qint64 measureEnvelopeBytes(const QJsonArray &samples,`

Remedy: build `by_tool` in one shared function that both the reply and the measurement use.

### Row 606 — Medium — `src/auditrunner.cpp:2754`

review-readability, lane 10-audit-engine-a, Q1. **Disposition:** queued, ANTS-6586.

The HTML fallback path at 2752–2768 is a second copy of `allocSarifPath` (712–732): try the XDG cache `/audit` folder, else `/tmp`, with a random tail. The comment itself shows one past fix (the reaper) had to be patched into both. A third copy of the same folder list is in `reapFallbackArtifactsOnce` (704–708). The copies already differ: the HTML copy has no sequence number and takes no lock. A …

Quote: `// ANTS-3614 — this branch allocates a fallback without going`

Remedy: one shared "allocate fallback path for this extension" function, with the reaper reading the same folder list.

### Row 607 — Medium — `src/auditrunner.cpp:954-988`

review-readability, lane 10-audit-engine-a, Q1. **Disposition:** queued, ANTS-6586.

The JSON branch and the line-based branch of `parseToolOutput` (1071–1104) each repeat the same tail, step for step: - drop a learned false positive; - drop an allowlist match; - add the finding, with its fingerprint (`fp`), under the `kSarifFindingsMax` cap; - add the sample. The ANTS-3615 and ANTS-1870 tags in both branches show past fixes had to patch each. They have already diverged (the raw-…

Quote: `if (isLearnedFp(learnedFps, fileStr, ruleStr, msg)) {`

Remedy: move the per-finding drop/record step into one function both branches call.

### Row 617 — High — `src/auditscope.cpp:83`

review-readability, lane 11-audit-engine-b, Q1. **Disposition:** queued, ANTS-6586.

`runGitRaw` (21–38), `runGitSucceeds` (42–55) and `runGit` (83–99) are three copies of one routine: start git, wait, kill on timeout. Two past fixes had to patch all three: the ANTS-4999 tag sits on each `setProcessEnvironment` line, and the ANTS-5527 HostExec change is in all three. The copies have already drifted. Only `runGit` checks `exitStatus() != QProcess::NormalExit`; the other two trust …

Quote: `QString runGit(const QString &root, const QStringList &args, bool *ok) {`

Remedy: one private runner, with "trim" and "exit-only" options, that all three wrap.

### Row 619 — High — `src/auditengine.cpp:308`

review-readability, lane 11-audit-engine-b, Q1. **Disposition:** queued, ANTS-6586.

The two copies differ. `AuditDialog::resolveProjectPath` (auditdialog.cpp:464–480) refuses any path that resolves outside the project root, using an anchored canonical-prefix check. `resolveProjectPathLocal` has no such check: it accepts absolute paths and `..`. Because the comment says "lifted", a newcomer would assume the safety guard came across and would fix future path problems in only one c…

Quote: `// Lifted from auditdialog.cpp: resolve ./relative.cpp references`

Remedy: one shared resolver that keeps the containment check, or say here that the guard is missing and why.

### Row 620 — Medium — `src/auditcache.cpp:348`

review-readability, lane 11-audit-engine-b, Q1. **Disposition:** queued, ANTS-6586.

`recordRun` lists the files a run owns (sarif, html, findings_file) by hand in three places: - the history entry it builds from the prior run (244–257); - `addKeep` (348–349); - the reaper's lookups and `reapOne` calls (359–389). ANTS-1870 had to add `findings_file` to all three (its tags are at 252 and 389, and in `addKeep`). If a newcomer adds a fourth artefact and misses the first list, that f…

Quote: `for (const QString &k : {QStringLiteral("sarif"), QStringLiteral("html"),`

Remedy: one constant list of artefact keys, used by all three.

### Row 626 — Medium — `src/auditengine.cpp:1337`

review-readability, lane 11-audit-engine-b, Q1. **Disposition:** queued, ANTS-6586.

The three native summarisers (cppcheck 1147–1179, clang-tidy 1239–1251, semgrep 1324–1346) each repeat the same tail: work out the level number, tally it, apply the floor, build an `AuditSummaryFinding`, add it to the pool. `summariseSarif` has a fourth variant. A newcomer adding a field to `AuditSummaryFinding` must set it in four places, and nothing lists them.

Quote: `if (ord < floorOrd) continue;`

Remedy: one local helper that tallies, filters and appends, taking (level, tier, rule, message, file, line).

### Row 638 — Medium — `src/roadmapstore.cpp:2781`

review-readability, lane 12-roadmap-store-a, Q1. **Disposition:** queued, ANTS-6612.

The list of tables that hang off an item is kept by hand in three places: - `deleteItem`'s step list (2781-2788) - `deregisterProject`'s step list (2185-2202) - `itemIsReferenced`'s EXISTS list (2808-2814) Two header comments repeat it again (`.h:605-607` and `.h:1015-1018`). The lists agree today. A stranger who adds a table with `REFERENCES item(item_pk)` to the DDL in `createSchema` will updat…

Quote: `static const char *const kSteps[] = {`

Remedy: one list of the tables that hang off an item, used by `deleteItem` and `deregisterProject`, with `itemIsReferenced` pointing at it.

### Row 639 — Medium — `src/roadmapstore.cpp:2908`

review-readability, lane 12-roadmap-store-a, Q1. **Disposition:** queued, ANTS-6612.

The comment at 2891-2895 calls an UPDATE that "always claims to have written" a known failure mode. Only some single-row setters guard against it: - **Guarded:** `setProjectSourceFormat`, `setProjectRoot` and `markIdParsed`. - **Unguarded:** `setLegend` (2880), `reassignItemId` (2943), `setSectionSlug`, `setSectionIntro`, `setSectionSource`, `updateSection`, and in `roadmapstore_feedback.cpp` `se…

Quote: `if (q.numRowsAffected() != 1) {`

Remedy: either apply the row-count refusal to every single-row setter, or say beside the 2891 comment which setters skip it and why.

### Row 644 — High — `src/roadmapmigrate.cpp:165`

review-readability, lane 13-roadmap-store-b, Q1. **Disposition:** queued under ANTS-6215.

Someone adding a status (as ANTS-4977 added "dropped") has to edit at least seven hand-kept copies of the status-marker table in this one file, and nothing lists them: - `statusFromMarker` (47–57) - `passKeyword` (66–85) - `keywordIsNamed` (90–109; its own comment admits it is "two halves of one table" with the reader's word list) - `looksLikeLegendLine` (165–171) - the `closed` test (349–350) - …

Quote: `bool looksLikeLegendLine(const QString &text) {`

Remedy: one table in RoadmapParse mapping each marker to its status, pass keyword(s), legend word and open/closed, which every site reads.

### Row 652 — Medium — `src/roadmapmigrateload.cpp:85`

review-readability, lane 13-roadmap-store-b, Q1. **Disposition:** queued, ANTS-6612.

The archive-filename pattern is hand-copied in four places: - this one - `archiveNameRx` (roadmapmigrate.cpp:473), whose comment claims the pattern is "expressed ONCE" - roadmapfoldin.cpp:501 - remotecontrol_roadmap_log_batch.cpp:3440 The comment above this copy says it "has to be" the discovery regex. roadmap-format.md:1088–1095 records that a planned widening (ANTS-4073) must reach every copy. …

Quote: `QStringLiteral("\\Adocs/roadmap/(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.md\\z"));`

Remedy: one exported archive-name pattern that all four use.

### Row 653 — Medium — `src/roadmapmigrateverb.cpp:416`

review-readability, lane 13-roadmap-store-b, Q1. **Disposition:** queued, ANTS-6612.

This hint hand-copies the list that `RoadmapSource::storeServedDialects()` returns (roadmapsource.cpp:509–511). roadmapsource.h:262–266 records that three copies of this list already disagreed once (ANTS-5335), and that the list accessor exists "for a refusal that names it" (ANTS-5161). The two match today. The next served dialect would leave this text wrong.

Quote: `"the ants-v1 and pass-headings dialects are served from the store, "`

Remedy: build the sentence from `storeServedDialects()`.

### Row 656 — Low — `src/roadmapmigrateload.cpp:977`

review-readability, lane 13-roadmap-store-b, Q1. **Disposition:** queued, ANTS-6612.

A private method with one call site (1125) that only forwards to `itemWriteFrom`. Two near-identical names send a reader looking for a difference that does not exist.

Quote: `RoadmapStore::ItemWrite Loader::itemWriteFor(const PlannedItem &it, const QString &id,`

Remedy: call `itemWriteFrom` directly and delete the method.

### Row 659 — High — `src/roadmapparse.cpp:974`

review-readability, lane 14-roadmap-parse-render-a, Q1. **Disposition:** queued under ANTS-6215.

**The set of five statuses is written out by hand in about seven places, and adding `dropped` had to touch every one of them.** A stranger adding a sixth status, or changing a glyph, would update `kPassStatuses` and stop there, because that table's own comment says *"A new status goes here and nowhere else in this file."* The copies: - `passheadingwrite.cpp:113`: `kPassStatuses` - `roadmapparse.c…

Quote: `if (statusWord == QStringLiteral("done") ||`

### Row 661 — Medium — `src/roadmapparse.cpp:1783`

review-readability, lane 14-roadmap-parse-render-a, Q1. **Disposition:** queued, ANTS-6611.

**The format detector keeps its own looser copies of the pass-heading and Status-line patterns.** `roadmapparse.h:447` says the grammar is shared *"so a change lands in both"*. A stranger who widens `passStatusLinePattern()` (say, to accept `Status:` without bold) would expect files in that spelling to be recognised as pass-headings. They would not be, because detection uses `rxStatusMarker` at 1…

Quote: `QStringLiteral("^####\\s+Pass\\s+\\d"));`

### Row 664 — Medium — `src/roadmapparse.cpp:1223`

review-readability, lane 14-roadmap-parse-render-a, Q1. **Disposition:** queued, ANTS-6611.

**Two code-span masks that disagree.** This one pairs backticks one at a time. `maskQuotedRegions` (588) uses CommonMark pairing through `MarkdownScan`. The comment at 572-574 admits the difference (*"documents that as a known miss; it is not the thing to copy"*), and no compatibility promise pins it. A stranger fixing a code-span bug in `maskQuotedRegions` would leave headline extraction on the …

Quote: `const QString maskedBody = [&body] {`

### Row 665 — Medium — `src/roadmapparse.cpp:117`

review-readability, lane 14-roadmap-parse-render-a, Q1. **Disposition:** queued, ANTS-6611.

**A copy of this function lives at `remotecontrol.cpp:1891` (`rcExtractBoldId`).** Its comment says ANTS-1937 changed it to *"match RoadmapParse::extractBoldId's looser pattern"*. So a past fix had to patch both. The parser's version is in an anonymous namespace, so nothing else can call it. `rcExtractCaretAnchor` (`remotecontrol.cpp:1908`) also copies `caretAnchorAtEnd()`. Fix: export `extractBo…

Quote: `bool extractBoldId(const QString &lineHead, QString *id) {`

### Row 680 — Low — `src/roadmapparse.cpp:887`

review-readability, lane 14-roadmap-parse-render-a, Q1. **Disposition:** queued, ANTS-6611.

**The "walk to the end of the pass block" loop is written four times:** `roadmapparse.cpp:898` and `941`, and `passheadingwrite.cpp:248` and `321`. ANTS-5337 had to remove a 50-line cap from two of them. This comment names `flipPassStatus` but not `annotatePass`, so a stranger changing the span would miss one copy. The shared `isPassBlockEnd` keeps the risk low. Fix: one helper that returns the b…

Quote: `// PassHeadingWrite::flipPassStatus scans the same span and must.`

### Row 681 — Low — `src/roadmapparse.cpp:867`

review-readability, lane 14-roadmap-parse-render-a, Q1. **Disposition:** queued, ANTS-6611.

**The "H2/H3 starts a section, slug it in document order" walk is written three times:** here, at `roadmapparse.cpp:2386`, and at `roadmapindex.cpp:103`. INV-3 in `roadmapindex.h` and the comment at 2373 say these must agree. The shared helpers `headingLevel` and `uniqueSlug` hold the rule, but not which levels count. Fix: one `isSectionLevel` predicate shared by all three.

Quote: `if (level == 2 || level == 3) {`

### Row 689 — Medium — `src/roadmapfoldin.cpp:310-313`

review-readability, lane 15-roadmap-parse-render-b, Q1. **Disposition:** queued, ANTS-6611.

This is the private `sniffRe`, one of the hand-kept copies of the id-prefix grammar. ANTS-3492 had to widen it alongside eight other sites (spec ANTS-3492 "Surface — sites WIDENED", row 2). Meanwhile `isValidIdPrefix` (header lines 152-154) claims to "single source the shape … so the digit-led-prefix rule can never drift". The copies already differ: `RoadmapParse::isIdPrefix` caps a prefix at 16 …

Quote: `// ANTS-3492 — digit-led-but-letter-containing prefix (3D_E-0042).`

Remedy: build `sniffRe` from a prefix fragment exported by RoadmapParse, or limit the header's "single source" claim to validation.

### Row 690 — Low — `src/roadmaprender.cpp:313`

review-readability, lane 15-roadmap-parse-render-b, Q1. **Disposition:** queued under ANTS-6215.

The comment says the legend uses "the export's kStatusOrder". In fact it uses its own copy (render.cpp:320) of export.cpp:39's array. A past fix had to check both: docs/specs/ANTS-4977-dropped-status.md:62 adds `dropped` to the render's array, and :155 checks the export's. A stranger adding a sixth status would believe one list governs both.

Quote: `// kStatusOrder, so two renders of one store agree (INV-7) and a re-load`

Remedy: share one status-order list, or say plainly that the render keeps its own copy.

### Row 692 — Low — `src/roadmaprender.cpp:396`

review-readability, lane 15-roadmap-parse-render-b, Q1. **Disposition:** queued, ANTS-6611.

The format marker is spelled three times in this file: in full at 343, and as a bare substring at 356 and 396. The parser's detector (roadmapparse.cpp:1788) matches the full `ants-roadmap-format: 1`. A stranger bumping the marker version changes 343 and the parser. The render's two substring tests would then still treat an old-version marker as present, and would not add the new one.

Quote: `if (lines.at(i).contains(QLatin1String("ants-roadmap-format")))`

Remedy: one constant for the marker token, shared by `formatMarker()` and both head scans.

### Row 695 — High — `src/roadmapdialog.cpp:958`

review-readability, lane 16-roadmap-dialog, Q1. **Disposition:** queued under ANTS-6215.

The five statuses are hand-listed in about ten places: - `kStatusLabels` (80-86) and `bulletPayload` (341-345) - `filterFor` (507-524) and `kStatusBits` (530-537) - `passesFilter` (954-958 and 965-967), `SectionCounts` (992-1005) and its tally (1014-1018) - the column-head emoji list (1234-1235) and the per-section count list (1262-1263) - the checkbox block (1997-2021), `statusBoxes` (3142-3143)…

Quote: `else if (rec.status == QString::fromUtf8(kEmojiDropped) && wantDropped) statusOk = true;`

Remedy: drive every list from one status table (emoji, bit, key, label, count field). Cost: pinned — anchor `tr(\"✅ Shipped\")` (line 60) pins the checkbox label the table would take over, and anchor `.rm-count{text-align:center;white-space:nowrap;` (line 10) pins the count-column CSS. Searched: anchors fi…

### Row 696 — Medium — `src/roadmapdialog.cpp:216`

review-readability, lane 16-roadmap-dialog, Q1. **Disposition:** queued, ANTS-6610.

The grid's column layout is stated twice. `kColLabel` and `kKindSpan` are worked out from `kCountColumns`, but `kColSummary`, `kColDateKind`, `kColDate` and `kGridColumns` (216-219) are fixed numbers that only work while `kCountColumns == 5`. Adding a status column (the change above) makes `kColLabel` 7, the same as `kColSummary`. Nothing fails to build; the grid in `applyCardColumnGrid` and `emi…

Quote: `constexpr int kColSummary = 7;`

Remedy: compute the later columns from `kColLabel`. Cost: one edit — Searched: anchors file for `kColSummary|kGridColumns|kKindSpan` (no hit); `docs/` for `kColSummary|kCountColumns` (no hit).

### Row 697 — Medium — `src/roadmapdialog.cpp:2217`

review-readability, lane 16-roadmap-dialog, Q1. **Disposition:** queued, ANTS-6610.

This confirms dup:7472797f297a, and the copies have drifted. `DialogChrome::install` already calls `applyTheme` (dialogchrome.cpp:218), which sets these dialog palette roles, `PlaceholderText` and the title-bar colours (dialogchrome.cpp:161-185). `roadmapdialog.cpp` sets them again at 2217-2226 (and the title bar at 2244-2247), and `roadmapshortcutsdialog.cpp:58-70` sets them a third time with ex…

Quote: `QPalette dp = palette();`

Remedy: delete the dialog-level palette and title-bar re-application in both dialogs; keep only the per-widget palettes (viewer, contents list, table); move any role that is really needed into `applyTheme`. Cost: one edit — Searched: anchors file for `setPalette` (no hit). `dup:7472797f297a`

### Row 710 — Medium — `src/claudeintegration.cpp:172`

review-readability, lane 17-claude-integration-a, Q1. **Disposition:** queued, ANTS-6590.

The check that the connecting process belongs to the same user exists 3 times: hook socket (172–187), tool socket (1378–1393), and `RemoteControl::onNewConnection` (remotecontrol_terminal.cpp, from line 182). Fix ANTS-1797 ("fail closed when there is no file descriptor") had to be applied to all three; each copy carries the same comment. They have drifted: only the RemoteControl copy logs why it …

Quote: `const qintptr fd = socket->socketDescriptor();`

Remedy: move the check into one shared helper that returns accept or refuse, and call it from all three sockets.

### Row 711 — Medium — `src/mcptoolregistry.cpp:515`

review-readability, lane 17-claude-integration-a, Q1. **Disposition:** queued, ANTS-6590.

The "audit finished" reply is built twice: by the synchronous `audit_run` completion (467–608) and by `auditJobPollEnvelope` in claudeintegration_callpolicy.cpp. The two have drifted. Past fixes ANTS-3585 and ANTS-3706 patched both; ANTS-4371 (`paths_given_total`, `tools_with_no_files`), ANTS-3710 (`exclude_paths_*`) and ANTS-1870 (`delta`) patched only the synchronous one. Nothing beside the syn…

Quote: `env["tools_with_no_files"] = noFilesTools;`

Remedy: build the done reply in one function from the `RunResult`, and call it from both paths. This is already roadmap item ANTS-6221.

### Row 712 — Medium — `src/claudeintegration.cpp:2436`

review-readability, lane 17-claude-integration-a, Q1. **Disposition:** queued, ANTS-6590.

Lines 2435–2437 type out the arguments every tool accepts (`caller_cwd`, `encoding`, `fields`). `mcp::isUniversalDispatchArg` (mcpprojection.cpp:674) keeps the same list by hand. The header (h:366–368) says this set exists so `doc_facts` and `ignored_args` "can never disagree", but these two copies can. ANTS-4524, which made `fields` universal, is the kind of change that edits one of them. They m…

Quote: `accepted.insert(QStringLiteral("encoding"));`

Remedy: build the published set from `isUniversalDispatchArg` (or a list it shares), not from typed names.

### Row 717 — Low — `src/claudeintegration.cpp:2501`

review-readability, lane 17-claude-integration-a, Q1. **Disposition:** queued, ANTS-6590.

`kArgSummary` holds short summaries for arguments that `kWireHidden` (2706–2725) removes from the wire right after the summary is applied (2777–2779). About 21 such entries never reach any caller: 13 for `workspace_search`, plus `file_outline/etags`, `file_outline/max_bytes`, `read_region/call_sequence`, `read_region/tail_lines`, `read_region/max_bytes`, and others. Someone editing one will see n…

Quote: `{QStringLiteral("workspace_search/timeout_sec"), QStringLiteral("ripgrep time budget in seconds (1-30, default 5).")},`

Remedy: drop the entries for hidden arguments, or note at `kArgSummary` that hidden arguments' entries are unused.

### Row 725 — Medium — `src/claudeintegration_reports.cpp:476`

review-readability, lane 18-claude-integration-b, Q1. **Disposition:** queued, ANTS-6590.

The changelog_log description lists five ops. Two more, `release` and `add_subsection`, are in the op list the input schema accepts (`clOpEnum`, lines 641–642) and in the handler (`remotecontrol_changelog.cpp:899-902`). The long `detail` text (494–621) leaves them out too. Someone adding or renaming an op will update the accepted list and the handler and miss both prose lists.

Quote: `"ops: add (default) | add_from_roadmap "`

Remedy: add `release` and `add_subsection` to the description's ops list and to `detail`.

### Row 726 — Medium — `src/claudeintegration_orientation.cpp:342`

review-readability, lane 18-claude-integration-b, Q1. **Disposition:** queued, ANTS-6590.

The spec_log description names set_status, append_loop and append_inv. It leaves out `append_status`, which the op list (line 407) and the handler (`remotecontrol_feedback.cpp:1184`) accept and the `detail` text describes. This is the same pattern as the changelog_log finding above.

Quote: `"markdown. op:\"set_status\" rewrites **Status:**; "`

Remedy: add append_status to the description.

### Row 727 — Medium — `src/claudeintegration_codenav.cpp:892`

review-readability, lane 18-claude-integration-b, Q1. **Disposition:** queued, ANTS-6590.

The file_outline description, and the `detail` language list at 904–907, both omit shell. Shell is in `modeEnum` (line 984) and in `FileOutline::parseMode` (`fileoutline.cpp:1301`). The comment at 981 already records the schema being "left behind" three times; the prose is the copy left behind this time.

Quote: `"Read. Languages by extension: cpp/c/glsl, py, md, json, html "`

Remedy: add `sh` to both prose lists.

### Row 729 — Medium — `src/claudeintegration_orientation.cpp:2052`

review-readability, lane 18-claude-integration-b, Q1. **Disposition:** queued, ANTS-6590.

This holds only for `description` and `detail`. Three other strings in the same descriptor still list the audit tools by hand: - `pathsProp`, 2240–2243: "cppcheck, clazy, clang-tidy, ruff, bandit, semgrep, shellcheck, mypy … gitleaks and trivy". - `exclProp`, 2265–2269. - `scopeProp`, 2169: "gitleaks/trivy skip". Separately, `toolsProp` writes "EXCEPT mypy" by hand while its count comes from `kAu…

Quote: `// this text cannot drift from what the runner runs.`

Remedy: narrow the comment to the two strings it covers, or build the per-tool lists from AuditRunner.

### Row 730 — Medium — `src/claudeintegration_orientation.cpp:515-521`

review-readability, lane 18-claude-integration-b, Q1,Q5. **Disposition:** queued, ANTS-6590.

Every descriptor repeats the same closing lines by hand: set properties, set required, maybe set `additionalProperties:false`, attach the schema, append. The copies differ on that third step, and no comment says why. Without `additionalProperties:false`: - spec_log (orientation 402–521) - workspace_search (codenav 352–731) - file_outline (codenav 961–1116) - mutation_probe (codenav 1231–1341) - r…

Quote: `schema["required"] = req;`

Remedy: one helper that closes a schema, with an explicit, commented opt-out for each tool that deliberately stays open.

### Row 731 — Low — `src/claudeintegration_orientation.cpp:893`

review-readability, lane 18-claude-integration-b, Q1. **Disposition:** queued, ANTS-6590.

Both `runner` enums come from `TestRunners::names()` (961, 1099). The runner names are still hand-written in five places: test_results description, `detail` (903–905) and selection_hint (921–922), and focused_test description (1023) and `detail` (1046–1047). Adding a runner updates both enums and none of the five prose lists.

Quote: `"duration_ms?}, parses ctest, pytest, jest, vitest, cargo test or "`

Remedy: build the prose lists from `TestRunners::names()`, as audit_run does with `kKnownTools()`.

### Row 732 — Low — `src/claudeintegration_orientation.cpp:2478`

review-readability, lane 18-claude-integration-b, Q1. **Disposition:** queued, ANTS-6590.

The default 12 and the [4, 30] bounds are copied five times in this descriptor: lines 2382, 2401, 2478–2480 and 2482. The sources are `testauditengine.h:104` and `kChunkSizeMin`/`kChunkSizeMax` in `testauditengine.cpp:95-96`. The comment at 2470–2475 names those sources, so someone editing here is warned. Someone changing the engine is not.

Quote: `chunkSizeProp["default"] = 12;`

Remedy: export the default and bounds from the engine header and build the schema and its text from them.

### Row 735 — Medium — `src/claudeintegration_sessions.cpp:885-886`

review-readability, lane 19-claude-integration-c, Q1. **Disposition:** queued, ANTS-6590.

The same "percent of the context window" sum appears four times: lines 885, 1108, 1125 and 1141. Three divide by the user's window setting (`m_contextWindowTokens`). This one divides by the fixed default (`kDefaultContextWindowTokens`). A stranger changing how the meter rounds or caps will fix whichever copy they find and leave the others behind. Nothing beside any copy points to the rest.

Quote: `snap.contextPercent = static_cast<int>(std::min<qint64>(`

Remedy: one private helper that turns tokens into a percent against `m_contextWindowTokens`, called from all four places; drop the unused `snap.contextPercent`.

### Row 736 — Medium — `src/claudeintegration_sessions.cpp:663`

review-readability, lane 19-claude-integration-c, Q1. **Disposition:** queued, ANTS-6590.

The whole-file size cap is written twice. `loadTranscript` keeps it as a local `kMaxTranscriptBytes` (line 624); `loadTranscriptTail` repeats the literal with the comment "Same whole-file cap as loadTranscript". Someone raising the cap in `loadTranscript` sees nothing there that points to the second copy.

Quote: `if (file.size() > 100LL * 1024 * 1024) return finish();`

Remedy: make the cap one file-level constant beside `kMaxTranscriptRecordBytes`.

### Row 737 — Medium — `src/claudeintegration_callpolicy.cpp:270-293`

review-readability, lane 19-claude-integration-c, Q1. **Disposition:** queued, ANTS-6590.

The async audit-poll reply (`auditJobPollEnvelope`) rebuilds the result fields the sync `audit_run` reply builds in `mcptoolregistry.cpp`. Two past fixes had to patch both copies, and the comments say so: ANTS-3585 "same richer surfaces as the sync provider" and ANTS-3706 "same detail sibling as the sync provider". The copies have already drifted: - The sync reply leaves out `incomplete_tools` wh…

Quote: `} else { // "done"`

Remedy: one function that writes the shared result fields from one result struct, called by both replies.

### Row 738 — Medium — `src/claudeintegration_store.cpp:2270-2276`

review-readability, lane 19-claude-integration-c, Q1. **Disposition:** queued, ANTS-6590.

Every tool's input schema ends with the same hand-copied block: set the properties, build `required`, maybe set `additionalProperties = false`, append the tool. Whether that strict flag is set varies by copy: - set: `roadmap_log`, `roadmap_migrate`, `session_message`, `know_how`, `run_trace`, `session_cost`, `spec_lint`; - not set: `feedback_query`, `feedback_log`, `audit_falsepos_log`, `audit_di…

Quote: `props["dry_run"] = makeDryRunProp();`

Remedy: one schema-finishing helper that takes the properties, the required list and an explicit strict flag.

### Row 742 — Low — `src/claudeintegration_store.cpp:1834-1837`

review-readability, lane 19-claude-integration-c, Q1. **Disposition:** queued, ANTS-6590.

`roadmap_migrate`'s two refusal lists have drifted apart: - `description` (1726-1731) has `mass_deletion`, `confirm_required` and `transient_root`, which `detail`'s closing list lacks; - neither list has `backup_failed` (raised by the `backup` property, line 2018) or `roadmap_exists` (raised by `init`, line 1935). A caller handling refusals from either list misses codes.

Quote: `"per-project. Refusals: "`

Remedy: make one list complete, and have the other refer to it.

### Row 750 — Medium — `src/claudestatuswidgets.cpp:388-389`

review-readability, lane 20-claude-session-widgets-a, Q1. **Disposition:** queued, ANTS-6591.

The "send `/model <tier>` with CR, then run the handshake" step is copied four times: 388-391 (chip click), 2130-2134 (auto-switch), 2455-2457 (deferred chip), 2776-2780 (undo). A past fix had to patch every copy: two copies carry "ANTS-1912 — `\r` not `\n` — see chip-click site" (2129, 2775). The header doc (h:297, above) is the copy that was missed. A stranger changing the terminator or the han…

Quote: `focused->sendToPty(`

Remedy: extract one `sendModelSwitch(term, tier)` that sends and starts the handshake, and call it from all four sites.

### Row 751 — Medium — `src/claudestatuswidgets.cpp:364-372`

review-readability, lane 20-claude-session-widgets-a, Q1. **Disposition:** queued, ANTS-6591.

The rule "is it safe to type `/model` into this tab now?" is hand-copied, and the copies differ: - chip click (364-372): Thinking, ToolUse, Compacting, or awaitingInput - undo (2759-2761): the same set - deferred fire (2430): not Idle, or awaitingInput - auto-switch (1908-1909): NotRunning, awaitingInput or Compacting, with the rest left to `decide()` The ANTS-2009 comment at 1902-1907 records a …

Quote: `const bool generating = (st == ClaudeState::Thinking ||`

Remedy: one named predicate (e.g. `canInjectModelSwitch(ShellState)`) used by the chip, undo and deferred paths, with the auto path's deliberate difference stated beside it.

### Row 752 — Medium — `src/claudestatuswidgets.cpp:2543-2545`

review-readability, lane 20-claude-session-widgets-a, Q1. **Disposition:** queued, ANTS-6591.

The cache-hit path re-implements the "halve the penalty in a mechanical session" rule inline instead of calling `ModelAutoSwitch::conservatismDwellMultiplier` (`src/modelautoswitch.cpp:226-240`, which holds the same `1.0 + penalty * 0.5` rule). The two copies already behave differently. If the cache was filled on a mechanical tick (1.5) and the next tick is not mechanical, the cache path returns …

Quote: `if (cached > 1.0 && isMechanical) {`

Remedy: cache the ledger inputs (measured, regret, floor) and call `conservatismDwellMultiplier` on every call.

### Row 756 — Medium — `src/claudestatuswidgets.cpp:1411`

review-readability, lane 20-claude-session-widgets-a, Q1. **Disposition:** queued, ANTS-6591.

The status label's padding string appears three times: 245, 979 and 1411. The elision maths at 1445 hard-codes `- 16` as "statusStyle's 8px padding each side". A stranger changing the padding in applyTheme would leave apply() overwriting it on the next state change and the elision room wrong.

Quote: `const QString statusStyle = QStringLiteral("padding: 0 8px; font-size: 11px;");`

Remedy: one named constant for the style, with the 16 derived from it.

### Row 757 — Low — `src/claudestatuswidgets.cpp:2233-2237`

review-readability, lane 20-claude-session-widgets-a, Q1. **Disposition:** queued, ANTS-6591.

"Send the continuation prompt plus `\r` after kSwitchContinuationDelayMs, guarded by QPointer" appears three times: 2233-2237, 2378-2384, 2411-2416. A change to the delay, the terminator or the guard would need three edits.

Quote: `if (!cont.isEmpty()) {`

Remedy: one `sendContinuationLater(term, cont)` helper.

### Row 758 — Low — `src/claudestatuswidgets.cpp:1052-1055`

review-readability, lane 20-claude-session-widgets-a, Q1. **Disposition:** queued, ANTS-6591.

pinSlots keeps its own copy of every state text that stateFace (161-232) produces. The only pointer to keep them in step is in the constructor comment (240-241). stateFace itself says nothing. A stranger adding a state there would miss this list, and the label would be cut off with "…". They match today ("waiting (88 bg)" matches `backgroundLabel`, `claudestateresolver.cpp:70`).

Quote: `for (const char *s : {"prompting", "planning", "auditing", "idle",`

Remedy: build the list from stateFace over each Display value, or put the "keep in step" note on stateFace.

### Row 759 — Low — `src/claudestatuswidgets.cpp:1690-1696`

review-readability, lane 20-claude-session-widgets-a, Q1. **Disposition:** queued under ANTS-6227.

"focused terminal → cwd → activeSessionPath" appears five times: 1098-1129, 1209-1222, 1690-1696, 1760-1766, 1932-1936. Each says "same pattern as" the previous one. A stranger changing how the focused transcript is found (as ANTS-5773 did at 1135-1137 for one copy) would have to find all five.

Quote: `// Resolve transcript path — same pattern as refreshTasksButton.`

Remedy: one `focusedTranscriptPath()` member.

### Row 760 — Low — `src/claudestatuswidgets.cpp:590`

review-readability, lane 20-claude-session-widgets-a, Q1. **Disposition:** queued, ANTS-6591.

The meter turns red above 80, but the tooltip (3004) says "Nearly full" at 80 and over. At exactly 80% the bar is amber while the tooltip warns. A stranger moving the threshold would change one of the two.

Quote: `if (percent > 80) chunkColor = th.ansi[1].name(); // red`

Remedy: one named threshold used by both.

Also reported as: "readability lane 20-claude-session-widgets-a Info correctness lead src/claudestatuswidgets.cpp:590"

### Row 770 — Medium — `src/claudebgtasks.cpp:60-76`

review-readability, lane 21-claude-session-widgets-b, Q1. **Disposition:** queued under ANTS-6228.

Several parts of the background-task tracker are copies of the same parts in `ClaudeTaskListTracker` (`claudetasklist.cpp:58-98, 147-236`, `.h:125-159`): - the constructor's debounce and watch hookup; - the destructor; - `setTranscriptPath`; - `rescan`'s before-read sampling of the change signal; - `poll`; - the `kRescanDebounceMs` member and `walkCache`. Past fixes had to patch both copies: ANTS…

Quote: `void ClaudeBgTaskTracker::setTranscriptPath(const QString &path) {`

Remedy: extract the bind / debounce / signal-sample / poll scaffold into one shared transcript-tracker base or template, with hooks for the per-tracker parse.

### Row 771 — Medium — `src/claudetabtracker.cpp:269-274`

review-readability, lane 21-claude-session-widgets-b, Q1. **Disposition:** queued, ANTS-6591.

The walker's rule is that a caller must call `canResume` and, on false, reset both cursor and accumulator (`claudetranscriptwalker.h:91-94`, `claudebgtasks.h:108-110`, `claudetasklist.h:92-94`). Each caller copies that rule by hand: - here; - `claudebgtasks.cpp:178-181`; - `claudetasklist.cpp:178-181`; - `claudebgtasks.cpp:576`, for helper transcripts. `parseIncremental` itself does not enforce i…

Quote: `if (!ClaudeTranscript::canResume(it->transcriptPath, it->bgCursor)) {`

Remedy: move the check-and-reset inside `parseIncremental`, or into one helper that every caller uses.

### Row 773 — Low — `src/claudebgtasks.cpp:392-403`

review-readability, lane 21-claude-session-widgets-b, Q1. **Disposition:** queued, ANTS-6591.

This flagged-launch block builds a `ClaudeBackgroundTask` (tool, description fallback, command, startedAt, append, index by tool_use id). It duplicates `startTask`'s new-task branch (316–324) in the same file. The two have diverged: the `startedAt` fallback and the way `kind` is set are different. A new field on the task would be set in one place and missed in the other.

Quote: `if (flagged) {`

Remedy: build the task in one helper that both call.

### Row 774 — Low — `src/claudebgtasks.cpp:112-114`

review-readability, lane 21-claude-session-widgets-b, Q1. **Disposition:** queued, ANTS-6591.

The "did the list shape change" check exists twice in this file, here and in `rescan` (193–201). A stranger widening what counts as a change, for example `outputPath`, which the dialog renders, edits one and misses the other. The task-list tracker has a third, deliberately different copy that also compares `subject`.

Quote: `same = next[i].id == m_tasks[i].id && next[i].finished == m_tasks[i].finished;`

Remedy: one private `sameShape(next)` helper.

### Row 777 — Medium — `src/claudeallowlist.cpp:66-93`

review-readability, lane 21-claude-session-widgets-b, Q1. **Disposition:** queued, ANTS-6591.

"Is this rule already present or covered?" is checked in three places, each with its own wording and severity: - this live-preview lambda: no duplicate check; "Already covered by: %1", not an error; - `onAddRule` (699–710): "Rule already exists", "Already covered by existing rule", as errors; - `prefillRule` (194–205): "Already in the Allow list", "Already covered by existing rule", not errors. A…

Quote: `connect(m_ruleInput, &QLineEdit::textChanged, this, [this](const QString &text) {`

Remedy: one `checkRuleAgainst(list, rule)` returning a hint and severity, used by all three.

### Row 778 — Low — `src/claudeallowlist.cpp:466-468`

review-readability, lane 21-claude-session-widgets-b, Q1. **Disposition:** queued, ANTS-6591.

`generalizeRule`'s `opRe` and `ruleSubsumes`'s `splitter` (578) split one Bash rule into segments in two different ways. Both comments say the two disagree on a quoted `|`. Neither says whether the disagreement is intended (each erring toward safety), or which one Claude Code's matcher actually follows. A stranger "fixing" one to match the other cannot tell which is right.

Quote: `// ruleSubsumes() splits on the same operators without the trailing`

Remedy: state in one place why each splitter differs, and name the matcher behaviour each one follows.

### Row 779 — Low — `src/claudeallowlist.cpp:669`

review-readability, lane 21-claude-session-widgets-b, Q1. **Disposition:** queued, ANTS-6591.

This companion-rule skip list and `generalizeRule`'s safety denylist (489–491: rm, sudo, SUDO_*) are two hand-kept lists of dangerous commands. A stranger adding, say, `dd` to one would miss the other.

Quote: `if (cmd != "cd" && cmd != "rm" && cmd != "sudo") {`

Remedy: one shared `isDangerousCommand` list.

### Row 789 — Low — `src/claudetranscript.cpp:134-141`

review-readability, lane 21-claude-session-widgets-b, Q1. **Disposition:** queued, ANTS-6591.

This hand-rolls getting the text out of a user content array, which `ClaudeContent::toText` already does, and the two have diverged. Here text blocks are concatenated with no separator; `toText` joins them with a newline. A stranger fixing how text is joined fixes one copy.

Quote: `QJsonArray contentArr = msg.value("content").toArray();`

Remedy: call `ClaudeContent::toText`, keeping the "only tool results" check.

### Row 791 — High — `src/remotecontrol.cpp:701`

review-readability, lane 22-mcp-transport, Q1. **Disposition:** queued under ANTS-6232.

The ID-prefix rule exists as three identical literals: here, `src/roadmapparse.cpp:1840` and `src/claudeintegration_store.cpp:1262`. The comment at 697-699 says to "keep the two literals in sync", so a stranger widening the prefix rule edits two and misses the third. ANTS-3492 already had to patch every copy.

Quote: `QStringLiteral("^(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]{0,15}$"));`

Remedy: keep one shared regex and have all three sites call it.

### Row 792 — High — `src/remotecontrol.cpp:712`

review-readability, lane 22-mcp-transport, Q1. **Disposition:** queued, ANTS-6606.

The rule "guess the prefix from the first four letters of the folder name" is in four places: `rlLeafDirPrefix` here, `remotecontrol_roadmap_log.cpp:2549`, `remotecontrol_roadmap_log_batch.cpp:948` and `roadmapmigrateload.cpp:913`. The copies already differ: the loader has no `"ROOT"` fallback for an empty folder name. The comment at 703-705 says the rule is "the same source op:flip already uses"…

Quote: `QString pfx = leaf.left(4).toUpper();`

Remedy: have the flip sites and the loader call one helper.

### Row 793 — High — `src/remotecontrol.cpp:282`

review-readability, lane 22-mcp-transport, Q1. **Disposition:** queued under ANTS-6231.

Three upward folder searches exist: `findRoadmapUnder` (162), `findChangelogUnder` (285) and `findYamlChangelogUnder` (313). ANTS-3350, ANTS-4447 and ANTS-6393 each had to patch more than one of them. ANTS-5667's `repo_root` redirect (163) is applied only in the roadmap search. The comment says the changelog search is "mirroring findRoadmapUnder", which it no longer does. Separately, the Markdown…

Quote: `// ANTS-3350 — walk up to the repo boundary, mirroring findRoadmapUnder,`

Remedy: one search helper that applies `repoRootOf` for all three callers, or a comment on the changelog search saying why it skips it.

### Row 794 — Medium — `src/remotecontrol.cpp:674`

review-readability, lane 22-mcp-transport, Q1. **Disposition:** queued under ANTS-6232.

The counter-ID rule is written out at 674 and again, identically, at 772. Both carry the same ANTS-3492 note, so that fix patched both. The rule at 1787 (`kCanonical`) also leaves out `S?`. The ants-v1 walker (2213) and `RoadmapParse::idTokenPattern` (roadmapparse.cpp:1753) both accept `-S?\d`. ANTS-4500 added `S?` to the read path and the walker only. Nothing in the code says whether leaving it …

Quote: `"^((?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*)"`

Remedy: build each from one shared pattern, or comment at each copy why S-IDs are excluded.

### Row 795 — Medium — `src/remotecontrol.cpp:1891`

review-readability, lane 22-mcp-transport, Q1. **Disposition:** queued, ANTS-6606.

This is a copy of `RoadmapParse::extractBoldId` (roadmapparse.cpp:117-132). The ANTS-1937 note says the write copy had to be brought into line with the read copy. `rcGfmCanonicalHeadline` (1926-1938) is likewise a copy of `splitOnEmDash` (roadmapparse.cpp:203). The next read-path change to either needs patching twice.

Quote: `bool rcExtractBoldId(const QString &lineHead, QString *id) {`

Remedy: export `extractBoldId` and `splitOnEmDash` from roadmapparse.h and call them.

### Row 796 — Medium — `src/remotecontrol.cpp:1552`

review-readability, lane 22-mcp-transport, Q1. **Disposition:** queued, ANTS-6606.

The rule "a bullet's body ends at the next non-blank line that starts in column 0" is written three times: `appendBodyNote` 1552-1562, `amendBodyExact` 1643-1648 and `rcScopeFencesToBullets` 2024-2028. The ANTS-3696 note (1550-1551) records a fix that had to make two of them agree. Both header comments still give the old rule, "terminated by a blank line" (1525-1527 and 1615-1616), contradicting …

Quote: `while (insertAt < lines.size()) {`

Remedy: one shared "end of body" helper, and correct both header comments.

### Row 815 — High — `src/remotecontrol_roadmap_log.cpp:2795`

review-readability, lane 23-mcp-roadmap-query-log-a, Q4. **Disposition:** queued, ANTS-6604.

The comment claims set_body and amend_body cannot drift, but `rlSetBodyStore`'s mutate (2825–2840) is a separate copy of amend_body's (3595–3613). Nothing is shared. A stranger who trusts it and changes amend_body's body write will miss set_body's copy. They will also miss `rlCommitBodyEdits` (4321–4335) and `rlStoreFlipOrAnnotate` (1466–1485), which repeat the same steps. The two reply-building …

Quote: `// mutate with the match removed, so the two cannot come to disagree about what`

Remedy: have amend_body's store path call one shared body-write helper (`rlSetBodyStore` or `rlCommitBodyEdits`), or correct the comment to name every copy.

### Row 816 — High — `src/remotecontrol_roadmap_log.cpp:1773`

review-readability, lane 23-mcp-roadmap-query-log-a, Q1. **Disposition:** queued under ANTS-6215.

Flip has its own hand-written status word-to-emoji table (1765–1774). `kRcStatuses` in `remotecontrol.cpp` says *"a new status is added here once"*, but there are other copies: - flip's table, and its error text (1777–1782); - replace_text's `kStatuses` (4553–4556); - append's error text (274–276). The ANTS-4977 tag shows a past fix had to patch flip's copy separately; `docs/specs/ANTS-4977-dropp…

Quote: `else if (toStatusResolved == QStringLiteral("dropped") || // ANTS-4977`

Remedy: build flip's table and replace_text's list from `kRcStatuses` (`rcStatusEmoji`/`rcStatusWord`).

### Row 823 — Medium — `src/remotecontrol_roadmap_log.cpp:1239`

review-readability, lane 23-mcp-roadmap-query-log-a, Q1. **Disposition:** queued, ANTS-6604.

The comment says "the same rankers", but there are three headline rankers and they have drifted apart: - `rlStoreSuggestions` (1268–1282) uses shared-prefix ranking with `stable_sort`; - the ants-v1 file branch (2238–2264) uses shared-prefix ranking with plain `sort`; - the GFM branch (2425–2474) uses token-Jaccard, which ANTS-3378 adopted because shared-prefix "surface[d] unrelated bullets". The…

Quote: `// ANTS-4485 § 4.5 — the same rankers, over the STORE's items: on a store-backed`

Remedy: one shared ants-v1 locate helper and one headline-suggestion ranker.

### Row 824 — Medium — `src/remotecontrol_roadmap_log.cpp:2097-2121`

review-readability, lane 23-mcp-roadmap-query-log-a, Q1. **Disposition:** queued, ANTS-6604.

Flip builds its reply in four hand-written copies: ants-v1 dry run and write (2097–2121, 2136–2170), and GFM dry run and write (2622–2648, 2676–2708). They have drifted: - the dry-run copies omit `note_scrubbed_params` and `post_bullets`, which the store path emits even on a dry run; - the GFM copies have no `format` key, which ants-v1 and the store path both set. amend_body already shows the fix…

Quote: `if (dryRun) {`

Remedy: one envelope lambda per format, taking a preview flag.

### Row 825 — Medium — `src/remotecontrol_roadmap_log.cpp:427`

review-readability, lane 23-mcp-roadmap-query-log-a, Q1. **Disposition:** queued under ANTS-6243.

The "resolve the write target and map its refusal" block (`why`, `seamErr`, `fromMemory`/`fromFile`, `roadmapWriteTarget`, `rcRoadmapSourceRefused`) appears 7 times in this file: 427, 1720, 1959, 2050, 3053, 3110, 3306. The batch file has more. The pass-headings probe (225–249, 1712–1731) and the caller_cwd→roadmap→read prologue (291–311/897–904, 1880–1905, 3017–3040) are copied the same way, and…

Quote: `RoadmapSource::ReadError why = RoadmapSource::ReadError::None;`

Remedy: one helper returning target-or-refusal from (callerCanonical, text).

### Row 826 — Medium — `src/remotecontrol_roadmap_log.cpp:922`

review-readability, lane 23-mcp-roadmap-query-log-a, Q1. **Disposition:** queued, ANTS-6604.

The unknown-section refusal differs between single append and its two batch copies (`_batch.cpp` 1832–1847, 2534–2550): - single append trims and sanitises the echoed slug and returns ranked `candidates[]` (ANTS-4556); - the batch copies echo the raw `section` and return a bare `bad_section`. A stranger following ANTS-4556's own comment ("never a second copy of [the ranker]") assumes every append…

Quote: `QString verbatim = section;`

Remedy: one section-lookup refusal helper shared by all three.

### Row 831 — Low — `src/remotecontrol_roadmap_log.cpp:2284`

review-readability, lane 23-mcp-roadmap-query-log-a, Q1. **Disposition:** queued, ANTS-6604.

Flip's unrecognised_format reply is an inline copy of `rlUnrecognisedFormat` (59–76), which a past fix (ANTS-1463) had to align.

Quote: `// ANTS-1463 — the same path, bytes, hint and expected_format fields as`

Remedy: call `rlUnrecognisedFormat`, with an op-specific error text.

### Row 832 — Low — `src/remotecontrol_roadmap_log.cpp:1157`

review-readability, lane 23-mcp-roadmap-query-log-a, Q1. **Disposition:** queued, ANTS-6604.

The open/write/commit block and its two `roadmap_write_failed` messages appear four times (1158–1169, 2124–2135, 2662–2673, 3800–3811). `rlWriteCounter` already exists for the counter-file equivalent.

Quote: `// Write ROADMAP.md atomically via QSaveFile.`

Remedy: an `rlWriteRoadmap(path, text)` helper.

### Row 833 — Low — `src/remotecontrol_roadmap_log.cpp:1598`

review-readability, lane 23-mcp-roadmap-query-log-a, Q1. **Disposition:** queued, ANTS-6604.

The four-line `note_scrubbed_params` array build is repeated 7 times (1598, 2157, 2696, 2872, 3449, 3656, 3788). `QJsonArray::fromStringList` is used elsewhere in this file.

Quote: `if (!noteScrubbedNames.isEmpty()) {`

Remedy: one `rlSetScrubbedParams(env, names)` helper.

### Row 834 — Low — `src/remotecontrol_roadmap_log.cpp:1644`

review-readability, lane 23-mcp-roadmap-query-log-a, Q1. **Disposition:** queued, ANTS-6604.

The local refusal lambda is redefined in append, `rlStoreFlipOrAnnotate`, flip, amend_body and amend_section. `mcp::refusal` (`mcprefusal.h`, ANTS-2216: "in one place") does the same job and is already used at 3832 and 3970. Append's copy has drifted: it adds an `example`.

Quote: `auto rlErr = [](const QString &code, const QString &message) {`

Remedy: use `mcp::refusal`, keeping append's example as a wrapper.

### Row 835 — Low — `src/remotecontrol_roadmap_log.cpp:4072`

review-readability, lane 23-mcp-roadmap-query-log-a, Q1. **Disposition:** queued, ANTS-6604.

The `op_unsupported`→`unsupported_format` remap is copied into 5 handlers (4071, 4167, 4404, 4565, 4719), and into more files.

Quote: `// Remapped for cmdRoadmapLogRender's reason: the shared prologue says`

Remedy: one remap helper taking the op's message.

### Row 841 — High — `src/remotecontrol_roadmap_query_verb.cpp:3253`

review-readability, lane 24-mcp-roadmap-query-log-b, Q1. **Disposition:** queued under ANTS-6129.

Section mode builds its rows with a hand copy of `rcBuildBulletCacheArray` (lines 45-82), and the two have drifted apart. The copy never calls `rcMaybeEmitEvidence` (line 69), so rows from a `section=` query have no `evidence` field. The comment at line 40 says the section copy differs only in "its own body cap and slug". A stranger adding a row field would add it to one builder and trust that co…

Quote: `for (const auto &b : bullets) {`

Remedy: one row builder that takes the body cap and an optional slug override. Cost: pinned — anchors `rcBuildBulletCacheArray` (anchor lines 253, 333) and `o["section_slug"] = b.sectionSlug` (277); specs ANTS-1881, ANTS-1437 and ANTS-1922 name `cmdRoadmapQuery`. Confirms dup:c6ae26a93878 and dup:40d23a66f…

### Row 842 — High — `src/remotecontrol_roadmap_query_verb.cpp:3428`

review-readability, lane 24-mcp-roadmap-query-log-b, Q1. **Disposition:** queued, ANTS-6604.

The bullet cache is filled by four copies of the same code: lines 2389-2401, 2420-2442, 2830-2847 and 3423-3441. Each copy is `roadmapBullets`, then `rcBuildBulletCacheArray`, then `rcComputeDuplicateIds`. Each carries its own ANTS-1646 note, so that fix had to be applied once per copy. This comment counts "the other two" when there are three. The next derived cache field will be added to some co…

Quote: `// ANTS-3793 — same swap as the other two fill sites.`

Remedy: one local lambda that fills the cache, called from all four places. Cost: pinned — anchors `m_roadmapCacheBullets = rcBuildBulletCacheArray(bullets);` (253) and `rcComputeDuplicateIds` (310). Confirms dup:f98c5f73cfd7.

### Row 843 — Medium — `src/remotecontrol_roadmap_query_verb.cpp:3464`

review-readability, lane 24-mcp-roadmap-query-log-b, Q1. **Disposition:** queued, ANTS-6604.

The "unrecognised format" fallback (header inventory, else a refusal) is written twice: lines 2466-2490 and 3464-3496. The ANTS-4861 note appears on both, so a past fix had to patch both.

Quote: `// ANTS-4861 — never on a store-served read. A migrated project whose store`

Remedy: one helper that returns the inventory or the refusal. Cost: pinned — anchor `buildHeaderInventoryEnvelope` (179). Confirms dup:48e3d5622dc8.

### Row 844 — Medium — `src/remotecontrol_roadmap_query_verb.cpp:1006`

review-readability, lane 24-mcp-roadmap-query-log-b, Q1. **Disposition:** queued under ANTS-6242.

The status emoji are hand-typed in four places (lines 95-97, 899-903, 1006-1010, 2495-2497). Lines 463-465 of the same file already use `RoadmapParse::kEmojiPlanned` and its siblings. The list of status names is also kept by hand in four places: `statusName`, `kAcceptedStatusFilters`, `aggregateOf` and `keepStatus`. Adding `dropped` (ANTS-4977) had to touch at least three of them (lines 903, 944,…

Quote: `static const QString plannedEmoji = QString::fromUtf8("\xF0\x9F\x93\x8B"); // 📋`

Remedy: use the `RoadmapParse` constants and one status table. Cost: pinned — anchors `plannedEmoji` (302), `kAcceptedStatusFilters` (240) and `keepStatus(s)` (244).

### Row 845 — Medium — `src/remotecontrol_roadmap_query_verb.cpp:1620`

review-readability, lane 24-mcp-roadmap-query-log-b, Q1. **Disposition:** queued, ANTS-6604.

Which arguments each mode accepts is spread over separate hand-kept lists: - the mode list `kModes` (line 1538) - `bullet_fields` (1620-1624) - `body_match` (1809-1813) - `version` (1844-1847) - the shipped-date window (1911-1920) - the ledger exclusion list (2015-2020) - a dozen separate combination guards (1672-1805) The newest mode, `ledger`, needed a reverse list of its own. Anyone adding a m…

Quote: `&& (mode == QLatin1String("headline_only")`

Remedy: one table mapping each mode to the arguments it accepts, checked once. Cost: pinned — anchor `static const QStringList kModes` (334); spec ANTS-1881 names `kModes`. Covers dup:dab54ceb2b41 and dup:f74db1a5df71.

### Row 846 — Medium — `src/remotecontrol_roadmap_query_verb.cpp:1169`

review-readability, lane 24-mcp-roadmap-query-log-b, Q1. **Disposition:** queued, ANTS-6604.

The "cut to 64 characters, replace control bytes with `?`" cleaning of echoed input is hand-copied in about nine places: - in this file: lines 963-967, 1053-1055 (200-character variant), 1169-1171, 1236-1238, 1280-1283, 1367-1370, 1547-1550 and 3097-3101 - in `remotecontrol_roadmap_query.cpp`: lines 166-170 Comments point at each other ("Same 64-byte + control-char hygiene the status / id echoes …

Quote: `if (kindArg.size() > 64) kindArg.truncate(64);`

Remedy: one sanitising helper. Cost: pinned — anchor `verbatim.at(i).unicode() < 0x20` (76). Confirms dup:641267cb2ae5.

### Row 847 — Medium — `src/remotecontrol_roadmap_query.cpp:712`

review-readability, lane 24-mcp-roadmap-query-log-b, Q1. **Disposition:** queued, ANTS-6604.

The Pass-heading write replies are built by hand in pairs: - dry-run vs real write in `cmdRoadmapLogPassAppend` (298-315 vs 321-336) - dry-run vs real write in `cmdRoadmapLogPassFlip` (706-721 vs 729-745) - refusal builders in five places (271-278, 406-412, 432-439, 647-656, 874-880) The ANTS-4116 and ANTS-4357 notes appear on both halves of each pair, so past fixes had to patch both. A new reply…

Quote: `out["file"] = QFileInfo(roadmapPath).fileName(); // ANTS-4116`

Remedy: one reply builder with a dry-run flag, and one refusal builder per operation name. Cost: pinned — specs ANTS-2126.md:197 and ANTS-4487-item-removal.md:64 name `cmdRoadmapLogPassAppend` and `cmdRoadmapLogPassFlip`. Confirms the query.cpp site of dup:0745ac399ba6, and dup:8f8fb646e4e2, dup:2a3d272950…

### Row 850 — Medium — `src/remotecontrol_roadmap_query_verb.cpp:1900`

review-readability, lane 24-mcp-roadmap-query-log-b, Q1. **Disposition:** queued under ANTS-5926.

The rule "the window ends at tomorrow by default" exists twice, and the copies disagree. The report copy (line 737) gets "today" from `RoadmapClock::today()` (line 2106). The comment at 2102-2105 explains why: reading the system clock directly makes results flake at period boundaries. This copy reads the system clock anyway.

Quote: `shippedUntil = QDate::currentDate().addDays(1);`

Remedy: use `RoadmapClock::today()` here too. Cost: one edit — Searched: anchors for `currentDate` and `shipped_until` (no hits).

### Row 858 — Low — `src/remotecontrol_roadmap_query.cpp:1853`

review-readability, lane 24-mcp-roadmap-query-log-b, Q1. **Disposition:** queued, ANTS-6604.

The error message lists the 21 accepted kinds by hand. That is a second copy of `RoadmapParse::canonicalKinds()` (`roadmapparse.cpp:1917`). Today the two match exactly.

Quote: `"the body declares Kind: %1, which is not one of "`

Remedy: build the list from `canonicalKinds()`, as the query side already does (verb 1159-1164). Cost: one edit — Searched: anchors for `implement, fix` and `accepted kinds` (no hits).

### Row 859 — Low — `src/remotecontrol_roadmap_query_verb.cpp:4318`

review-readability, lane 24-mcp-roadmap-query-log-b, Q1. **Disposition:** queued under ANTS-6129.

The list of operations in this message is kept by hand beside the dispatch chain at 4169-4315. I checked all 35 names and they match today.

Quote: `QStringLiteral("roadmap_log: unknown op \"%1\" — expected "`

Remedy: build the message from one operation table. Cost: pinned — `docs/standards/mcp-error-codes.md:88` names `cmdRoadmapLogDispatch`.

### Row 860 — Low — `src/remotecontrol_roadmap_query_verb.cpp:3066`

review-readability, lane 24-mcp-roadmap-query-log-b, Q1. **Disposition:** queued, ANTS-6604.

Four copies of the "could not open %1 for reading" refusal (2303-2308, 2448-2457, 3066-3075, 3144-3149). Two of them sit in identical index-filling blocks.

Quote: `if (m_roadmapIndex.isEmpty()) {`

Remedy: one helper. Cost: pinned — specs name `cmdRoadmapQuery`; anchors have no hit for the message text. Confirms dup:a9b06adf4b79.

### Row 861 — High — `src/remotecontrol_roadmap_log_batch.cpp:3089-3127`

review-readability, lane 25-mcp-roadmap-batch, Q1. **Disposition:** queued, ANTS-6603.

append_batch has two separate reply-building tails, one for the database route (3089–3127) and one for the file route (3214–3305), and both copy the single `op:append`. They have diverged. The database tail never adds the ANTS-4989 review-provenance advisory, which the file tail adds at 3270–3294 and single append adds on both its routes (`remotecontrol_roadmap_log.cpp:716` and `:1197`). The code…

Quote: `QJsonArray possibleDuplicates;`

Remedy: build both routes' advisory and post-bullet tail through one helper shared with single append.

### Row 862 — Medium — `src/remotecontrol_roadmap_backfill.cpp:368-386`

review-readability, lane 25-mcp-roadmap-batch, Q1. **Disposition:** queued under ANTS-6251.

The same "re-label `op_unsupported` as not-store-served" block appears four times: backfill 368–386, `publish.cpp:93-121`, `release.cpp:298-311` and `repair.cpp:146-162`. Only the wording passed in differs. Someone changing which refusal codes get re-labelled, or adding a fifth store-only op, will fix one copy.

Quote: `if (env.value(QStringLiteral("code")).toString() ==`

Remedy: one wrapper around `roadmapSectionOpTarget` that takes the op's lead text and advice and does the re-labelling.

### Row 864 — Medium — `src/remotecontrol_roadmap_release.cpp:48-55`

review-readability, lane 25-mcp-roadmap-batch, Q1. **Disposition:** queued, ANTS-6603.

The version ordering key and the "after the highest lower release, else before the lowest higher one" placement exist twice: `rlPlacementKey` plus `release.cpp:580-603`, and create_section's local `key` lambda plus `log_batch.cpp:1357-1398`. They have diverged. When there is no version-led section, release has an extra `else if (lastH2 >= 0) afterPos = lastH2;` that puts the new release at the en…

Quote: `// numbered patch of its minor. The same lambda create_section uses.`

Remedy: one shared placement helper used by both, with any intended difference named in a comment.

### Row 865 — Medium — `src/remotecontrol_roadmap_log_batch.cpp:1576-1589`

review-readability, lane 25-mcp-roadmap-batch, Q1. **Disposition:** queued, ANTS-6603.

"Step past the anchor's deeper sections" is written at least five times: - `createSectionSkipCount` (1244), used by create_section's database route (1576–1584) and file route (1662–1667) - move_section (4231–4236) - `rcSubtreeEnd` (4081–4088) - `release.cpp` `subtreeOf` (356–363) and its insert walk (613–619) The move_section, `rcSubtreeEnd` and release copies stop at a section in another roadmap…

Quote: `if (s.position <= afterRow->position)`

Remedy: one shared subtree-end helper that takes the file into account, used by all sites.

### Row 866 — Medium — `src/remotecontrol_roadmap_log_batch.cpp:1399-1420`

review-readability, lane 25-mcp-roadmap-batch, Q1. **Disposition:** queued, ANTS-6603.

The case-insensitive "bad_case, else bad_section" section lookup is copied at 1399–1420, 1829–1848, 2533–2551 and per bullet at 2757–2768 (plus `log.cpp:936-943` per the candidate). The per-bullet copy has diverged: it reports into `skipped[]` without `canonical_slug`, and it keeps the last case-only match where the others return on the first. Changing this refusal (adding ranked candidates as `r…

Quote: `const QString needCi = afterSection.toLower();`

Remedy: one helper that returns the slug match or the refusal fields.

### Row 867 — Medium — `src/remotecontrol_roadmap_log_batch.cpp:33-36`

review-readability, lane 25-mcp-roadmap-batch, Q1. **Disposition:** queued under ANTS-6215.

The status set is kept by hand in at least six places: - this table - `kRcStatuses` behind `rcStatusEmoji` (`remotecontrol.cpp:1076`) - three hand-written error lists, at 189–190, 436–437 and 738–739 - set_legend's `kStatuses` (3986–3989), plus its `kLegendLine` word list (3994) The ANTS-4977 tag shows the last added status had to be patched in. The next one will be added to the table and missed …

Quote: `{"considered", "💭"}, {"dropped", "🚫"}, // ANTS-4977`

Remedy: resolve through `kRcStatuses` and build the "expected …" text from it.

### Row 869 — Low — `src/remotecontrol_roadmap_log_batch.cpp:531-601`

review-readability, lane 25-mcp-roadmap-batch, Q1. **Disposition:** queued, ANTS-6603.

In flip_batch's locator loop, the id > headline > line_range matcher is written three times: over GFM bullets (532–549), over the ants-v1 fallback (557–571) and over ants-v1 (587–601). A change to matching (another locator key, say) must be made three times, and the fallback copy already differs by skipping `anchor`.

Quote: `if (!locId.isEmpty()) {`

Remedy: one local lambda that matches a locator against a bullet list.

### Row 870 — Medium — `src/remotecontrol_roadmap_log_batch.cpp:1712-1722`

review-readability, lane 25-mcp-roadmap-batch, Q1. **Disposition:** queued, ANTS-6603.

In flip_batch (1078–1095 vs 1138–1155), create_section (1679–1692 vs 1712–1723) and bundle_row (2186–2197 vs 2213–2221), the dry-run reply and the real reply are built separately. Past fixes stamped both copies: `// ANTS-4470, ANTS-5358` at 1085 and 1144, `// ANTS-5315 § 2.3a` at 1687 and 1718. They have diverged. The real create_section and bundle_row replies have no `op` key, while their dry-ru…

Quote: `out["bytes_written"] = bytesInserted;`

Remedy: build the reply once and add the dry-run-only keys conditionally.

### Row 873 — Low — `src/remotecontrol_roadmap_log_batch.cpp:2223-2236`

review-readability, lane 25-mcp-roadmap-batch, Q1. **Disposition:** queued, ANTS-6603.

[Q1] LOW `src/remotecontrol_roadmap_log_batch.cpp:2223-2236` (and 1253, 1741, 137; `backfill.cpp:391`, `repair.cpp:170`, `release.cpp:264`, `publish.cpp:191`, `log_batch.cpp:3323`) — `auto rlErr = [](const QString &code, const QString &message) {` — The `{ok:false, code, error}` reply builder is written nine times in this lane. `publish.cpp:187-190` explains its own local copy (not exporting a fi…

Remedy: one shared refusal builder in `remotecontrol_internal.h`.

### Row 874 — Low — `src/remotecontrol_roadmap_log_batch.cpp:223-241`

review-readability, lane 25-mcp-roadmap-batch, Q1. **Disposition:** queued, ANTS-6603.

The "caller_cwd → canonical path → find ROADMAP.md → read it" opening, with identical refusal texts, is repeated in flip_batch (223–241), create_section (1292–1312), bundle_row (1805–1825), append_batch (2368–2384 and 2512–2518), `roadmapSectionOpTarget` (3355–3399) and `publish.cpp:202-224`. A wording or code change needs six edits.

Quote: `const QString callerCanonical = QFileInfo(callerRaw).canonicalFilePath();`

Remedy: one resolver that returns the paths and text or a refusal.

### Row 875 — Low — `src/remotecontrol_roadmap_log_batch.cpp:1106-1116`

review-readability, lane 25-mcp-roadmap-batch, Q1. **Disposition:** queued, ANTS-6603.

The atomic write with its two `roadmap_write_failed` refusals is copied at 1107, 1696, 2201 and 3203. Each copy then reports `bytes_written` with a different meaning: create_section counts the inserted bytes, bundle_row counts one row, and flip_batch uses `rcSetWriteBytes` (before and after sizes). A reader would assume the same key means the same thing.

Quote: `QSaveFile rw(roadmapPath);`

Remedy: one write helper, and say in the reply what the byte count covers.

### Row 876 — Low — `src/remotecontrol_roadmap_log_batch.cpp:2138-2150`

review-readability, lane 25-mcp-roadmap-batch, Q1. **Disposition:** queued, ANTS-6603.

The "split a table row on unescaped pipes" code exists twice in bundle_row: `rowColumns` (2036–2052) and the sorted-insert loop. They have diverged. The loop never chops a trailing `|` and never appends the final `cur`, so a row without a trailing pipe loses its last cell.

Quote: `if (t.startsWith(QChar('|'))) t.remove(0, 1);`

Remedy: one splitter lambda used by both.

### Row 883 — Medium — `src/remotecontrol_workspace.cpp:4116`

review-readability, lane 26-mcp-state-workspace-a, Q1. **Disposition:** queued under ANTS-6253.

The project-root lookup is copied into every verb in four different forms: - **workspace_search and cited_by:** the `~global` sentinel, then the raw `caller_cwd`, then the provider, then the process cwd. - **file_outline, read_log, read_region, read_regions, apply_edits and codebase_index:** the sentinel, then `resolveRootCanonical(m_roots, req)`. - **mutation_probe and build_target_for:** `resol…

Quote: `const QString rootCanonical = QFileInfo(callerRaw).canonicalFilePath();`

Remedy: one shared root-resolution helper per intended policy, with a comment on each verb that deliberately differs.

### Row 884 — Medium — `src/remotecontrol_workspace.cpp:699`

review-readability, lane 26-mcp-state-workspace-a, Q1. **Disposition:** queued, ANTS-6605.

The parse-budget guard and the JSON-event start of each line handler exist three times: - workspace_search, 697–706; - cited_by, 1577–1586; - co_change_family, 4183–4190. The copies have drifted. Two use `& 0x7FF` against a clock started when the verb began; co_change_family uses `% 2048` against `2LL * kWorkspaceSearchHardKillMs` on a clock started just before rg. Its comment calls this "cited_b…

Quote: `if ((++scanCounter & 0x7FF) == 0 && wall.hasExpired(totalBudgetMs)) {`

Remedy: one named constant and one budget-check helper shared by the three handlers.

### Row 888 — Medium — `src/remotecontrol_workspace.cpp:3894`

review-readability, lane 26-mcp-state-workspace-a, Q1. **Disposition:** queued under ANTS-6259.

The "find this project's feedback file in a directory" step is written twice, and the copies have drifted. The parent-directory copy (3832–3857) tries the conventional names and then the ANTS-3439 normalised-stem match. The redirect to the configured root (3894–3906) tries the conventional names only. Someone fixing name matching in one copy would not know the other exists.

Quote: `for (const QString &name : feedbackConventionalNames(leaf)) {`

Remedy: one helper that resolves a leaf within a given directory, called for both.

### Row 897 — Low — `src/remotecontrol_workspace.cpp:252`

review-readability, lane 26-mcp-state-workspace-a, Q1. **Disposition:** queued, ANTS-6605.

The literal "max 30" appears in five hints (252, 920, 934, 1645, 1673), and "256 bytes" in two errors (355, 410). Each copies a constant (`kWorkspaceSearchMaxBudgetMs = 30000` and `kWorkspaceSearchGlobBytesCap = 256`, in remotecontrol_roadmap_log_batch.cpp:4447 and 4465). Raising the cap leaves the hints wrong.

Quote: `"raise timeout_sec (max 30), or narrow with lane= / glob=")`

Remedy: format each text from its constant.

### Row 898 — Low — `src/remotecontrol_workspace.cpp:1260`

review-readability, lane 26-mcp-state-workspace-a, Q1. **Disposition:** queued, ANTS-6605.

The hidden-path stamp exists four times (1027, 1047, 1082, 1260), each with its own test for "zero". The normal path tests `matches.isEmpty()`, which is also true when `offset` pages past real matches. The comment at 229–230 promises the four "cannot answer the same silence differently", but only the text is shared.

Quote: `if (matches.isEmpty() && hiddenGlobSkipped) {`

Remedy: decide it once from `nothingFound` inside `echoEffectiveArgs`, keeping the normal path's hint order.

### Row 899 — Low — `src/remotecontrol_workspace.cpp:2010`

review-readability, lane 26-mcp-state-workspace-a, Q1. **Disposition:** queued, ANTS-6605.

The `{ok:false, code, error}` refusal is built five ways in this file: - `wsErr`; - `mcp::refusal`; - the `mpErr` lambda; - the `argErr` lambda (3090); - about twenty hand-built objects, for example 1736, 2718 and 4333. Adding a field to every refusal means finding all five.

Quote: `auto mpErr = [](const QString &code, const QString &message) {`

Remedy: use `wsErr` (or `mcp::refusal`) everywhere.

### Row 900 — Low — `src/remotecontrol_workspace.cpp:3354`

review-readability, lane 26-mcp-state-workspace-a, Q1. **Disposition:** queued, ANTS-6605.

A third skip-row builder, beside `addSkip` and `addSkipNearMiss`. The comment at 3224–3227 lists "the other four call sites" and does not count this one.

Quote: `s["reason"] = QStringLiteral("count_mismatch");`

Remedy: route it through `addSkip` and add the extra fields after.

### Row 902 — Medium — `src/remotecontrol_state.cpp:599`

review-readability, lane 27-mcp-state-workspace-b, Q1. **Disposition:** queued, ANTS-6605.

The comment above says this list of "narrow" suffixes copies the one in `classifyAuditScope` "verbatim". The two lists already behave differently. The picker lower-cases the whole filename, while `classifyAuditScope` (538–541) matches case-sensitively on `baseName()`. So `-POSTFIX` counts as narrow in one place and not the other. A stranger adding a suffix trusts "verbatim", edits one list, and t…

Quote: `c.narrow = lower.contains(QStringLiteral("-postfix"))`

Remedy: one shared narrow-name check that both call. Cost: pinned. The anchors `pickForeignReport` (`test_mcp_last_audit_summary.cpp:916`) and `classifyAuditScope(` (`test_mcp_last_audit_summary.cpp:721`) pin both functions.

### Row 903 — Medium — `src/remotecontrol_state.cpp:2589`

review-readability, lane 27-mcp-state-workspace-b, Q1. **Disposition:** queued, ANTS-6605.

The comment says this skips the decisions index "as task_priors treats it". `task_priors` (3295–3296) skips it case-insensitively; this check is case-sensitive. A `readme.md` therefore counts as a decision record in `invariant_check` but not in `task_priors`. A stranger who trusts the comment won't look for the difference.

Quote: `if (dirRel == kDecisionsDir && fname == QLatin1String("README.md"))`

Remedy: one shared "is the ADR index" check. Cost: one edit. Searched: the anchors file for `README|scanOneDir` (no hit) and `docs/` for `README.md` near "decision". Only ANTS-1306 row 10 matched, which says "minus README.md" and names no function.

### Row 904 — Medium — `src/remotecontrol_state.cpp:4070`

review-readability, lane 27-mcp-state-workspace-b, Q1. **Disposition:** queued, ANTS-6605.

`cmdFindCaller` builds its closing fields twice, once at 4063–4086 for `files_only` and again at 4099–4119. Those fields are the lane echo, the definition with its optional body, the type hint and the scan fields. The comment at 4017 ("echoed on both emission paths below") and the `ANTS-3805` tags at 4070 and 4105 show a past fix had to patch both copies. The next added field will need two edits …

Quote: `if (!lane.isEmpty()) out["lane"] = lane; // ANTS-3805`

Remedy: build the shared fields once, before the branch. Cost: pinned. The anchor `RemoteControl::cmdFindCaller` (`test_mcp_symbol_query.cpp:573, 625, 637, 674`) scrapes this body, and `sqAttachBody` (`:617, :630`) is pinned too. Confirms dup:e8a206742ac1.

### Row 905 — Low — `src/remotecontrol_state.cpp:3804-3807`

review-readability, lane 27-mcp-state-workspace-b, Q1. **Disposition:** queued, ANTS-6605.

`focused_test` has two copies of "run, re-run the full suite on zero matches, refuse if the output is unrecognised". The ctest copy's `unrecognised_output` refusal has no `output_tail`; the other-runner copy (3598–3600) attaches one. Nothing explains the difference. Someone changing the refusal shape will fix one copy.

Quote: `if (!parsed.recognised) {`

Remedy: give both the same refusal, or comment why ctest omits the tail. Cost: pinned. The anchors `RemoteControl::cmdFocusedTest(` (`test_mcp_focused_test.cpp:250`) and `downgraded_to_full` (`:254`) pin this body.

### Row 906 — Low — `src/remotecontrol_state.cpp:3152-3156`

review-readability, lane 27-mcp-state-workspace-b, Q1. **Disposition:** queued, ANTS-6605.

The same "score descending, then sort key" ordering is written three times: at 3152, 3216 (as `stable_sort`) and 3318. A stranger changing how ties are broken for one bucket of results can miss the other two.

Quote: `std::sort(hits.begin(), hits.end(),`

Remedy: one named comparator on `TpScored`. Cost: pinned. The anchor `QJsonDocument RemoteControl::cmdTaskPriors(` (`test_mcp_task_priors.cpp:72`) pins the body. `docs/` has no mention of `TpScored`. Confirms dup:6e775f00724d and adds the third site.

### Row 907 — Low — `src/remotecontrol_state.cpp:333`

review-readability, lane 27-mcp-state-workspace-b, Q1. **Disposition:** queued, ANTS-6605.

`lanesAsJson` wraps the array in a `{lanes:[…]}` object. Its only caller throws the wrapper away. A stranger will assume that object shape is sent to callers somewhere and won't dare change it.

Quote: `QJsonArray lanesJson = lanesAsJson(lanes).value("lanes").toArray();`

Remedy: inline it, or make it return the array. Cost: one edit. Searched: `lanesAsJson` across the tree (definition at 231, one use at 333) and the anchors file (no hit).

### Row 917 — Medium — `src/remotecontrol_review.cpp:465`

review-readability, lane 28-mcp-review-verbs, Q1. **Disposition:** queued, ANTS-6602.

There are two copies of the "was this report truncated" test, and they measure different things: - The inline copy (line 481) compares `QString::size()`, which counts UTF-16 units. The comment at 478–480 says this was done deliberately, to match the engine's real cutoff. - The `reports_dir` copy compares the file's size in bytes. A stranger trusting that comment would assume both paths agree, but…

Quote: `if (fi.size() > IndieReviewEngine::kMaxScanBytes) {`

Remedy: measure the disk path in the engine's unit, and give the constant a name that says which unit it is.

### Row 918 — Medium — `src/remotecontrol_review.cpp:1405`

review-readability, lane 28-mcp-review-verbs, Q1. **Disposition:** queued under ANTS-6262.

The echo sanitiser (cut to 64 characters, replace control characters with `?`) has one helper here and many inline copies elsewhere: - remotecontrol_roadmap_query_verb.cpp:964, 1054, 1171, 1238, 1282, 1369, 1549, 3100 - remotecontrol_roadmap_query.cpp:169 - remotecontrol_roadmap_log.cpp:925 The `queryArg` copy at :1054 replaces control characters, but no 64-character truncation of `queryArg` turn…

Quote: `// INV-12). The roadmap verbs still inline the same loop (ANTS-6262).`

Remedy: route the roadmap copies through the helper (the ANTS-6262 item).

### Row 929 — High — `src/remotecontrol_docs.cpp:2290-2299`

review-readability, lane 29-mcp-content-verbs-a, Q1. **Disposition:** queued, ANTS-6601.

`cmdDocLint` builds its spec-checker inputs by hand, copying `cmdSpecLint` lines 975-992. ANTS-6315 (a past bug fix) already had to patch this copy to "agree with spec_lint" (CHANGELOG). It is still missing `opts.siblingInvNumbers`, which `cmdSpecLint` sets at 991. So the two copies have diverged, and the next spec_lint input will be missed here too. The same pair also repeats the `excludedNames`…

Quote: `opts.spec.testClauseCoverage = // ANTS-6315 — as spec_lint reads it`

### Row 930 — Medium — `src/remotecontrol_docs.cpp:345-369`

review-readability, lane 29-mcp-content-verbs-a, Q1. **Disposition:** queued, ANTS-6601.

Five doc verbs repeat the same opening by hand: - the "no focused project" refusal; - the `path` check with `validatePath`; - the `docPathNotFound` refusal; - the `docs_dir` default; - the `maxDocsPerRun` cut. The copies are at 133-150/176-187, 345-369, 926-950, 1395-1420 and 2177-2199. Four verbs (at 442-447, 772-777, 1019-1021 and 1446-1451) also repeat the per-document read: size cap, open, `r…

Quote: `if (check.resolved.isEmpty())`

### Row 931 — Medium — `src/remotecontrol_docs.cpp:233-234`

review-readability, lane 29-mcp-content-verbs-a, Q1. **Disposition:** queued, ANTS-6601.

The bounded directory walk in `docIntegrityEnumerate` copies the one in `docCitationsMdScan` (1688-1707). Its own comment says the same fix was applied to both ("as in docCitationsMdScan (ANTS-5098)", then ANTS-5623). The copies differ: the mdScan copy passes `QDir::NoSymLinks`, this one does not. Nothing says whether skipping symlinked `.md` files is meant for one walk only. Fix: one bounded mar…

Quote: `QDirIterator it(dirAbs, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,`

### Row 932 — Medium — `src/remotecontrol_terminal.cpp:204-237`

review-readability, lane 29-mcp-content-verbs-a, Q1. **Disposition:** queued, ANTS-6601.

The SO_PEERCRED check (it confirms the connecting program runs as the same user) exists in three copies. The other two are `ClaudeIntegration::onHookConnection` (claudeintegration.cpp:172-191) and `onMcpConnection` (1378-1393). ANTS-1797's fail-closed fix had to be written into all three, and each copy carries that comment. The copies have diverged: only this one logs why a peer was refused. Fix:…

Quote: `const qintptr fd = socket->socketDescriptor();`

### Row 936 — Low — `src/remotecontrol_terminal.cpp:668-680`

review-readability, lane 29-mcp-content-verbs-a, Q1. **Disposition:** queued, ANTS-6601.

Three copies pick the target terminal: `tab`, else `caller_cwd`, else the focused tab. They are `cmdGetText` (600-618), `cmdRecentErrors` (668-680) and `cmdLastSelection` (731-746). The 663 comment says the second "mirrors cmdGetText", but they refuse differently: - `get_text`: `not_found` via `mcp::refusal`, after ANTS-6379, with "no tab at index N". - `recent_errors` and `last_selection`: a han…

Quote: `target = m_main->terminalAtTab(tabVal.toInt());`

### Row 939 — Low — `src/remotecontrol_know_how.cpp:21-27`

review-readability, lane 29-mcp-content-verbs-a, Q1. **Disposition:** queued, ANTS-6601.

The same three-key refusal builder is written out by hand in `know_how`, `run_trace` and `session_message` (per the candidate). Copies with other prefixes exist in `versioncheck.cpp:17`, `quotationcheckverb.cpp:231` and `remotecontrol_ci.cpp:46`. `mcp::refusal(code, message)` (`mcprefusal.h:11`) already does this. Fix: call `mcp::refusal` with the prefixed message.

Quote: `QJsonDocument refuse(const char *code, const QString &message) {`

### Row 940 — Low — `src/remotecontrol_docs.cpp:2390-2394`

review-readability, lane 29-mcp-content-verbs-a, Q1. **Disposition:** queued, ANTS-6601.

`doc_dedup`'s pair and cluster wire format is written twice: `docDedupBuildResponse` (1470-1508) and `docLintBuildResponse` (2390-2413). The copies differ: doc_dedup always sends `pairs` and `clusters`, while doc_lint sends them only when non-empty, and no comment says why. A change to the cluster shape in one copy would miss the other. Fix: one shared serialiser.

Quote: `const auto passageObj = [](const DocDedup::Passage &p) {`

### Row 941 — Low — `src/remotecontrol_terminal.cpp:979-988`

review-readability, lane 29-mcp-content-verbs-a, Q1. **Disposition:** queued, ANTS-6601.

The resolve-the-widget-then-refuse block appears three times (`inject-key`, `inject-click`, `grab-image`). Only the verb name differs. Fix: fold the two refusals into a helper beside `e2eResolveTarget`.

Quote: `if (!widget.isEmpty() && !target)`

### Row 945 — Medium — `src/remotecontrol_changelog.cpp:1387-1457`

review-readability, lane 30-mcp-content-verbs-b, Q1. **Disposition:** queued, ANTS-6601.

In op `add`, the dry-run reply (1387-1416) and the write reply (1423-1457) are hand-kept twins. Past fixes have had to patch both: ANTS-4847 at 1397 and 1431, ANTS-5673 at 1405 and 1442, ANTS-4563 at 1402 and 1439. The same pattern repeats in: - `normalize` (973-989 vs 996-1013): these have already diverged; `changed` is `res.changed` in one and `true` in the other, and `bytes` vs `rcSetWriteByte…

Quote: `// ANTS-4563 — same contract on the write path as on the preview.`

Remedy: build the shared fields once per op, then add only the dry-run or write fields.

### Row 946 — Medium — `src/remotecontrol_changelog.cpp:1189-1197`

review-readability, lane 30-mcp-content-verbs-b, Q1. **Disposition:** queued, ANTS-6601.

Leaked tool XML is scrubbed in four places, but they disagree on whether the reply reports it. Op `add` and op `amend` report the dropped names as `scrubbed_params` (393-397, 1410-1414, 1452-1456). `add_subsection` scrubs into `bscrub` and `scrubbed` and never reports either. `resolveClBatchEntry` scrubs into a local (1555) that it throws away. A stranger extending scrubbing, or debugging a missi…

Quote: `QStringList bscrub;`

Remedy: report `scrubbed_params` on every op that scrubs, through one helper, replacing the three copies of the array-building block.

### Row 947 — Medium — `src/remotecontrol_feedback.cpp:1244-1308`

review-readability, lane 30-mcp-content-verbs-b, Q1. **Disposition:** queued, ANTS-6601.

spec_log's dry-run and write replies are twins. ANTS-6364 had to add `new_status` to both (1255 and 1304), and ANTS-4353/4364 `row_shape`/`row_order` to both (1260-1261 and 1307-1308).

Quote: `out["new_status"] = res.newValue; // ANTS-6364`

Remedy: build the shared reply once, then add `dry_run`/`bytes` or `rcSetWriteBytes`.

### Row 948 — Medium — `src/remotecontrol_changelog.cpp:420-426`

review-readability, lane 30-mcp-content-verbs-b, Q1. **Disposition:** queued, ANTS-6601.

`mcp::refusal` (src/mcprefusal.h:11) already builds the `{ok:false, code, error}` reply, and `remotecontrol_feedback.cpp` uses it. This lane still hand-rolls the same reply nine times: - `err` (changelog:420), which copies `clErr` in the same file (changelog:275) - `slErr` (feedback:1074) and `afErr` (feedback:2100) - `gitErr` (feedback:1324) and `refuse` (githooksreport.cpp:25), both of which ad…

Quote: `auto err = [](const QString &code, const QString &message) {`

Remedy: route each copy through `mcp::refusal`, keeping the `stderr` and prefix extras as wrappers.

### Row 955 — Low — `src/remotecontrol_changelog.cpp:1130-1132`

review-readability, lane 30-mcp-content-verbs-b, Q1. **Disposition:** queued, ANTS-6601.

`release` computes `bytes_written` by re-reading the file's size from disk and emits no `file_bytes`. Every other write op uses `rcSetWriteBytes` (bytes_written plus file_bytes, ANTS-3723), and add_batch hand-rolls the same pair (1829-1848). A caller reading `file_bytes` gets it everywhere except `release`.

Quote: `out["bytes_written"] =`

Remedy: use `rcSetWriteBytes` in `release` and add_batch.

### Row 957 — Low — `src/remotecontrol_session_message.cpp:146-150`

review-readability, lane 30-mcp-content-verbs-b, Q1. **Disposition:** queued, ANTS-6601.

The `unknown_project` refusal is worded three ways: - opSend (176-177) prefixes `session_message: ` - sendPreview (147-150) and opOutbox (261-263) do not, while every other refusal in the file is prefixed (`refuseStore`)

Quote: `QStringLiteral("no project is registered under "`

Remedy: add the prefix inside `refuseUnknownProject`.

### Row 958 — Low — `src/remotecontrol_feedback.cpp:1601-1605`

review-readability, lane 30-mcp-content-verbs-b, Q1. **Disposition:** queued, ANTS-6601.

The four git_state ops each repeat the same opening: resolve the root, then refuse with `bad_path` (1601, 1696, 1829, 2033). runDiffOp and runRenameSweepOp also repeat the range-validation refusal (1850-1853, 2044-2047).

Quote: `QStringLiteral("git_state: project root does not exist"));`

Remedy: a single function that resolves the root and returns either the root or the refusal.

### Row 959 — Low — `src/remotecontrol_feedback.cpp:205-215`

review-readability, lane 30-mcp-content-verbs-b, Q1. **Disposition:** queued, ANTS-6601.

`cmdFeedbackQuery` (206-215) and `cmdFeedbackLog` (713-722) have the same path-resolution preamble. ANTS-5500 had to add `derivedReason` to both.

Quote: `QString derivedReason; // ANTS-5500`

Remedy: have `resolveFeedbackPath` return one struct.

### Row 960 — Low — `src/githooksreport.cpp:218`

review-readability, lane 30-mcp-content-verbs-b, Q1. **Disposition:** queued, ANTS-6601.

`CiWorkflows::files` is documented as "what counts as a workflow file, for every reader of the directory" (ciworkflows.h:56-57). The directory path is still spelt out separately here and at ciworkflows.cpp:438, 444, 446, 470 and 495. Someone changing where workflows are read from edits `files()` and misses these.

Quote: `const QDir wfDir(absUnder(root, QStringLiteral(".github/workflows")));`

Remedy: one shared constant or function for the directory.

### Row 961 — High — `src/docfacts.cpp:294`

review-readability, lane 31-doc-engines, Q1. **Disposition:** queued under ANTS-6135; ANTS-5958 (also settled ANTS-6135).

Two engines check the same thing: a lead-in that counts its list. `DocFacts` `count_mismatch` (docfacts.cpp:259-298, `singleCardinal` + `countListItems`) and `DocIntegrity` `list_count` (docintegrity.cpp:616-838, `statedCount` + `countItems`) both do it, and `doc_lint` runs both on every prose document. They disagree: - docfacts reports a list shorter than its count. docintegrity deliberately doe…

Quote: `if (actual != stated)`

Remedy: keep one number-and-list-counting implementation, used by both kinds (or retire one kind).

### Row 962 — High — `src/doclint.cpp:148`

review-readability, lane 31-doc-engines, Q1. **Disposition:** queued under ANTS-6135.

The TOC fixer is told to "keep the two identical" with DocIntegrity (doclint.cpp:178-186, docintegrity.cpp:310-312). The region loop is identical, but the headings feeding it are not: - `headingsOf` accepts 0–3 leading spaces and keeps a closing `##`. - DocIntegrity's `atxRe`/`trailHashRe` (docintegrity.cpp:33, 40) accept no indent and strip it. So for `## Setup ##` the checker reports `Setup` wh…

Quote: `static const QRegularExpression re(QStringLiteral(R"(^ {0,3}(#{1,6})\s+(\S.*?)\s*$)"));`

Remedy: make `headingsOf` use DocIntegrity's exact heading rule (shared regexes, or move them to MarkdownScan), and say so in both pointer comments.

### Row 963 — Medium — `src/docdedup.cpp:52`

review-readability, lane 31-doc-engines, Q1. **Disposition:** queued, ANTS-6598.

`blankExcludedSections` (ANTS-5471) works out heading sections by hand. `MarkdownScan::headings` already returns each section's `endLine` under the same "next heading of its level or higher" rule, and handles blockquoted headings, which this copy does not. It is one of five heading recognisers in the lane, and they disagree on indent and closing `#`s: - docintegrity.cpp:33 - doclint.cpp:148 - doc…

Quote: `static const QRegularExpression atx(QStringLiteral("^(#{1,6}) +(.*)$"));`

Remedy: route `blankExcludedSections` through `MarkdownScan::headings`. List the remaining recognisers as deliberate or merge them.

### Row 964 — Medium — `src/docsindex.cpp:710`

review-readability, lane 31-doc-engines, Q1. **Disposition:** queued, ANTS-6598.

`serve()`'s cache load/write and `staleDocsWith`'s prefix comparison are copies of codebaseindex.cpp's `serve()`/`staleFilesWith`. Each defect was fixed twice under two tickets: the cache-write failure is ANTS-5099 here and ANTS-5103 there, and the capped-prefix staleness rule is repeated at codebaseindex.cpp:325-345. That is the "past fix patched both" case.

Quote: `// ANTS-5099 — a failed write or commit was ignored, so the next call`

Remedy: extract a shared cached-index load/prefix-stale/atomic-write helper used by both indexes.

### Row 965 — Medium — `src/doccitations.cpp:1121`

review-readability, lane 31-doc-engines, Q1. **Disposition:** queued, ANTS-6598.

`dcQuotationPass` decides "where a block ends" in three places: - `paragraphAttribution` (1120-1122) stops at blank or fenced lines. - The window-opening test (1176-1178) and the window builder (1190-1192) also stop at a table row. So the attribution walk climbs through table rows above a quotation that the window treats as a separate block. No comment says whether this is intended. A stranger ch…

Quote: `if (prefix.at(j).trimmed().isEmpty()) break;`

Remedy: one `isBlockBoundary(j)` lambda used at all three sites, with the table-row exception (if intended) stated.

### Row 980 — Low — `src/doccitations.cpp:380`

review-readability, lane 31-doc-engines, Q1. **Disposition:** queued, ANTS-6598.

roadmapmigrate.cpp:35-42 `decodeUtf8` says it is the "Same idiom as doccitations.cpp's decodeChecked()" but omits this NUL rejection. Neither comment names the difference.

Quote: `if (bytes.contains('\0')) return false;`

Remedy: share one decoder, or state at roadmapmigrate why NUL is acceptable there.

### Row 981 — Low — `src/doccitations.cpp:1550`

review-readability, lane 31-doc-engines, Q1. **Disposition:** queued, ANTS-6598.

The (docLine, docCol) order is written four times in this file: `byPosition` at 351, `posBefore` at 450, and lambdas at 1550 and 1790. A stranger changing the tiebreak edits one.

Quote: `std::stable_sort(citations.begin(), citations.end(),`

Remedy: reuse `byPosition` (move it to file scope).

### Row 982 — High — `src/speclog.cpp:200`

review-readability, lane 32-spec-engines, Q1. **Disposition:** queued, ANTS-6613.

This is one of several hand-kept "split a table row into cells" functions, and they disagree. - `speclint.cpp:127` `tableCells` honours an escaped `\|`. - `specconformance.cpp:45` `cellsOf` and this function split on every `|`. - So does `roadmapmigrate.cpp:138`. A fifth splitter, `runtraceverb.cpp:83`, I located but did not read. - `appendTableRow` itself writes `\|` into cells (line 357). Its o…

Quote: `static QStringList slRowCells(const QString &line) {`

Remedy: one escape-aware cell splitter shared by all three spec engines.

### Row 983 — Medium — `src/speclog.cpp:211`

review-readability, lane 32-spec-engines, Q1. **Disposition:** queued, ANTS-6613.

Three separator-row detectors in this lane disagree. - `speclint.cpp:158` `isDelimiterRow` is regex-based and accepts an empty cell. - `specconformance.cpp:54` `isSeparatorRow` accepts an empty cell and any whitespace. - This one rejects an empty cell (`| --- | |`). - `roadmapmigrate.cpp:128` holds another `isSeparatorRow`, which I located but did not read. - So one table can be "a table" to spec…

Quote: `static bool slIsTableSeparator(const QString &line) {`

Remedy: share one detector with the cell splitter above.

### Row 984 — Medium — `src/speclint.cpp:641`

review-readability, lane 32-spec-engines, Q1. **Disposition:** queued, ANTS-6613.

`checkHeaderBlock` re-implements header-field reading with its own field regex and its own block end. That rule has a shared copy: `SpecParse::headerField` and `isHeaderBlockEnd`, whose header (specparse.h:73-76) says it is "Exported so the bound has ONE expression". They differ in two ways: - **Where the block ends.** This copy skips fenced `## ` lines; `isHeaderBlockEnd` does not. - **How much …

Quote: `// SpecParse reads a wrapped value from the next line.`

Remedy: build `checkHeaderBlock` on `headerField`/`isHeaderBlockEnd`, or comment why it cannot.

### Row 1005 — Medium — `src/indiereviewdialog.cpp:214`

review-readability, lane 33-review-engines, Q1. **Disposition:** queued under ANTS-6279.

These are the same in `coldeyesdialog.cpp`, with no shared copy: - `lanesToReReview` (326) - `selectedReviewLanes`, `setLaneSelected`, `isLaneSelected`, `laneByName` - the narrative half of `performFoldIn` (357) - the lane checklist, re-review button and fold-in box in `buildControls` (49–126) - the `FoldInMode` enum and `Results` struct in both headers Some copies have already drifted: - `rebuil…

Quote: `QStringList IndieReviewDialog::lanesToReReview() const {`

Remedy: move the lane checklist, re-review collection and narrative fold-in into `ReviewDialogBase`.

### Row 1006 — Medium — `src/indiereviewengine.cpp:907`

review-readability, lane 33-review-engines, Q1. **Disposition:** queued, ANTS-6609.

`assembleBriefForDispatch` has two hand-written copies (903–910 and 958–965) of the "resolve the path and keep it inside the project" check. That check already lives in `BriefDispatch::safeCanon`. `briefdispatch.cpp:23–26` records a past fix (ANTS-1731) made only in the shared copy: an absolute path joined onto the project path fails to resolve and the file is silently dropped. These copies still…

Quote: `if (canon.isEmpty() || rootCanon.isEmpty()`

Remedy: call `BriefDispatch::inlineBodies` with `kMaxSourceBodyBytes`, or at least `safeCanon`, and keep the "standard" label.

### Row 1008 — Medium — `src/indiereviewengine.cpp:421`

review-readability, lane 33-review-engines, Q1. **Disposition:** queued under ANTS-6137.

The noise-directory check and walk filters are hand-copied in six places. Sites: `collectLaneFiles` 413–426, `laneUncountedFiles` 606–618, `deriveComputedPartition` 472–476, `unassignedForLanes` 697–701, `buildBasenameIndex` 990–994, `ignoredDirPrefixes` 365–367. They have drifted: - `ignoredDirPrefixes` checks every segment, including the last. - Only two of the walks apply `underIgnoredDir`. Th…

Quote: `for (int i = 0; i + 1 < segs.size(); ++i)`

Remedy: one named predicate for "a reviewable relative path", passed to each walk, with each site's own gate kept separate.

### Row 1009 — Medium — `src/coldeyesengine.cpp:393`

review-readability, lane 33-review-engines, Q1. **Disposition:** queued, ANTS-6609.

These are copied by hand from `projectlayoutengine.cpp` and commented "mirrors": - `caseInsensitiveResolve` (52–77, same behaviour as projectlayoutengine 119–146) - `countLines` / `kStandardsMinLines` (247–262 vs `countLinesProbe` 333–343) - the standards name-glob regex (271–273) - `kSpecDirs` (397–402) The comment says this set "tracks the project_layout probe set as ANTS-1493 widens it further…

Quote: `// projectlayoutengine.cpp's kSpecsCandidates so the cold-eyes`

Remedy: expose project_layout's resolver and candidate lists and call them here.

### Row 1010 — Medium — `src/indiereviewengine.cpp:1444`

review-readability, lane 33-review-engines, Q1. **Disposition:** queued under ANTS-6137.

This lambda is identical to `auditengine.cpp:1413–1428`. There are four fold-in renderers (`templateRoadmapFoldInBlock`, `templateColdEyesFoldInBlock`, `templateDebtSweepFoldInBlock`, `templateIndieReviewFoldInBlock`), each building the `- 📋 [ID] … Kind: / Source:` bullet by hand. ANTS-3473/3497 had to patch every one (the comment in `debtsweepengine.h:278` says so). They differ with no comment: …

Quote: `auto laneFromPath = [](const QString &p) -> QString {`

Remedy: one shared bullet-skeleton helper in `RoadmapFoldIn`, with each tool's own fields kept.

### Row 1018 — Low — `src/coldeyesdialog.cpp:242`

review-readability, lane 33-review-engines, Q1. **Disposition:** queued under ANTS-6137.

The truncation markers spell out "200 KiB" here, at 249–250, and at `indiereviewdialog.cpp:154`. The cap is `ReviewDialogBase::kPromptCapBytes = 200 * 1024`, promoted there by ANTS-2205 as the single source. Changing the constant leaves three markers that are wrong.

Quote: `"200 KiB budget]\n");`

Remedy: build the marker text from `kPromptCapBytes`.

### Row 1019 — Low — `src/debtsweepengine.cpp:2275`

review-readability, lane 33-review-engines, Q1. **Disposition:** queued, ANTS-6609.

The location in that first line is wrong; the real site is `src/debtsweepengine.cpp:1625`, the line `"=== Per-category guidance ===\n\n"`. `triagePrompt`'s guidance is a hand-kept list of five detector IDs. `scanAll` (1386–1406) runs more detectors that report non-fixable findings and get no guidance: `stale_todo`, `dead_branch_after_return`, `dead_suppression`, `dep_pin_mismatch`, `version_drift…

Quote: `out += QStringLiteral(`

Remedy: add guidance for each non-fixable detector, or note beside `scanAll` that this list must follow it.

### Row 1020 — Low — `src/debtsweepengine.cpp:161`

review-readability, lane 33-review-engines, Q1. **Disposition:** queued under ANTS-6137.

There are three hand-kept extension lists (161–165, 283–288, 498–503). Two list eight extensions and one lists six (no `.go`/`.rs`). Only the header (79) documents the six-entry list. `todoMarkerRe()` (451) says "same shape detectAddedTodos uses", but `detectAddedTodos` keeps its own copy of the pattern (299) instead of calling it. Adding a language, or a TODO marker, takes three edits.

Quote: `static const QStringList kExts = {`

Remedy: one named list for the TODO detectors, and have `detectAddedTodos` call `todoMarkerRe()`.

### Row 1021 — Low — `src/debtsweepengine.cpp:599`

review-readability, lane 33-review-engines, Q1. **Disposition:** queued, ANTS-6609.

There are three copies of the "list .cpp/.h, read through the cache, scan" loop (599–610, 686–706, 802–814). Only two skip `isFixtureData`. The comment at 141–142 explains that split. A new per-file exclusion (such as vendor/) takes three edits.

Quote: `QList<Finding> detectDuplicateIncludes(`

Remedy: one helper that takes the scan function and a skip-fixtures flag.

### Row 1024 — Medium — `src/testauditdialog.cpp:56`

review-readability, lane 34-test-audit-verify, Q1. **Disposition:** queued, ANTS-6616.

Three files in this lane each hand-roll an atomic owner-only file write, and they set permissions differently: - `writeReport` sets 0600 only after `commit()`, so the temporary file is written with default permissions. - `testrescache.cpp:252-263` (`recordTests`) sets permissions both before writing and after commit. - `mutationjournal.cpp:50-54` calls `f.setPermissions` and ignores its result, w…

Quote: `bool writeReport(const QString &path, const QString &body) {`

Remedy: route all three through `writeFileAtomicOwnerOnly` with `TempPerms::Refuse`.

### Row 1027 — Medium — `src/testrunners.cpp:658`

review-readability, lane 34-test-audit-verify, Q1. **Disposition:** queued, ANTS-6616.

For Jest and the default case, the narrowed command is built from `c.fullArgs`. For Vitest (658) and Go (663) it is retyped by hand. Changing a flag in the full command at 558 or 567 would leave the narrowed command without it. Go's `-v` matters most: `parseGo` only recognises output through `=== RUN` lines, which Go prints only with `-v`, and no comment says so.

Quote: `c.args = {QStringLiteral("--no-install"), QStringLiteral("vitest"),`

Remedy: derive both from `fullArgs`, and comment why Go needs `-v`.

### Row 1030 — Low — `src/testauditengine.cpp:1188`

review-readability, lane 34-test-audit-verify, Q1. **Disposition:** queued, ANTS-6616.

`partition`, `brief`, `synthesize` and `foldIn` (1188, 1522, 1602, 2004) each repeat the caller_cwd canonicalise-and-refuse block. The copies have drifted: `foldIn`'s message drops the path. A change to how the root is resolved (for example, refusing symlinks) needs four edits.

Quote: `const QString canon = QFileInfo(req.callerCwd).canonicalFilePath();`

Remedy: one helper that takes the verb name.

### Row 1031 — Low — `src/testauditengine.cpp:2218`

review-readability, lane 34-test-audit-verify, Q1. **Disposition:** queued, ANTS-6616.

The narrative branch (2048-2077) and the per-finding branch (2214-2238) of `foldIn` repeat the insert, write_failed refusal and success fill. They already differ: only per-finding sets `failedCount` on failure. A new result field must be added in both places.

Quote: `const bool wrote = req.dryRun`

Remedy: one tail helper that takes the block and count.

### Row 1041 — Low — `src/testauditdialog.cpp:304`

review-readability, lane 34-test-audit-verify, Q1. **Disposition:** queued, ANTS-6616.

Both truncation markers (304, 312) hard-code "200 KiB". The cap they describe is `ReviewDialogBase::kPromptCapBytes` (`reviewdialogbase.h:54`). Changing the cap leaves the messages wrong.

Quote: `"\n[brief truncated — pre-pass context omitted to fit the 200 KiB "`

Remedy: format the size from `kPromptCapBytes`.

### Row 1046 — Medium — `src/findsources.cpp:110`

review-readability, lane 35-code-index-search-a, Q1. **Disposition:** queued, ANTS-6592.

find_sources keeps its own C/C++ extension list (.cpp .cc .cxx .h .hpp) instead of calling `FileOutline::isCFamilyExt`. `codebaseindex.cpp:31-32` records that the C-family copy had to be patched before (ANTS-2148), and this copy never got that patch: `.c`, `.hh` and `.hxx` are missing. Someone adding an extension to `isCFamilyExt` will think find_sources follows it. It does not.

Quote: `QStringLiteral(".cpp"), QStringLiteral(".cc"),`

Remedy: call `FileOutline::isCFamilyExt` on the suffix in `maybeAddSource`.

### Row 1047 — Medium — `src/findsources.cpp:167`

review-readability, lane 35-code-index-search-a, Q1. **Disposition:** queued, ANTS-6592.

the comment at line 138 says this walk "Mirrors codebaseindex.cpp::candidates", but the noise check differs. Here `isNoiseDir` runs on every path part, file name included. `codebaseindex.cpp:161` checks folders only (`i + 1 < parts.size()`). `isNoiseDir` is true for any name starting with `build` or `.` (projectsettings.cpp:311-312), so a file like `src/buildinfo.cpp` is indexed by codebase_index…

Quote: `for (const QString &comp : rel.split(QLatin1Char('/')))`

Remedy: skip the last part as codebaseindex does, or share one walk.

### Row 1048 — Medium — `src/codebaseindex.cpp:85`

review-readability, lane 35-code-index-search-a, Q1. **Disposition:** queued under ANTS-6139.

`FindSources::classifyRole` (findsources.cpp:94) answers the same question ("impl" | "header" | "test") with different rules: - roleFor checks for a header first, and it knows `.hh` and the `test_` name prefix. - classifyRole checks for tests first, and it knows `/tests/` in the middle of a path. So `tests/x.h` is a "header" in one tool and a "test" in the other. A fix made to one will miss the o…

Quote: `QString roleFor(const QString &relPath, const QString &basename) {`

Remedy: one shared role function.

### Row 1049 — Medium — `src/codebaseindex.cpp:709`

review-readability, lane 35-code-index-search-a, Q1. **Disposition:** queued, ANTS-6592.

codebaseindex.cpp and docsindex.cpp contain twin code for loading the cache, checking it, deciding what is stale and writing it back. Past fixes had to land in both: - the cache-write fix: ANTS-5103 here, ANTS-5099 in docsindex - the capped-prefix stale fix: codebaseindex.cpp:323 and docsindex.cpp:366 - ANTS-2198, applied to both docsindex already moved the cap loop into `fillIndex`. This file st…

Quote: `// ANTS-5103 — a failed write or commit was ignored, so an unwritable`

Remedy: pull the load/stale/write steps into one shared helper that both indexes use.

### Row 1050 — Medium — `src/trace/languages.cpp:160`

review-readability, lane 35-code-index-search-a, Q1. **Disposition:** queued, ANTS-6592.

there are two `#!` parsers, and they disagree: - This one takes the word straight after `env`, so `#!/usr/bin/env -S bash` gives `-S`. It knows sh, bash, zsh and dash. - `FileOutline::shebangInterpreter` (fileoutline.cpp:52) skips `-` flags. Its shell list adds ksh and ash. fileoutline.cpp:77-78 points at this one ("the test trace's languageForFile uses", which is a garbled sentence), so the two …

Quote: `if (interp == "env" && words.size() > 1) interp = words.at(1);`

Remedy: one shared shebang reader, or a comment at each copy naming the other and the inputs they treat differently.

### Row 1051 — Low — `src/fileoutline.cpp:21`

review-readability, lane 35-code-index-search-a, Q1. **Disposition:** queued, ANTS-6592.

this comment says a second GLSL list "would be that defect". But there are three hand-kept copies: - `isGlslExt` here - `SymbolQuery::langForExt` (symbolquery.cpp:70+) - trace/languages.cpp:98-100 The same is true of the shell and brace-family lists in `langForExt`, which calls `isCFamilyExt` but writes the rest out by hand. The three agree today. Someone adding a shader extension will update one…

Quote: `// ANTS-3800 — the SAME extension set symbolquery.cpp's ANTS-3558 lane accepts,`

Remedy: have `langForExt` call `isGlslExt`, `isShExt` and `isGenericExt`, and note the trace copy's link-boundary reason beside it.

### Row 1052 — Low — `src/similarcode.cpp:26`

review-readability, lane 35-code-index-search-a, Q1. **Disposition:** queued, ANTS-6592.

fileoutline.h:22-27 says `kMaxSymbolsCap` was made public because "two copies of a cap is how they diverge". This is a second copy of that cap.

Quote: `constexpr int kPerFileSymbols = 1000; // FileOutline's own cap ceiling`

Remedy: use `FileOutline::kMaxSymbolsCap`.

### Row 1053 — Low — `src/findsources.cpp:397`

review-readability, lane 35-code-index-search-a, Q1. **Disposition:** queued under ANTS-6139.

256 KiB is written three times: findsources.h:44, findsources.cpp:240 and here. If someone changes the `Options` default, prewarm keeps warming the old amount, and line 240 keeps the old fallback.

Quote: `constexpr qint64 kContentByteCap = 256 * 1024;`

Remedy: one named constant used by all three.

### Row 1054 — Low — `src/fileoutline.cpp:1179`

review-readability, lane 35-code-index-search-a, Q1. **Disposition:** queued, ANTS-6592.

the HTML `<script>` matching code copies `scanGenericLine` and has diverged from it. The comment says how they differ, not why, and roadmap ANTS-5992 records the difference as a defect, not a design choice. Someone adding a brace-family rule cannot tell whether the HTML copy should get it too.

Quote: `// A hand copy of the Generic arm, not a call to it: it skips the`

Remedy: call `scanGenericLine` from the HTML arm, or state the reason for the difference.

### Row 1055 — Low — `src/fileoutline.cpp:162`

review-readability, lane 35-code-index-search-a, Q1. **Disposition:** queued, ANTS-6592.

`cppre::optimized()` exists to build a pattern and call `.optimize()` on it. About 20 regex builders in this file still write out the same three lines by hand. A change to how patterns are compiled would be made in `optimized()` and miss all of them.

Quote: `QRegularExpression r(QStringLiteral(`

Remedy: route every builder through `cppre::optimized`.

### Row 1056 — Low — `src/readregion.cpp:332`

review-readability, lane 35-code-index-search-a, Q1. **Disposition:** queued, ANTS-6592.

this file and functionsizes.cpp:80-82 each decide which languages end a function with a closing brace, and they disagree on Ruby: - read_region treats Ruby as brace-delimited, and its comment says "nobody has measured it". - function_sizes refuses to measure Ruby ("closes with `end`"). Someone fixing Ruby in one place will not know about the other.

Quote: `const bool braceDelimited =`

Remedy: one shared "how does this language end a body" function.

### Row 1074 — Medium — `src/codewalk.cpp:53-54`

review-readability, lane 36-code-index-search-b, Q1. **Disposition:** queued, ANTS-6592.

The comment (repeated at codewalk.h:42-43) says the generated-file test was moved out of indiereviewengine.cpp and that findsources keeps its own copy. Both halves are false: - findsources.cpp:120-124 now calls `CodeWalk::isGeneratedSource`. - indiereviewengine.cpp:62-76 (`sourcePathsForLane`) still has its own `kGenPrefixes` copy, prefix-only. That is the very shape ANTS-4096 fixed here, so the …

Quote: `// indiereviewengine.cpp (ANTS-4120); auditdialog and findsources still keep`

Remedy: correct both comments to name the live copies (auditdialog, indiereviewengine), and make indiereviewengine call `isGeneratedSource`.

### Row 1075 — Medium — `src/symbolquery.cpp:53-55`

review-readability, lane 36-code-index-search-b, Q1. **Disposition:** queued, ANTS-6592.

`langForExt` re-lists by hand the Generic extensions from `FileOutline::genericLangName` and the GLSL extensions from `FileOutline::isGlslExt`. The lists match today. But `CodebaseIndex::isIndexableSuffix` (codebaseindex.cpp:28-65) records the same set drifting four times (ANTS-2148, 2150, 4096, 4826) and now delegates to FileOutline. Adding a language to FileOutline will silently leave find_defi…

Quote: `// ANTS-2150 — brace family + Ruby (kept in step with`

Remedy: replace the two hand lists with `FileOutline::isGenericExt(ext)` / `FileOutline::isGlslExt(ext)`.

### Row 1076 — Medium — `src/sourcescrapes.cpp:187`

review-readability, lane 36-code-index-search-b, Q1. **Disposition:** queued, ANTS-6592.

The literal-placeholder markers are constants `kLitOpen`/`kLitClose` (lines 33-34). Yet six regexes hard-code `\x01`/`\x02` instead: lines 178, 187, 199, 358, 363 and 513, four of them the same `idx` pattern. Anyone changing the placeholder constants (say, to avoid a clash) would break every anchor lookup without an error.

Quote: `static const QRegularExpression idx(QStringLiteral("\\x01(\\d+)\\x02"));`

Remedy: build one shared placeholder regex from `kLitOpen`/`kLitClose` and use it at all sites.

### Row 1077 — Medium — `src/cochangefamily.cpp:44`

review-readability, lane 36-code-index-search-b, Q1. **Disposition:** queued, ANTS-6592.

"Clip to a UTF-8 byte budget" is written at least five times, in two styles that behave differently: - Back to a code-point start, keeping a whole final character: cochangefamily.cpp:44-52, readregion.cpp:38-41, remotecontrol_feedback.cpp:273-276. - Truncate, then chop continuation bytes and the lead byte: trace/extract.cpp:63-68 and trace/trace.cpp:159-163. This always drops a final multi-byte c…

Quote: `QString clipUtf8(const QString &s, int maxBytes) {`

Remedy: one shared UTF-8 clip helper used by all sites.

### Row 1078 — Low — `src/commentclaims.cpp:557-559`

review-readability, lane 36-code-index-search-b, Q1. **Disposition:** queued, ANTS-6592.

The `include_tests`/`include_generated` argument parsing is copied in three verbs: commentclaims.cpp:557-559, functionsizes.cpp:143-145 and duplicateblocks.cpp:298-300. A fourth walk option would need three matching edits.

Quote: `CodeWalk::Options walkOpts;`

Remedy: a `CodeWalk::optionsFrom(args)` helper beside `scannedSummary`. (Confirms dup:137f5be66ec2.)

### Row 1079 — Low — `src/paginationengine.cpp:86-90`

review-readability, lane 36-code-index-search-b, Q1. **Disposition:** queued under ANTS-6139.

The block "measure, keep at least one row, copy the slice" appears three times in `pageBullets`: 85-90, 95-102 and 122-127. The ANTS-5103 "at least one row" fix already had to be applied to all three (the third says `// ANTS-5103 — see above`).

Quote: `const int n = qMin(qMax(measureCutPoint(bounded, budget), 1),`

Remedy: one local helper returning the budget-cut slice.

### Row 1080 — Low — `src/symbolquery.cpp:903-908`

review-readability, lane 36-code-index-search-b, Q1. **Disposition:** queued, ANTS-6592.

The invalid-symbol refusal is built three times, at 903-908, 941-947 and 992-997. Only the verb name in the message differs. Changing the refusal code or wording means three edits. (Confirms dup:3e10023fceb9 and adds the third site.)

Quote: `if (!isValidSymbol(symbol)) {`

Remedy: one `invalidSymbol(verb)` builder.

### Row 1107 — Medium — `src/modelswitchledger.cpp:298`

review-readability, lane 38-model-switching, Q1. **Disposition:** queued, ANTS-6607.

There are two diverged copies of "what a `/model` command looks like in a transcript": - `modelrecommender.cpp:69-84` (`modelTierFromLocalCommand`) reads `{type:"system", subtype:"local_command"}` events and accepts any `<command-args>`. Its comment says that is how Claude Code records `/model`. - `modelswitchledger.cpp:239-245` (`parseUserModelCommand`) matches `^/model (haiku|sonnet|opus)` in *…

Quote: `const QString cmd = parseUserModelCommand(t.userText);`

Remedy: one shared `/model` event parser in ModelSwitchLedger (core lib), used by both.

### Row 1108 — Medium — `src/modelnearmissledger.cpp:103-112`

review-readability, lane 38-model-switching, Q1. **Disposition:** queued under ANTS-6142.

The lock, readLines, append, evictToCap, writeLinesAtomic sequence has three copies: - `modelnearmissledger.cpp:103-112` - `modelswitchledger.cpp:132-145` - `modelswitchledger.cpp:158-167` (`writeRecords`) Past fixes had to patch every copy: ANTS-1989 added the lock to each (the near-miss copy says "see ModelSwitchLedger::appendRecord for the rationale"), and ANTS-5105's `kGuiTimeoutMs` is in all…

Quote: `bool appendRecord(const QString &path, const Record &rec, qint64 capBytes) {`

Remedy: a `JsonlFile` helper taking the path, lines and an eviction function, called by all three.

### Row 1109 — Medium — `src/modelnearmissledger.cpp:25`

review-readability, lane 38-model-switching, Q1. **Disposition:** queued, ANTS-6607.

The nine blocker tokens are hand-kept twice: as literals spread through `decide()` (modelautoswitch.cpp:106-217) and as the ordered list here. The near-miss comment points at `decide()`, but `decide()`'s comment ("New tokens always go last", cpp:105) does not point back. A stranger adding a 10th blocker in `decide()` would miss this list, so ties for the dominant blocker would be broken arbitrari…

Quote: `const QStringList &taxonomyOrder() {`

Remedy: one shared token list both files read, or at least a cross-reference comment at cpp:105.

### Row 1110 — Medium — `src/modelautoswitch.h:40`

review-readability, lane 38-model-switching, Q1. **Disposition:** queued under ANTS-6142.

"forward-declared" is false: `kMinDwellMs` is simply defined later in the header (:254), so the default is a literal copy. The 90 s dwell default also appears in: - `modelswitchledger.h:222` (`int minDwellSec = 90;`) - `remotecontrol_state.cpp:1329` (`.toInt(90)`) A stranger changing `kMinDwellMs` at :254 sees no pointer to any of these, so the Gate default and the stats report silently keep the …

Quote: `qint64 configuredMinDwellMs = 90'000; // == kMinDwellMs (forward-declared)`

Remedy: move the constants block above `struct Gate` and initialise from `kMinDwellMs`, as spec ANTS-1894:159 shows. Name the cross-lib copies in a comment, since core cannot include this header.

### Row 1111 — Medium — `src/modelswitchledger.cpp:375-382`

review-readability, lane 38-model-switching, Q1. **Disposition:** queued, ANTS-6607.

The `changed` flag compares `Outcome` field by field by hand. Nothing at `struct Outcome` (modelswitchledger.h:60-75) points here. ANTS-1891 and ANTS-1957 each added a field and had to extend this list (both tagged). A stranger adding a field would update `toJson`/`fromJson` and miss this, so the new field would never be flushed to disk.

Quote: `res.changed = (res.outcome.pending != inputBefore.pending)`

Remedy: give `Outcome` an `operator==` and compare whole structs.

### Row 1115 — Low — `src/modelautoswitch.cpp:317-319`

review-readability, lane 38-model-switching, Q1. **Disposition:** queued, ANTS-6607.

This is the same billing-safety gate as `shouldContinueAfterUnarmedConfirm` (:265-269). The comment at :309-316 records that ANTS-2186 had to patch this copy to match the other after they diverged. A stranger changing the ANTS-1959 rule in one would miss the other, though each comment names its twin.

Quote: `bool shouldContinueAfterDirectSwitch(bool autoModeOn, bool activeTurn) {`

Remedy: have one delegate to the other, or both call a shared `mayContinueTurn(autoModeOn, activeTurn)`.

### Row 1116 — Low — `src/modelrecommender.cpp:272-277`

review-readability, lane 38-model-switching, Q1. **Disposition:** queued, ANTS-6607.

`score()` copies the three current-model provenance fields from `def` into a fresh `Result` at two sites (:272-277 and :354-358). ANTS-1944 had to patch both, as the tags show. A stranger adding a provenance field to `Result` would update one site.

Quote: `r.currentModelFromCommand = def.currentModelFromCommand; // ANTS-1944`

Remedy: start each result as a copy of `def` and override only tier, reason and isMechanical.

### Row 1129 — High — `src/sessionmanager.cpp:44`

review-readability, lane 39-config-session-project, Q1. **Disposition:** queued, ANTS-6593.

This comment names `restore()`'s `readCell` as the only other half of the on-disk cell format. A third copy exists: `firstLineWithin` (lines 147–157) hard-codes the encoded sizes — 13 bytes per cell, 4+1 bytes per line header, 8+4n bytes per combining entry, and a 6×4+4+4 byte header. A stranger who adds a byte to `writeCell` would follow the comment, update `readCell`, and leave the size estimat…

Quote: `// One cell on disk: codepoint, fg, bg, then a flags byte. restore()'s`

Remedy: name `firstLineWithin`'s byte arithmetic in this comment (and in `firstLineWithin`'s own comment), or derive the 13 from one shared constant.

### Row 1130 — Medium — `src/sessionmanager.cpp:398`

review-readability, lane 39-config-session-project, Q1. **Disposition:** queued, ANTS-6593.

`restore()` decodes "one line of cells plus its combining table" in three separate loops: scrollback (394–413), screen (426–447) and the skip-extra-rows loop (449–459). Each carries its own bound check. A change to the per-line format must be made in all three, plus `serializeStream` and `firstLineWithin`. This is also where the 282-line function loses a reader: the screen loop truncates to `rows…

Quote: `if (in.status() != QDataStream::Ok || cellCount < 0 || cellCount > kMaxRestoreLineCells) return false;`

Remedy: extract one "read a line" lambda beside `readCell`/`readCombining` and call it from all three loops.

### Row 1131 — High — `src/projectsettings.cpp:514`

review-readability, lane 39-config-session-project, Q1. **Disposition:** queued, ANTS-6593.

The set of recognised settings keys is kept by hand in four places: - `load()` (124–135) - `applyWrite()`'s branch flags (514–520, 530, 567–568) - `splitUndeclared()`'s per-key arms (615–636) - `psLayoutKeys()` in `remotecontrol_docs.cpp:1745–1749` The comment at 525–527 records that `id_format` once fell through `applyWrite`'s unknown-key passthrough and was written unvalidated, so a past change…

Quote: `const bool dirArray = key == QLatin1String("source_roots")`

Remedy: one key→kind table (dir array, dir, file, bool, id_format) read by both `load()` and `applyWrite()`.

### Row 1132 — Medium — `src/projectsettings.cpp:630`

review-readability, lane 39-config-session-project, Q1. **Disposition:** queued, ANTS-6593.

The conventional paths ("tests"/"test", "docs", "docs/specs", "ROADMAP.md", "CHANGELOG.md") are written out in both `proposeAuxLayout` (224–253) and `splitUndeclared` (620–636). "docs/specs" appears a third time in `specsDirOf` (173). They agree today. Nothing links them, so changing a convention in one leaves `splitUndeclared` reporting a key as `unavailable` while `detect` proposes it.

Quote: `|| validDirUnder(rootCanonical, QStringLiteral("docs/specs"));`

Remedy: one table of conventional path per key, used by all three.

### Row 1145 — Low — `src/sessionmemoryengine.cpp:191`

review-readability, lane 39-config-session-project, Q1. **Disposition:** queued, ANTS-6593.

Error text hard-codes "100 KiB", "16 KiB" (line 286) and `{1,64}` (line 255). The real limits are `kMaxStoreBytes`, `kMaxValueBytes` and `kMaxKeyLength`. Changing a constant would leave the messages wrong.

Quote: `"100 KiB cap (%1 bytes)").arg(body.size());`

Remedy: format the constants into the strings.

### Row 1147 — Medium — `src/pluginmanager.cpp:307`

review-readability, lane 40-plugins-lua, Q1. **Disposition:** queued, ANTS-6608.

There are three hand-kept copies of "may this engine receive this event". `fireEvent` (lines 681–682) and `wants` (line 704) check `handlesEvent` and the permission from `permissionFor`. `dispatchTo` re-implements only the health check, and checks no permission. The header (`pluginmanager.h:90-91`) says it *shares* `healthyEngines()`'s filter. Today's callers send `PaletteAction` and `Keybinding`…

Quote: `if (!engine || m_demoted.contains(engine)) return;`

Remedy: one shared "engine may receive event" check, used by all three.

### Row 1161 — Medium — `src/changelogquery.cpp:501`

review-readability, lane 41-changelog-feedback, Q1. **Disposition:** queued, ANTS-6588.

One file has three different rules for "this line is a bullet": - `parse` (line 281): a `- ` at column 0 only. - `lint` (line 365): up to 3 spaces of indent, then `-`, `*`, `+` or a number. - `releaseLint` (line 501): any indent, then `-`, `*` or `+`. So a column-0 `* ANTS-12 …` counts as a citation for `releaseLint` but is not an entry for `parse`. The comment at line 527 says "as in parse()", b…

Quote: `static const QRegularExpression bullet(QStringLiteral(R"(^\s*[-*+]\s)"));`

Remedy: one named bullet predicate, with a comment on any reader that must differ, and why.

### Row 1162 — Medium — `src/changelogquery.cpp:370`

review-readability, lane 41-changelog-feedback, Q1. **Disposition:** queued under ANTS-6033.

The changelog code uses two fence rules (a fence is a fenced code block): - `lint` and `releaseLint` (line 506) use `MarkdownScan::fenceMask`. It follows list indentation and never closes an unterminated fence at a heading (markdownscan.cpp:85 onward). - `parse` (lines 227–236) and the writer's `ChangelogLog::fencedLines` (changeloglog.cpp:148) close an unterminated fence at a column-0 `## [`. Th…

Quote: `const QVector<bool> fence = MarkdownScan::fenceMask(lines);`

Remedy: name the fence rule each reader uses beside each one, or use one shared rule.

### Row 1163 — Medium — `src/changeloglog.cpp:70`

review-readability, lane 41-changelog-feedback, Q1. **Disposition:** queued, ANTS-6588.

The writer treats category names case-insensitively. The comment at lines 65–66 says `### fixed` merges like `### Fixed`, and `insertUnreleasedEntry` at line 514 writes into an existing `### fixed`. The reader's `categoryForHeading` (changelogquery.cpp:142) uses `isValidCategory` (changeloglog.cpp:59), which is case-sensitive. So bullets under `### fixed` are dropped from `parse`, and `lint` repo…

Quote: `if (cats.at(c).compare(name, Qt::CaseInsensitive) == 0) return c;`

Remedy: comment beside both on why the writer folds case and the reader does not, or align them.

### Row 1164 — Medium — `src/changeloglog.cpp:775`

review-readability, lane 41-changelog-feedback, Q1. **Disposition:** queued, ANTS-6588.

Two copies of one job: find the `### <Category>` heading in a range, note the first heading that sorts later, then insert at the top of it or create it in canonical order. - `insertUnreleasedEntry`: lines 507–520 and 524–552. - `closeUnreleasedDated`: lines 755–782. They have diverged. The dated copy adds a blank line before a new heading when the line above is not blank. The flat copy (lines 541…

Quote: `if (at > 0 && !lines.at(at - 1).trimmed().isEmpty())`

Remedy: extract one "place block under category within [from, to)" helper that both call.

### Row 1173 — Medium — `src/claudesetup.cpp:380`

review-readability, lane 42-app-entry-dialogs, Q1. **Disposition:** queued under ANTS-6296.

`installStatusHooks` (297-308) and `installGitContextHook` (377-387) repeat the same steps: write the script, create the folder, take the lock, read `settings.json`, pull out `hooks`. The ANTS-5109 comment on both shows a past fix had to patch both. Only the git-context copy points at its twin, so someone fixing the status-hook copy will miss the other. Fix: extract one helper that does write-scr…

Quote: `// ANTS-5109 — one hold across read, merge and write, as installStatusHooks.`

### Row 1176 — Medium — `src/diffviewer.cpp:474-480`

review-readability, lane 42-app-entry-dialogs, Q1. **Disposition:** queued, ANTS-6584.

The rule for which untracked files get a jump target is written three times: - `anchoredPaths` at 474-480; - the New-files list at 678-684 (the same `?? ` filter, `mid(3).trimmed()`, `!isEmpty && !endsWith('/')`); - `lineCountSuffix` at 160-161, in part. `listsUntracked` (443-445) and the Status renderer (514) parse `?? ` on their own as well. The comment at 464-466 promises "never dead links", w…

Quote: `for (const QString &sl : state->status.split('\n')) {`

### Row 1177 — Low — `src/diffviewer.cpp:531-534`

review-readability, lane 42-app-entry-dialogs, Q1. **Disposition:** queued, ANTS-6584.

The `fileLink` lambda (484-490) builds the link markup, but the Status rename branch (531-536) and the diffstat branch (642-650) build the same `<a href='#%1'>%2</a>` with `fileAnchorId` by hand. That makes three copies. A change to link markup made in `fileLink` misses the other two. Fix: give `fileLink` a display-text parameter and call it from all three places.

Quote: `if (anchoredPaths.contains(key.trimmed()))`

### Row 1182 — Low — `src/settingsdialog.cpp:1356`

review-readability, lane 42-app-entry-dialogs, Q1. **Disposition:** queued, ANTS-6584.

The regex check is copied in the Highlights loop (1331-1336) and the Triggers loop (1353-1358). The comment "Same regex-validity gate as Highlights above" (1348) and the ANTS-1178 save-time fix in CHANGELOG show that past fixes landed in both. The copies are adjacent and linked by that comment, so the risk is small. Fix: one `isUsablePattern(pattern)` helper used by both loops.

Quote: `if (!re.isValid() || AuditEngine::isCatastrophicRegex(pattern))`

### Row 1183 — Low — `src/diffviewer.cpp:319-322`

review-readability, lane 42-app-entry-dialogs, Q1. **Disposition:** queued, ANTS-6584.

The loading placeholder names 3 of the 5 probes (`status`, `diff HEAD`, `log @{u}..HEAD`); it misses `for-each-ref` and the cross-branch log. The ProbeState comment at 354-356 lists where a new probe must be added, but not this text. Fix: make the placeholder generic, or add it to that list.

Quote: `"<span style='color: %3;'>Loading git status, diff, and "`

### Row 1189 — Low — `src/claudesetup.cpp:169-171`

review-readability, lane 42-app-entry-dialogs, Q1. **Disposition:** queued, ANTS-6584.

"Find begin, then find end after it" is written three times: `blockBody` 169-171, `withBlock` 181-183 and `withoutBlock` 192-194. Tightening the marker match (for example, requiring the marker at line start) needs all three. Fix: one `findBlock(text, begin, end)` that returns the range, used by all three.

Quote: `const qsizetype b = text.indexOf(begin);`

### Row 1190 — Medium — `src/buildcache.cpp:157`

review-readability, lane 43-diagnostics-logging, Q1. **Disposition:** queued, ANTS-6596.

The save step (`recordBuild`, lines 139-167) and the load step (`loadBuild`, 169-186) are near-copies of `recordTests` / `loadTests` in `src/testrescache.cpp` (lines 240-282). The same ANTS-5151 owner-only fix comment sits in both copies, so a past fix already had to patch both. A stranger changing the write, permissions or load behaviour in one would miss the other.

Quote: `// ANTS-5151 — compiler output quotes the project's code: private or not`

Remedy: extract one shared "write private JSON / load versioned JSON" helper and keep the public functions as thin wrappers.

### Row 1191 — Low — `src/buildcache.cpp:29`

review-readability, lane 43-diagnostics-logging, Q1. **Disposition:** queued under ANTS-6147.

The comment implies one twin. In fact the `/.audit_cache` path is built separately in auditcache.cpp:30, testrescache.cpp:35, auditrunner.cpp:635, auditfpledger.cpp:182, remotecontrol_state.cpp:720 and auditdialog.cpp (4 sites). Renaming the cache folder from here would miss most of them.

Quote: `// Same path the AuditCache GUI/runner uses (see auditcache.h);`

Remedy: one core-lib constant for the folder name, which keeps buildcache off the audit library as the comment requires.

### Row 1215 — High — `CMakeLists.txt:2057`

review-readability, lane 45-build-system, Q1. **Disposition:** queued, ANTS-6587.

The "link every library in one group" list is copied by hand at least eight times, and the copies have drifted: - bench_partition_walk 2000–2008 - bench_transcript_walk 2059–2067 - bench_mcp_reply_tail 2113–2121 - ants_add_widget_bench 2150–2157 (no `ants_roadmapstore_lib`) - ants-terminal 1201–1216 - test_chrome 2377–2380 (no `ants_roadmapstore_lib`) - test_claude 2674–2682 - test_dialogs 3948–3…

Quote: `# guard bench_full_paint uses, and for the same reason: an ordered link`

Remedy: hold the full-stack set in one list variable and have every full-stack target use it.

### Row 1216 — Medium — `CMakeLists.txt:1678`

review-readability, lane 45-build-system, Q1. **Disposition:** queued, ANTS-6587.

`ants_add_gui_bundle` (1661–1681) and `ants_add_core_bundle` (1697–1717) each carry an identical 17-line timeout rationale and an identical `gtest_discover_tests` call. Both copies include the ANTS-6331 DISCOVERY_TIMEOUT 300 change, so a past fix already had to patch both. The next timeout or label change will be needed twice too.

Quote: `gtest_discover_tests(${name}`

Remedy: move the discovery call and its comment into one small helper that both bundle functions call.

### Row 1221 — Medium — `CMakeLists.txt:4559`

review-readability, lane 45-build-system, Q1. **Disposition:** queued, ANTS-6587.

The shared scrape defines are hand-copied into each bundle: - `ANTS_MAINWINDOW_SOURCES` + `ANTS_MCP_REGISTRY_SOURCE`, with the same comment, in test_vt 2326–2329, test_chrome 2538–2541, test_claude 3444–3447, test_dialogs 4067–4070, test_lua 4151–4153 and test_core 4558–4561. - `ANTS_RC_SOURCES`, with the identical ANTS-3833 paragraph, in test_claude, test_audit, test_dialogs and test_core. ANTS-…

Quote: `# ANTS-4932 § 2.3 — setupClaudeMcpProviders() registers the project-scoped`

Remedy: one function that adds the split-class source-list defines to a bundle, called once per bundle.

### Row 1222 — Medium — `CMakeLists.txt:132`

review-readability, lane 45-build-system, Q1. **Disposition:** queued, ANTS-6587.

The `QT_VERSION_STR` regex is kept by hand in three places: the named-candidate probe (135), the scan fallback (158) and `cmake/QtVersionGuard.cmake:50`. The comment names only the guard as its twin. If someone widens the guard's pattern and misses the configure-time copies, the probe stops finding the header and the guard is silently not wired (only a warning). If they widen the probes and miss …

Quote: `# Same pattern the guard matches with, so "found here" at configure`

Remedy: define the pattern once, for example in a small `.cmake` file that both the probe and the guard `include()`.

### Row 1234 — Low — `CMakeLists.txt:1368`

review-readability, lane 45-build-system, Q1. **Disposition:** queued, ANTS-6587.

The relro/now/noexecstack link options are copied three times: ants-terminal 1368–1372, ants-mcpd 1447–1451 and ants-osc133-sign 1461–1465. The tree-sitter library also hand-copies part of `ants_warnings`' hardening flags (1401–1402). A new hardening flag will be added to one copy only.

Quote: `target_link_options(ants-terminal PRIVATE`

Remedy: put the link hardening on one INTERFACE target that all three link.

### Row 1235 — Medium — `tools/install-hooks.sh:125`

review-readability, lane 46-tools-ci-gate, Q1. **Disposition:** queued, ANTS-6617.

The hook pack's script set is written out by hand in six places: `hook_entries` (lines 71-89), this regex, and the glob `"$src_dir"/ants-*.sh "$src_dir"/_common.sh` at lines 153, 191, 222 and 237. They match today: the regex names 7 scripts, `hook_entries` names the same 7, and `hooks/` holds those 7 plus `_common.sh`. A stranger adding a hook to `hook_entries` alone would leave uninstall unable …

Quote: `pack_hook_regex='/ants-(session-preamble|bash-veto|read-roadmap-veto|drift-check|precompact-snapshot|inbox-notice|know-how-notice)\.sh$'`

Remedy: derive the regex and the copy list from one array. Cost: pinned — `docs/specs/ANTS-6310-know-how-store.md` § 4.4 names both `hook_entries` and `pack_hook_regex` in `tools/install-hooks.sh`. Searched: `pack_hook_regex|hook_entries` in docs/.

### Row 1236 — Low — `tools/e2e/smoke.sh:15`

review-readability, lane 46-tools-ci-gate, Q1. **Disposition:** queued under ANTS-6155.

`cases.sh:29` defines the same helper as `grep -qF` (a plain-text match) and has a comment saying the needle is not a regex. smoke.sh's copy treats the needle as a pattern. A stranger who copies a case or needle containing `.` or `[` from cases.sh into smoke.sh gets a different match. The other copied lines (`fails`, `pass`, `fail`, the `cd` and `source`) are identical.

Quote: `has() { grep -q "$2" <<<"$1"; }`

Remedy: move `has`/`pass`/`fail` into run.sh, using the `-F` form. Cost: one edit — Searched: "has()" / `` `has` `` in docs/ and tests/: only unrelated getter-name hits. No ANCHORS entry. dup:a0b146a0457e (confirmed; the divergence sits just past the tool's window).

### Row 1237 — Low — `tools/install-hooks.sh:188`

review-readability, lane 46-tools-ci-gate, Q1. **Disposition:** queued under ANTS-6098.

Uninstall's backup ignores a failed copy. Install's backup at line 230 has no `|| true`, so under `set -e` a failed copy aborts the install. No comment gives the reason. A stranger cannot tell which behaviour is intended, and "tidying" one to match the other changes whether a run continues without a backup.

Quote: `cp --no-dereference -- "$target" "${target}.ants-backup" 2>/dev/null || true`

Remedy: make the two agree, or add one line on why uninstall tolerates a failed backup. Cost: one edit — changes only these lines; no anchor pins them. dup:7ee304600876.

### Row 1254 — Medium — `tools/roadmap-conformance.py:39`

review-readability, lane 47-tools-roadmap-surveys, Q1. **Disposition:** queued, ANTS-6618.

The 21-value kind list is kept by hand in four places: here, `tools/roadmap-corpus-survey.py:42` (identical), `tools/roadmap-import-verify.py:27` (`TAXONOMY`), and `src/roadmapparse.cpp` `canonicalKinds()`. Only import-verify says it mirrors the C++ and must be updated with it. A stranger adding a kind in the C++ would update import-verify at most, and conformance and corpus-survey would then qui…

Quote: `CANONICAL_KINDS = {`

Remedy: move the list (and import-verify's `ALIASES`) into one shared module next to `roadmap_paths.py`, which exists for exactly this reason ("One copy, so a fix … reaches every script"), with a comment naming `canonicalKinds()`/`mappedKind()`.

### Row 1255 — Medium — `tools/roadmap-import-verify.py:40`

review-readability, lane 47-tools-roadmap-surveys, Q1. **Disposition:** queued under ANTS-6065.

This requires the ID prefix to start with a letter and has no synthesised `S`. But `idTokenPattern()` (`src/roadmapparse.cpp:1742`) allows a digit-first prefix that holds a letter (ANTS-3492) and `S` ids. The `roadmap-corpus-survey.py` docstring records that this exact blind spot once under-counted the corpus by about 14%. A stranger would assume every declaring bullet gets checked; bullets with …

Quote: `BULLET = re.compile(r"^- (?:📋|🚧|✅|💭) \[([A-Za-z][A-Za-z0-9_-]*-\d+)\]")`

Remedy: match the canonical grammar, or comment why it is narrower.

Also reported as: "readability lane 47-tools-roadmap-surveys Info correctness lead tools/roadmap-import-verify.py:40"

### Row 1262 — Low — `tools/spec-header-survey.py:285`

review-readability, lane 47-tools-roadmap-surveys, Q1. **Disposition:** queued, ANTS-6618.

The Usage text (26-27) does not mention `--values`, yet `tests/features/spec_field_extent/test_spec_field_extent.cpp:31` relies on it. The help and the parser disagree.

Quote: `if "--values" in sys.argv:`

Remedy: add `--values` to Usage.

### Row 1263 — Low — `tools/migrate_expect_helper.py:477`

review-readability, lane 47-tools-roadmap-surveys, Q1. **Disposition:** queued under ANTS-6152.

The FAIL-shim regex appears here, at 263 and (in detection form) at 168. Separately, the "read the body's indent, check the first 200 characters, insert `expect_reset();`" block appears three times (280-285, 434-439, 456-462). A fix to either would need three edits.

Quote: `r"if\s*\(\s*runMain\((?P<args>[^)]*)\)\s*!=\s*0\s*\)\s*FAIL\(\)\s*;",`

Remedy: one shim constant and one insert-reset helper.

### Row 1272 — Medium — `packaging/flatpak/make-flathub-manifest.sh:44`

review-readability, lane 48-hooks-packaging, Q1. **Disposition:** queued, ANTS-6599.

Four copies read the base version from CMakeLists.txt: - `check-version-drift.sh:33` - `release.sh:98`, whose comment says "same regex the drift script trusts" - `src/remotecontrol_docs.cpp:2102` - this one, which has diverged: it has no `project(` anchor and takes the first three-part `VERSION` anywhere in the file. It works today only because line 1 is `cmake_minimum_required(VERSION 3.20)`, wh…

Quote: `version=$(grep -oE 'VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' "$repo/CMakeLists.txt" | head -n1 | awk '{print $2}')`

Remedy: use the anchored regex here, or call `release.sh`'s/the drift script's extractor.

### Row 1273 — Medium — `packaging/release.sh:167`

review-readability, lane 48-hooks-packaging, Q1. **Disposition:** queued under ANTS-6158.

There are three hand-kept copies of the Keep-a-Changelog category set: - line 167 (`unreleased_has_content`) - line 180 (`section_has_content`) - line 241 (`split("Added Changed Deprecated Removed Fixed Security", …)` in `merge_unreleased`) The bodies at 163-169 and 176-182 are otherwise identical. `unreleased_has_content` is `section_has_content "Unreleased"` written out again. Someone changing …

Quote: `inseg && /^### (Added|Changed|Deprecated|Removed|Fixed|Security)/ { found=1 }`

Remedy: have `unreleased_has_content` call `section_has_content Unreleased`, and keep the category list in one variable.

### Row 1274 — Medium — `hooks/ants-inbox-notice.sh:41`

review-readability, lane 48-hooks-packaging, Q1. **Disposition:** queued, ANTS-6599.

Three hooks sanitise a session id three different ways: - `_common.sh:47` (`ants_validate_session_id`) rejects anything outside `[a-zA-Z0-9_-]{1,64}`. - This copy rejects underscore and has no length limit. - `ants-session-preamble.sh:32` strips bad characters (`tr -cd 'A-Za-z0-9_-' | head -c 64`) rather than rejecting. The preamble reads the file that precompact writes under the validated id, so…

Quote: `*[!A-Za-z0-9-]*) sid="" ;;`

Remedy: give `_common.sh` a non-exiting validator and use it in all three, or name the other two sites beside `ants_validate_session_id`.

### Row 1275 — Medium — `packaging/archlinux/PKGBUILD:66`

review-readability, lane 48-hooks-packaging, Q1. **Disposition:** queued, ANTS-6599.

Three copies run the test suite: this one, `debian/rules:22-25` and `ants-terminal.spec:285-364`. The spec's `%check` has three test-environment fixes the other two lack: `QT_QPA_PLATFORM=offscreen`, `LC_ALL=C.UTF-8` and `-LE perf`. The twin comments in all three say "a test-environment fix made there belongs here too", yet none of those three fixes was ported, and nothing says whether that was d…

Quote: `# The full ctest suite, Qt widget tests included. Twin of %check in`

Remedy: either port the three settings or, in each twin comment, name which settings are left out on purpose and why.

### Row 1293 — Low — `packaging/completions/ants-terminal.bash:24`

review-readability, lane 48-hooks-packaging, Q1. **Disposition:** queued, ANTS-6599.

`src/main.cpp:177-181` takes two arguments, `<export.jsonl> <project-root>`. The zsh completion knows both (`_ants-terminal:19`). Bash (and fish, line 16) complete only the file, so the second argument gets option names instead of directories. The bash, fish and zsh headers also say the CLI is "parsed in src/main.cpp via QCommandLineParser", but `--export-roadmaps` and `--import-roadmap` are hand…

Quote: `--import-roadmap)`

Remedy: complete a directory for the second `--import-roadmap` argument in bash and fish, and adjust the header.

### Row 1347 — Info — `src/passheadingwrite.cpp:269`

review-readability, lane 14-roadmap-parse-render-a, correctness lead. **Disposition:** queued, ANTS-6611.

- `src/passheadingwrite.cpp:269` and `:331` — Pass-heading dates come from `QDate::currentDate()`, while `remotecontrol_roadmap_log.cpp:79` uses `RoadmapClock::today()`. A test override does not reach the markdown pass path, and a test could flake at midnight.

### Row 1349 — Info — `src/passheadingwrite.cpp:94`

review-readability, lane 14-roadmap-parse-render-a, correctness lead. **Disposition:** queued, ANTS-6611.

The pass-headings headline locator does not drop trailing punctuation, while `RoadmapParse::normaliseHeadline` does. The same locator can match on one dialect and miss on the other.

### Row 1378 — Info — `src/roadmapmigrateload.cpp:913`

review-readability, lane 22-mcp-transport, correctness lead. **Disposition:** queued, ANTS-6606.

The leaf-prefix fallback has no `"ROOT"` default, so an empty folder name gives an empty prefix, unlike the three other copies.

### Row 1388 — Info — `src/remotecontrol_roadmap_query_verb.cpp:3253-3309`

review-readability, lane 24-mcp-roadmap-query-log-b, correctness lead. **Disposition:** queued, ANTS-6604.

Rows from a `section=` query never carry `evidence`, while rows from a whole-file query do.

### Row 1425 — Info — `src/doclint.cpp:671`

review-readability, lane 31-doc-engines, correctness lead. **Disposition:** queued, ANTS-6598.

The same heading-parser mismatch as the HIGH above. `headingsOf` sees indented headings DocIntegrity does not, so `seen`-based slug suffixes can differ and an inserted TOC row could link a wrong `-1` slug.

### Row 1429 — Info — `src/speclog.cpp:319`

review-readability, lane 32-spec-engines, correctness lead. **Disposition:** queued, ANTS-6613.

`slRowCells` ignores the `\|` this function writes at line 357. A row with an escaped pipe miscounts its columns, so the gap-continuation test ends the table early.

### Row 1432 — Info — `src/specparse.cpp:107`

review-readability, lane 32-spec-engines, correctness lead. **Disposition:** queued, ANTS-6613.

`[Ii]nvariants` misses an `## INVARIANTS` heading, while speclog's `findSectionHeading` (fully case-insensitive) finds it. `append_inv` would insert under it while its duplicate guard reads no invariants.

## Other bugs

### Row 2 — Medium — `ptyhandler.cpp:621-625`

review-code, lane 01-vt-parser-pty, dim 2. **Disposition:** queued, ANTS-6658.

The injected SGR reset lands in whatever state the parser is in, not in Ground.

Quote: `emit dataReceived(QByteArrayLiteral("\033[0m"));`

Remedy: inject only when the parser is in Ground with `m_utf8Remaining == 0`. For example, VtStream could ask the parser for an "at ground" flag before feeding the reset. Or emit the reset after draining, from VtStream, under the same check.

### Row 3 — Low — `ptyhandler.cpp:624`

review-code, lane 01-vt-parser-pty, dim 2. **Disposition:** queued, ANTS-6658.

the synthetic `\033[0m` goes through `beginFeed`, so it lands in `VtBatch::rawBytes`. The header describes those as "the bytes it already read", which feed session logging and asciicast recording. Recordings and logs therefore contain bytes the child never wrote.

Quote: `vtstream.h:62-65`

Remedy: feed the reset to the parser without appending it to the raw bytes, or document that it is included.

### Row 4 — Low — `vtparser.cpp:445`

review-code, lane 01-vt-parser-pty, dim 2. **Disposition:** queued, ANTS-6658.

C0 controls other than BEL/ESC/CAN/SUB are appended to the OSC payload. The Williams `osc_string` state, which `docs/escape-sequences.md` names as the model, ignores 0x00-0x17, 0x19 and 0x1C-0x1F. Title, OSC 8 URI and OSC 52 payloads can therefore carry raw control bytes. I did not check whether `TerminalGrid::handleOsc` strips them.

Quote: `else if (!appendUtf8(m_oscString, ch, kMaxStringBytes))`

Remedy: drop C0 bytes in `OscString`. Leave DCS alone: Williams passes C0 through there.

### Row 5 — Low — `ptyhandler.cpp:651-671`

review-code, lane 01-vt-parser-pty, dim 16. **Disposition:** queued, ANTS-6658.

at EOF, `waitpid(WNOHANG)` often returns 0. On Linux the slave closes, causing the master's EIO, before the exiting process becomes reapable. When that happens, `finished(-1)` is emitted, the real exit code is lost, and the zombie stays until `~Pty` (tab close).

Quote: `waitpid(WNOHANG)`

Remedy: when `w == 0`, re-poll briefly (a short `QTimer` retry) before emitting `finished`.

### Row 10 — Medium — `terminalgrid.cpp:672–673, 797–800`

review-code, lane 02-terminal-grid, dim 2. **Disposition:** queued, ANTS-6653.

(`case 'S': scrollUp(param(0))`, `case 'r': setScrollRegion(…)`, `case 's': saveCursor()`) — `handleCsi` dispatches on the final byte without checking the intermediate or private marker, except for `m`, `p`, `q`, `c`, `n`, `u`, `h`, `l` and `g`.

Quote: `case 'S': scrollUp(param(0))`

Remedy: require `a.intermediate.empty()` on every case that does not explicitly handle a marker.

### Row 11 — Medium — `terminalgrid.cpp:3157`

review-code, lane 02-terminal-grid, dim 2. **Disposition:** queued, ANTS-6653.

Any DCS containing a `q` is decoded as Sixel. XTGETTCAP `DCS + q 4d73 ST`, which Neovim sends to probe terminal capabilities (I have not confirmed which version), becomes a 1-column image of black pixels at the cursor. Each one also uses up an inline-image slot.

Quote: `size_t qpos = payload.find('q'); if (qpos == std::string::npos) return;`

Remedy: treat the payload as Sixel only when it matches `[0-9;]*q`.

### Row 12 — Medium — `terminalgrid.cpp:3538–3541, 3620–3623, 3692–3695`

review-code, lane 02-terminal-grid, dim 2. **Disposition:** queued, ANTS-6653.

A Kitty response is sent for every command: when `i` is absent (as `i=0`), and whatever `q=1` or `q=2` asks.

Quote: `std::string resp = "\x1B_Gi=" + std::to_string(imageId) + ";OK\x1B\\";`

Remedy: honour `q`, and send nothing when neither `i` nor `I` was given.

Also reported as: "readability lane 02-terminal-grid Info correctness lead src/terminalgrid.cpp:3539,3548,3621,3693"

### Row 14 — Medium — `terminalgrid.cpp:2540`

review-code, lane 02-terminal-grid, dim 4. **Disposition:** queued, ANTS-6653.

Two accounting models have drifted apart. `recomputeImageBudget` counts a shared Kitty and inline buffer once (on alt-screen entry/exit and `d=a`), but the per-image releases still subtract the full cost from each container.

Quote: `3531`

Remedy: one accounting model, for example always recompute, since N ≤ 300.

### Row 15 — Medium — `terminalgrid.cpp:2470–2478`

review-code, lane 02-terminal-grid, dim 4. **Disposition:** queued, ANTS-6653.

Clearing for alt-screen entry leaves `isWideChar` / `isWideCont` (and `softWrapped`) set from the main screen. `clearRow` resets them.

Quote: `for (auto &c : line.cells) { c.codepoint = ' '; c.attrs = CellAttrs{}; … }`

Remedy: reset both flags and `softWrapped`, or call `clearRow(r)` for every row.

### Row 16 — Medium — `terminalgrid.cpp:2912–2955`

review-code, lane 02-terminal-grid, dim 2. **Disposition:** queued, ANTS-6653.

On a width change, every trailing blank row survives `joinLogical` and `rewrap`. So when rows also shrink, the overflow taken from the front pushes the visible content, including the prompt line, into scrollback and leaves blank rows on screen.

Quote: `const int overflow = std::max(0, static_cast<int>(reflowed.size()) - rows);`

Remedy: track the cursor's position in its logical line through `rewrap`, and drop trailing blank logical lines below the cursor before computing the overflow.

### Row 17 — Medium — `terminalgrid.cpp:2786–2887`

review-code, lane 02-terminal-grid, dim 2. **Disposition:** queued, ANTS-6653.

Reflowing scrollback changes how many lines it holds, but the global line numbers in `PromptRegion` (`startLine`, `endLine`, `outputStartLine`) are not remapped. Only eviction is tracked, through `m_promptRegionShift`.

Quote: `m_scrollback = std::move(newScrollback);`

Remedy: remap each region's lines through the old→new line map that the reflow builds.

### Row 18 — Medium — `terminalgrid.cpp:1045`

review-code, lane 02-terminal-grid, dim 2. **Disposition:** queued under ANTS-5818/ANTS-5819 (partial).

RIS resets values that the header documents as monotonic or never reused:

Quote: `*this = TerminalGrid(m_rows, m_cols);`

Remedy: carry these across RIS the same way `m_quotas` is carried.

### Row 19 — Medium — `inlineimagedecode.cpp:97`

review-code, lane 02-terminal-grid, dim 3. **Disposition:** queued, ANTS-6653.

OSC 1337 passes `format=nullptr`, so Qt guesses the format from the bytes. Untrusted PTY output therefore reaches every installed QImageIO plugin (TIFF, SVG, and the kimageformats set). Kitty, by contrast, is pinned to `"PNG"`.

Quote: `terminalgrid.cpp:1525–1526`

Remedy: check the detected format against a short allowlist (PNG, JPEG, GIF) before decoding.

### Row 20 — Low — `terminalgrid.cpp:1953–2025`

review-code, lane 02-terminal-grid, dim 2. **Disposition:** queued, ANTS-6653.

DCH/ICH shift cells but do not shift or drop OSC 8 spans, and do not break wide pairs, so links end up covering different text. Fix: `dropHyperlinkSpans` plus a column shift, and `breakWidePairsAround`.

Quote: `dropHyperlinkSpans`

Remedy: `dropHyperlinkSpans` plus a column shift, and `breakWidePairsAround`.

Also reported as: "readability lane 02-terminal-grid Info correctness lead src/terminalgrid.cpp:1953-1990"

### Row 21 — Low — `terminalgrid.cpp:2041–2048`

review-code, lane 02-terminal-grid, dim 4. **Disposition:** queued under ANTS-5218.

moves `m_hyperlinkStartRow` but `scrollDown`, `insertLines` and `deleteLines` do not, so a link open across RI or IL lands on the wrong rows.

Quote: `scrollUp`

### Row 22 — Low — `inlineimagedecode.cpp:93`

review-code, lane 02-terminal-grid, dim 4. **Disposition:** queued, ANTS-6653.

against `terminalgrid.cpp:1098`, which refuses truncated payloads only for OSC 8 and 52. The grid still decodes a truncated OSC 1337 prefix. Fix: add `"1337"` to the refusal.

Quote: `if (action.truncated) return nullptr;`

Remedy: add `"1337"` to the refusal.

### Row 23 — Low — `terminalgrid.cpp:348`

review-code, lane 02-terminal-grid, dim 2. **Disposition:** queued, ANTS-6653.

treats a valid line 0 as unset; the header says unset is −1. Fix: `>= 0`.

Quote: `(pr.outputStartLine > 0) ? …`

Remedy: `>= 0`.

### Row 24 — Low — `terminalgrid.cpp:3507–3536`

review-code, lane 02-terminal-grid, dim 2. **Disposition:** queued, ANTS-6653.

Kitty `d=A` and `d=I` (and the other delete selectors) do nothing. Lowercase `d=i` frees the image data but leaves its placement on screen, which is the reverse of the spec.

Quote: `d=A`

### Row 25 — Low — `terminalgrid.h:713`

review-code, lane 02-terminal-grid, dim 2. **Disposition:** queued, ANTS-6653.

one Kitty keyboard flag stack is shared by the main and alt screens; the spec requires separate stacks.

### Row 26 — Low — `terminalgrid.cpp:1470`

review-code, lane 02-terminal-grid, dim 2. **Disposition:** queued, ANTS-6653.

OSC 9;N subcommands other than 4 (for example ConEmu's 9;9 cwd report) become desktop notifications.

### Row 27 — Low — `terminalgrid.cpp:1559–1566`

review-code, lane 02-terminal-grid, dim 2. **Disposition:** queued, ANTS-6653.

iTerm2 `width=100px`, `50%` and `auto` are parsed by `stoi` as a cell count.

Quote: `width=100px`

### Row 28 — Low — `terminalgrid.cpp:917`

review-code, lane 02-terminal-grid, dim 2. **Disposition:** queued, ANTS-6653.

CPR reports the absolute row even when origin mode (DECOM) is set.

### Row 29 — Low — `terminalgrid.h:963`

review-code, lane 02-terminal-grid, dim 5. **Disposition:** queued, ANTS-6653.

is never closed. There is no destructor, and RIS orphans the handle and silently turns the debug log off.

Quote: `m_debugFile`

Also reported as: "readability lane 02-terminal-grid Info correctness lead src/terminalgrid.cpp:1045"

### Row 33 — Medium — `terminalwidget.cpp:3227-3238`

review-code, lane 03-terminal-widget-a, dim 2. **Disposition:** queued, ANTS-6654.

Kitty Ctrl+letter is mis-encoded.

Quote: `:3214`

Remedy: take the key code from `event->key()` (Key_A..Z → 'a'..'z', with the other unshifted keys mapped) rather than from `text()`.

Also reported as: "readability lane 03-terminal-widget-a Low Q4 src/terminalwidget.cpp:3226; readability lane 03-terminal-widget-a Info correctness lead src/terminalwidget.cpp:3227-3238"

### Row 34 — Medium — `terminalwidget.cpp:3233`

review-code, lane 03-terminal-widget-a, dim 2. **Disposition:** queued, ANTS-6654.

under Kitty flag 1, Shift alone is treated as an ambiguous modifier.

Quote: `\e[97;2u`

Remedy: skip CSI-u when the only modifier is Shift and flag 8 is not set.

### Row 35 — Medium — `terminalwidget.cpp:2242-2245`

review-code, lane 03-terminal-widget-a, dim 2. **Disposition:** queued, ANTS-6654.

IME commits skip `sendKeyData`.

Quote: `QByteArray data = event->commitString().toUtf8(); ptyWrite(data);`

Remedy: send commits through a text variant of `sendKeyData`, stamp the keystroke time and reset the scroll. Broadcast needs a text-carrying callback, because the current one takes a `QKeyEvent`.

### Row 36 — Medium — `terminalwidget.cpp:2529-2535`

review-code, lane 03-terminal-widget-a, dim 2. **Disposition:** queued, ANTS-6654.

a second failure with the same exit code never fires `commandFailed`.

Quote: `if (exitCode != 0 && exitCode != m_lastTrackedExitCode)`

Remedy: emit from the `commandFinished` callback when the code is non-zero, under `m_commandFinishedBudget`.

Also reported as: "readability lane 03-terminal-widget-a Info correctness lead src/terminalwidget.cpp:2530-2531"

### Row 37 — Low — `terminalwidget.cpp:2770`

review-code, lane 03-terminal-widget-a, dim 3. **Disposition:** queued, ANTS-6654.

pasted screenshots are not private from the start.

Quote: `:2789-2790`

Remedy: use `ensurePrivateDir` and create the file owner-only; emit `captureFailed` on failure.

Also reported as: "readability lane 03-terminal-widget-a Info correctness lead src/terminalwidget.cpp:2770"

### Row 40 — Low — `terminalwidget.cpp:2185-2200`

review-code, lane 03-terminal-widget-a, dim 2. **Disposition:** queued, ANTS-6654.

high-resolution wheel events over-report.

Quote: `QWheelEvent`

Remedy: add up `angleDelta` and report once per 120 units.

### Row 41 — Low — `terminalwidget.cpp:2887`

review-code, lane 03-terminal-widget-a, dim 3. **Disposition:** queued, ANTS-6654.

the risky-paste preview hides the tail of a long line, which is where the dangerous part can sit. — fix: wrap long lines instead of cutting them, or put the full payload in a scroll area.

Quote: `if (ln.size() > 120) ln = ln.left(117) + "...";`

Remedy: wrap long lines instead of cutting them, or put the full payload in a scroll area.

### Row 44 — Medium — `terminalwidget.cpp:3664`

review-code, lane 04-terminal-widget-b, dim 2. **Disposition:** queued, ANTS-6654.

this branch serves every mode without SGR, not just X10 mode 9. xterm ctlseqs says normal tracking (1000/1002/1003) reports release as Cb low bits = 3. The same branch also builds `cb = button + mods + 32` at :3667 without the +32 motion bit, which the SGR branch adds at :3656.

Quote: `if (release) return; // X10 doesn't report release`

Remedy: in the non-SGR path, encode release as button 3, add +32 for drag motion, and suppress release only for mode 9 X10.

Also reported as: "readability lane 04-terminal-widget-b Info correctness lead src/terminalwidget.cpp:3664"

### Row 45 — Medium — `terminalwidget.cpp:3686-3694`

review-code, lane 04-terminal-widget-b, dim 2. **Disposition:** queued, ANTS-6654.

Back, Forward and other extra buttons match none of the branches, so `button` stays 0.

Quote: `int button = 0; … else if (!press && !release) button = 35;`

Remedy: return early for buttons that aren't left, middle or right (or map them to 128+ per xterm).

### Row 46 — Medium — `terminalaccessible.cpp:91-95`

review-code, lane 04-terminal-widget-b, dim 2. **Disposition:** queued, ANTS-6654.

when `offset` is already on a boundary (for example the first character of a word), `toPreviousBoundary()` returns the start of the previous segment, not its end. The result is the segment two before.

Quote: `f.setPosition(offset); int end = f.toPreviousBoundary(); … int start = f.toPreviousBoundary();`

Remedy: use `int end = f.isAtBoundary() ? offset : f.toPreviousBoundary();`.

Also reported as: "readability lane 04-terminal-widget-b Info correctness lead src/terminalaccessible.cpp:92"

### Row 47 — Medium — `terminalaccessible.cpp:20-21`

review-code, lane 04-terminal-widget-b, dim 2. **Disposition:** queued, ANTS-6654.

spec ANTS-1078 §2.3 says "a terminal row is both", and INV-5 says `LineBoundary` returns "the line containing offset". But `QTextBoundaryFinder::Line` finds wrap opportunities, so the AT gets a word-sized fragment. The code's own comment (ANTS-5831) admits this.

Quote: `case QAccessible::LineBoundary: case QAccessible::ParagraphBoundary: return QTextBoundaryFinder::Line;`

Remedy: for Line/Paragraph, split on `'\n'` in `accessibleText()` instead of using the finder.

### Row 49 — Medium — `terminalwidget.cpp:5242`

review-code, lane 04-terminal-widget-b, dim 2. **Disposition:** queued, ANTS-6654.

bookmarks store raw global line numbers, and nothing in my range or elsewhere in `src/` adjusts them (searched `m_bookmarks`). The same applies to `m_searchMatches` (cleared only at :4715 and :4725).

Quote: `m_bookmarks.push_back(globalLine);`

Remedy: store indices relative to `scrollbackPushed()` (or shift by the eviction count), and drop entries that fall off the ring.

### Row 50 — Medium — `terminalwidget.cpp:4674`

review-code, lane 04-terminal-widget-b, dim 2. **Disposition:** queued, ANTS-6654.

when neither `code` nor `kate` is found, the editor falls back to `xdg-open` (:4583). As far as I recall, xdg-utils' `xdg-open` rejects any argument starting with `-`, including `--` ("unexpected option"). I didn't check this on the machine.

Quote: `args << "--" << filePathSafe;`

Remedy: leave out `--` for `xdg-open` (`pathSafe` already prefixes `./` to a leading `-`).

Also reported as: "readability lane 04-terminal-widget-b Info correctness lead src/terminalwidget.cpp:4671"

### Row 51 — Low — `terminalwidget.cpp:3309`

review-code, lane 04-terminal-widget-b, dim 4. **Disposition:** queued, ANTS-6654.

the HTML half of a rich copy drops combining marks. The plain half (`selectedText` → `a11yComposeCols`) keeps them, so the two halves of one clipboard item differ.

Quote: `runText += QString::fromUcs4(... &cp, 1);`

Remedy: append `combiningAt(line, col)`.

### Row 52 — Low — `terminalwidget.cpp:4677`

review-code, lane 04-terminal-widget-b, dim 7. **Disposition:** queued, ANTS-6654.

the return value is ignored, so a failed launch is silent.

Quote: `QProcess::startDetached(editor, args);`

Remedy: emit `captureFailed` on false.

### Row 53 — Low — `terminalwidget.cpp:5218`

review-code, lane 04-terminal-widget-b, dim 7. **Disposition:** queued, ANTS-6654.

the write result is unchecked, so a full disk gives a silently headerless cast.

Quote: `m_recordFile->write(header.toUtf8() + "\n");`

Remedy: check the result and stop or report.

### Row 54 — Low — `terminalwidget.cpp:3353-3355`

review-code, lane 04-terminal-widget-b, dim 2. **Disposition:** queued, ANTS-6654.

`delta` counts columns, but one arrow moves one character.

Quote: `int count = std::abs(delta); … arrows += arrow;`

Remedy: count characters between the two columns, skipping `isWideCont` cells.

### Row 55 — Low — `terminalwidget.cpp:6136-6146`

review-code, lane 04-terminal-widget-b, dim 2. **Disposition:** queued, ANTS-6654.

the fish history parser keeps the `when: …` and `paths:` lines as commands.

Remedy: accept only `- cmd:` lines in the fish file.

### Row 56 — Low — `terminalwidget.cpp:4529-4532`

review-code, lane 04-terminal-widget-b, dim 3. **Disposition:** queued, ANTS-6654.

`urlRe` (:4364) matches `file://` and `ftp://`, makes spans with `isFilePath=false`, and Ctrl+click sends them here to `QDesktopServices::openUrl` with no confirmation. The comment is wrong for those spans.

Quote: `// … file-path spans go through openFileAtPath, never here.`

Remedy: confirm or refuse `file://` from regex spans, and correct the comment.

### Row 57 — Medium — `mainwindow_tabs.cpp:825`

review-code, lane 05-mainwindow, dim 4. **Disposition:** queued, ANTS-6636.

closing a tab checks for a running program in only one pane: the focused pane, or else the first. Closing the window checks every pane (`mainwindow.cpp:2128`, `for (TerminalWidget *t : liveTerminals())`). So a split tab with `vim` in an unfocused pane closes silently and kills it. This is the guard ANTS-1102 exists for.

Quote: `TerminalWidget *term = activeTerminalInTab(w);`

Remedy: in `closeTab`, loop over `w->findChildren<TerminalWidget*>()` plus `w` itself, as `closeEvent` does.

### Row 58 — Medium — `mainwindow.cpp:2298`

review-code, lane 05-mainwindow, dim 3. **Disposition:** queued, ANTS-6636.

an OSC 9/777 title and body (`mainwindow_tabs.cpp:383-388`) come from whatever is printing to the PTY, and here they become `notify-send`'s argv with no end-of-options marker. A title like `--hint=string:image-path:/…`, `-w` or `-a Bank` is parsed as an option, not as text. Notification servers may also render markup in the body. Whether the grid sanitises these values was not checked (it is outs…

Quote: `return QProcess::startDetached(QStringLiteral("notify-send"), {shown, body});`

Remedy: pass `{"--", shown, body}`, and escape markup in the body.

### Row 59 — Medium — `mainwindow_tabs.cpp:454-461`

review-code, lane 05-mainwindow, dim 2. **Disposition:** queued, ANTS-6636.

every terminal is wired to the focused session's `toolFinished`/`sessionStopped`, since `claudeintegration.cpp:348` emits only `if (isFocused)`. Scenario: tab B shows an AskUserQuestion; the user switches to tab A, where Claude is working; A's next PostToolUse clears B's "awaiting input" dot. B cannot re-light it, because `claudeQuestionDetected` returns early for a terminal that is not focused (…

Quote: `connect(m_claudeIntegration, &ClaudeIntegration::toolFinished, terminal, [terminal](…){ terminal->clearClaudeQuestionPrompt(); });`

Remedy: clear only the terminal whose shell owns the hook's session (use the session id or `shellForSessionId`).

### Row 61 — Medium — `mainwindow_tabs.cpp:719`

review-code, lane 05-mainwindow, dim 2. **Disposition:** queued, ANTS-6636.

(in `cleanupEmptySplitters`, root-promotion branch) when a shell exits in a *background* split tab (the `shellExited` path at `:249`), promoting the last pane switches the user to that tab and moves focus there. The same applies to `splitCurrentPane:606`, but there the tab is already current.

Quote: `m_tabWidget->setCurrentIndex(tabIdx);`

Remedy: record `m_tabWidget->currentWidget()` before `removeTab`, and restore that widget afterwards.

### Row 62 — Medium — `mainwindow.cpp:2224-2225`

review-code, lane 05-mainwindow, dim 16. **Disposition:** queued, ANTS-6636.

`Config::save()` returns `void` and can silently fail to write: a latched load failure (`config.cpp:183`) or a failed atomic write. The claimed ants-mcpd snapshots are deleted anyway, so those savings are lost for good. Spec ANTS-5311 §5 covers only a crash between save and release (which double-counts), not a save that did not happen.

Quote: `2233-2234`

Remedy: make `save()` report success, and call `releaseClaimed` only on success.

### Row 63 — Medium — `mainwindow_tabs.cpp:1006`

review-code, lane 05-mainwindow, dim 16. **Disposition:** queued, ANTS-6636.

after Restart now (`mainwindow.cpp:2840`), only the Cancel *button* clears the relaunch flag. If the user dismisses the window-close dialog with Escape or its title-bar close, the flag stays set. The next ordinary quit, possibly hours later, then relaunches the app unasked.

Quote: `connect(cancelBtn, &QPushButton::clicked, this, [] { g_relaunchOnQuit = false; });`

Remedy: clear the flag on the dialog's `rejected`/`finished` signal, or on `destroyed` unless Close anyway was chosen.

### Row 64 — Low — `mainwindow.h:770`

review-code, lane 05-mainwindow, dim 8. **Disposition:** queued, ANTS-6636.

The last-declared member is destroyed after `~MainWindow`'s body but *before* `~QWidget` deletes the children `ClaudeIntegration` (which joins the dispatch worker) and `RemoteControl`.

Quote: `std::unique_ptr<ants::RootProvider> m_rootProvider = makeRootProvider(this);`

Remedy: tear down `m_claudeIntegration` and `m_remoteControl` explicitly in `~MainWindow`'s body.

### Row 65 — Low — `mainwindow_sessions.cpp:243-247`

review-code, lane 05-mainwindow, dim 4. **Disposition:** queued, ANTS-6636.

The default tab, which `newTab()` registered with `trackTerminalShell`, is dropped without `releaseTerminalShell`, unlike `performTabClose:872-875`. Its pid stays in all three trackers.

Quote: `m_tabWidget->removeTab(0); defaultTab->deleteLater();`

### Row 66 — Low — `mainwindow_menus.cpp:94`

review-code, lane 05-mainwindow, dim 2. **Disposition:** queued, ANTS-6636.

`writeCommand` already appends `\n` (`terminalwidget.cpp:5609`), so this sends a stray second Enter. `menus.cpp:483` (`claudeLaunch(…) + QLatin1Char('\n')`) does the same thing, there into the starting Claude TUI.

Quote: `t->writeCommand("cd '" + escaped + "'\n");`

### Row 67 — Low — `mainwindow_menus.cpp:370-389`

review-code, lane 05-mainwindow, dim 2. **Disposition:** queued, ANTS-6636.

Reload User Themes rebuilds the Themes menu but not the command palette. The palette keeps dead "View > Themes > X" proxies and lacks the new themes. Fix: call `rebuildCommandPalette()`.

Quote: `rebuildCommandPalette()`

Remedy: call `rebuildCommandPalette()`.

### Row 69 — Low — `mainwindow_internal.h:47`

review-code, lane 05-mainwindow, dim 2. **Disposition:** queued, ANTS-6636.

has no `Q_OBJECT`, against qt.md's "without exception". `mainwindow.cpp:3303` and `mainwindow.cpp:485` manually `delete` parented children, against qt.md's "never manually delete a parented child". `[tool: clazy]`

Quote: `class StayOpenOnToggleFilter : public QObject {`

### Row 70 — Low — `qt.md`

review-code, lane 05-mainwindow, dim 13. **Disposition:** queued, ANTS-6636.

qt.md's "`tr()` around every user-visible string" is unmet across these files (menu labels, `"Theme: %1"`, `"Failed to start shell!"`, and others). Filed because the language standard is named in the tail as a contract. `[tool: clazy]`

Quote: `"Theme: %1"`

### Row 72 — Medium — `mcpdforwarder.cpp:117-126`

review-code, lane 06-mcp-transport, dim 16. **Disposition:** queued under ANTS-5912.

After `st->sent = true`, the terminal has the request and may already have carried out a write. Three cases still return `no_terminal`, which says "no Ants Terminal is running" with the hint "start Ants Terminal": the socket closing mid-reply, closing with no reply, and a reply over 10 MiB.

Quote: `:105-107`

Remedy: when `st->sent` is true, every failure should return `terminalTimeoutEnvelope` (or a new `terminal_dropped` code), with its "check its effect before retrying" hint.

### Row 73 — Medium — `remotecontrol.cpp:710-714`

review-code, lane 06-mcp-transport, dim 2. **Disposition:** queued under ANTS-5921/ANTS-5936.

This is the last fallback in `rlResolveCounterPrefix` (`:743`). Its result goes straight into new ids. Both callers I checked (`remotecontrol_roadmap_log.cpp:999`, `remotecontrol_roadmap_log_batch.cpp:2632`) use it without checking it against `kIdPrefixShape` or `RoadmapFoldIn::isValidIdPrefix`.

Quote: `QString pfx = leaf.left(4).toUpper();`

Remedy: `my.app`→`MY.A`, `2026`→`2026` (no letter), `a b`→`A B`, `über`→`ÜBER`.

### Row 74 — Medium — `mcpdmain.cpp:197-202`

review-code, lane 06-mcp-transport, dim 8. **Disposition:** queued, ANTS-6643.

(declaration order) together with `:342-349` (the 60 s EOF grace) and `mcpdtrustclient.h:17` (`kTrustPromptTimeoutMs = 120 * 1000`)

Quote: `:342-349`

Remedy: stop the pipeline's workers before `rc` goes out of scope, or declare `rc` before `pipeline`.

### Row 75 — Medium — `remotecontrol.cpp:202-213`

review-code, lane 06-mcp-transport, dim 3. **Disposition:** queued, ANTS-6643.

The `gitdir:` line of a `.git` file decides which project's store and roadmap a write is keyed to (`rcProjectRootFor`). A directory unpacked from an untrusted archive can carry a `.git` file reading `gitdir: /home/u/OtherProj/.git/worktrees/x`. A write verb whose `caller_cwd` is that directory is then sent to OtherProj's roadmap.

Quote: `:216-238`

Remedy: read `<gitDir>/gitdir` and require it to name `dir/.git`.

### Row 76 — Low — `remotecontrol.cpp:1412-1427`

review-code, lane 06-mcp-transport, dim 5. **Disposition:** queued, ANTS-6643.

. The comment at `:1291-1296` admits the scan is O(n²) and caps notes at 4096 characters. Bodies are "not routed through this cap", so a body of up to the 256 KiB request ceiling, full of unclosed `<parameter name=` openers, gets about k·n lazy scans, run twice, on a dispatch worker. I did not see the body's own size check (it is in another TU). Fix: apply a body cap before the scrub, or make the…

Quote: `"[\\s\\S]*?</parameter>"`

Remedy: apply a body cap before the scrub, or make the pair regex possessive or bounded.

### Row 77 — Low — `remotecontrol.cpp:1098-1102`

review-code, lane 06-mcp-transport, dim 9. **Disposition:** queued, ANTS-6643.

. This counts UTF-16 units, so it can cut an emoji's surrogate pair and write a lone surrogate into ROADMAP.md. The appended `…` also makes the result `maxLen+1` long. `rcElideBody` (`:459-465`) has the same split risk on `left`/`right`. Fix: step back one unit when the cut lands on a high surrogate.

Quote: `out.truncate(maxLen);`

Remedy: step back one unit when the cut lands on a high surrogate.

### Row 78 — Low — `mcpdsocket.cpp:84-90`

review-code, lane 06-mcp-transport, dim 16. **Disposition:** queued, ANTS-6643.

. A stale socket whose pid has been reused by another of this user's processes counts as live. If it has the newest mtime it beats the real terminal, so every forwarded verb answers `no_terminal`. The reaper (`:107`) only removes on ESRCH, so it never clears this file. Fix: test-connect the chosen candidate and try the next one on refusal.

Quote: `live = kill(pid,0)==0`

Remedy: test-connect the chosen candidate and try the next one on refusal.

### Row 79 — Low — `mcpdcall.cpp:68`

review-code, lane 06-mcp-transport, dim 16. **Disposition:** queued, ANTS-6643.

has no deadline. A handler that never ends its request hangs `--call` (and any pre-push hook calling it) forever. Fix: a single-shot timer that exits with `CallFailed`.

Quote: `if (!done) loop.exec();`

Remedy: a single-shot timer that exits with `CallFailed`.

### Row 83 — Medium — `src/claudeintegration.cpp:2882`

review-code, lane 07-claude-integration-a, dim 7. **Disposition:** queued, ANTS-6626.

the code doesn't distinguish these cases: an `arguments` that is a string, array or number becomes `{}` silently;

Quote: `const QJsonObject argsObj = mcp::coerceArgTypes(params.value("arguments").toObject(), …`

Remedy: refuse a non-object `arguments` with JSON-RPC `-32602` (or `bad_args`) naming its JSON type. Emit the "not an Ants bug" text only when the `arguments` key was absent.

### Row 84 — Medium — `src/claudeintegration.cpp:1426`

review-code, lane 07-claude-integration-a, dim 7. **Disposition:** queued, ANTS-6626.

an over-cap MCP request is dropped with no JSON-RPC reply.

Quote: `if (buf->size() > 256 * 1024) { socket->disconnectFromServer(); return; }`

Remedy: write a `-32600` "request exceeds 256 KiB" error before disconnecting, and copy the hook path's clear-and-latch.

Also reported as: "code lane 07-claude-integration-a Low dim 4 None"

### Row 85 — Medium — `src/claudeintegration.cpp:1176-1183`

review-code, lane 07-claude-integration-a, dim 3. **Disposition:** queued, ANTS-6626.

`get_session_info` skips the `<ants_mcp_data>` wrap as "server-generated state". Why it isn't server-generated: its `changed_files` and `current_tool` (`:3214-3216`) come from hook events, which any same-UID process may send (`:163-167`), and from transcript `tool_use` inputs. The code itself calls transcripts attacker-influenceable (`claudeintegration_sessions.cpp:1295`).

Quote: `toolName == QStringLiteral("get_session_info") || …`

Remedy: wrap `get_session_info`, or scrub `changed_files` and `current_tool` with the same close-tag and comment-marker scrub.

### Row 86 — Low — `src/claudeintegration_sessions.cpp:1146-1149`

review-code, lane 07-claude-integration-a, dim 2. **Disposition:** queued, ANTS-6626.

together with `:1196-1197` `if (toolName == "Read" && isSecretFileName(filePath)) emit sensitiveFileRead(filePath);` — `sensitiveFileRead` is said to fire "on every read" (header `:691`). It actually fires:

Quote: `:1196-1197`

Remedy: emit it only from the `PostToolUse` hook path.

### Row 87 — Low — `src/claudeintegration_sessions.cpp:275-291`

review-code, lane 07-claude-integration-a, dim 2. **Disposition:** queued, ANTS-6626.

when Claude exits, `m_activeSessionId`, `m_changedFiles` and `m_currentTool` are not cleared. `get_session_info` keeps returning the exited session's id and files, and `emit stateChanged(m_state, m_currentTool)` sends NotRunning with a stale tool name. Fix: clear all three as `setShellPid` does.

Quote: `m_activeSessionId`

Remedy: clear all three as `setShellPid` does.

### Row 88 — Low — `src/claudeintegration.cpp:3438`

review-code, lane 07-claude-integration-a, dim 7. **Disposition:** queued, ANTS-6626.

`const QJsonObject r = handler(params);` — the `ants/verifyTrustPrompt` handler runs on the Bulk worker outside the exception boundary. `:479-481` says a throw escaping a worker job "would terminate it and kill them all". Fix: wrap it in the same try/catch and reply `-32603`.

Quote: `const QJsonObject r = handler(params);`

Remedy: wrap it in the same try/catch and reply `-32603`.

### Row 89 — Medium — `claudeintegration_codenav.cpp:729`

review-code, lane 08-claude-integration-b, dim 2. **Disposition:** queued, ANTS-6626.

A declared alias for a required argument can't be sent on its own.

Quote: `required.append("pattern");`

Remedy: remove the aliased key from `required[]` (as `apply_edits` did, :2107) and keep the either/or check in the handler.

### Row 92 — Low — `claudeintegration_store.cpp:294-295`

review-code, lane 08-claude-integration-b, dim 2. **Disposition:** queued, ANTS-6626.

`roadmap_log` sends callers to a mode value `roadmap_query`'s schema doesn't list.

Quote: `"(or its alias mode:\"sections\")"`

Remedy: add `sections` to the enum, or drop the mention.

### Row 93 — Low — `claudeintegration_terminal.cpp:479-481`

review-code, lane 08-claude-integration-b, dim 2. **Disposition:** queued, ANTS-6626.

The `filter` alias accepts only a string, though `status` accepts a string or an array (:449-457).

Quote: `filterAliasProp["enum"] = statusEnum;`

Remedy: give each the same `anyOf` shape it is described as accepting.

### Row 96 — Low — `claudeintegration_orientation.cpp:2717-2718`

review-code, lane 08-claude-integration-b, dim 2. **Disposition:** queued, ANTS-6626.

`required` holds only `lane` (:2868-2870), while the detail says it is used "per (tool, caller_cwd)" and the rate limit is keyed on `caller_cwd` (:2720-2721).

Quote: `"Required: lane."`

### Row 97 — Medium — `remotecontrol_roadmap_query_verb.cpp:1059-1065`

review-code, lane 09-mcp-roadmap-query-log, dim 3. **Disposition:** queued, ANTS-6641.

Untrusted regex with no step limit. Where: `remotecontrol_roadmap_query_verb.cpp:1059-1065`, `1084-1089`, `1138`, `1105`. Example: `const mcp::QueryMatcher matcher(queryArg, queryMode);`

Remedy: in the `QueryMatcher` Regex branch, run `isCatastrophicRegex` and then `hardenUserRegex`, and cap `body_match` at 200 characters like `query`.

### Row 98 — Medium — `ROADMAP.md`

review-code, lane 09-mcp-roadmap-query-log, dim 3. **Disposition:** queued, ANTS-6641.

writes `new_text` with no sanitising, so it can inject structure into ROADMAP.md.

Quote: `new_text`

Remedy: pass `newHeadline` through `rcSanitizeBulletField(…, 500)` and refuse an embedded newline, on both paths.

### Row 101 — Medium — `remotecontrol_roadmap_query.cpp:268-338`

review-code, lane 09-mcp-roadmap-query-log, dim 2. **Disposition:** queued, ANTS-6641.

Pass-headings file append can create a duplicate pass, and its dry run reports the id under the real key.

Remedy: an `id_taken` check against `parseBullets(markdown)` plus a `seen` set, and `would_be_id` on the preview.

### Row 102 — Medium — `remotecontrol_roadmap_query.cpp:2667-2669`

review-code, lane 09-mcp-roadmap-query-log, dim 4. **Disposition:** queued, ANTS-6641.

The `lanes` column was missed by the ANTS-5094 sanitising pass.

Remedy: apply evidence's `addItem` rules to lanes.

### Row 103 — Low — `remotecontrol_roadmap_log.cpp:412`

review-code, lane 09-mcp-roadmap-query-log, dim 2. **Disposition:** queued, ANTS-6641.

Store-append dry run takes a different path from the real write on a machine whose store has no row for the project. `remotecontrol_roadmap_log.cpp:412` (`if (!dryRun) { … roadmapRebuildIfAbsent …`) skips the rebuild, so the preview drops to the `.roadmap-counter` markdown path (different `would_be_id`, a `line`, a `bullet`), while the real call rebuilds and allocates from the store. Fix: run the…

Quote: `if (!dryRun) { … roadmapRebuildIfAbsent …`

Remedy: run the rebuild check read-only under dry run and preview through the store path.

### Row 104 — Low — `remotecontrol_roadmap_log.cpp:4447-4448`

review-code, lane 09-mcp-roadmap-query-log, dim 7. **Disposition:** queued, ANTS-6641.

scrub leaked tool-XML from `new_text` but throw away the list of what was removed (`remotecontrol_roadmap_log.cpp:4447-4448`, 4546-4547: `QStringList scrubbed; rcScrubLeakedToolXml(newText, scrubbed);`). `amend_body` reports it as `note_scrubbed_params` (3656), and ANTS-6391 requires that report. Fix: echo it per row and per call.

Quote: `replace_text`

Remedy: echo it per row and per call.

### Row 105 — Low — `const bool alreadyPresent = !note.isEmpty() && before->body.contains(note); (remotecontrol_roadmap_log.cpp:1390)`

review-code, lane 09-mcp-roadmap-query-log, dim 7. **Disposition:** queued, ANTS-6641.

(`remotecontrol_roadmap_log.cpp:1390`) is a substring test. A short note such as "Done." that already appears anywhere in the body is not written. The envelope does say `note_already_present`. Fix: compare against whole appended lines.

Quote: `remotecontrol_roadmap_log.cpp:1390`

Remedy: compare against whole appended lines.

### Row 106 — Low — `remotecontrol_roadmap_log.cpp:1447-1462`

review-code, lane 09-mcp-roadmap-query-log, dim 2. **Disposition:** queued, ANTS-6641.

A flip to the item's current status still writes `status`, records an old==new history row and stamps `last_modified` (`remotecontrol_roadmap_log.cpp:1447-1462`). That contradicts the comment at 1442-1445: "an item nothing touched must not read as modified today". Fix: skip all three when `before->status == targetStatusWord`.

Quote: `last_modified`

Remedy: skip all three when `before->status == targetStatusWord`.

### Row 107 — Low — `remotecontrol_roadmap_query_verb.cpp:1824`

review-code, lane 09-mcp-roadmap-query-log, dim 3. **Disposition:** queued, ANTS-6641.

(`remotecontrol_roadmap_query_verb.cpp:1824`) echoes the argument raw. Every other echoed argument gets the 64/200-byte cap and control-byte replacement (INV-11, 938-939).

Quote: `remotecontrol_roadmap_query_verb.cpp:1824`

### Row 108 — Low — `if (plan && !doc.object().value("ok").toBool()) rlRestorePlan(...) (remotecontrol_roadmap_query_verb.cpp:4087)`

review-code, lane 09-mcp-roadmap-query-log, dim 7. **Disposition:** queued, ANTS-6641.

(`remotecontrol_roadmap_query_verb.cpp:4087`) also restores the plan after `write_failed`, where the store has already committed (`remotecontrol_roadmap_query.cpp:1659`). Re-applying an append plan sent with `force:true` files a duplicate. Fix: don't restore on `write_failed`.

Quote: `remotecontrol_roadmap_query_verb.cpp:4087`

Remedy: don't restore on `write_failed`.

### Row 109 — Low — `remotecontrol_roadmap_log.cpp:1926-1934`

review-code, lane 09-mcp-roadmap-query-log, dim 2. **Disposition:** queued, ANTS-6641.

Store-served pass flip and annotate find the pass in the file (`PassHeadingWrite::annotatePass(markdown, …)`, `remotecontrol_roadmap_log.cpp:1926-1934`), not in the store. ANTS-4485 (1952-1956) says an item the store holds must stay writable even when the file is behind.

Quote: `remotecontrol_roadmap_log.cpp:1926-1934`

### Row 111 — Low — `remotecontrol_roadmap_log.cpp:4685`

review-code, lane 09-mcp-roadmap-query-log, dim 11. **Disposition:** queued, ANTS-6641.

The `amend_field field:"section"` `locators[]` list has no count cap (`remotecontrol_roadmap_log.cpp:4685`), unlike the 500 cap on `amend_field_batch` and `amend_batch`.

Quote: `locators[]`

### Row 112 — Medium — `remotecontrol_roadmap_log_batch.cpp:1316-1317`

review-code, lane 10-mcp-roadmap-batch, dim 4. **Disposition:** queued, ANTS-6640.

`append_batch` changed this same gate at `:2524-2525` to `!writeTarget && preflightBullets.isEmpty()`. Its reason (ANTS-5371): "the store serves a project with no items yet, and the file it renders then has no bullets to parse."

Quote: `if (auto refusal = rlUnrecognisedFormat(roadmapPath, markdown, preflightBullets.isEmpty()))`

Remedy: pass `parsedNothing` only when there is no store write target. That means resolving the target before the gate, as `append_batch` does.

### Row 113 — Medium — `remotecontrol_roadmap_migrate.cpp:180`

review-code, lane 10-mcp-roadmap-batch, dim 2. **Disposition:** queued, ANTS-6640.

Refusal codes missing from the error-code taxonomy:

Remedy: add rows for all seven codes.

### Row 114 — Medium — `log_batch.cpp:1754`

review-code, lane 10-mcp-roadmap-batch, dim 9. **Disposition:** queued, ANTS-6640.

The escape does not double an existing backslash. A cell holding `a\|b` (a regex, say) is written as `a\\|b`.

Quote: `s.replace(QStringLiteral("|"), QStringLiteral("\\|"));`

Remedy: replace `\` with `\\` before escaping `|`.

### Row 115 — Low — `remotecontrol_roadmap_repair.cpp:385-394`

review-code, lane 10-mcp-roadmap-batch, dim 7. **Disposition:** queued, ANTS-6640.

The history note is attached only when `strip_runs` or `clear_placeholder_source` is set.

Quote: `if (!dryRun) rlAttachHistoryNote(env, store, hist);`

Remedy: attach the note whenever `!dryRun`.

Also reported as: "readability lane 25-mcp-roadmap-batch Info correctness lead src/remotecontrol_roadmap_repair.cpp:385-395"

### Row 116 — Low — `remotecontrol_roadmap_backfill.cpp:98-99`

review-code, lane 10-mcp-roadmap-batch, dim 9. **Disposition:** queued, ANTS-6640.

The walk covers every file in the archive directory, and the `-U0` diff carries no fence context.

Quote: `if (slash > 0) specs.insert(r.sourcePath->left(slash));`

Remedy: limit the directory pathspec to `*.md` files that match the archive naming rule.

### Row 117 — Low — `log_batch.cpp:948`

review-code, lane 10-mcp-roadmap-batch, dim 11. **Disposition:** queued under ANTS-5936.

The derived prefix skips the `^[A-Z][A-Z0-9_-]{0,15}$` check that `prefix_hint` gets (`:953`).

Quote: `prefix = QFileInfo(callerCanonical).fileName().left(4).toUpper();`

Remedy: run the same regex and fall back to `ROOT`.

### Row 118 — Low — `log_batch.cpp:2857-2876`

review-code, lane 10-mcp-roadmap-batch, dim 11. **Disposition:** queued, ANTS-6640.

`id_hint` has no upper bound. A hint near `INT64_MAX` overflows `++nextId` (`:2893`), which is undefined behaviour, and it burns the counter.

Quote: `nextId = hint;`

Remedy: cap the hint, for example at the high-water mark plus 10⁶.

### Row 119 — Low — `log_batch.cpp:756-782`

review-code, lane 10-mcp-roadmap-batch, dim 5. **Disposition:** queued, ANTS-6640.

the parts-guard loop restarts its full scan after every removal.

Remedy: drop all refused items in one pass, then repeat.

### Row 123 — Medium — `remotecontrol_workspace.cpp:3323-3324`

review-code, lane 11-mcp-state-workspace, dim 4. **Disposition:** queued, ANTS-6642.

`read_log`, `read_region` and `file_outline` refuse anything that is not a regular file (ANTS-5110: opening a FIFO blocks the worker until a writer appears). Four open sites lack that guard:

Quote: `QFile in(resolved); if (!in.open(QIODevice::ReadOnly))`

Remedy: add the same `QFileInfo(...).isFile()` refusal before each `open`.

### Row 124 — Medium — `remotecontrol_workspace.cpp:2764`

review-code, lane 11-mcp-state-workspace, dim 15. **Disposition:** queued, ANTS-6642.

`read_log` applies `opts.redactSecrets = Config().claudeMcpRedactSecrets();`, but `read_region` (`return QJsonDocument(ReadRegion::extract(check.resolved, opts));`) returns the same file with nothing redacted. So do `read_regions` and `workspace_search`. Pointing `read_region` at the debug log, with `caller_cwd` set to the log's folder, gets around `read_log`'s redaction, and the unredacted text …

Quote: `:2835`

Remedy: apply `SecretRedact::scrub` under the same key on the content-read verbs, or at least on `read_region` / `read_regions`.

### Row 125 — Medium — `remotecontrol_workspace.cpp:2187`

review-code, lane 11-mcp-state-workspace, dim 3. **Disposition:** queued, ANTS-6642.

the comment at `:2005-2008` presents argv-only as the narrowing that keeps this from being "an arbitrary-command surface reachable from a tool call". It doesn't: `test_command:["sh","-c","…"]` (or `build_command`) runs any program. There is no allowlist and no confirmation, so any MCP client, including a prompt-injected one (OWASP LLM excessive agency), can run commands.

Quote: `HostExec::start(p, command.first(), command.mid(1));`

Remedy: either limit `argv[0]` to the known test runners and build tools, or correct the comment and document the verb as command execution. See Open questions.

### Row 126 — Low — `remotecontrol_state.cpp:691`

review-code, lane 11-mcp-state-workspace, dim 2. **Disposition:** queued, ANTS-6642.

with `top_n:0`, `topFindings` is empty, so `distinct == 0` falls to `"broad"`. A single-file rerun is then tagged broad and `narrow_run_warning` is suppressed.

Quote: `:545-551`

Remedy: classify from a fixed, separate parse budget, not from the caller's `topN`.

### Row 127 — Low — `remotecontrol_state.cpp:864`

review-code, lane 11-mcp-state-workspace, dim 16. **Disposition:** queued, ANTS-6642.

if the clock goes backwards or the file's mtime is in the future, `ageMs` is negative. That passes the 5-minute freshness check, so `fresh:true` can return for a snapshot of any age.

Quote: `const qint64 ageMs = nowMs - mtimeMs;`

Remedy: treat `ageMs < 0` as `stale_mtime`.

### Row 132 — Medium — `remotecontrol_terminal.cpp:522-524`

review-code, lane 12-mcp-content-verbs-a, dim 2. **Disposition:** queued, ANTS-6637.

`QJsonValue::toInt()` returns its default, 0, for a fractional or out-of-`int` double. So `{"tab":1.5}` or `{"tab":1e10}` sends keystrokes to tab 0's shell instead of refusing.

Quote: `if (tabVal.isDouble()) { const int idx = tabVal.toInt(); target = m_main->terminalAtTab(idx);`

Remedy: refuse unless `d == std::floor(d)` and `d` is within `int` range.

### Row 133 — Medium — `remotecontrol_run_trace.cpp:97`

review-code, lane 12-mcp-content-verbs-a, dim 4. **Disposition:** queued under ANTS-5953/ANTS-6271.

`SessionCost::projectsDirs()` (`sessioncostverb.cpp:89`) returns `$CLAUDE_CONFIG_DIR/projects` *first* when that variable is set, and `~/.claude/projects` last.

Quote: `RunTraceVerb::transcriptForSession(dirs.constLast(), cwd, sessionId)`

Remedy: try each folder in order and take the first that holds the file.

### Row 134 — Low — `remotecontrol_changelog.cpp:633-636`

review-code, lane 12-mcp-content-verbs-a, dim 7. **Disposition:** queued, ANTS-6637.

In `mode:"lint"`, a file that exists but cannot be read lints as empty and returns `ok:true, findings:[]`. The comment at `:629-631` says a push gate keys on that reply, so the gate would pass.

Quote: `if (f.open(QIODevice::ReadOnly | QIODevice::Text)) md = QString::fromUtf8(f.readAll());`

Remedy: refuse `changelog_read_failed` when the open fails.

Also reported as: "readability lane 30-mcp-content-verbs-b Info correctness lead src/remotecontrol_changelog.cpp:632-636"

### Row 135 — Low — `remotecontrol_docs.cpp:1344`

review-code, lane 12-mcp-content-verbs-a, dim 2. **Disposition:** queued, ANTS-6637.

Spec ANTS-4108 §2.5 says a `max_cases` outside [1, 1000] refuses the call. But `1e10`, `2.5` or `"500"` silently become the default of 200, against the comment just above.

Quote: `opt.maxCases = req.value(QStringLiteral("max_cases")).toInt(opt.maxCases);`

Remedy: refuse with `bad_args` when the value is present but is not a whole number within `int` range.

### Row 137 — Low — `remotecontrol_feedback.cpp:1794`

review-code, lane 12-mcp-content-verbs-a, dim 4. **Disposition:** queued, ANTS-6637.

The `git_state log` body is cut with no UTF-8 back-off, so a body can end in U+FFFD (the replacement character).

Quote: `QString::fromUtf8(b.left(kGitLogBodyCapBytes - 1))`

Remedy: use the `feedback_query` back-off in both places.

### Row 138 — Low — `feedback.cpp:827`

review-code, lane 12-mcp-content-verbs-a, dim 11. **Disposition:** queued, ANTS-6637.

Several batch arrays have no per-call cap, unlike `changelog_log add_batch`'s 200 (ANTS-5098):

Remedy: add the same cap.

### Row 141 — Medium — `quotationcheckverb.cpp:152-157`

review-code, lane 13-mcp-content-verbs-b, dim 3. **Disposition:** queued, ANTS-6637.

the recursion goes one level deeper for each `*` in a caller-supplied `allowed` glob. The 64M-state guard at `:204-206` limits work, not depth.

Quote: `if (c == '*') { ... for (qsizetype k = pi; k <= p.size(); ++k) { if (globMatchAt(g, rest, p, k, failed))`

Remedy: collapse consecutive `*`, cap the glob's length, or make the matcher iterative.

### Row 142 — Medium — `ciworkflows.cpp:161-173`

review-code, lane 13-mcp-content-verbs-b, dim 3. **Disposition:** queued, ANTS-6637.

the recursion depth equals the bracket nesting depth, and each level rescans its text, so the work is O(n²). The input is a workflow file from the working tree, or from any commit in the clone via `readAtCommits`. `read()` (`:452`) has no size cap.

Quote: `for (const QString &item : flowItems(rest.mid(1, rest.size() - 2))) l.append(value(item));`

Remedy: cap the nesting depth (mark the file unread past it) and cap the size of the file read.

### Row 143 — Medium — `cireport.cpp:563`

review-code, lane 13-mcp-content-verbs-b, dim 3. **Disposition:** queued, ANTS-6637.

the annotation text is quoted in full and unescaped into a finding the LLM reads. Anyone whose workflow run emits `::warning::` can write it — for example a pull request's own run, since `gh run list` returns runs for every event. The keyword filter is passed just by including "deprecated".

Quote: `f.message = QStringLiteral("GitHub warns: \"%1\"").arg(text);`

Remedy: cap the quoted length (for example 300 characters) and mark it as quoted data.

### Row 146 — Low — `runtraceverb.cpp:423`

review-code, lane 13-mcp-content-verbs-b, dim 9. **Disposition:** queued, ANTS-6637.

a tab in the subject is accepted. The cost row at `:354-358` then joins it with tabs, which gives `run-costs.tsv` a 13th column. `costKind` already rejects tabs (`:319`). Fix: reject `\t` in `start`.

Quote: `if (subject.trimmed().isEmpty() || hasNewline(subject))`

Remedy: reject `\t` in `start`.

### Row 147 — Low — `quotationcheckverb.cpp:354-364`

review-code, lane 13-mcp-content-verbs-b, dim 5. **Disposition:** queued, ANTS-6637.

walks every occurrence of the needle. A one-character `text` with a distant `line` over a 16 MiB subject is millions of iterations per item, times 500 items. `describeMiss` (`:325-331`) runs a full-subject `indexOf` per bisection step. Both are bounded but heavy, and no per-call time budget is visible in this seam.

Quote: `nearestOccurrence`

### Row 148 — Low — `sessioncostverb.cpp:111,184`

review-code, lane 13-mcp-content-verbs-b, dim 5. **Disposition:** queued, ANTS-6637.

keeps every user block as an `Item` and fully `stable_sort`s them just to return the top N. Transcript walks are on the performance contract. Fix: a size-N heap.

Quote: `Item`

Remedy: a size-N heap.

### Row 149 — Info — `cihistory.cpp:239`

review-code, lane 13-mcp-content-verbs-b. **Disposition:** queued, ANTS-6637.

INFO — `cihistory.cpp:239`: an attempt that returns `Stopped` is dropped without clearing `jobsComplete`. Whether the handler still saves a truncated fetch is outside my lane; see Open questions.

Quote: `Stopped`

### Row 151 — Medium — `knowhowstore.cpp:460-463`

review-code, lane 14-roadmap-store, dim 9. **Disposition:** queued, ANTS-6649.

. The spec (ANTS-6310 § 4.1, INV-1) says only a write "that commits no row" leaves an absent file absent. Here the call that *created* the empty file deletes the file and both WAL sidecars whenever *its own* reply fails.

Quote: `if (created && !reply.value(QStringLiteral("ok")).toBool()) { for (const QString &f : storeFiles(m_path)) QFile::remove(f); }`

Remedy: delete only if, under the write lock, `SELECT count(*) FROM know_how` is 0. Or never delete once another connection may have opened the file.

### Row 152 — Medium — `roadmapmigrateload.cpp:1199-1201`

review-code, lane 14-roadmap-store, dim 2. **Disposition:** queued, ANTS-6649.

. `writeLinks()` (and with it `syncConvertedLinks`) is skipped for every item whose id this load *allocates*, and for every id-less bullet matched by the weak key. `markUnresolvedLinks()` (lines 1300-1313) records only the link ids that do *not* resolve. So a resolvable `Blocked-by:`/`Supersedes:` line on such a bullet goes into neither a relationship row nor `extras.unresolved_links`.

Quote: `for (const PlannedItem &it : plan.items) { if (it.id.isEmpty()) continue;`

Remedy: key `src` on `matchPk[i]` or the pk `putItem` returned, not on `findItem(it.id)`.

### Row 153 — Medium — `roadmapstore.cpp:432-445`

review-code, lane 14-roadmap-store, dim 3. **Disposition:** queued, ANTS-6649.

SQLite creates the `.partial` file with umask permissions (typically 0644), and it holds every project's roadmap, `internal` items included. It stays that way until the chmod.

Quote: `q.prepare(QStringLiteral("VACUUM INTO ?")); … if (!QFile::setPermissions(tmp, …ReadOwner | …WriteOwner))`

Remedy: create `tmp` empty with `NewOnly` and `setOwnerOnlyPerms` before `VACUUM INTO`. SQLite accepts an existing empty target, and line 415 already treats an empty `.partial` as "ours".

### Row 154 — Low — `roadmapstore.cpp:2780-2802`

review-code, lane 14-roadmap-store, dim 2. **Disposition:** queued, ANTS-6649.

runs `"DELETE FROM history WHERE item_pk = ?"` without `m_historyTotal.reset()`. The header (`roadmapstore.h:1344-1346`) promises the cached total is "dropped by … any delete that bypasses appendHistory()", and `deregisterProject()` does it (line 2210). After `rebuildElements()` deletes absent items inside the load's transaction, the cached total is too high, so `appendHistory()` can refuse below…

Quote: `deleteItem()`

Remedy: reset the cache in `deleteItem()`.

### Row 155 — Low — `roadmapstore.cpp:2038-2079`

review-code, lane 14-roadmap-store, dim 8. **Disposition:** queued, ANTS-6649.

, `deleteElement`, `fileItem`, `markIdParsed` and `deleteSection` keep a stepped-but-unfinished `SELECT` (`sel`/`cur`/`q`) alive while they issue the write. The ANTS-5499 note (lines 146-156) says such methods must take `BEGIN IMMEDIATE` before reading, or the read-to-write upgrade fails "database is locked" with no busy wait. Every caller I found runs inside a mutation transaction (open question…

Quote: `2081-2116`

Remedy: `ImmediateTx` in each, or `finish()` the select before writing.

### Row 156 — Low — `roadmapmigrateload.cpp:1093-1096`

review-code, lane 14-roadmap-store, dim 9. **Disposition:** queued, ANTS-6649.

then `1219-1233` — when an item absent from source is deleted, a plan item linking to it still resolves through the own-project prefix fallback (`resolveLinkTarget`, `roadmapstore.cpp:1355-1383`). `relateCrossProject` then stores a row whose `dst_project` is this project's own slug, pointing at an id that no longer exists. `markUnresolvedLinks` ran before the delete, so the id is not in `extras` …

Quote: `1219-1233`

Remedy: run `markUnresolvedLinks` against the post-delete set, or refuse a self-slug cross row.

### Row 157 — Low — `roadmapmigrateload.cpp:912-914`

review-code, lane 14-roadmap-store, dim 2. **Disposition:** queued, ANTS-6649.

is not checked against the id-prefix grammar (`RoadmapParse::isIdPrefix`). A leaf such as `3D_Engine`, or one containing a space or `.`, produces ids like `3D_E-S0001` that `idHighWaterMark` skips (line 3171). See the open question on whether roadmap_log's fallback validates.

Quote: `prefix = leaf.left(4).toUpper();`

### Row 160 — Medium — `findsources.cpp:338`

review-code, lane 15-code-index-search-a. **Disposition:** queued, ANTS-6628.

`unmatchedTerms` can wrongly list a word that was found. The guard tests whether *any* word matched the file name, not whether *this* word did.

Quote: `if (filenameTokenMatches == 0) ++tokenHitCount[i];`

Remedy: drop the guard, or track which words matched the name and skip only those.

Also reported as: "readability lane 35-code-index-search-a Medium Q5 src/findsources.cpp:338; readability lane 35-code-index-search-a Info correctness lead src/findsources.cpp:338"

### Row 161 — Medium — `duplicateblocks.cpp:181`

review-code, lane 15-code-index-search-a, dim 2. **Disposition:** queued, ANTS-6628.

some duplicated code is never reported.

Quote: `if (pa > 0 && pb > 0 && id(fa, pa - 1) == id(fb, pb - 1)) return false;`

Remedy: reject only when the previous seed is itself in a bucket that `run()` will extend, or start the extension from the first seed that is kept.

### Row 162 — Medium — `fileoutline.cpp:1389`

review-code, lane 15-code-index-search-a, dim 2. **Disposition:** queued, ANTS-6628.

one long line can hide every later function in a file.

Quote: `if (withoutEol.size() > kMaxLineBytes) continue;`

Remedy: still run `netBraceDelta` (or `cpp.trackScope`) on long lines in Cpp/Glsl mode, and skip only the regexes.

### Row 164 — Low — `applyedits.cpp:249`

review-code, lane 15-code-index-search-a, dim 9. **Disposition:** queued, ANTS-6628.

a file ending in `\n` gets a phantom empty last element from `split`. So `end_line = N+1` with `expect_*_line:""` is accepted instead of refused `range_out_of_bounds`. Replacing or deleting that line removes the file's final newline (`"a\nb\n"` with `[3,3]` deleted gives `"a\nb"`). This breaks ANTS-2022 INV-12 ("trailing-newline state is unchanged") and the spec's "end_line past the last line → r…

Quote: `endLine > lines.size()`

Remedy: when the text ends in `\n`, measure bounds against `lines.size()-1`.

### Row 165 — Low — `fileoutline.cpp:1322`

review-code, lane 15-code-index-search-a, dim 2. **Disposition:** queued, ANTS-6628.

Text mode turns `\r\n` into `\n` when reading. If so, `total_bytes` and the per-symbol `bytes` undercount CRLF files by one byte per line, against the ANTS-5103 note that "the byte count stays exact". This rests on my reading of Qt's Text-mode behaviour, not on a run. Fix: open without `Text`; the loop at `:1379` already strips `\r`.

Quote: `QIODevice::ReadOnly | QIODevice::Text`

Remedy: open without `Text`; the loop at `:1379` already strips `\r`.

### Row 166 — Low — `functionsizes.cpp:94`

review-code, lane 15-code-index-search-a, dim 2. **Disposition:** queued, ANTS-6628.

FileOutline's C++ signature is the raw line, trailing comment included (`fileoutline.cpp:958`, ANTS-3738). So `void f(); // note` is not recognised as a declaration. It is reported as `unmeasured: "no body the scan could find"` instead of being skipped as the header promises. Fix: strip a trailing `//` comment before the test.

Quote: `signature … endsWith(';')`

Remedy: strip a trailing `//` comment before the test.

Also reported as: "readability lane 35-code-index-search-a Info correctness lead src/readregion.cpp:256"

### Row 167 — Low — `commentclaims.cpp:318`

review-code, lane 15-code-index-search-a, dim 2. **Disposition:** queued, ANTS-6628.

this contradicts the file's own header (`:6-7`: "a capped walk is skipped or counted, never reported as dead"). Nothing flags the cap. Fix: when the walk is capped, count unresolved references as unchecked.

Quote: `kMaxPaths = 200000; // … beyond it, a reference can read dead`

Remedy: when the walk is capped, count unresolved references as unchecked.

### Row 168 — Low — `sourcescrapes.cpp:1030-1034`

review-code, lane 15-code-index-search-a, dim 2. **Disposition:** queued, ANTS-6628.

cuts glob results with no flag, so a capped list looks complete. Also `sourcescrapes.cpp:123,126` — `if (open < 0) break;` stops lexing the rest of the file silently. Fix: report both in `scanned`/`not_covered`.

Quote: `kMaxListed = 2000`

Remedy: report both in `scanned`/`not_covered`.

### Row 169 — Low — `codewalk.cpp:140`

review-code, lane 15-code-index-search-a, dim 2. **Disposition:** queued, ANTS-6628.

overlapping `paths` (for example `src` and `src/x`) walk the same files twice. That inflates `skipped_tests`/`skipped_generated` and uses up `kMaxFiles` early. Fix: deduplicate `walked` before filtering.

Quote: `walk(root, fi.canonicalFilePath(), walked);`

Remedy: deduplicate `walked` before filtering.

### Row 170 — Low — `fileoutline.cpp:778-780`

review-code, lane 15-code-index-search-a, dim 2. **Disposition:** queued, ANTS-6628.

the 2 KiB header_doc cap is measured in UTF-8 bytes but enforced with `doc.truncate(kHeaderDocByteCap)` in UTF-16 units. Non-ASCII text can go past the cap, and a surrogate pair can be split.

Quote: `doc.truncate(kHeaderDocByteCap)`

### Row 172 — Medium — `readregion.cpp:213`

review-code, lane 16-code-index-search-b, dim 4. **Disposition:** queued, ANTS-6628.

`braceBalancedEndLine` treats every `'` as the start of a character literal. Two common cases fool it:

Quote: `if (c == QLatin1Char('\'')) { inChar = true; continue; }`

Remedy: apply `CppScope`'s rule, so a `'` preceded by a number token is not a literal. Also treat `'ident` with no closing `'` on the line as a lifetime, not a literal.

### Row 175 — Medium — `extract.cpp:178`

review-code, lane 16-code-index-search-b, dim 5. **Disposition:** queued, ANTS-6628.

only the parse has the 2 s deadline (`:142-158`). Query matching has no match limit and no time limit.

Quote: `ts_query_cursor_exec(qc, query, root);`

Remedy: set `ts_query_cursor_set_match_limit` and a progress/time limit via exec options. Index class ranges once (sorted intervals) instead of the nested scans.

### Row 178 — Low — `regexharden.cpp:26`

review-code, lane 16-code-index-search-b, dim 3. **Disposition:** queued, ANTS-6628.

A caller-supplied `(*LIMIT_HEAP=…)` or `(*LIMIT_DEPTH=…)` prefix skips the `LIMIT_MATCH=100000` backstop. PCRE2's default limit of 10,000,000 then applies, 100× the intended budget.

Quote: `if (pattern.startsWith(QStringLiteral("(*LIMIT_"))) return pattern;`

Remedy: pass through only `(*LIMIT_MATCH=` and always prepend ours, since a pattern can only lower a limit.

### Row 179 — Low — `trace.cpp:319`

review-code, lane 16-code-index-search-b, dim 3. **Disposition:** queued, ANTS-6628.

. Converting an out-of-range double to `int` (e.g. `depth:1e20`) is undefined behaviour. Fix: test the range before casting. [tool: UBSan / clang-tidy bugprone-narrowing]

Quote: `if (!d.isDouble() || dv != int(dv) || dv < 1 || dv > 3)`

Remedy: test the range before casting. [tool: UBSan / clang-tidy bugprone-narrowing]

### Row 180 — Low — `trace.cpp:324-328`

review-code, lane 16-code-index-search-b, dim 2. **Disposition:** queued, ANTS-6628.

is checked only for `> 0` and has no ceiling. Every other read verb clamps to 4 MiB, and spec § 2.1 calls it "the standard response cap". Fix: clamp to the 4 MiB family ceiling. Which side is wrong is in Open questions.

Quote: `max_bytes`

Remedy: clamp to the 4 MiB family ceiling. Which side is wrong is in Open questions.

### Row 181 — Low — `symbolquery.cpp:963`

review-code, lane 16-code-index-search-b, dim 2. **Disposition:** queued, ANTS-6628.

Two needles that differ only in case (`Foo`, `foo`) overwrite each other, so only the last one gets its `fileStemHint`.

Quote: `byStem.insert(needles.at(i).toLower(), i);`

Remedy: map a lowercased stem to a list of slots.

### Row 184 — Medium — `src/secretredact.h:179`

review-code, lane 17-mcp-infra, dim 15. **Disposition:** queued, ANTS-6638.

the general secret rule needs `:` or `=` right after the keyword, so the JSON form `"password": "hunter22xyz"` never matches. The `"` between the keyword and the colon stops it. Other misses:

Quote: `"(?i)(?:api[_-]?key|token|password|passwd|secret)\\s*[:=]\\s*['\"]?([A-Za-z0-9+/=_\\-\\.]{8,})['\"]?"`

Remedy: allow an optional closing quote after the keyword (`['"]?\s*[:=]`), and add rules for `Basic <b64>` and URL credentials inside `scrub()`.

### Row 185 — Medium — `src/mcpspill.cpp:73-92`

review-code, lane 17-mcp-infra, dim 8. **Disposition:** queued, ANTS-6638.

(called at `:546`) — `if (QFile::remove(fi.absoluteFilePath())) { total -= sz; --count; }` eviction is not synchronised, but more than one process writes the spill folder. Each `ants-mcpd` turns offload on (`mcpdmain.cpp:189`), the terminal does too, and both use one `~/.cache/ants-terminal/mcp-spill/`.

Quote: `:546`

Remedy: on a failed `remove`, check whether the file still exists and reduce the counts if it is gone; or hold an `flock` on the folder during write plus eviction.

### Row 186 — Medium — `src/mcpprojection.cpp:229-257`

review-code, lane 17-mcp-infra, dim 2. **Disposition:** queued, ANTS-6638.

the header contract (`mcpprojection.h:75-78`) says *"field absent / non-string / empty → omitted, and NAMED in `fields_unmatched`"*. The code silently skips non-string and empty names.

Quote: `if (!f.isString()) continue; … if (!name.isEmpty() && !src.contains(name)) unmatched.append(name);`

Remedy: name every non-matching entry, written as JSON text if it is not a string.

### Row 188 — Low — `src/mcpundeclaredargs.cpp:40-43`

review-code, lane 17-mcp-infra, dim 5. **Disposition:** queued, ANTS-6638.

On the GUI thread (`claudeintegration.cpp:1271-1274`), every call that carries an undeclared argument does: a lock wait of up to `kGuiTimeoutMs`, a full read of a file up to 256 KiB, and an atomic rewrite.

Remedy: buffer records and flush them on a timer, or off the GUI thread.

### Row 189 — Low — `src/tokenusageengine.cpp:427`

review-code, lane 17-mcp-infra, dim 3. **Disposition:** queued under ANTS-6008.

The value comes from an untrusted snapshot (spec § 2.6). A value above `INT_MAX` passes the `v < 0` check and then wraps to a negative call count. That defeats § 2.6's "negative counter → skipped" rule.

Quote: `if (i == 0) tc.nCalls = int(v);`

Remedy: reject `v > INT_MAX` for `n_calls`, and set a sanity ceiling on the byte counters.

### Row 190 — Low — `src/tokenusageengine.cpp:520-526`

review-code, lane 17-mcp-infra, dim 16. **Disposition:** queued, ANTS-6638.

If the terminal dies between the claim-rename and `releaseClaimed`, `<stem>.claimed-<pid>` is left behind.

Quote: `if (!QFile::rename(path, claimed)) continue;`

Remedy: on a read, adopt `.claimed-<pid>` files whose pid is dead.

### Row 192 — Low — `src/secretredact.h:80`

review-code, lane 17-mcp-infra, dim 15. **Disposition:** queued, ANTS-6638.

`-----BEGIN PGP PRIVATE KEY BLOCK-----` does not match, because ` BLOCK` comes before the dashes.

Quote: the PEM header regex in `secretredact.h:80` (BEGIN, then `[^\\-\\n]*`, then PRIVATE KEY, each between five dashes). Paraphrased so the secret scanner does not read it as a key.

Remedy: allow `PRIVATE KEY( BLOCK)?-----`.

### Row 197 — Medium — `pluginmanager.cpp:482-504`

review-code, lane 18-plugins-lua, dim 16. **Disposition:** queued, ANTS-6645.

only symlinks are rejected. A `manifest.json` that is a FIFO (a named pipe, which a tarball can carry) blocks `QFile::open` on the main thread inside `scanAndLoad`. That happens at every startup, before the user can reach Settings to disable the plugin.

Quote: `if (QFileInfo(initLua).isSymLink()) {`

Remedy: require `QFileInfo::isFile()` with no symlink for both files before opening them.

### Row 198 — Medium — `pluginmanager.cpp:645`

review-code, lane 18-plugins-lua, dim 11. **Disposition:** queued, ANTS-6645.

the "comfortable headroom" claim rests on `LUAI_MAXCCALLS = 200`. But each nesting level of a `string.gsub` with a function replacement (gsub → callback → gsub …), or of a Lua `pcall`, holds a 1 KiB `luaL_Buffer` and a ~0.5 KiB `MatchState`, plus the interpreter, call and `jmp_buf` stack frames. At roughly 2–3 KiB per level, 200 levels approaches 512 KiB. The hardening flags make frames bigger.

Quote: `thread->setStackSize(512 * 1024);`

Remedy: measure deep recursion under the release flags and size the stack from that, or raise it to 1–2 MiB.

### Row 200 — Low — `luaengine.cpp:529-531`

review-code, lane 18-plugins-lua, dim 2. **Disposition:** queued, ANTS-6645.

. If `init.lua` catches the budget kill with an inner `pcall` and then returns normally, `m_timedOut` stays `true`. `fireEvent:637` then stops the first event after its first handler and logs a false "timed out". Fix: clear `m_timedOut` in `startPcallBudget`.

Quote: `init.lua`

Remedy: clear `m_timedOut` in `startPcallBudget`.

### Row 206 — Medium — `secureio.h:274-276`

review-code, lane 19-config-session-project, dim 16. **Disposition:** queued, ANTS-6629.

this runs on every *ancestor* of the path, not only on the leaf.

Quote: `if (!S_ISDIR(st.st_mode)) return false; // file/symlink blocks us`

Remedy: allow ancestors that already exist via `stat`. Keep `lstat` for components it creates and for the leaf.

### Row 211 — Low — `projectlayoutengine.cpp:706`

review-code, lane 19-config-session-project, dim 2. **Disposition:** queued, ANTS-6629.

. Deleting a probed file (for example `ROADMAP.md`) is never stale, so the cache reports `roadmap_found:true` for up to 7 days. The spec's code has the same gap. fix: treat "existed at scan, missing now" as stale.

Quote: `if (fi.exists() && …)`

Remedy: treat "existed at scan, missing now" as stale.

### Row 213 — Low — `projectsettings.cpp:139-145`

review-code, lane 19-config-session-project, dim 3. **Disposition:** queued, ANTS-6629.

`claudeChromeFor` walks every ancestor, all the way to `/`, and trusts the first `.ants/project.json` it finds.

Remedy: skip any file not owned by `getuid()`.

### Row 214 — Low — `sessionmanager.cpp:383`

review-code, lane 19-config-session-project. **Disposition:** queued, ANTS-6629.

. The combining-mark column (`col`) read from disk is never checked against `cellCount` or `cols`; a negative or out-of-range value is stored. Whether a consumer indexes `cells[col]` with it is in another lane; see Open questions.

Quote: `combining[col] = std::move(cps);`

### Row 217 — Medium — `gitwrap.cpp:99-109`

review-code, lane 20-shared-utilities, dim 2. **Disposition:** queued, ANTS-6650.

Wrong file paths for common filenames.

Quote: `QString p = line.mid(4); if (p.startsWith(QLatin1String("a/"))) p = p.mid(2);`

Remedy: decode C-quoting and strip a trailing `\t` in the parser, and have callers pass `-c core.quotePath=false --no-ext-diff --src-prefix=a/ --dst-prefix=b/`.

Also reported as: "readability lane 44-shared-utilities Info correctness lead src/gitwrap.cpp:99-108"

### Row 219 — Medium — `pathvalidation.cpp:225,234-239`

review-code, lane 20-shared-utilities, dim 2. **Disposition:** queued, ANTS-6650.

Files with decomposed (NFD) Unicode names cannot be reached.

Quote: `const QString nfc = rawPath.normalized(QString::NormalizationForm_C); … QFileInfo(joined).canonicalFilePath()`

Remedy: try the raw form first and fall back to NFC.

### Row 220 — Medium — `treewatcher.cpp:116`

review-code, lane 20-shared-utilities, dim 2. **Disposition:** queued, ANTS-6650.

New directories under some folders are never noticed.

Quote: `dirs.insert(QFileInfo(abs).path()); // the file's directory`

Remedy: insert every ancestor directory up to `topLevel`.

### Row 221 — Low — `treewatcher.cpp:75-80`

review-code, lane 20-shared-utilities, dim 2. **Disposition:** queued, ANTS-6650.

Renaming a watched folder leaves stale and leaked watches.

Quote: `if (ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) { const QString gone = m_wdToPath.take(ev->wd);`

Remedy: call `inotify_rm_watch` on IN_MOVE_SELF, and on re-insert remove any older path that maps to the same descriptor.

### Row 222 — Low — `treewatcher.h:91`

review-code, lane 20-shared-utilities, dim 11. **Disposition:** queued, ANTS-6650.

A fixed per-instance cap.

Quote: `int m_maxWatches = 20000;`

Remedy: derive the cap from `/proc/sys/fs/inotify/max_user_watches`.

### Row 223 — Low — `clipboardguard.h:43-44`

review-code, lane 20-shared-utilities, dim 2. **Disposition:** queued, ANTS-6650.

Truncation can split a character in two.

Quote: `text.truncate(kUntrustedMaxChars);`

Remedy: step back one unit when the last one is a high surrogate.

### Row 224 — Low — `gitwrap.cpp:22`

review-code, lane 20-shared-utilities, dim 2. **Disposition:** queued, ANTS-6650.

The "git missing" error is wrong in two cases.

Quote: `if (!p.waitForStarted(500)) { r.started = false;`

Remedy: map exit code 127 under Flatpak to `git_missing`, and widen the start timeout.

### Row 226 — Info — `remotecontrol_docs.cpp:862-868`

review-code, lane 20-shared-utilities. **Disposition:** queued, ANTS-6650.

Outside this lane:

Quote: `--- a/`

### Row 230 — Medium — `verifytrustmodal.cpp:176`

review-code, lane 21-dialog-chrome-theme, dim 2. **Disposition:** queued, ANTS-6633.

this is an application-modal `QMessageBox` whose parent is the frameless, translucent MainWindow (`mainwindow.cpp:1187`). That is the setup that `aboutdialogs.cpp:26-33` and `dialogfocus.h:44-49` say drops button clicks on Wayland (QTBUG-79126). Either the trust prompt's buttons are affected, or those comments overstate the bug. I cannot tell which side is wrong without a compositor (see Open que…

Quote: `box.exec();`

Remedy: verify on KWin Wayland. If the clicks are dropped, use the non-modal show-plus-local-event-loop pattern.

### Row 232 — Low — `updatedialog.cpp:80-82`

review-code, lane 21-dialog-chrome-theme, dim 3. **Disposition:** queued, ANTS-6633.

with `Qt::RichText` — the version comes from the GitHub release tag and is not escaped. `showRestartPage` (line 177) does escape it, so the two copies have drifted apart. Line 94-97 puts `dir` into an auto-format label the same way. — fix: `toHtmlEscaped()`.

Quote: `tr("<b>Version %1 is available</b> …").arg(version, …)`

Remedy: `toHtmlEscaped()`.

### Row 233 — Low — `updatedialog.cpp:225-226`

review-code, lane 21-dialog-chrome-theme, dim 7. **Disposition:** queued under ANTS-5843.

when the URL is the reason, `error()` is empty, so the message gives no reason. — fix: a separate message for a refused URL.

Quote: `if (!SelfUpdate::isAllowedUrl(url) || !m_session->begin()) fail(tr("The download could not start: %1").arg(m_session->error()));`

Remedy: a separate message for a refused URL.

Also reported as: "readability lane 08-dialog-chrome-theme Info correctness lead src/updatedialog.cpp:225-226"

### Row 234 — Low — `updatedialog.cpp:199,230`

review-code, lane 21-dialog-chrome-theme, dim 16. **Disposition:** queued, ANTS-6633.

no transfer timeout is set. If the connection stalls, the progress bar spins forever, and Cancel is the only way out. — fix: `req.setTransferTimeout(…)` in `makeRequest`.

Quote: `m_nam->get(SelfUpdate::makeRequest(url))`

Remedy: `req.setTransferTimeout(…)` in `makeRequest`.

### Row 235 — Low — `dialogchrome.cpp:63-68`

review-code, lane 21-dialog-chrome-theme, dim 16. **Disposition:** queued, ANTS-6633.

closing a dialog while it is maximised saves the maximised size, so it reopens screen-sized (against D3's "the size the user picks"). A spontaneous Show on un-minimise also re-centres a dialog the user had moved (D4 says "every open", not every restore). — fix: save `normalGeometry().size()` when maximised or full-screen, and skip `recenter()` for `ev->spontaneous()` Shows after the first.

Quote: `case QEvent::Show: onShow(); … case QEvent::Hide: saveSize();`

Remedy: save `normalGeometry().size()` when maximised or full-screen, and skip `recenter()` for `ev->spontaneous()` Shows after the first.

### Row 236 — Low — `dialogshowtracer.cpp:42-49`

review-code, lane 21-dialog-chrome-theme, dim 3. **Disposition:** queued, ANTS-6633.

the window title (MainWindow's title follows the shell's OSC title) is substituted first. A title containing `%1`–`%9` is then filled in by later `.arg` calls, and newlines in it fake extra log lines. This only happens with the tracer on. — fix: one multi-arg `.arg` with every value pre-converted to `QString`, and escape control characters.

Quote: `.arg(cls, name, title).arg(geom.width())…`

Remedy: one multi-arg `.arg` with every value pre-converted to `QString`, and escape control characters.

### Row 237 — Low — `verifytrustmodal.cpp:71-76,128`

review-code, lane 21-dialog-chrome-theme, dim 3. **Disposition:** queued, ANTS-6633.

bidi override and other invisible characters (CVE-2021-42574 class) pass through. The first-command preview the user trusts can then look different from the bytes that will run. — fix: show non-printing and bidi code points visibly in the preview and the detailed text.

Quote: `preview.toHtmlEscaped()`

Remedy: show non-printing and bidi code points visibly in the preview and the detailed text.

### Row 240 — Medium — `claudesetup.cpp:181-187`

review-code, lane 22-app-entry-dialogs, dim 2. **Disposition:** queued, ANTS-6620.

a begin marker with no end marker after it makes `withBlock` append a second, complete block.

Quote: `:192-197`

Remedy: when exactly one marker is present, refuse (or repair the orphan) rather than append.

### Row 243 — Medium — `diffviewer.cpp:852`

review-code, lane 22-app-entry-dialogs, dim 2. **Disposition:** queued, ANTS-6620.

`git status --short` C-quotes paths that contain whitespace, and paths with non-ASCII bytes (with the default `core.quotePath`).

Quote: `:678-682`

Remedy: use `git status -z --porcelain` (or `-c core.quotePath=false` plus unquoting), and key anchors on paths from the repository root.

### Row 244 — Low — `claudesetup.cpp:238-239`

review-code, lane 22-app-entry-dialogs, dim 4. **Disposition:** queued, ANTS-6620.

checks only that the event key exists. `gitContextStatus` (`:346`) uses `eventRuns(…, scriptPath)`. A user whose own hooks sit on those events (SessionStart always has the orientation hook) reads "✓ Installed" after the Ants entry is gone. fix: use `eventRuns` for each event.

Quote: `if (!hooks.contains(event)) { hooksWired = false; break; }`

Remedy: use `eventRuns` for each event.

### Row 245 — Low — `settingsdialog.cpp:375-376`

review-code, lane 22-app-entry-dialogs, dim 2. **Disposition:** queued, ANTS-6620.

Trigger: config has `switch_enabled=true` and MCP disabled.

Quote: `:1110`

Remedy: have the slot call `mirrorMaster(m_claudeMcpEnabled->isChecked())`.

### Row 246 — Low — `settingsdialog.cpp:1332-1335`

review-code, lane 22-app-entry-dialogs, dim 7. **Disposition:** queued, ANTS-6620.

. A row the user typed disappears on Apply/OK with no message, disabled rows included. That contradicts the comment at `:1325-1326` ("Saved-but-disabled rows still get persisted"). Same at `:1354-1357`. fix: warn and keep the dialog open, or persist the row as disabled.

Quote: `if (!re.isValid() || AuditEngine::isCatastrophicRegex(pattern)) continue;`

Remedy: warn and keep the dialog open, or persist the row as disabled.

### Row 247 — Low — `claudesetup.cpp:686-688`

review-code, lane 22-app-entry-dialogs, dim 3. **Disposition:** queued, ANTS-6620.

. The path, built from `applicationDirPath()` or `$HOME`, goes into `~/.bashrc`/`~/.zshrc` inside double quotes without escaping `$`, backtick, `"` or `\`. It is evaluated in every new shell. fix: single-quote it, with `'` escaped.

Quote: `const QByteArray q = '"' + sourced.toUtf8() + '"';`

Remedy: single-quote it, with `'` escaped.

### Row 248 — Low — `antshelpermain.cpp:157-158`

review-code, lane 22-app-entry-dialogs, dim 2. **Disposition:** queued, ANTS-6620.

Missed position: the F7 "stdin not readable" check misses `-` in any other position (`drift-check --repo-root X -`).

Quote: `args[2] == QStringLiteral("-")`

Remedy: test the `jsonArg == "-"` that was already parsed.

Also reported as: "readability lane 42-app-entry-dialogs Info correctness lead src/antshelpermain.cpp:157-158"

### Row 249 — Low — `antshelpermain.cpp:119-123`

review-code, lane 22-app-entry-dialogs, dim 2. **Disposition:** queued, ANTS-6620.

with no arguments it prints help text to stdout and exits 2. Every other usage error emits the JSON envelope (INV-7, ANTS-1123 F1). fix: call `usageFail` for the no-argument case.

Quote: `usageFail`

Remedy: call `usageFail` for the no-argument case.

### Row 250 — Low — `main.cpp:163-189`

review-code, lane 22-app-entry-dialogs, dim 2. **Disposition:** queued, ANTS-6620.

. This scans every argv slot, including option values. For example, `--remote-text --import-roadmap a b` would run a roadmap import. fix: only honour these as `argv[1]`, or route them through `QCommandLineParser`.

Quote: `if (qstrcmp(argv[i], "--export-roadmaps") != 0) continue;`

Remedy: only honour these as `argv[1]`, or route them through `QCommandLineParser`.

### Row 251 — Low — `diffviewer.cpp:972-976`

review-code, lane 22-app-entry-dialogs, dim 5. **Disposition:** queued, ANTS-6620.

every `changed` signal calls `runProbes()`, which starts five git processes without cancelling the round still running. If a probe takes longer than the watcher's debounce, processes pile up. I did not read `treewatcher` to confirm the debounce interval. fix: skip or kill the previous round's processes.

Quote: `changed`

Remedy: skip or kill the previous round's processes.

### Row 253 — Low — `main.cpp:90-96`

review-code, lane 22-app-entry-dialogs, dim 7. **Disposition:** queued, ANTS-6620.

returns true without checking `QTextStream`/`QFile` status, so `--new-plugin` reports success after a short write. fix: check `out.status()` and `f.error()` after flushing.

Quote: `writeFile`

Remedy: check `out.status()` and `f.error()` after flushing.

### Row 255 — Medium — `aidialog.cpp:349`

review-code, lane 23-model-switching, dim 2. **Disposition:** queued, ANTS-6644.

Insert is enabled on a reply that was cut at the size cap.

Quote: `m_insertBtn->setEnabled(result.ok);`

Remedy: `setEnabled(result.ok && !result.truncated)`.

### Row 256 — Medium — `aidialog.cpp:104-106`

review-code, lane 23-model-switching, dim 3. **Disposition:** queued, ANTS-6644.

confirming runs the command; it does not just type it.

Quote: `"It will be typed into the active terminal if you confirm."`

Remedy: write without the trailing newline and with LF/CR stripped (or use bracketed paste), or change the dialog wording to "will be run".

### Row 257 — Medium — `aidialog.cpp:63`

review-code, lane 23-model-switching, dim 2. **Disposition:** queued, ANTS-6644.

pressing Enter while a reply is streaming silently drops that reply.

Quote: `connect(m_input, &QLineEdit::returnPressed, this, &AiDialog::onSend);`

Remedy: return early from `onSend` while `m_client->busy()`, or apply `resetTransient`'s cancel notice here.

### Row 259 — Low — `llmdispatcher.cpp:23-32`

review-code, lane 23-model-switching, dim 2. **Disposition:** queued, ANTS-6644.

INV-9 / ANTS-5009 say the owner aborts in-flight clients and that `allFinished` fires "once in-flight drains". But `LlmClient::abort()` suppresses `finished()`, so an aborted job never calls `done`, `m_inFlight` never goes down, and `allFinished` never fires. Later batches also lose that slot. It is latent: today's only caller of `cancelAll()` is a destructor. fix: give the runner contract an "ab…

Quote: `llmclient.cpp:60-68`

Remedy: give the runner contract an "aborted → call done with an error result" path.

### Row 260 — Low — `llmclient.cpp:348-353`

review-code, lane 23-model-switching, dim 2. **Disposition:** queued, ANTS-6644.

this fires on any backlog over 10 MiB, including complete lines not yet parsed because of the 256-lines-per-tick cap. The comment (340-345) describes only the single giant frame case. A fast server can get a valid reply marked truncated and aborted. fix: before clearing, check that the buffer holds no `'\n'`.

Quote: `if (m_sseLineBuffer.size() > kMaxBytes) { m_sseLineBuffer.clear();`

Remedy: before clearing, check that the buffer holds no `'\n'`.

### Row 261 — Low — `llmclient.cpp:135-143`

review-code, lane 23-model-switching, dim 3. **Disposition:** queued, ANTS-6644.

the embedded-IPv4 check covers NAT64 and `::/96` but not 6to4 `2002::/16`, where bytes 2-5 carry the IPv4 address — the same rationale the comment gives for NAT64. Hostnames are documented as out of scope (ANTS-2109 H2). fix: add `a6[0]==0x20 && a6[1]==0x02`, embedded address = `a6[2..5]`.

Quote: `::/96`

Remedy: add `a6[0]==0x20 && a6[1]==0x02`, embedded address = `a6[2..5]`.

### Row 262 — Low — `modelrecommender.cpp:39-41`

review-code, lane 23-model-switching, dim 2. **Disposition:** queued, ANTS-6644.

"planning" and "planned" (double n) never match. The old bare `contains()` matched them, so ANTS-2010 removed the commonest plan words from the Opus plan signal. fix: give "plan" its own alternation, `plan(?:s|ned|ning)?`, as `kCommitIntentAlternations` already does for commit.

Quote: `kw + "(?:s|ed|ing)?"`

Remedy: give "plan" its own alternation, `plan(?:s|ned|ning)?`, as `kCommitIntentAlternations` already does for commit.

### Row 268 — Medium — `hooks/ants-inbox-notice.sh:89`

review-code, lane 24-shell-surface, dim 2. **Disposition:** queued, ANTS-6651.

The reply is a `QJsonObject` (`remotecontrol_session_message.cpp:246-248`), and Qt serialises its keys in sorted order: `count`, then `messages`, then `unacked_count`. With `limit:1` the reply carries one message body before the real count.

Quote: `count="$(printf '%s' "$reply" | grep -o '"unacked_count":[0-9]*' | head -n 1 | cut -d: -f2)"`

Remedy: Use `jq -r '.unacked_count // empty'` when `jq` is present, which the script already checks at `:34`. Otherwise anchor the match to the end of the object.

### Row 272 — Medium — `packaging/shell-integration/ants-osc133.zsh:96-98`

review-code, lane 24-shell-surface, dim 4. **Disposition:** queued under ANTS-6107.

The bash copy emits B at the end of PS1 (`.bash:124-128`). The zsh copy emits B after Enter.

Quote: `__ants_osc133_preexec() { __ants_osc133_emit "B"; __ants_osc133_emit "C"; }`

Remedy: Emit B at the end of the prompt, for example by appending `%{$(__ants_osc133_emit B)%}` to PS1 with `prompt_subst`. Keep only C in preexec.

### Row 275 — Low — `_common.sh:108`

review-code, lane 24-shell-surface, dim 2. **Disposition:** queued, ANTS-6651.

. This misses clustered flags where `r` is not first (`grep -nr`, `grep -irn`). ANTS-2141 §2.2 says "carrying -r". Fix: Match `-[a-zA-Z]*[rR]` on an option word.

Quote: `*" -r"*|*" -R"*|*" --recursive"*`

Remedy: Match `-[a-zA-Z]*[rR]` on an option word.

### Row 276 — Low — `_common.sh:95,186`

review-code, lane 24-shell-surface, dim 2. **Disposition:** queued, ANTS-6651.

. These substring exemptions also hit project sources. `cat src/processgroup.h` and `grep -r x src/processgroup.h` contain `/proc`, so they are exempted and get no nudge. Blind spot (c) in the spec covers only search *patterns*. Fix: Anchor the match to a word start (`" /proc"`).

Quote: `*/proc*|*/sys*|*/var*…`

Remedy: Anchor the match to a word start (`" /proc"`).

### Row 277 — Low — `_common.sh:291-293`

review-code, lane 24-shell-surface, dim 2. **Disposition:** queued under ANTS-6093.

treats any `$ANTS_PROJECT_ROOT/hooks/*.sh` as "the pack". The comment says "Fires only where the pack is AUTHORED", but the check is only "a `hooks/` directory exists". Any other `.ants-project` repository that has its own hooks gets a false "stale vs repo — run tools/install-hooks.sh" line. Fix: Require `$src/_common.sh` and `$src/ants-bash-veto.sh`.

Quote: `ants_hook_drift_line`

Remedy: Require `$src/_common.sh` and `$src/ants-bash-veto.sh`.

### Row 278 — Low — `precompact:6,8`

review-code, lane 24-shell-surface, dim 2. **Disposition:** queued, ANTS-6651.

the header promises a `$CLAUDE_TRANSCRIPT_PATH` fallback and an "in-progress spec path" (ANTS-1252 §1.5). The code does neither. Either implement them or correct the comment and the spec.

Quote: `$CLAUDE_TRANSCRIPT_PATH`

### Row 279 — Low — `cc-job:180`

review-code, lane 24-shell-surface, dim 2. **Disposition:** queued, ANTS-6651.

. Cleaning job `a` also closes the watch window of job `a-b`. Fix: Use `cc-job-watch-$name-[0-9]*`.

Quote: `systemctl --user stop "cc-job-watch-$name-*"`

Remedy: Use `cc-job-watch-$name-[0-9]*`.

### Row 280 — Low — `cc-job:128-140`

review-code, lane 24-shell-surface, dim 3. **Disposition:** queued, ANTS-6651.

the `.env` file holds every exported variable, tokens included (mode 0600). If the unit fails before the inner bash sources it (for example, an exec failure after `systemd-run` returns), the file stays in `$STATE` until someone runs `clean`.

Quote: `.env`

### Row 281 — Low — `ants-terminal.bash:24-27`

review-code, lane 24-shell-surface, dim 2. **Disposition:** queued, ANTS-6651.

Completions:

Quote: `ants-terminal.fish:16`

### Row 283 — Low — `decision:"block"`

review-code, lane 24-shell-surface, dim 2. **Disposition:** queued, ANTS-6651.

is the deprecated PreToolUse output (`veto:151`, `read-roadmap-veto:53`). The current form is `hookSpecificOutput.permissionDecision:"deny"`. It still maps to deny as far as I know; I did not check this against current Claude Code docs.

Quote: `veto:151`

### Row 284 — Medium — `titlebar.cpp:101`

review-code, lane 25-chrome-widgets, dim 3. **Disposition:** queued, ANTS-6625.

, which reaches `elidedlabel.h:113` — `QLabel::setText(displayed);` The program running in the terminal controls the window title (OSC 0/2). The path is `terminalwidget.cpp:2475` → `MainWindow::onTitleChanged` (`mainwindow.cpp:1596`) → here.

Quote: `m_titleLabel->setFullText(title);`

Remedy: call `setTextFormat(Qt::PlainText)` in `ElidedLabel`'s constructor (or in `TitleBar`), and give the tooltip a `toHtmlEscaped()` copy of the text.

### Row 285 — Medium — `coloredtabbar.cpp:319`

review-code, lane 25-chrome-widgets, dim 5. **Disposition:** queued, ANTS-6625.

The comment at lines 290–291 says `setTabButton()` "deletes the displaced widget, so we never leak Qt's CloseButton". From my knowledge of Qt 6 (I could not open Qt source in this tree), `QTabBar::setTabButton` only *hides* the displaced widget, and its documentation says "Any previously set widget in position is hidden".

Quote: `setTabBar`

Remedy: `QWidget *old = tabButton(index, side);` → `setTabButton(...)`; then `if (old && old != btn) old->deleteLater();`. Also correct the comment.

### Row 286 — Low — `globalshortcutsportal.cpp:279`

review-code, lane 25-chrome-widgets, dim 7. **Disposition:** queued, ANTS-6625.

`CreateSession` has a 30 s response timeout (ANTS-5081). `BindShortcuts` has none.

Quote: `m_bus.connect(... SLOT(onBindShortcutsResponse(...)))`

Remedy: add a matching single-shot timer per bind request path that fails the bind the way the error path at line 322 does.

### Row 287 — Low — `kwinpositiontracker.cpp:83`

review-code, lane 25-chrome-widgets, dim 8. **Disposition:** queued, ANTS-6625.

The script name is unique per process and per tag, but not per call. Two overlapping runs with the same tag reuse the name: `pos`, `move` or `center` fired twice quickly, for example clicking ✥ twice, since `centerWindow` fires both `pos` and `center`.

Quote: `QStringLiteral("string:ants_terminal_%1_%2").arg(tag).arg(QCoreApplication::applicationPid())`

Remedy: add a per-call counter to the name.

### Row 288 — Low — `commandpalette.cpp:28`

review-code, lane 25-chrome-widgets, dim 2. **Disposition:** queued, ANTS-6625.

The ghost suffix is anchored to the caret, not to the end of the text. If the user moves the caret left while a ghost shows, the suffix is painted over the typed characters.

Quote: `int x = cr.right() + 1;`

Remedy: anchor at the end of the text, or show the ghost only when the caret is at the end.

Also reported as: "readability lane 07-chrome-widgets Info correctness lead src/commandpalette.cpp:28"

### Row 292 — Medium — `auditdialog.cpp:3400`

review-code, lane 26-audit-dialog, dim 2. **Disposition:** queued, ANTS-6621.

When "Since baseline" is on (`:3225`) or Recent-only is on (findings dropped at `:3002–3023`), `totalFindings` is a filtered count. That filtered count is written to `trend.json` as the run's real total.

Quote: `if (!m_showNewOnly && !m_snapshotPersisted) { appendSnapshot(curr);`

Remedy: gate on `!m_sinceBaseline && !m_recentOnly` too, or count the unfiltered set.

Also reported as: "readability lane 09-audit-dialog Info correctness lead src/auditdialog.cpp:3400"

### Row 293 — Medium — `auditdialog.cpp:2556`

review-code, lane 26-audit-dialog, dim 2. **Disposition:** queued, ANTS-6621.

Under a user's `diff.noprefix`, `diff.mnemonicPrefix`, `color.ui=always` or `diff.external`, no header matches. The sets come back empty with no error, so "Changed lines only" and "Since baseline" hide every filed finding.

Quote: `:2562`

Remedy: pass `--no-color --no-ext-diff --src-prefix=a/ --dst-prefix=b/ -c core.quotePath=false`.

### Row 294 — Medium — `auditdialog.cpp:828`

review-code, lane 26-audit-dialog, dim 2. **Disposition:** queued, ANTS-6621.

Findings carry working-tree line numbers, but this blames HEAD. On a file with uncommitted edits the author, date and sha come from the wrong line.

Quote: `args << "HEAD" << "--" << file;`

Remedy: drop `HEAD` so git blames the working tree, and clear `m_blameCache` in `runAudit`.

### Row 295 — Medium — `auditdialog_debtsweep.cpp:322`

review-code, lane 26-audit-dialog, dim 8. **Disposition:** queued, ANTS-6621.

A background `requestDebtScan` result (queued at `:56–73`, and started by an earlier [fix] or [allow]) can replace `m_debtFindings` while the modal dialog is open.

Quote: `:337`

Remedy: re-find the entry by its content after the dialog closes, or bounds-check it and compare it with `f`.

### Row 296 — Medium — `auditdialog.cpp:1291–1299`

review-code, lane 26-audit-dialog, dim 2. **Disposition:** queued, ANTS-6621.

ANTS-1111 §3.2 promises a modal with one checkbox per finding (unchecking excludes it) and a status line naming the allocated IDs.

Quote: `QInputDialog::getText(…heading…)`

### Row 297 — Medium — `auditdialog.cpp:106`

review-code, lane 26-audit-dialog, dim 2. **Disposition:** queued under ANTS-5848.

in `kCommonNoiseExcludes`, applied to `todo_scan` Matching is case-insensitive, so `todo_scan` ("Find TODO, FIXME…", `catalogue.cpp:29`) drops the usual `TODO:` / `FIXME:` forms.

Quote: `:115`

### Row 298 — Low — `auditdialog.cpp:2252–2272`

review-code, lane 26-audit-dialog, dim 15. **Disposition:** queued, ANTS-6621.

the Claude-review handoff writes verbatim snippets, including `secrets_scan` and gitleaks hits, plus project docs. Claude Code then reads that file. The AI-triage paths scrub with `SecretRedact::scrub` (ANTS-4448); this one does not. Fix: scrub `plainTextResults()`.

Quote: `secrets_scan`

Remedy: scrub `plainTextResults()`.

### Row 299 — Low — `auditdialog_export.cpp:153`

review-code, lane 26-audit-dialog, dim 2. **Disposition:** queued, ANTS-6621.

the comment says "fall back to the ledger's own note", but the code exports the AI's triage text as the suppression justification. That text could be a TRUE_POSITIVE rationale.

Quote: `if (reason.isEmpty()) reason = f.aiReasoning;`

### Row 300 — Low — `auditdialog_export.cpp:99`

review-code, lane 26-audit-dialog, dim 2. **Disposition:** queued, ANTS-6621.

the dedup key contains the line number (`:1767`), so GitHub alerts re-key whenever lines shift. Use a fingerprint that does not depend on the line.

Quote: `partialFp["primaryLocationLineHash/v1"] = f.dedupKey;`

### Row 301 — Low — `auditdialog.cpp:1861`

review-code, lane 26-audit-dialog, dim 2. **Disposition:** queued, ANTS-6621.

the Rule Quality dialog is a raw `new QDialog(this)` with no `DialogChrome::install`. That breaks D1–D4 of `dialogs.md`, and the INV-12 test cannot see it.

Quote: `new QDialog(this)`

### Row 302 — Low — `auditdialog.cpp:1968`

review-code, lane 26-audit-dialog, dim 3. **Disposition:** queued, ANTS-6621.

is put into a RichText label unescaped. Likewise `:3272` `prev.timestamp.left(10)` (from a project-side `trend.json`) goes into rich text unescaped. Use `toHtmlEscaped()`.

Quote: `"<b>Project:</b> " + m_projectPath`

### Row 303 — Low — `auditdialog.cpp:2709–2713`

review-code, lane 26-audit-dialog, dim 16. **Disposition:** queued, ANTS-6621.

→ `:2840` — if the child survives SIGKILL (a hung mount, which ANTS-5083 acknowledges), `start()` on a still-running `QProcess` fails but `waitForStarted` returns true. Every later check is then reported "Timed out" and never runs.

Quote: `:2840`

### Row 305 — Low — `auditdialog_export.cpp:647`

review-code, lane 26-audit-dialog, dim 2. **Disposition:** queued, ANTS-6621.

a file name containing `%1` takes the message. The same chained-`.arg` pattern is at `auditdialog.cpp:3489` and `export.cpp:639–643`.

Quote: `.arg(f.file, …).arg(f.message)`

### Row 306 — Low — `auditdialog_triage.cpp:516–531`

review-code, lane 26-audit-dialog, dim 3. **Disposition:** queued, ANTS-6621.

batch verdicts are applied to any key the model returns, not only the keys sent. Snippet-borne prompt injection can therefore mark unrelated findings FALSE_POSITIVE.

### Row 311 — Medium — `auditdelta.cpp:41`

review-code, lane 27-audit-engine, dim 2. **Disposition:** queued, ANTS-6622.

. `changedFiles` holds paths relative to the project root, as git prints them. Two tools report other forms: ruff's JSON `filename` is absolute (outside knowledge).

Quote: `if (changedFiles.contains(fileOf(v)))`

Remedy: normalise every finding's `file` to project-relative before fingerprinting.

### Row 312 — Medium — `auditscope.cpp:247-249`

review-code, lane 27-audit-engine, dim 2. **Disposition:** queued, ANTS-6622.

. Without `-uall`, an untracked directory shows as `newdir/`. `filterForTool` (`:120-121`) then drops it for cppcheck, clazy, clang-tidy, ruff, bandit, mypy and shellcheck, so new source files in a new directory are never scanned. This breaks ANTS-1504 INV-1 ("untracked included"). fix: add `--untracked-files=all`.

Quote: `runGitRaw(canonProject, {"status","--porcelain"}, …)`

Remedy: add `--untracked-files=all`.

### Row 315 — Medium — `auditautofix.cpp:74-85`

review-code, lane 27-audit-engine, dim 2. **Disposition:** queued, ANTS-6622.

the `stale_todo` rule fires on line shape alone; nothing checks the finding message. That contradicts the header (`auditautofix.h:5-6`, "gated on BOTH a finding signal and the exact source-line shape"). Separately, all three rules match a line inside a multi-line raw string literal: removing or rewriting it changes program data, which is not behaviour-neutral. fix: gate on the finding, and skip l…

Quote: `stale_todo`

Remedy: gate on the finding, and skip lines inside raw strings (`lineHasCode` can tell).

Also reported as: "readability lane 10-audit-engine-a Low Q4 src/auditautofix.h:6"

### Row 316 — Low — `auditengine.cpp:411`

review-code, lane 27-audit-engine, dim 2. **Disposition:** queued, ANTS-6622.

marks `latest_*.cpp` and `contest_*` as test paths, costing −20 confidence. fix: match `test_` at a path-segment start.

Quote: `lp.contains("test_")`

Remedy: match `test_` at a path-segment start.

### Row 317 — Low — `audithygiene.cpp:101`

review-code, lane 27-audit-engine, dim 2. **Disposition:** queued, ANTS-6622.

takes only the first `ignore` or `extend-ignore` array, but the header says "both count". fix: use `globalMatch`.

Quote: `ignoreRe.match(body)`

Remedy: use `globalMatch`.

### Row 319 — Low — `auditrunner.cpp:2554-2560`

review-code, lane 27-audit-engine, dim 5. **Disposition:** queued, ANTS-6622.

the group SIGKILL happens only `if (p->state() != NotRunning)`. If the leader exits on SIGTERM within the grace period and was the last pending tool, `procs` is freed and the grace timer is cancelled. Helpers that ignored SIGTERM survive, against the ANTS-5038 intent. fix: always send SIGKILL to each recorded pgid at reap.

Quote: `if (p->state() != NotRunning)`

Remedy: always send SIGKILL to each recorded pgid at reap.

### Row 320 — Low — `auditrunner.cpp:1664-1680`

review-code, lane 27-audit-engine, dim 8. **Disposition:** queued, ANTS-6622.

abandoned lane workers outlive their run slot, so repeated timed-out runs pile them up beyond `kMaxConcurrentRuns`. ANTS-5067 § 2.1 accepts one per run; pile-up across runs is not addressed.

Quote: `kMaxConcurrentRuns`

### Row 322 — Low — `auditrunner.cpp:2717,2727`

review-code, lane 27-audit-engine, dim 2. **Disposition:** queued under ANTS-5859.

passes the raw caller cwd where `buildVcsProvenanceBlock` documents a canonical root.

Quote: `writeSarif(..., req.projectRoot, ...)`

Also reported as: "readability lane 10-audit-engine-a Info correctness lead src/auditrunner.cpp:2717"

### Row 323 — Low — `auditrunner.cpp:2711-2732`

review-code, lane 27-audit-engine, dim 7. **Disposition:** queued, ANTS-6622.

if both the cache and fallback SARIF writes fail, the result is still `ok:true` with no `sarif_path` and no error.

Quote: `ok:true`

### Row 324 — Low — `falseposledger.cpp:206`

review-code, lane 27-audit-engine, dim 2. **Disposition:** queued, ANTS-6622.

splits an oversize line, and its tail fragments are parsed as separate records. The header promises a "per-line skip".

Quote: `readLine(kPerLineCap + 1)`

### Row 327 — Medium — `roadmapparse.cpp:2385-2398`

review-code, lane 28-roadmap-parse-render, dim 2. **Disposition:** queued, ANTS-6648.

The bullet walk, the section index and `parsePassHeadingBullets` (`:866-875`) never consult `MarkdownScan::fenceMask`.

Quote: `roadmapindex.cpp:102-103`

Remedy: compute `fenceMask(lines)` once in `parseBulletLines`, `buildIndex` and `parsePassHeadingBullets`, and skip fenced lines.

### Row 328 — Medium — `roadmapparse.cpp:1642-1643`

review-code, lane 28-roadmap-parse-render, dim 2. **Disposition:** queued under ANTS-5875.

Any column-0 line starting with `#` ends a pass block. That includes `#include`, `#hashtag` and shell comments in a fence. CommonMark only counts `#`–`######` followed by a space as a heading.

Quote: `return line.startsWith(QChar('#')) && !line.startsWith(QStringLiteral("#####"));`

Remedy: require `#{1,4}` followed by a space, tab or end of line, outside a fence.

### Row 329 — Medium — `roadmapsource.cpp:539-541`

review-code, lane 28-roadmap-parse-render, dim 4. **Disposition:** queued, ANTS-6648.

This swallows a failed lookup and falls back to the ants-v1 format. `roadmapwrite.cpp:489-506` (`dialectOf`) was fixed under ANTS-5087 for exactly this failure; this copy was not.

Quote: `if (const auto project = store.readProject(projectId, nullptr)) dialect = project->sourceFormat;`

Remedy: use `dialectOf()`. If it returns nullopt, fail with `ReadError::StoreFailed`.

### Row 331 — Medium — `roadmapfoldin.cpp:45-46`

review-code, lane 28-roadmap-parse-render, dim 2. **Disposition:** queued, ANTS-6648.

`roadmapwrite.h:195-199` says `liveRoadmapPath` must be resolved by the caller from the `.ants/project.json` `roadmap` override. The fold-in passes a hard-coded `ROADMAP.md`.

Quote: `:1050`

Remedy: resolve the override the way the `roadmap_log` callers do (`findRoadmapUnder`).

Also reported as: "readability lane 15-roadmap-parse-render-b Info correctness lead src/roadmapfoldin.cpp:45, 329, 454, 1021"

### Row 332 — Low — `roadmapfoldin.cpp:654`

review-code, lane 28-roadmap-parse-render, dim 11. **Disposition:** queued, ANTS-6648.

The store floor is a 64-bit number, and `findIdHighWaterMark` accepts up to 12 digits (`roadmapparse.cpp:1902-1905`). A floor above about 2.1 billion is truncated, giving negative or reissued ids.

Quote: `:689`

Remedy: keep the value 64-bit and refuse anything above `INT_MAX`.

### Row 333 — Low — `roadmaprender.cpp:888`

review-code, lane 28-roadmap-parse-render, dim 2. **Disposition:** queued under ANTS-5879.

`contentOf` is keyed by the resolved, canonical path. A relative or non-canonical `liveRoadmapPath` (the fold-in passes one built from an uncanonicalised `projectPath`) writes no stamp.

Quote: `if (contentOf.contains(opts.liveRoadmapPath))`

Remedy: key the stamp on `liveAbs`.

Also reported as: "readability lane 15-roadmap-parse-render-b Info correctness lead src/roadmaprender.cpp:888"

### Row 334 — Low — `roadmapparse.cpp:2509`

review-code, lane 28-roadmap-parse-render, dim 2. **Disposition:** queued, ANTS-6648.

On store reads this compiles and JIT-optimises a declared pattern once per item, up to 7,000 items, and never uses it because `gfmHere` is false.

Quote: `fillBulletRecord(rec, head, body, /*gfmHere=*/false, CompiledIdFormat(fmt));`

Remedy: build one `CompiledIdFormat` in `buildRecords`, or skip the compile when `gfmHere` is false.

### Row 338 — Medium — `roadmapdialog.cpp:501`

review-code, lane 29-roadmap-dialog, dim 2. **Disposition:** queued, ANTS-6647.

the `id:NNNN` search drops leading zeros. `roadmap-format.md` writes IDs zero-padded (`[PROJ-0123]`, `[ANTS-0512]`), so `id:0512` builds the marker `-512]`, which never matches `[ANTS-0512]`. Every padded ID below 1000 cannot be found this way. Separately, `id:S0001` (the `S?` namespace in `idTokenPattern`) fails `toInt` and falls back to plain search.

Quote: `:897`

Remedy: keep the digit text exactly as typed and match `-0*<n>]` with a regex.

### Row 339 — Medium — `roadmapdialog.cpp:2858-2864`

review-code, lane 29-roadmap-dialog, dim 2. **Disposition:** queued, ANTS-6647.

`isValidAnchorTarget` accepts only ASCII, but `RoadmapIndex::slugifyHeading` (`roadmapindex.cpp:38`) keeps every `QChar::isLetterOrNumber`.

Quote: `const bool ok = (u >= u'A' && u <= u'Z') || …`

Remedy: accept `isLetterOrNumber()` in the validator. That still excludes `"`, `'`, `<` and space, so the protection against hostile anchors is kept.

### Row 340 — Low — `roadmapdialog.cpp:1564`

review-code, lane 29-roadmap-dialog, dim 2. **Disposition:** queued, ANTS-6647.

LOW [dim 2] `roadmapdialog.cpp:1564` vs `:1642`: `headingLevel(raw, &hText)` runs before `if (inHtmlComment || …)`. a `##`/`###` line inside a multi-line `<!-- … -->` block is rendered as a section row. ANTS-5727's comment says comment blocks are invisible. `extractToc` and `headingSlugsInWalkOrder` count such headings too, so the anchors stay consistent with each other, just wrongly visible.

Quote: `:1642`

Remedy: track the comment state before the heading check.

### Row 342 — Low — `roadmapdialog.cpp:2394-2410`

review-code, lane 29-roadmap-dialog, dim 2. **Disposition:** queued, ANTS-6647.

the status and Kind checkboxes were moved into `QMenu` popups through `QWidgetAction` (`:2031-2037`, `:2125`), which are separate windows. `QWidget::setTabOrder` between widgets in different windows only warns and does nothing, so the middle of the chain is a no-op. The four ANTS-4412/4415 buttons (`m_tocToggleBtn`, `m_statusFilterBtn`, `m_kindFilterBtn`, `m_resetFiltersBtn`) are not in the chain…

Quote: `for (QCheckBox *cb : statusBoxes()) if (cb) tabChain << cb;`

Remedy: chain the four buttons in place of the checkboxes.

Also reported as: "readability lane 16-roadmap-dialog Info correctness lead src/roadmapdialog.cpp:2394-2410"

### Row 343 — Low — `roadmapdialog.cpp:3329`

review-code, lane 29-roadmap-dialog, dim 11. **Disposition:** queued under ANTS-5884.

LOW [dim 11] `roadmapdialog.cpp:3329`: `if (markdown.size() >= kAssembledCap)` the cap is checked only before each archive is added, so one archive can add up to 8 MiB past it. It also compares UTF-16 code units with a byte cap. The header promises "Total assembled-buffer cap is 64 MiB".

Quote: `if (markdown.size() >= kAssembledCap)`

Remedy: clamp each archive read to `kAssembledCap - markdown.size()`.

Also reported as: "readability lane 16-roadmap-dialog Info correctness lead src/roadmapdialog.cpp:3329"

### Row 344 — Low — `roadmapdialog.cpp:2365`

review-code, lane 29-roadmap-dialog, dim 2. **Disposition:** queued, ANTS-6647.

a timer left over from an earlier arm can disarm a later one, so the confirming second click is taken as a first click.

Quote: `QTimer::singleShot(3000, resetBtn, [resetBtn]() { resetBtn->setText(tr("Reset View")); });`

Remedy: use one member timer and restart it.

### Row 346 — Medium — `claudestatuswidgets.cpp:1129`

review-code, lane 30-claude-session-widgets, dim 2. **Disposition:** queued, ANTS-6627.

The chips can show another tab's session.

Quote: `:1222`

Remedy: use `m_tracker->transcriptPath(focusedPid)` when it is non-empty, and hide the chips or refuse the click when the tab's state is `NotRunning`.

### Row 347 — Medium — `claudetabtracker.cpp:212`

review-code, lane 30-claude-session-widgets, dim 2. **Disposition:** queued, ANTS-6627.

A quick Claude restart keeps the old transcript.

Quote: `if (!entry.transcriptPath.isEmpty()) return;`

Remedy: in the `entry.claudePid != found` branch, call `releaseWatch` and clear `transcriptPath` so it is resolved again.

### Row 348 — Medium — `claudestatuswidgets.cpp:2421-2431`

review-code, lane 30-claude-session-widgets, dim 2. **Disposition:** queued, ANTS-6627.

A queued model switch outlives its session.

Quote: `if (ss.state != ClaudeState::Idle || ss.awaitingInput) return;`

Remedy: drop the deferral whenever that shell's state becomes `NotRunning`.

### Row 349 — Low — `claudeallowlist.cpp:209`

review-code, lane 30-claude-session-widgets, dim 2. **Disposition:** queued, ANTS-6627.

the dialog has no Save button, only OK, Apply and Cancel.

Quote: `"Added to Allow list. Click Save to persist."`

### Row 354 — Low — `claudebgtasks.cpp:575`

review-code, lane 30-claude-session-widgets, dim 5. **Disposition:** queued, ANTS-6627.

a helper transcript with an unfinished last line makes `walkHelperTranscripts` return true on every poll, so `poll()` rescans every 2 s while a helper is writing. This is cheap because the walk resumes from where it stopped.

Quote: `walkHelperTranscripts`

### Row 355 — Medium — `remotecontrol_review.cpp:1111-1120`

review-code, lane 31-mcp-review-verbs, dim 3. **Disposition:** queued, ANTS-6639.

With `cache_only:true`, the call skips the RcGate check. That is the check that refuses a `caller_cwd` other than the focused tab. The call then goes into the same implementation, which still installs the real trust client and runs `loadGateConfig`.

Quote: `:1225`

Remedy: on the probe path, use `VerifyTrust::AlwaysDenyClient` or a non-prompting peek. On a first-contact config, return `cache_miss`.

### Row 356 — Medium — `remotecontrol_review.cpp:51-57`

review-code, lane 31-mcp-review-verbs, dim 2. **Disposition:** queued under ANTS-5944.

`IndieReviewEngine::derivePartition` returns a valid override as-is (`indiereviewengine.cpp:721-724`). A committed `.indie-review/partition.json` that declares a single lane is therefore silently replaced by the computed partition.

Quote: `:147-149`

Remedy: never replace a lane set that came from a valid override. When `derived`, set `path` to the computed source.

### Row 359 — Low — `remotecontrol_coldeyes.cpp:885`

review-code, lane 31-mcp-review-verbs, dim 2. **Disposition:** queued, ANTS-6639.

The check is `isDouble()`, but `QJsonValue::toInt()` returns its default, 0, for a non-integral or out-of-range value. So `exit_code:1.5` or `4294967297` is recorded as a passing exit 0.

Quote: `:999`

Remedy: refuse when `toDouble() != toInt()`.

### Row 360 — Low — `remotecontrol_coldeyes.cpp:668`

review-code, lane 31-mcp-review-verbs, dim 2. **Disposition:** queued, ANTS-6639.

Converting a double outside the `int` range to `int` is undefined behaviour in C++ (for example `step:1e300`).

Quote: `state["step"] = static_cast<int>(stepVal.toDouble());`

Remedy: range-check, or use `toInt()` and refuse when the round trip fails.

### Row 361 — Low — `remotecontrol_coldeyes.cpp:461`

review-code, lane 31-mcp-review-verbs, dim 3. **Disposition:** queued, ANTS-6639.

Echo hygiene has drifted apart. `ceSanitiseEcho` (64-character cap, control characters replaced) is applied to keys, but these sites put caller text into an LLM-facing error without it:

Remedy: route each through `ceSanitiseEcho`.

### Row 362 — Low — `remotecontrol_coldeyes.cpp:172`

review-code, lane 31-mcp-review-verbs, dim 2. **Disposition:** queued, ANTS-6639.

When `lane` names an existing partition lane, any `doc_paths` the caller passed are dropped with no `doc_paths_rejected` or warning.

Quote: `if (!match) { const QJsonValue dpV = req.value("doc_paths"); ...`

Remedy: emit a `doc_paths_ignored` note.

### Row 363 — Low — `remotecontrol_coldeyes.cpp:47`

review-code, lane 31-mcp-review-verbs, dim 16. **Disposition:** queued, ANTS-6639.

Both TTLs are measured on the wall clock. If the clock steps backwards, the difference goes negative and the entry counts as fresh until the clock catches up.

Quote: `remotecontrol_review.cpp:1260`

Remedy: use `QElapsedTimer` or a monotonic clock for in-process TTLs.

### Row 369 — Medium — `docintegrity.cpp:27`

review-code, lane 32-doc-engines, dim 2. **Disposition:** queued under ANTS-6275.

What goes wrong. A CommonMark link with a title, `[t](other.md "Title")`, yields filePart `other.md "Title"`. `:929` then reports `broken_link` "does not exist" for a file that exists.

Quote: `R"(\[[^\]]*\]\(([^)]*)\))"`

Remedy: strip an optional whitespace-separated `"…"`, `'…'` or `(…)` title before resolving.

### Row 370 — Medium — `doccitations.cpp:1106`

review-code, lane 32-doc-engines, dim 11. **Disposition:** queued, ANTS-6634.

What goes wrong. The quotation pass caches every attributed document's whole folded text in `foldedByPath`. That memory is not charged against `maxCacheBytes`, the in-memory cap `Options` names (`doccitations.h:81`).

Quote: `:1338`

Remedy: charge the folded size to the reader's cache budget and evict, or re-read past the cap.

### Row 371 — Low — `markdownscan.cpp:222-227`

review-code, lane 32-doc-engines, dim 5. **Disposition:** queued, ANTS-6634.

. `markdownscan.cpp:222-227`. Pass 2 re-scans every line between run `a` and run `b`, calling `trimmed()` (one allocation each), for every candidate `b`. Used runs are only skipped *after* that scan. One stray backtick at the top of a large boundary-free block costs O(runs × lines). A GFM table is such a block, because it has no blank lines. A 20 000-line table is about 4×10⁸ `trimmed()` calls.

Quote: `a`

Remedy: compute a per-line boundary flag once, and stop at the first boundary.

### Row 372 — Low — `doccitations.cpp:1474`

review-code, lane 32-doc-engines, dim 7. **Disposition:** queued, ANTS-6634.

. `doccitations.cpp:1474`. When `docText` is unset and the document no longer exists, `check()` returns `ok:true` with zero citations. doc_lint's Phase 2 re-reads each document after Phase 1, so a document deleted in between reports as checked clean rather than `check_failed`.

Quote: `docText`

Remedy: return the `read_failed` refusal when the file is absent.

Also reported as: "readability lane 31-doc-engines Info correctness lead src/doccitations.cpp:1474"

### Row 373 — Medium — `featurecoverage.cpp:755-756`

review-code, lane 33-spec-engines, dim 7. **Disposition:** queued, ANTS-6652.

Neither drift runner passes `truncatedOut`. Nor does the third caller, `debtsweepengine.cpp:169` (outside this lane).

Quote: `832-834`

Remedy: set the flag on the per-file skip too. Have all three runners pass the flag and emit one lane-level line when it is set.

### Row 375 — Medium — `specconformance.cpp:28-32`

review-code, lane 33-spec-engines, dim 2. **Disposition:** queued, ANTS-6652.

')) ++n;` … `if (close < 0) break; // unterminated fence — nothing further is safe`

Quote: `144-152`

Remedy: track top-level fences with `MarkdownScan::fenceOpenerChar` / `fenceCloses`. When an opener has no closer, emit a candidate rather than breaking.

### Row 377 — Low — `speclog.cpp:150`

review-code, lane 33-spec-engines, dim 9. **Disposition:** queued, ANTS-6652.

The status, label and body strings are written verbatim. The handler (`remotecontrol_feedback.cpp:1170-1227`) does not reject newlines either.

Quote: `188`

Remedy: refuse, or escape, `\n` in these arguments.

### Row 378 — Low — `speclint.cpp:443-452`

review-code, lane 33-spec-engines, dim 2. **Disposition:** queued, ANTS-6652.

The `CMakeLists.txt` scan also matches commented-out lines (`# …tests/features/x/test_x.cpp`) and strings inside messages. Commenting a test out is a common way to unwire it, and this is the trap `test_surface_unwired` exists to catch.

Quote: `R"(tests/features/([a-z0-9_]+)/test_[a-z0-9_]+\.(?:cpp|sh|py)\b)"`

Remedy: strip `#` comments before matching.

### Row 380 — Low — `speclog.cpp:535-540`

review-code, lane 33-spec-engines, dim 2. **Disposition:** queued, ANTS-6652.

`speclog.h:41-49` promises `rowOrder` is `oldest_first`, `newest_first` or `ambiguous` for `append_loop`. The create-section path returns an empty string.

Quote: `r.rowShape = QStringLiteral("bullet");`

Remedy: set `"ambiguous"`.

### Row 382 — Low — `speclint.cpp:654-655`

review-code, lane 33-spec-engines, dim 2. **Disposition:** queued, ANTS-6652.

Only a trailing full stop is stripped. `feature,` or `feature` yields `header_kind_unknown`, while `statusWord` strips exactly this punctuation from Status.

Quote: `if (kind.endsWith(QLatin1Char('.'))) kind.chop(1);`

### Row 383 — Medium — `coldeyesdialog.cpp:188-189`

review-code, lane 34-review-engines, dim 2. **Disposition:** queued, ANTS-6646.

What the spec says: ANTS-1721 §2.2 builds keywords from `L.name`, the spec ID, and "significant words from `m.summary`" (the H1). The point is that ROADMAP and CHANGELOG contribute "only its relevant slices, not a blind 32 KiB head-truncation".

Quote: `add(lane.name); add(lane.summary);`

Remedy: tokenise `m.summary`, add the spec ID as one keyword (`ANTS-1435`), and drop generic tokens (`spec`, `lane`, `single`, `ants`, `docs`).

### Row 384 — Medium — `indiereviewengine.cpp:913`

review-code, lane 34-review-engines, dim 2. **Disposition:** queued, ANTS-6646.

a lane `sourcePath` that is a directory gets an empty fenced body. Directories reach this point from `parsePartitionOverride` (any project-relative path is accepted, `:150-160`) and from `deriveFileListPartition` (which keeps entries that point at "a real file or directory", `:225-229`). The header (ANTS-4100) says lanes may name directories.

Quote: `out += BriefDispatch::fenceBody(sp, slurpCappedUtf8(canon));`

Remedy: expand directory entries through `collectLaneFiles` before inlining.

### Row 388 — Medium — `debtsweepengine.cpp:1022`

review-code, lane 34-review-engines, dim 7. **Disposition:** queued, ANTS-6646.

on a repo with no tags and 10 or fewer commits, `resolveSinceRef` returns `HEAD~10` (`:99`), which does not exist. `git diff` exits non-zero, and `runGit` deliberately records no failure for a non-zero exit. So `diffSet` is empty, and every `[Unreleased]` bullet that cites a file is reported as `stale_changelog_bullet`.

Quote: `const QStringList diffPaths = diffedFiles(projectPath, since, {});`

Remedy: if the diff fails or is capped, stop the detector (or emit `git_failed`) instead of flagging.

### Row 389 — Low — `indiereviewengine.cpp:926`

review-code, lane 34-review-engines, dim 3. **Disposition:** queued, ANTS-6646.

ROADMAP slice lines, and `lane.name`, `summary` and `sourcePaths` from a repo-controlled `partition.json` (`:882-892`), go into the dispatch brief outside the "treat as data" fence. This is a prompt-injection path (OWASP LLM01).

Quote: `out += line;`

Remedy: fence the ROADMAP slice with `fenceBody(..., "roadmap")`.

### Row 390 — Low — `debtsweepengine.cpp:982`

review-code, lane 34-review-engines, dim 1. **Disposition:** queued, ANTS-6646.

This is a substring match, so `ANTS-125` counts as "mentioned" by any `ANTS-1250…1259` subject, and real misses are hidden.

Quote: `if (log.contains(p.first)) continue;`

Remedy: use a `\bANTS-125\b` regex.

### Row 391 — Low — `debtsweepengine.cpp:1483`

review-code, lane 34-review-engines, dim 1. **Disposition:** queued, ANTS-6646.

The stale-file guard does not check the variable name. If the file has shifted since the scan, a different, valid `Q_UNUSED` marker now on that line is deleted.

Quote: `if (!kMarkerRe.match(target).hasMatch()) { staleVerdict(); return v; }`

Remedy: compare the captured variable name against the one in the finding.

### Row 392 — Low — `indiereviewdispatcher.cpp:213-214`

review-code, lane 34-review-engines, dim 7. **Disposition:** queued, ANTS-6646.

On a write failure, the error shows 4 KiB of the model's report instead of the cause, which is `f.errorString()`.

Quote: `.arg(redactAndTruncate(utf8, req.apiKey, 4 * 1024));`

Remedy: use `f.errorString()`.

Also reported as: "readability lane 33-review-engines Info correctness lead src/indiereviewdispatcher.cpp:213"

### Row 393 — Low — `indiereviewengine.cpp:760`

review-code, lane 34-review-engines, dim 1. **Disposition:** queued, ANTS-6646.

A merged lane named `"a, b"` never matches the ROADMAP needle (`:922`) or the false-positive ledger filter (`:943`). So both blocks are silently empty for merged lanes.

Quote: `existing.name += QStringLiteral(", ") + l.name;`

Remedy: match against each component of the name.

### Row 394 — Low — `indiereviewengine.cpp:1367`

review-code, lane 34-review-engines, dim 2. **Disposition:** queued under ANTS-5978.

The dialog and the engine serve any `projectCwd`, so this text is wrong for other projects.

Quote: `"You are reviewing an Ants Terminal codebase..."`

Remedy: interpolate the project name, or drop it.

### Row 395 — Low — `coldeyesdialog.cpp:306-308`

review-code, lane 34-review-engines, dim 1. **Disposition:** queued, ANTS-6646.

`m_staleByLane` is cleared only on re-partition. A re-review round, or a lane that has since been deselected, still shows stale citations from earlier rounds. That makes INV-3's per-round surfacing inaccurate.

Quote: `m_results.staleFindings << it.value();`

Remedy: clear it in `prepareDispatch()` and in the re-review path.

### Row 396 — Low — `coldeyesdialog.cpp:412`

review-code, lane 34-review-engines, dim 2. **Disposition:** queued, ANTS-6646.

ANTS-1721 §2.3 and ANTS-1258 §2.3 say each row shows "the per-lane context". `f.contexts` is never rendered.

Quote: `indiereviewdialog.cpp:289`

### Row 397 — Low — `filecontentcache.h:45`

review-code, lane 34-review-engines, dim 1. **Disposition:** queued, ANTS-6646.

The cache entry is keyed on modification time alone. A file rewritten with its modification time kept (`cp -p`, `rsync -t`), or rewritten twice within one millisecond, is served stale.

Quote: `if (... mtime != 0 && it->first == mtime) return it->second;`

Remedy: add the file size to the key.

### Row 400 — Medium — `verifyengine.cpp:287`

review-code, lane 35-test-audit-verify, dim 2. **Disposition:** queued, ANTS-6655.

ANTS-1289 INV-5 says the summary and the `The following tests FAILED:` block are parsed per line over the combined output. The code parses only the rolling tail window, which can be as small as 10 lines.

Quote: `:360`

Remedy: parse each line as it arrives in the `readyRead` lambda, independent of the tail window.

### Row 401 — Medium — `testauditdialog.cpp:415-422`

review-code, lane 35-test-audit-verify, dim 2. **Disposition:** queued, ANTS-6655.

on a resumed run, `prepareDispatch` sends only the chunks not yet reviewed (`:229-236`). `m_collectedReports` then holds only this session's reports.

Quote: `for (auto it = m_collectedReports.cbegin(); …) reports << it.value();`

Remedy: parse `reportsRelDir()/*.md` from disk for fold-in.

### Row 402 — Medium — `testauditdialog.cpp:324-327`

review-code, lane 35-test-audit-verify, dim 7. **Disposition:** queued, ANTS-6655.

a failed brief still becomes a real LLM job. Its reply is collected as that chunk's report, written to disk, synthesised and parsed for fold-in.

Quote: `req.userPrompt = QStringLiteral("[test-audit: brief failed (%1)]")`

Remedy: fail the lane rather than dispatch it.

### Row 404 — Medium — `mutationprobe.cpp:196-198`

review-code, lane 35-test-audit-verify, dim 2. **Disposition:** queued, ANTS-6655.

a mutant is scored Killed whenever the counts can be read, even when `failed == 0`. pytest printing "5 passed, 1 error" (a collection error in the mutated module) exits 1. `parseCounts` ignores "error" (`:117-133`), so the result is passed=5, failed=0, Killed.

Quote: `if (c.passed < 0 || c.failed < 0) return MutantVerdict::Broken; return MutantVerdict::Killed;`

Remedy: return Killed only when `c.failed > 0`, otherwise Broken.

### Row 405 — Low — `testauditdialog.cpp:64-66`

review-code, lane 35-test-audit-verify, dim 4. **Disposition:** queued, ANTS-6655.

permissions are set to owner-only only after the rename. A new report briefly exists with umask permissions.

Quote: `if (!f.commit()) return false; if (!setOwnerOnlyPerms(path))`

Remedy: use the `testrescache` order in both.

Also reported as: "readability lane 34-test-audit-verify Info correctness lead src/testauditdialog.cpp:56-64"

### Row 408 — Low — `testauditengine.cpp:1005-1006`

review-code, lane 35-test-audit-verify, dim 2. **Disposition:** queued, ANTS-6655.

dimension names are matched in snake_case. Natural headers such as "## Coverage gaps", "## Error handling" or "## Hardcoded data" match nothing.

Quote: `if (header.contains(d, Qt::CaseInsensitive)) return d;`

Remedy: normalise spaces and `/` to `_` before matching.

### Row 409 — Low — `testauditengine.cpp:2208-2209`

review-code, lane 35-test-audit-verify, dim 9. **Disposition:** queued, ANTS-6655.

caller-supplied fields go into `ROADMAP.md` unescaped. A newline in `fix` injects arbitrary lines, including a `## ` heading that `findActiveReleaseHeading` may later pick.

Quote: `<< " - Fix: " << f.value(QStringLiteral("fix")).toString()`

Remedy: " << f.value(QStringLiteral("fix")).toString()`

### Row 410 — Low — `testauditengine.cpp:1749-1752`

review-code, lane 35-test-audit-verify, dim 3. **Disposition:** queued, ANTS-6655.

only the exact-case closing tag is escaped. `</CHUNK_REPORT>` or `</chunk_report >` get through and can forge a fence.

Quote: `contents.replace(QStringLiteral("</chunk_report>"), …)`

Remedy: use a case-insensitive regex that allows whitespace.

### Row 411 — Low — `mutationprobe.cpp:23`

review-code, lane 35-test-audit-verify, dim 2. **Disposition:** queued, ANTS-6655.

`QString::count` counts overlapping matches, but `replace()` replaces only non-overlapping ones. "aa" in "aaa" reports 2 occurrences and replaces 1.

Quote: `r.occurrences = content.count(m.oldText);`

Remedy: count with `indexOf`, stepping by `oldText.size()`.

### Row 412 — Low — `testrescache.cpp:79-80`

review-code, lane 35-test-audit-verify, dim 2. **Disposition:** queued, ANTS-6655.

ctest's `*Not Run` (missing executable) is not treated as a failure. It gets no `failing_tests` entry, and `readKill` misses it too.

Quote: `return tok == u"Failed" || tok == u"Subprocess" || tok == u"Timeout" || tok == u"Exception";`

### Row 413 — Low — `buildtargets.cpp:180`

review-code, lane 35-test-audit-verify, dim 2. **Disposition:** queued, ANTS-6655.

a list item `${CMAKE_SOURCE_DIR}` is dropped. The header (`:93`) says the root itself gives `""`.

Quote: `if (!rel.isEmpty()) paths.append(rel);`

### Row 414 — Low — `testrunners.cpp:325-326`

review-code, lane 35-test-audit-verify, dim 16. **Disposition:** queued, ANTS-6655.

a go build failure (`FAIL pkg [build failed]`, no `=== RUN` line) and a cargo compile error (no `test result:` line) both come back `recognised:false`.

Quote: `:289`

### Row 416 — Medium — `changeloglog.cpp:946–947`

review-code, lane 36-changelog-feedback, dim 4. **Disposition:** queued, ANTS-6624.

`op:normalize` silently flips a dated-led mixed section into a flat-led one.

Quote: `const int topicLine = firstFeatureGroupedTopicLine(lines, unrel + 1, sectionEnd);`

Remedy: refuse `mixed_section` in `normalizeUnreleased` when `classifyUnreleased(...).mixed`.

### Row 417 — Medium — `feedbackfile.cpp:490`

review-code, lane 36-changelog-feedback, dim 2. **Disposition:** queued, ANTS-6624.

an unclosed code fence in a finding's text can hide later findings.

Quote: `return v.replace(QStringLiteral("\n"), QStringLiteral("\n "));`

Remedy: indent continuations by 4 or more spaces, or escape fence runs; fold `session_label` to one line and indent `note`.

### Row 418 — Medium — `feedbackfile.cpp:568`

review-code, lane 36-changelog-feedback, dim 2. **Disposition:** queued, ANTS-6624.

text inside a finding can be mistaken for its id line.

Quote: `"^ *(?:[-*] +)?\\*{0,2} *proposed id[ *:]+(.*)$"`

Remedy: restrict the id-line match to lines with fewer than 2 leading spaces, i.e. not continuations.

### Row 421 — Low — `changeloglog.cpp:1069–1076`

review-code, lane 36-changelog-feedback, dim 2. **Disposition:** queued, ANTS-6624.

can return overlapping ranges. A dated `### ` topic's range runs past the bullets under it, so when the heading and a bullet both cite `id`, one occurrence counts twice and the call refuses a false `text_ambiguous`. Fix: dedupe overlapping ranges, or end a topic range at its first bullet.

Quote: `entriesCiting`

Remedy: dedupe overlapping ranges, or end a topic range at its first bullet.

### Row 422 — Low — `changeloglog.cpp:560–566`

review-code, lane 36-changelog-feedback, dim 2. **Disposition:** queued, ANTS-6624.

counts placeholder prose such as "(Nothing yet.)" as an entry. Per `changelogquery.cpp:390` that is the skeleton's empty-[Unreleased] note. A placeholder-only section is released instead of refusing `nothing_to_release`. I did not open the skeleton to confirm the exact text.

Quote: `isReleasableLine`

### Row 424 — Low — `feedbackfile.cpp:668`

review-code, lane 36-changelog-feedback, dim 2. **Disposition:** queued, ANTS-6624.

`set_title` breaks when the current project title itself contains an em dash. `— A — B` becomes `— A — New`, and `oldTitle` is reported as `B`. Fix: split at the first dash after the prefix.

Quote: `line.lastIndexOf(kDash)`

Remedy: split at the first dash after the prefix.

### Row 425 — Low — `feedbackfile.cpp:331–334`

review-code, lane 36-changelog-feedback, dim 4. **Disposition:** queued, ANTS-6624.

a finding's `entryHeading` comes from the v1 boundary scanner, which accepts any `# ` line, including a pasted shell comment. The v2 enumerator rejects those lines (`:596`). In pre-ANTS-3695 files, `entry_heading` and `filed_on` can therefore name a shell comment and come back empty.

Quote: `entryHeading`

### Row 426 — Low — `feedbackstore.cpp:203,220–223`

review-code, lane 36-changelog-feedback, dim 2. **Disposition:** queued, ANTS-6624.

when a dry run or a no-op write imports a file, the call reports `imported:true` but rolls the import back.

Quote: `imported:true`

Also reported as: "readability lane 41-changelog-feedback Info correctness lead src/feedbackstore.cpp:200-223"

### Row 427 — Low — `feedbackstore.cpp:115`

review-code, lane 36-changelog-feedback, dim 9. **Disposition:** queued, ANTS-6624.

returns true when the file cannot be opened. An unreadable export then refuses `export_drift` with "was edited outside the feedback verbs", which misstates the cause.

Quote: `fileDrifted`

### Row 428 — Low — `feedbackstore.cpp:95,108`

review-code, lane 36-changelog-feedback, dim 7. **Disposition:** queued, ANTS-6624.

the return value of `QFile::setPermissions` is ignored. A failed chmod to 0444 still returns success, against INV-5's 0444 mode.

Quote: `QFile::setPermissions`

### Row 432 — Medium — `buildcache.cpp:200`

review-code, lane 37-diagnostics-logging, dim 2. **Disposition:** queued, ANTS-6632.

staleness is measured against the time the result was recorded, not the time the build started. A file edited after the build began but before `op=record` is reported as not stale, even though the recorded result may not include that edit. Editing in parallel with a long build is a normal pattern.

Quote: `:234`

Remedy: compare against `startedAtMs` when it was supplied, and fall back to `recordedAtMs` otherwise.

### Row 433 — Medium — `buildcache.cpp:122-123`

review-code, lane 37-diagnostics-logging, dim 2. **Disposition:** queued under ANTS-6039.

GCC 9 and later, and clang, print source-excerpt and caret lines (` 3 | bar(1);` / ` | ^~~`) between an `error:` and its `note:` by default. So the note is almost never directly adjacent, and every candidate note counts as a warning. One error with five candidate notes reports `warnings_count:5`.

Quote: `if (gap > 1) lastSeverity.clear();`

Remedy: let excerpt and caret lines (`^\s*\d*\s*\|`) keep the chain alive, after amending the spec.

### Row 435 — Medium — `readlog.cpp:163`

review-code, lane 37-diagnostics-logging, dim 3. **Disposition:** queued, ANTS-6632.

the `include` and `exclude` regexes come from the caller and run against lines of up to `max_bytes` (4 MiB). The ANTS-5110 time budget (`:238`) is only checked between lines, so it cannot interrupt a single catastrophic-backtracking match. One call can hold the worker indefinitely.

Quote: `:279`

Remedy: cap the pattern length, and run a match limit (PCRE2 `(*LIMIT_MATCH=…)` prefix) or a per-line time check.

### Row 436 — Low — `readlog.cpp:246`

review-code, lane 37-diagnostics-logging, dim 2. **Disposition:** queued, ANTS-6632.

. `QIODevice::readLine(char*, maxSize)` reads at most `maxSize-1` bytes. So a line whose content is exactly `max_bytes` long, plus its `\n`, comes back without the newline and is skipped as oversized. Spec § 2.3 says only lines *longer* than `max_bytes` are skipped. Fix: pass `maxBytes + 2`.

Quote: `BoundedReadLine::readTight(f, qint64(maxBytes) + 1, scratch)`

Remedy: pass `maxBytes + 2`.

### Row 437 — Low — `readlog.cpp:288`

review-code, lane 37-diagnostics-logging, dim 2. **Disposition:** queued, ANTS-6632.

. Keeping at least one line lets the `lines[]` array exceed `max_bytes` by up to about six times when a line is full of control bytes (see `lineCost`). Spec § 2.3 says `max_bytes` bounds the emitted array. Fix: escape-truncate the single line, or document the exception.

Quote: `:317`

Remedy: escape-truncate the single line, or document the exception.

### Row 438 — Low — `readlog.cpp:251-254`

review-code, lane 37-diagnostics-logging, dim 11. **Disposition:** queued, ANTS-6632.

the oversize-drain `do … while` loop never checks the time budget. One very long line is drained in full regardless of `budgetMs`. Memory stays bounded. Fix: check `clock.elapsed()` inside the loop.

Quote: `do … while`

Remedy: check `clock.elapsed()` inside the loop.

### Row 439 — Low — `readlog.cpp:64`

review-code, lane 37-diagnostics-logging, dim 15. **Disposition:** queued, ANTS-6632.

. This misses `-----BEGIN PGP PRIVATE KEY BLOCK-----`, so the body lines of a PGP key block are not redacted. Fix: allow an optional ` BLOCK` before the closing dashes.

Quote: `secretredact.h:80`

Remedy: allow an optional ` BLOCK` before the closing dashes.

### Row 440 — Low — `debuglog.cpp:163-166`

review-code, lane 37-diagnostics-logging, dim 15. **Disposition:** queued, ANTS-6632.

. "Clear Log File" leaves `debug.log.1` in place, which can hold up to 10 MiB of logged PTY output. `debuglog.h:61` also says clear "truncates the file", but the code deletes it. Fix: remove the `.1` file too, and correct the header comment.

Quote: `QFile::remove(logFilePath());`

Remedy: remove the `.1` file too, and correct the header comment.

Also reported as: "readability lane 43-diagnostics-logging Low Q4 src/debuglog.h:61; readability lane 43-diagnostics-logging Info correctness lead src/debuglog.cpp:166"

### Row 441 — Low — `debuglog.cpp:120`

review-code, lane 37-diagnostics-logging, dim 15. **Disposition:** queued, ANTS-6632.

The read_log default target (debug log) returns Input/Pty-category lines to the MCP caller. The comment at `debuglog.cpp:120` says these carry keystrokes, and `SecretRedact::scrub` cannot recognise a typed password. I am not sure whether this is accepted; see Open questions.

Quote: `SecretRedact::scrub`

### Row 442 — Low — `debuglog.cpp:100-103`

review-code, lane 37-diagnostics-logging, dim 8. **Disposition:** queued, ANTS-6632.

. If two instances both cross the cap, the second one's `remove` deletes the generation the first one just rotated out. Fix: rotate under an advisory `flock`.

Quote: `QFile::remove(rotated); QFile::rename(path, rotated);`

Remedy: rotate under an advisory `flock`.

### Row 443 — Low — `tooldetectionengine.cpp:51`

review-code, lane 37-diagnostics-logging, dim 16. **Disposition:** queued, ANTS-6632.

is a wall clock. If the clock moves backwards, `now - probedAtMs` goes negative and a cached miss is not re-probed until the clock catches up, which breaks INV-3's 30 s promise. Fix: use a monotonic clock (`QElapsedTimer` / `steady_clock`).

Quote: `:54`

Remedy: use a monotonic clock (`QElapsedTimer` / `steady_clock`).

### Row 444 — Low — `buildcache.cpp:190-193`

review-code, lane 37-diagnostics-logging, dim 2. **Disposition:** queued, ANTS-6632.

returns `staleKnown=true, stale=false`. A cache with no timestamp is reported as known-fresh. Fix: set `staleKnown=false`.

Quote: `recordedAtMs <= 0`

Remedy: set `staleKnown=false`.

Also reported as: "readability lane 43-diagnostics-logging Low Q5 src/buildcache.cpp:190"

### Row 445 — Low — `buildcache.cpp:63`

review-code, lane 37-diagnostics-logging, dim 2. **Disposition:** queued, ANTS-6632.

cuts at a UTF-16 code unit, so it can split a surrogate pair. Fix: step back one unit if the cut lands on a high surrogate.

Quote: `:80`

Remedy: step back one unit if the cut lands on a high surrogate.

### Row 447 — Medium — `tools/local-ci.sh:275`

review-code, lane 38-dev-shell-tools, dim 8. **Disposition:** queued, ANTS-6631.

the push gate runs ninja in `build-asan/` before taking the shared build lock.

Quote: `if ninja -C "$asan_dir" build.ninja >/dev/null 2>&1; then`

Remedy: take the lock before the `ninja -n` / regen block and skip the leg if it is held.

### Row 449 — Medium — `packaging/release.sh:120`

review-code, lane 38-dev-shell-tools, dim 2. **Disposition:** queued, ANTS-6631.

the release build relinks `build/ants-terminal` in place.

Quote: `cmake --build build >/dev/null 2>&1`

Remedy: build and test through `tools/build-and-stage.sh` (or `build-fast/`) in `build_and_test`.

### Row 450 — Low — `tools/check-shipped-coverage.sh:47`

review-code, lane 38-dev-shell-tools, dim 4. **Disposition:** queued, ANTS-6631.

loops forever

Quote: `--since) SINCE="${2:-}"; shift 2 ;;`

Remedy: `[[ $# -ge 2 ]] || { usage; exit 2; }`.

### Row 451 — Low — `tools/check-shipped-coverage.sh:86,173`

review-code, lane 38-dev-shell-tools, dim 3. **Disposition:** queued, ANTS-6631.

/ `shipped >= '${SINCE}'` — SQL is built by pasting text in. A checkout path containing `'` makes the project lookup fail, and the gate then "SKIPS" with exit 0, so it passes without checking. `--since` is unvalidated input going into SQL (the database is opened read-only). Fix: check `SINCE` with a `^[0-9]{4}-[0-9]{2}-[0-9]{2}$` pattern and escape `'` in `ROOT` (`${ROOT//\'/\'\'}`).

Quote: `WHERE root='${ROOT}'`

Remedy: check `SINCE` with a `^[0-9]{4}-[0-9]{2}-[0-9]{2}$` pattern and escape `'` in `ROOT` (`${ROOT//\'/\'\'}`).

Also reported as: "readability lane 46-tools-ci-gate Info correctness lead tools/check-shipped-coverage.sh:86,121,141,245"

### Row 452 — Low — `tools/vendor-grammars.sh:80-81`

review-code, lane 38-dev-shell-tools, dim 7. **Disposition:** queued, ANTS-6631.

then `rm -rf "$runtime_dir"` — `fetch` runs inside `$(…)`, where `set -e` is off. With `--pin` and an empty hash, a failed `curl` gives `got=""`. That empty value is pinned, the `tar` failure is ignored, and an empty `dest` is returned.

Quote: `src=$(fetch '.runtime' tree-sitter)`

Remedy: check that `$src/lib/src` exists before the `rm -rf`, and have `fetch` return failure when `curl`, `sha256sum` or `tar` fails.

### Row 453 — Low — `tools/local-ci.sh:407-414`

review-code, lane 38-dev-shell-tools, dim 7. **Disposition:** queued, ANTS-6631.

any non-zero exit from the guard is reported as a floor violation. That includes infrastructure failures: `podman volume rm` failing at `qt62-guard.sh:325-326`, a marker write failing at `:334-335`, or a podman run error. That sends the developer looking for an API that isn't there. Fix: give infrastructure failures their own exit code (for example 4) and print a separate message for it.

Quote: `elif ! tools/qt62-guard.sh --warm-only; then … "Qt 6.2 FLOOR violation"`

Remedy: give infrastructure failures their own exit code (for example 4) and print a separate message for it.

### Row 454 — Low — `tools/asan-nightly.sh:84-87`

review-code, lane 38-dev-shell-tools, dim 16. **Disposition:** queued, ANTS-6631.

the trap marks the tree and exits but does not stop the background `cmake`, which inherits lock fd 9. If the script receives a signal outside systemd's cgroup kill (for example a `kill` sent to its process ID only), the build is left running without its parent and keeps holding the lock. Compare `qt62-guard.sh:331-333`, which stops its container. Fix: `kill "$!"` in the trap.

Quote: `cmake`

Remedy: `kill "$!"` in the trap.

Also reported as: "readability lane 46-tools-ci-gate Info correctness lead tools/asan-nightly.sh:84-87"

### Row 455 — Medium — `release-audit.yml:145-152`

review-code, lane 39-dev-python-ci-packaging, dim 7. **Disposition:** queued, ANTS-6630.

The step has no `shell:` key, so GitHub runs it as `bash -e {0}`. `set -uo pipefail` adds options but never turns `-e` off.

Quote: `set -uo pipefail`

Remedy: `set +e` at the top of both scripts, or `rc=0; python3 … || rc=$?`, and `|| MAGIC=""` / `|| got=""`.

### Row 456 — Medium — `release.yml:172-176`

review-code, lane 39-dev-python-ci-packaging, dim 3. **Disposition:** queued, ANTS-6630.

An unpinned, unverified binary is run in the job that holds `contents: write`, and two steps later the same job is handed `ANTS_UPDATE_SIGNING_KEY` (`:231`).

Quote: `curl -fsSL … -o linuxdeploy https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/…`

Remedy: pin linuxdeploy to a recorded sha256 (`sha256sum -c`). Separately, sign in a separate job that receives only the artefact. Correct the header.

### Row 457 — Medium — `tools/roadmap-import-verify.py:41`

review-code, lane 39-dev-python-ci-packaging, dim 4. **Disposition:** queued under ANTS-6064.

The docstring (`:14-21`) says this "MIRRORS `src/roadmapparse.cpp`". But the C++ `rxKind()` (`roadmapparse.cpp:336-337`) stops the value at the first `.` (`([^\.\n]+?)\s*[\.\n]`) and carries backtick guards and a `(?!:)` guard.

Quote: `KIND = re.compile(r"Kind:\s*\**\s*([^\n*]+)")`

Remedy: use the C++ value pattern, which stops at `.`, and add its guards.

Also reported as: "readability lane 47-tools-roadmap-surveys Medium Q1 tools/roadmap-import-verify.py:41"

### Row 459 — Medium — `PKGBUILD:40-48`

review-code, lane 39-dev-python-ci-packaging, dim 2. **Disposition:** queued under ANTS-6105.

`-DANTS_TESTS=ON` makes CMakeLists.txt:1610-1616 fall back to `FetchContent … GIT_REPOSITORY https://github.com/google/googletest.git` when no system GTest exists.

Quote: `makedepends=( 'cmake' 'ninja' 'layer-shell-qt' 'ripgrep' )`

Remedy: add `gtest`, `git` and a monospace font (e.g. `ttf-liberation`) to makedepends.

### Row 460 — Medium — `PKGBUILD:30`

review-code, lane 39-dev-python-ci-packaging, dim 2. **Disposition:** queued, ANTS-6630.

(optdepends) while `'layer-shell-qt'` is in makedepends CMakeLists.txt:968 is `target_link_libraries(ants_chrome_lib PRIVATE LayerShellQt::Interface)`, a direct link. The built binary therefore has a hard `NEEDED` entry for that library.

Quote: `:43`

Remedy: move `layer-shell-qt` to `depends`.

### Row 461 — Medium — `tools/migrate_expect_helper.py:221-254`

review-code, lane 39-dev-python-ci-packaging, dim 9. **Disposition:** queued under ANTS-6071.

Pattern A captures everything between the `if (g_failures) {…}` block and the next `return 0;` at the same indent (DOTALL, non-greedy). The replacement then throws that text away.

Quote: `(?P<tail>.*?)^(?P=indent)return\s+0;\s*\n`

Remedy: put `m.group("tail")` back before `return expect_finish();`, or refuse when the tail is not blank.

### Row 462 — Medium — `release.yml:84`

review-code, lane 39-dev-python-ci-packaging, dim 3. **Disposition:** queued, ANTS-6630.

`grep` matches per line. A `workflow_dispatch` tag of `v1.2.3\nversion=…` passes the guard on its first line.

Quote: `if ! printf '%s' "$REF" | grep -qE '^v[0-9]+\.[0-9]+\.[0-9]+$'; then`

Remedy: `[[ $REF =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]`.

### Row 463 — Low — `tools/ci_workflow.py:74-77`

review-code, lane 39-dev-python-ci-packaging, dim 2. **Disposition:** queued, ANTS-6630.

, with the comment "GitHub's own bash flags follow it". Those are the flags for an explicit `shell: bash`. ci.yml's steps set no shell, so GitHub runs them with `bash -e {0}`, without `pipefail`.

Quote: `BASH = ["bash", "--noprofile", "--norc", "-eo", "pipefail", "-c"]`

Remedy: use `["bash", "-e", "-c"]` for steps with no `shell:` key.

### Row 466 — Low — `tools/roadmap-corpus-survey.py:288`

review-code, lane 39-dev-python-ci-packaging, dim 7. **Disposition:** queued, ANTS-6630.

raises `ZeroDivisionError` when the roadmaps found hold no items.

Quote: `missing * 100.0 / n`

Also reported as: "readability lane 47-tools-roadmap-surveys Info correctness lead tools/roadmap-corpus-survey.py:288"

### Row 467 — Low — `tools/token-bench/bench.py:30-31,173`

review-code, lane 39-dev-python-ci-packaging, dim 2. **Disposition:** queued, ANTS-6630.

"a run that edited the test cannot pass". `git checkout <start> -- tests` restores tracked files but leaves new untracked ones, such as a `conftest.py` the run added under `tests/`. Fix: also run `git clean -fdx tests`.

Quote: `git checkout <start> -- tests`

Remedy: also run `git clean -fdx tests`.

### Row 1294 — Info — `src/vtparser.cpp:375`

review-readability, lane 01-vt-parser-pty, correctness lead. **Disposition:** queued, ANTS-6658.

A trailing empty parameter or sub-parameter is dropped at the final byte (see the Q5 finding). This may be intended to mean "default".

### Row 1295 — Info — `src/terminalgrid.cpp:2128-2167`

review-readability, lane 02-terminal-grid, correctness lead. **Disposition:** queued under ANTS-5218.

- `src/terminalgrid.cpp:2128-2167` (plus `insertLines` and `deleteLines`) — Unlike `scrollUp`, these do not move `m_hyperlinkStartRow`. A link open across a scroll-down, insert-lines or delete-lines closes on the wrong rows.

### Row 1296 — Info — `src/terminalgrid.cpp:1045`

review-readability, lane 02-terminal-grid, correctness lead. **Disposition:** queued, ANTS-6653.

Reset also clears `m_scrollbackInsertPaused` while the user may still be scrolled up.

### Row 1297 — Info — `src/scrollbackexporter.cpp:224`

review-readability, lane 02-terminal-grid, correctness lead. **Disposition:** queued under ANTS-5822.

An HTML span opens only on a colour change. A bold, italic or underline change at the same colour keeps the first cell's classes. Combining characters are also dropped from HTML. This may be inherited from the old `exportAsHtml` (spec INV-2 requires identical bytes).

### Row 1298 — Info — `src/inlineimagedecode.cpp:61`

review-readability, lane 02-terminal-grid, correctness lead. **Disposition:** queued under ANTS-5823.

`keys->find("inline=1")` is a substring match, so `inline=10` or `xinline=1` also count as inline.

### Row 1299 — Info — `src/osc133signmain.cpp:60`

review-readability, lane 02-terminal-grid, correctness lead. **Disposition:** queued, ANTS-6653.

The signer drops one trailing newline from the key; the grid's verifier uses `$ANTS_OSC133_KEY` raw (`terminalgrid.cpp:174`). If the key itself ends in a newline, signatures will not match.

### Row 1300 — Info — `src/terminalwidget.cpp:897`

review-readability, lane 03-terminal-widget-a, correctness lead. **Disposition:** queued, ANTS-6654.

the minutes form prints `3m7s`, not the `3m07s` its comment shows (no zero padding).

### Row 1301 — Info — `src/terminalwidget.cpp:5642`

review-readability, lane 04-terminal-widget-b, correctness lead. **Disposition:** queued, ANTS-6654.

"Clear Scrollback" writes `ESC[3J ESC[H ESC[2J` through `ptyWrite`. That sends them as input to the shell (keystrokes), not to the screen.

### Row 1302 — Info — `src/terminalwidget.cpp:3287`

review-readability, lane 04-terminal-widget-b, correctness lead. **Disposition:** queued, ANTS-6654.

The HTML half of a rich copy drops combining marks and wide-character spacing, while the plain half keeps them.

### Row 1303 — Info — `src/terminalwidget.cpp:6233`

review-readability, lane 04-terminal-widget-b, correctness lead. **Disposition:** queued, ANTS-6654.

Per-style font families are dropped by any later `updateFontMetrics()` call (541–560; callers 128, 605, 6236).

### Row 1304 — Info — `src/terminalwidget.cpp:5730`

review-readability, lane 04-terminal-widget-b, correctness lead. **Disposition:** queued, ANTS-6654.

"Fold Output" is offered on the newest block, which paint (1480) never draws a fold indicator for. I did not read what paint does with a folded last block.

### Row 1305 — Info — `src/terminalwidget.cpp:5654`

review-readability, lane 04-terminal-widget-b, correctness lead. **Disposition:** queued, ANTS-6654.

"Search Web" is suppressed for short multi-line selections on wide panes.

### Row 1306 — Info — `src/mainwindow.cpp:3249-3264`

review-readability, lane 05-mainwindow-a, correctness lead. **Disposition:** queued, ANTS-6636.

In the snippets dialog, Edit, Delete and Insert treat the table's row number as an index into the full, unfiltered snippets list. When the search box filters the table, they act on the **wrong snippet**. After an edit or delete, `loadSnippets()` also reloads unfiltered while the search text is still showing.

### Row 1308 — Info — `src/mainwindow.cpp:3059-3073`

review-readability, lane 05-mainwindow-a, correctness lead. **Disposition:** queued, ANTS-6636.

The regex cache in `checkAutoProfileRules` is shared across every window, but it is keyed on each window's own `Config` generation number. With two windows open, they may clear each other's cache on every tick. I am unsure: I have not checked whether that generation number is per window.

### Row 1309 — Info — `src/mainwindow.cpp:2734-2739`

review-readability, lane 05-mainwindow-a, correctness lead. **Disposition:** queued, ANTS-6636.

The review probe's error handler hides the Review button without the check, added to the success handler for ANTS-5080, that the probe was for the tab now in front.

### Row 1310 — Info — `src/mainwindow.cpp:1119-1125`

review-readability, lane 05-mainwindow-a, correctness lead. **Disposition:** queued, ANTS-6636.

The system light/dark signal is connected only if auto colour scheme is on when the window is built. Turning it on later, in Preferences or in the file, has no effect until restart.

### Row 1311 — Info — `src/mainwindow.cpp:2893-3010`

review-readability, lane 05-mainwindow-a, correctness lead. **Disposition:** queued, ANTS-6636.

Editing `config.json` by hand does not apply `quake_mode`, `claude.mcp_enabled` or the plugin `window_config_reloaded` event. This is the same divergence as the first Q1 finding.

### Row 1312 — Info — `src/mainwindow_tabs.cpp:1227`

review-readability, lane 06-mainwindow-b, correctness lead. **Disposition:** queued under ANTS-5832.

`newTabForRemote` never calls `saveTabOrderOnly()`. ANTS-1159's spec (the `newTab()` and `newTabWith*` paths bullet) requires it for new-tab paths, so a tab made by rc/MCP/welcome-install is not persisted until the 30 s timer.

### Row 1313 — Info — `src/mainwindow_menus.cpp:1023`

review-readability, lane 06-mainwindow-b, correctness lead. **Disposition:** queued, ANTS-6636.

"Reload Plugins" dereferences `m_pluginManager` without a null check. The other uses in this file check it (973, 1001).

### Row 1314 — Info — `src/titlebar.cpp:38`

review-readability, lane 07-chrome-widgets, correctness lead. **Disposition:** queued, ANTS-6625.

The title label is transparent to mouse events, so the full-title tooltip that ElidedLabel sets when it truncates probably never shows.

### Row 1315 — Info — `src/toggleswitch.cpp:79`

review-readability, lane 07-chrome-widgets, correctness lead. **Disposition:** queued, ANTS-6625.

`nextCheckState` calls the base version, which calls `setChecked`, which calls `checkStateSet`, which already animates and emits. The override then does both again, so each click sends two accessibility events and restarts the animation.

### Row 1316 — Info — `src/coloredtabbar.cpp:118`

review-readability, lane 07-chrome-widgets, correctness lead. **Disposition:** queued, ANTS-6625.

For a user theme whose `bgSecondary` luminance is about 0.05–0.1, even black fails 3:1, so `best` stays 0 and the function returns a black dot that fails the floor.

### Row 1317 — Info — `src/selfupdate.cpp:113`

review-readability, lane 08-dialog-chrome-theme, correctness lead. **Disposition:** queued under ANTS-5845.

`"1.2.3-rc1"` vs `"1.2.3"`: `"3-rc1"` isn't a number, so text comparison ranks it *above* `"3"`, and a release candidate counts as newer than its release.

### Row 1318 — Info — `src/selfupdate.cpp:85-91`

review-readability, lane 08-dialog-chrome-theme, correctness lead. **Disposition:** queued under ANTS-5844.

`namesDir` checks only where a match ends, not where it starts. So `$APPDIR` matched inside a longer path (e.g. `/x/tmp/.mount_A` for APPDIR `/tmp/.mount_A`) counts as a hit.

### Row 1319 — Info — `src/themes.cpp:357-374`

review-readability, lane 08-dialog-chrome-theme, correctness lead. **Disposition:** queued, ANTS-6633.

A valid JSON file whose top level isn't an object (e.g. an array) is moved aside as corrupt, with the error text "no error occurred".

### Row 1320 — Info — `src/reviewdialogbase.cpp:433`

review-readability, lane 08-dialog-chrome-theme, correctness lead. **Disposition:** queued, ANTS-6633.

`QMessageBox::warning` opens a modal dialog. `aboutdialogs.cpp:26-33` says that pattern loses clicks on Wayland (QTBUG-79126).

### Row 1321 — Info — `src/auditdialog.cpp:3487`

review-readability, lane 09-audit-dialog, correctness lead. **Disposition:** queued under ANTS-5855.

The results row prints `file:0` when `line == 0` (`>= 0`); so does `auditdialog_export.cpp:646`. Elsewhere the code tests `> 0`.

### Row 1322 — Info — `src/auditdialog_triage.cpp:408`

review-readability, lane 09-audit-dialog, correctness lead. **Disposition:** queued, ANTS-6621.

If AI is turned off while batches are queued, each batch returns silently, with no status line.

### Row 1323 — Info — `src/auditdialog_triage.cpp:436`

review-readability, lane 09-audit-dialog, correctness lead. **Disposition:** queued, ANTS-6621.

The batch path swaps out 4-backtick runs "as the single-finding path does". But the batch snippet goes inside a JSON string with no fence, so the defence it copies is not there.

### Row 1324 — Info — `src/auditdialog.cpp:115`

review-readability, lane 09-audit-dialog, correctness lead. **Disposition:** queued, ANTS-6621.

`"TODO:", "FIXME:"` in the common noise list are applied to `todo_scan` too, so the TODO scanner drops every `TODO:` line. This is already filed as ROADMAP ANTS-5848.

### Row 1325 — Info — `src/auditrunner.cpp:1614-1631`

review-readability, lane 10-audit-engine-a, correctness lead. **Disposition:** queued, ANTS-6622.

- `src/auditrunner.cpp:1614-1631`: `measureEnvelopeBytes` leaves out `paths_given`, `scanned_whole_project` and `no_files`, which the real reply carries, so the 18 KiB / 20 KiB caps measure too few bytes.

### Row 1326 — Info — `src/auditrunner.cpp:993-995`

review-readability, lane 10-audit-engine-a, correctness lead. **Disposition:** queued, ANTS-6622.

- `src/auditrunner.cpp:993-995`: a JSON tool's raw count is capped at 10 000 but its suppressed count is not, so `afterFilterCount = rawCount - suppressedCount` (2400) can come out too low, or below zero, for a tool past 10 000.

### Row 1327 — Info — `src/auditrulequality.cpp:260-264`

review-readability, lane 10-audit-engine-a, correctness lead. **Disposition:** queued, ANTS-6622.

- `src/auditrulequality.cpp:260-264`: `reload()` does not clear `m_dirty`, so after a reload the destructor can rewrite the file from what it just reloaded.

### Row 1328 — Info — `src/auditscope.cpp:35`

review-readability, lane 11-audit-engine-b, correctness lead. **Disposition:** queued, ANTS-6622.

- `src/auditscope.cpp:35` and `src/auditscope.cpp:54` — `runGitRaw` and `runGitSucceeds` trust `exitCode()` without checking `exitStatus()`. If git crashes, its exit code is not meaningful, so `cat-file -e` or `rev-parse --verify` could be read as success.

### Row 1330 — Info — `src/falseposledger.cpp:301`

review-readability, lane 11-audit-engine-b, correctness lead. **Disposition:** queued, ANTS-6622.

The "(truncated …)" line is added after the size check, and the preamble is never checked, so the output can exceed `maxBlockBytes`.

### Row 1331 — Info — `src/auditengine.cpp:1231`

review-readability, lane 11-audit-engine-b, correctness lead. **Disposition:** queued, ANTS-6622.

The clang-tidy summary drops every finding whose file path contains a space, because the trailer filter keys on spaces.

### Row 1332 — Info — `src/auditengine.cpp:1038`

review-readability, lane 11-audit-engine-b, correctness lead. **Disposition:** queued, ANTS-6622.

`chop(6)` assumes the path ends in `.sarif`.

### Row 1333 — Info — `src/debtsweepengine.cpp:1574`

review-readability, lane 11-audit-engine-b, correctness lead. **Disposition:** queued, ANTS-6622.

- `src/debtsweepengine.cpp:1574` and `src/indiereviewengine.cpp:1478` — These fold-in templates paste message, title and file text into ROADMAP markdown without escaping. The audit template escapes (ANTS-2003).

### Row 1334 — Info — `src/auditcache.h:99`

review-readability, lane 11-audit-engine-b, correctness lead. **Disposition:** queued, ANTS-6622.

"7-char short" is not guaranteed: `git rev-parse --short` grows past 7 characters in large repositories.

### Row 1335 — Info — `src/roadmapstore.cpp:2280`

review-readability, lane 12-roadmap-store-a, correctness lead. **Disposition:** queued, ANTS-6649.

- `src/roadmapstore.cpp:2280` (and 2257): deregistered projects resolve through `projectIdForRoot`, `projectIdForSlug` and `projectIdContaining`. The callers in `roadmapmigrateload.cpp:1285` and `remotecontrol_changelog.cpp:40` may act on a deregistered project as if it were live.

### Row 1336 — Info — `src/roadmapstore.cpp:2880`

review-readability, lane 12-roadmap-store-a, correctness lead. **Disposition:** queued, ANTS-6649.

- `src/roadmapstore.cpp:2880`: `setLegend`, `reassignItemId`, `setSection*`, `updateSection` and `setFeedbackExportSha` report success for an id that does not exist.

### Row 1337 — Info — `src/roadmapstore.cpp:2956-2970`

review-readability, lane 12-roadmap-store-a, correctness lead. **Disposition:** queued, ANTS-6649.

- `src/roadmapstore.cpp:2956-2970`: `markIdParsed` keeps its SELECT query `cur` active while it runs the UPDATE, so outside a caller's transaction the read becomes a write mid-statement. That is the stale-snapshot hazard ANTS-5499 describes. `setItemField` and `clearItemField` also read then write with no ImmediateTx when no transaction is open.

### Row 1338 — Info — `src/roadmapstore.cpp:2943`

review-readability, lane 12-roadmap-store-a, correctness lead. **Disposition:** queued, ANTS-6649.

- `src/roadmapstore.cpp:2943`: `reassignItemId` changes `id` but leaves `provenance.id` unchanged, while `markIdParsed` sets it to `asserted`. It may be deliberate.

### Row 1339 — Info — `src/roadmapstore.cpp:3896-3900`

review-readability, lane 12-roadmap-store-a, correctness lead. **Disposition:** queued, ANTS-6649.

- `src/roadmapstore.cpp:3896-3900`: a legal prefix such as `ANTS-2` (the grammar at `.h`/`main.cpp` allows `-` and digits) holds ids like `ANTS-2-0001`. Those match `ANTS-[0-9]*`, the CAST reads them as 2, and so they feed `ANTS`'s floor.

### Row 1340 — Info — `src/roadmapstore.cpp:2146-2151`

review-readability, lane 12-roadmap-store-a, correctness lead. **Disposition:** queued, ANTS-6649.

- `src/roadmapstore.cpp:2146-2151`: when the section or the item does not exist, `fileItem` reports "section X is not in project -1".

### Row 1341 — Info — `src/roadmapmigrateload.cpp:363`

review-readability, lane 13-roadmap-store-b, correctness lead. **Disposition:** queued, ANTS-6649.

a SQL failure looking up a missing parent section sets `err`, which line 366 then clears. The section is silently filed at top level.

### Row 1342 — Info — `src/roadmapmigrateload.cpp:212`

review-readability, lane 13-roadmap-store-b, correctness lead. **Disposition:** queued, ANTS-6649.

- `src/roadmapmigrateload.cpp:212` (also 202 and 1364) — notes raised by the database load leave `Note::sourceIndex` at its default 0, which roadmapmigrate.h:153–158 defines as "the live roadmap". The verb sends it out unchanged (roadmapmigrateverb.cpp:71), so every load note claims `source_index:0`.

### Row 1343 — Info — `src/roadmapmigrateload.cpp:495`

review-readability, lane 13-roadmap-store-b, correctness lead. **Disposition:** queued, ANTS-6649.

the duplicate-id refusal says "Neither is an id-shaped token" after testing only the second item's `idOrigin`.

### Row 1344 — Info — `src/roadmapmigrateverb.cpp:820`

review-readability, lane 13-roadmap-store-b, correctness lead. **Disposition:** queued, ANTS-6649.

a refusal reports `store_backed:true` for an already-registered github-task-list project, while the success path reports false for the same project (see the Q3 finding).

### Row 1345 — Info — `src/knowhowstore.cpp:285`

review-readability, lane 13-roadmap-store-b, correctness lead. **Disposition:** queued, ANTS-6649.

the first 8 words are taken before 1-character words are dropped, so a query starting with short words can lose its real terms.

### Row 1346 — Info — `src/roadmapmigrateverb.cpp:1045`

review-readability, lane 13-roadmap-store-b, correctness lead. **Disposition:** queued, ANTS-6649.

`loadInOpenTransaction` returns the raw discovery code (`not_found`), where `run()` maps it to `no_roadmap` (step 3). I did not check whether the convert's caller maps it.

### Row 1348 — Info — `src/passheadingwrite.cpp:284-290`

review-readability, lane 14-roadmap-parse-render-a, correctness lead. **Disposition:** queued, ANTS-6648.

On a Status line holding only `**`, the fallback writes a glyph in place of the `**`. That contradicts the decoration rule at 177-178.

### Row 1350 — Info — `src/roadmapparse.h:472-477`

review-readability, lane 14-roadmap-parse-render-a, correctness lead. **Disposition:** queued, ANTS-6648.

- `src/roadmapparse.h:472-477` and `src/roadmapparse.cpp:2438` — Dropped items now render 🚫 and parse into records. The "dropped exclusion" these comments credit to ANTS-3793 § 2.1.2 may no longer hold for the store read path.

### Row 1351 — Info — `src/roadmapexport.cpp:1193-1198`

review-readability, lane 15-roadmap-parse-render-b, correctness lead. **Disposition:** queued, ANTS-6648.

`readProjectByRoot` and `readProjectBySlug` both write into `err`, and neither result's error is checked. A failed query reads as "absent", and the import goes on to `rebuildProject`, where the UNIQUE constraints are the only remaining guard.

### Row 1352 — Info — `src/roadmapfoldin.cpp:915`

review-readability, lane 15-roadmap-parse-render-b, correctness lead. **Disposition:** queued, ANTS-6648.

`idPrefixFor(...).value_or(QString())` discards a query failure, and its error text in `err` is then overwritten by later calls. A failed read silently falls back to sniffing the file.

### Row 1353 — Info — `src/roadmapdialog.cpp:1500`

review-readability, lane 16-roadmap-dialog, correctness lead. **Disposition:** queued under ANTS-5885.

- `src/roadmapdialog.cpp:1500` and `:3538` — The user-visible strings "⚠ Duplicate roadmap IDs" and "Could not read this roadmap." are not wrapped in `tr()`, so they cannot be translated.

### Row 1354 — Info — `src/claudeintegration_terminal.cpp:1569`

review-readability, lane 17-claude-integration-a, correctness lead. **Disposition:** queued, ANTS-6626.

`get_text` reads `max_bytes` but does not declare it. So the `ignored_args` notice will report a working argument as dropped, the state the comment at terminal:1121–1125 calls "the worse of both states".

### Row 1355 — Info — `src/claudeintegration.cpp:2785`

review-readability, lane 17-claude-integration-a, correctness lead. **Disposition:** queued, ANTS-6626.

On verbs that are neither always-loaded nor lean, a `caller_cwd` built by `makeCallerCwdReadProp` is shortened to "Your $PWD (required); picks the project." On `get_text`, `recent_errors` and `last_selection`, `caller_cwd` is not required (a `tab` index is accepted instead), so the shortened text loses that and is wrong.

### Row 1356 — Info — `src/mcptoolregistry.cpp:1040`

review-readability, lane 17-claude-integration-a, correctness lead. **Disposition:** queued under ANTS-5888.

`if (!rc())` calls the getter without checking it is set, unlike `rcDelegate`'s `rc ? rc() : nullptr`. Line 1086 calls `rc()` again on the worker thread with no null check.

### Row 1357 — Info — `src/claudeintegration_callpolicy.cpp:240`

review-readability, lane 17-claude-integration-a, correctness lead. **Disposition:** queued, ANTS-6626.

The async poll's done reply lacks the "this tool was handed no files" evidence that ANTS-4371 calls the most consequential result. Already filed as ANTS-6221.

### Row 1358 — Info — `src/claudeintegration_terminal.cpp:283`

review-readability, lane 17-claude-integration-a, correctness lead. **Disposition:** queued, ANTS-6626.

roadmap_query's `detail` says "Query the active tab's ROADMAP.md", but the tool requires `caller_cwd`. Line 214 puts a changing count in the text Claude reads ("9 of the 24 projects on this machine").

### Row 1359 — Info — `src/claudeintegration_terminal.cpp:1557`

review-readability, lane 17-claude-integration-a, correctness lead. **Disposition:** queued, ANTS-6626.

`get_text`'s description presents `tab` and `caller_cwd` as optional, but with neither the call is refused (`tab_or_cwd_required`, claudeintegration.cpp:3078).

### Row 1360 — Info — `src/auditrunner.cpp:1621`

review-readability, lane 17-claude-integration-a, correctness lead. **Disposition:** queued, ANTS-6626.

`measureEnvelopeBytes` leaves out the ANTS-4371 per-tool fields, so it under-counts the real reply size. The same pattern is in `src/testauditengine.cpp:1590`: the brief's `byteCount` leaves out `ok` and `framework_context`.

### Row 1361 — Info — `src/claudeintegration_reports.cpp:922`

review-readability, lane 18-claude-integration-b, correctness lead. **Disposition:** queued, ANTS-6626.

`\\u2014` inside `QStringLiteral` produces the literal six characters `\u2014` on the wire, not an em dash. The same happens at orientation 1737, 1748 and 1802, while reports 507 and 509 use `\u2014` correctly.

### Row 1362 — Info — `src/claudeintegration_codenav.cpp:103`

review-readability, lane 18-claude-integration-b, correctness lead. **Disposition:** queued, ANTS-6626.

find_sources requires `topic` in its schema, but accepts `symbol` as an alias (line 88). A strict client would refuse a call that sends only `symbol`. The description also says caller_cwd is "Required", but it is not in `required[]`.

### Row 1363 — Info — `src/claudeintegration_orientation.cpp:2507`

review-readability, lane 18-claude-integration-b, correctness lead. **Disposition:** queued, ANTS-6626.

test_audit_partition and test_audit_brief (2545–2547) declare `caller_cwd`, `scope`, `dimensions`, `chunk_id` and `partition_token` with a type and no description. That is unlike every other descriptor in these files.

### Row 1364 — Info — `src/claudeintegration_sessions.cpp:1025,1093`

review-readability, lane 19-claude-integration-c, correctness lead. **Disposition:** queued, ANTS-6626.

the `/compact`, `/audit` and plan-mode detection walks the narrow first window, even when the state search had to widen to find any state record. A long bookkeeping tail can hide a fresh `/compact` or permission-mode record from these loops.

### Row 1365 — Info — `src/claudeintegration_callpolicy.cpp:893-898`

review-readability, lane 19-claude-integration-c, correctness lead. **Disposition:** queued, ANTS-6626.

with a cap of 0, the empty-bucket branch accepts the first call of each window.

### Row 1366 — Info — `src/claudeintegration_callpolicy.cpp:912-915`

review-readability, lane 19-claude-integration-c, correctness lead. **Disposition:** queued, ANTS-6626.

the two-argument `setRateLimitCapsForTest` leaves any earlier BriefAssembly override in place, so a test can inherit another test's cap.

### Row 1367 — Info — `src/claudeintegration_callpolicy.cpp:276-277`

review-readability, lane 19-claude-integration-c, correctness lead. **Disposition:** queued, ANTS-6626.

the async poll reply always sends `incomplete_tools`, even empty, while the sync reply leaves it out when empty. A caller that checks whether the key is present reads the two differently.

### Row 1368 — Info — `src/claudestatuswidgets.cpp:2537-2548`

review-readability, lane 20-claude-session-widgets-a, correctness lead. **Disposition:** queued, ANTS-6627.

For up to 60 s, a multiplier cached on a mechanical tick (1.5) is returned unchanged on later non-mechanical ticks, where `conservatismDwellMultiplier` would give 2.0.

### Row 1369 — Info — `src/claudestatuswidgets.cpp:944-974`

review-readability, lane 20-claude-session-widgets-a, correctness lead. **Disposition:** queued, ANTS-6627.

`resetForTabSwitch` clears the model-state cache but not `m_modelStateLastPendingTier`. Probably harmless, because path and mtime are also cleared.

### Row 1370 — Info — `src/claudebgtasks.cpp:455-491`

review-readability, lane 21-claude-session-widgets-b, correctness lead. **Disposition:** queued, ANTS-6627.

The completion fallback runs on *any* tool_result whose event-level `toolUseResult.status` is completed, killed or failed. That plausibly includes a foreground Agent call's result. If that result's text mentions a running background id as a whole word, the scan marks that task finished.

### Row 1371 — Info — `src/claudetabtracker.cpp:126`

review-readability, lane 21-claude-session-widgets-b, correctness lead. **Disposition:** queued, ANTS-6627.

- `src/claudetabtracker.cpp:126` + `177-184` — The `-1` sentinel makes the next `detectClaudeChild` clear the injected transcript path, the opposite of its stated intent. It also forces `scanOrphans` on every poll for that entry.

### Row 1373 — Info — `src/claudeallowlist.cpp:169`

review-readability, lane 21-claude-session-widgets-b, correctness lead. **Disposition:** queued, ANTS-6627.

`m_validationLabel->clear()` empties the label but does not hide it, so a reopened dialog can show an empty hint row.

### Row 1374 — Info — `src/claudebgtasksdialog.cpp:71-73`

review-readability, lane 21-claude-session-widgets-b, correctness lead. **Disposition:** queued under ANTS-5908.

`tailPathSafe` accepts only `/tmp/claude-$UID/`. An Agent task's `outputPath` comes from `toolUseResult.outputFile` (`claudebgtasks.cpp:443`). If that file lives elsewhere, agent output never shows.

### Row 1375 — Info — `src/mcpdforwarder.cpp:106`

review-readability, lane 22-mcp-transport, correctness lead. **Disposition:** queued under ANTS-5912.

After the request is sent (`st->sent`), an oversized reply, a disconnect or a socket error still reports `no_terminal` with the hint "start Ants Terminal". The terminal took the request and may already have done a write. Only the timeout path makes the ANTS-4845 distinction.

### Row 1376 — Info — `src/remotecontrol.cpp:740`

review-readability, lane 22-mcp-transport, correctness lead. **Disposition:** queued, ANTS-6643.

`rlResolveCounterPrefix` calls `rlDetectCounterPrefix(markdown)` without the project's declared ID format, even though it holds `callerCanonical`. That is the case remotecontrol_internal.h:553-556 warns about (INV-13).

### Row 1377 — Info — `src/remotecontrol.cpp:285`

review-readability, lane 22-mcp-transport, correctness lead. **Disposition:** queued, ANTS-6643.

`findChangelogUnder` ignores `ProjectSettings::repoRootOf`. An outer folder that declares `repo_root` reads the clone's roadmap but searches its own folder for the changelog.

### Row 1379 — Info — `src/remotecontrol.cpp:772`

review-readability, lane 22-mcp-transport, correctness lead. **Disposition:** queued, ANTS-6643.

`rlMaxExistingIdForPrefix` doesn't match `PREFIX-S0042` IDs, which the ants-v1 walker accepts. This may be deliberate under ANTS-4500; the code doesn't say.

### Row 1380 — Info — `src/remotecontrol_roadmap_log.cpp:1049`

review-readability, lane 23-mcp-roadmap-query-log-a, correctness lead. **Disposition:** queued, ANTS-6641.

the `id_taken` message names ROADMAP.md, but the floor may come from CHANGELOG, `docs/roadmap` or the store

### Row 1381 — Info — `src/remotecontrol_roadmap_log.cpp:2097`

review-readability, lane 23-mcp-roadmap-query-log-a, correctness lead. **Disposition:** queued, ANTS-6641.

file-path flip dry runs omit `note_scrubbed_params` and `post_bullets`; the store path (1598, 1621) emits both on a dry run

### Row 1382 — Info — `src/remotecontrol_roadmap_log.cpp:2626`

review-readability, lane 23-mcp-roadmap-query-log-a, correctness lead. **Disposition:** queued, ANTS-6641.

the GFM flip reply has no `format` key, while ants-v1 (2105) and the store (1510) set one

### Row 1383 — Info — `src/remotecontrol_roadmap_log.cpp:2252`

review-readability, lane 23-mcp-roadmap-query-log-a, correctness lead. **Disposition:** queued, ANTS-6641.

plain `std::sort` makes tied suggestions come back in no fixed order; the store ranker uses `stable_sort` (1277)

### Row 1384 — Info — `src/remotecontrol_roadmap_log.cpp:3211`

review-readability, lane 23-mcp-roadmap-query-log-a, correctness lead. **Disposition:** queued, ANTS-6641.

amend_body's file-path not-found and ambiguous refusals carry no `suggestions`, while flip's and amend_body's own store path do

### Row 1385 — Info — `src/remotecontrol_roadmap_log.cpp:2049`

review-readability, lane 23-mcp-roadmap-query-log-a, correctness lead. **Disposition:** queued, ANTS-6641.

this store block may be unreachable after the 1957 dispatch, except where `detectRoadmapFormat` and `walkGfmBullets` disagree; I am unsure

### Row 1386 — Info — `src/remotecontrol_roadmap_log.cpp:3099`

review-readability, lane 23-mcp-roadmap-query-log-a, correctness lead. **Disposition:** queued, ANTS-6641.

the pass locate goes through `flipPassStatus(..., "planned")`. If that refuses a no-op flip, a planned pass could not be located for amend

### Row 1387 — Info — `src/remotecontrol_roadmap_log.cpp:3839`

review-readability, lane 23-mcp-roadmap-query-log-a, correctness lead. **Disposition:** queued, ANTS-6641.

amend_field_batch entries are refused with `op:"amend_field"` in the message

### Row 1389 — Info — `src/remotecontrol_roadmap_query_verb.cpp:1900`

review-readability, lane 24-mcp-roadmap-query-log-b, correctness lead. **Disposition:** queued under ANTS-5926.

The default end of the shipped-date window reads `QDate::currentDate()`, bypassing the `RoadmapClock` test seam that the report path uses.

### Row 1390 — Info — `src/remotecontrol_roadmap_query.cpp:1011`

review-readability, lane 24-mcp-roadmap-query-log-b, correctness lead. **Disposition:** queued, ANTS-6641.

The markdown fallback reads `idFormatFor(projectRoot)` rather than the resolved `root`. The code's own comment names this as ANTS-5923. The same subdirectory miss appears at verb 3209 and 3234, which pass `callerCanonical` unresolved.

### Row 1391 — Info — `src/remotecontrol_roadmap_query_verb.cpp:4317`

review-readability, lane 24-mcp-roadmap-query-log-b, correctness lead. **Disposition:** queued, ANTS-6641.

An unknown `op` is refused with code `bad_op_combo`, but `docs/standards/mcp-errors.md:32` defines `bad_op` for an op outside the allowed set.

### Row 1392 — Info — `src/remotecontrol_roadmap_log_batch.cpp:3089-3127`

review-readability, lane 25-mcp-roadmap-batch, correctness lead. **Disposition:** queued, ANTS-6640.

append_batch's database route never gives the ANTS-4989 review-provenance warning; single append gives it on both routes.

### Row 1393 — Info — `src/remotecontrol_roadmap_log_batch.cpp:3258-3267`

review-readability, lane 25-mcp-roadmap-batch, correctness lead. **Disposition:** queued, ANTS-6640.

the file route's evidence advisory loops over every request bullet, skipped ones included, so it can warn about bullets that were never filed.

### Row 1394 — Info — `src/remotecontrol_roadmap_log_batch.cpp:797-800`

review-readability, lane 25-mcp-roadmap-batch, correctness lead. **Disposition:** queued, ANTS-6640.

the database-route sort uses `firstLine`, which holds a negated key for targets found in the database, so `flipped[]` is not in file order. That contradicts the comment at 794.

### Row 1395 — Info — `src/remotecontrol_roadmap_log_batch.cpp:1712-1717`

review-readability, lane 25-mcp-roadmap-batch, correctness lead. **Disposition:** queued, ANTS-6640.

- `src/remotecontrol_roadmap_log_batch.cpp:1712-1717` and `:2213-2220` — the real create_section and bundle_row file-route replies have no `op`, while their dry-run and database replies do.

### Row 1396 — Info — `src/remotecontrol_roadmap_log_batch.cpp:2138-2150`

review-readability, lane 25-mcp-roadmap-batch, correctness lead. **Disposition:** queued, ANTS-6640.

sorting a bundle_row table whose rows have no trailing `|` drops the last cell, so sorting on the last column compares empty strings.

### Row 1397 — Info — `src/remotecontrol_roadmap_log_batch.cpp:1576-1584`

review-readability, lane 25-mcp-roadmap-batch, correctness lead. **Disposition:** queued, ANTS-6640.

create_section's database route counts sections from every roadmap file after the anchor when stepping past deeper sections. move_section and release stop at another file. I could not tell whether positions are unique per file.

### Row 1398 — Info — `src/remotecontrol_workspace.cpp:996`

review-readability, lane 26-mcp-state-workspace-a, correctness lead. **Disposition:** queued under ANTS-5937.

- `src/remotecontrol_workspace.cpp:996`: with `files_only` and `matches_only` both set, the line handler skips the distinct-string harvest, but the matches_only reply still returns. The result is `matches:[]` and `distinct_count:0`, beside a non-zero `count`.

### Row 1399 — Info — `src/remotecontrol_workspace.cpp:3892`

review-readability, lane 26-mcp-state-workspace-a, correctness lead. **Disposition:** queued, ANTS-6642.

- `src/remotecontrol_workspace.cpp:3892`: redirecting to the configured `claude.mcp_feedback_root` skips the ANTS-3439 normalised-stem match. A `Fin_Break` checkout would derive a new `Fin_Break_…` file next to an existing `finbreak_…` one.

### Row 1400 — Info — `src/remotecontrol_workspace.cpp:1803`

review-readability, lane 26-mcp-state-workspace-a, correctness lead. **Disposition:** queued, ANTS-6642.

- `src/remotecontrol_workspace.cpp:1803`: with a `filter` and a negative `max_symbols` (unclamped at 1885), `while (kept.size() > maxSymbols) kept.removeLast()` runs until `removeLast` is called on an empty array.

### Row 1401 — Info — `src/remotecontrol_workspace.cpp:2743`

review-readability, lane 26-mcp-state-workspace-a, correctness lead. **Disposition:** queued, ANTS-6642.

- `src/remotecontrol_workspace.cpp:2743`: when the default debug log is not a regular file, the error names `rawPath`, which is empty there.

### Row 1403 — Info — `src/remotecontrol_state.cpp:815`

review-readability, lane 27-mcp-state-workspace-b, correctness lead. **Disposition:** queued, ANTS-6642.

A report that fails to parse returns `parse_failed`, but spec ANTS-1254 INV-10 and its edge-case table say an empty `runs[]` should return `not_audited`.

### Row 1404 — Info — `src/remotecontrol_state.cpp:2171`

review-readability, lane 27-mcp-state-workspace-b, correctness lead. **Disposition:** queued, ANTS-6642.

`sqLastLoopDate` finds the first "loop log" anywhere in the body, not a heading. A sentence earlier in the body that mentions a loop log moves the window, and dates in the following prose count as review dates.

### Row 1405 — Info — `src/remotecontrol_state.cpp:1935`

review-readability, lane 27-mcp-state-workspace-b, correctness lead. **Disposition:** queued under ANTS-5940.

The suggestion's `next_step` always says "index these source_roots", even when only `specs_dir`, `roadmap` or `changelog` are suggested (the ANTS-3353 case).

### Row 1406 — Info — `src/remotecontrol_state.cpp:4004`

review-readability, lane 27-mcp-state-workspace-b, correctness lead. **Disposition:** queued, ANTS-6642.

`find_caller` has no `walk_unreached` or `walk_stopped_in` on a capped walk. `CallResult` has no such fields, unlike `DefResult`, so the ANTS-5478 "an empty result is not 'not here'" signal is missing for callers.

### Row 1407 — Info — `src/remotecontrol_state.cpp:4237-4246`

review-readability, lane 27-mcp-state-workspace-b, correctness lead. **Disposition:** queued, ANTS-6642.

`similar_code`'s body extraction accepts a `symbol_ambiguous` result, which `sqAttachBody` (3932) rejects. It may return the wrong overload's body.

### Row 1408 — Info — `src/remotecontrol_state.cpp:2967`

review-readability, lane 27-mcp-state-workspace-b, correctness lead. **Disposition:** queued, ANTS-6642.

`task_priors` gives no id boost for non-ANTS project ids (see the Q5 finding). The spec agrees with the code, but its row 6 still says it scans `ANTS-*.md`, while the code (3119) scans every `*.md`.

### Row 1409 — Info — `src/remotecontrol_state.cpp:547`

review-readability, lane 27-mcp-state-workspace-b, correctness lead. **Disposition:** queued, ANTS-6642.

With `top_n:0` the cached summary has no top findings, so `scope` is always `"broad"` and the narrow-run warning can never fire.

### Row 1410 — Info — `src/remotecontrol_state.cpp:3804`

review-readability, lane 27-mcp-state-workspace-b, correctness lead. **Disposition:** queued, ANTS-6642.

The ctest `unrecognised_output` refusal has no `output_tail`, while the other runners include one (also under Q1).

### Row 1411 — Info — `src/remotecontrol_review.cpp:802`

review-readability, lane 28-mcp-review-verbs, correctness lead. **Disposition:** queued, ANTS-6639.

A lane name taken from a derived (computed) `indie_review_partition` reply is refused by `indie_review_dispatch` as unknown, because dispatch never computes the fallback.

### Row 1412 — Info — `src/remotecontrol_review.cpp:264`

review-readability, lane 28-mcp-review-verbs, correctness lead. **Disposition:** queued, ANTS-6639.

`partition_source` reports `"module_map"` whenever `mapLaneCount > 0`. But `IndieReviewEngine::derivePartition` returns the `.indie-review/partition.json` override lanes first (indiereviewengine.cpp:721–725), so a one-lane override is reported as coming from the module map.

### Row 1413 — Info — `src/remotecontrol_review.cpp:465`

review-readability, lane 28-mcp-review-verbs, correctness lead. **Disposition:** queued, ANTS-6639.

The `reports_dir` path flags truncation by file size in bytes, while the inline path uses UTF-16 length. Files with many non-ASCII characters may be over-flagged.

### Row 1414 — Info — `src/remotecontrol_review.cpp:1032`

review-readability, lane 28-mcp-review-verbs, correctness lead. **Disposition:** queued, ANTS-6639.

Git reports an untracked directory as one `?? dir/` entry. Its path does not end in a source suffix, so `.cpp` files inside a new untracked directory never reach `addedSources`, and the orphaned-source warning misses them.

### Row 1415 — Info — `src/remotecontrol_coldeyes.cpp:745`

review-readability, lane 28-mcp-review-verbs, correctness lead. **Disposition:** queued, ANTS-6639.

`project_layout` says `caller_cwd "…" is not a directory` even when the path does not exist, unlike `session_memory`, which tells the two apart.

### Row 1416 — Info — `src/remotecontrol_docs.cpp:2290-2299`

review-readability, lane 29-mcp-content-verbs-a, correctness lead. **Disposition:** queued, ANTS-6637.

`doc_lint` never sets `opts.spec.siblingInvNumbers`. On a project that numbers invariants across all its specs, `doc_lint` reports gaps in the numbering that `spec_lint` hides (and counts in `id_gaps_suppressed`).

### Row 1417 — Info — `src/dependencyinventory.cpp:245-249`

review-readability, lane 29-mcp-content-verbs-a, correctness lead. **Disposition:** queued, ANTS-6637.

The `CMakeLists.txt` branch calls `scanByRegex` twice, so `readLines` runs twice and `files_read` counts that one file twice.

### Row 1418 — Info — `src/versioncheck.cpp:80-121`

review-readability, lane 29-mcp-content-verbs-a, correctness lead. **Disposition:** queued, ANTS-6637.

`report()` overwrites the row's `status` each time it runs. A file whose pattern matches twice records `pattern_ambiguous`, which a drifted version on the same file then overwrites. `files[].status` shows only the last finding.

### Row 1419 — Info — `src/remotecontrol_terminal.cpp:1162-1165`

review-readability, lane 29-mcp-content-verbs-a, correctness lead. **Disposition:** queued, ANTS-6637.

A failed PNG save in `grab-image` refuses with code `bad_args`, although the caller's arguments passed every check.

### Row 1420 — Info — `src/remotecontrol_changelog.cpp:1189-1197`

review-readability, lane 30-mcp-content-verbs-b, correctness lead. **Disposition:** queued, ANTS-6637.

add_subsection scrubs into `bscrub` and `scrubbed` and never reports either; resolveClBatchEntry (1555) also discards its list. Dropped names are silent on these paths.

### Row 1421 — Info — `src/remotecontrol_changelog.cpp:1131`

review-readability, lane 30-mcp-content-verbs-b, correctness lead. **Disposition:** queued, ANTS-6637.

`release`'s `bytes_written` comes from a disk re-stat after the write, and there is no `file_bytes`.

### Row 1422 — Info — `src/runtraceverb.cpp:557-559`

review-readability, lane 30-mcp-content-verbs-b, correctness lead. **Disposition:** queued, ANTS-6637.

If an index has the header but no `|---|` separator row, `lastRow == headerAt`. The new row then goes in at `headerAt+1`, but `rowFor` starts scanning at `headerAt+2`. So the row is never found again, `already_recorded` never fires, and the table has no separator.

### Row 1423 — Info — `src/remotecontrol_run_trace.cpp:97`

review-readability, lane 30-mcp-content-verbs-b, correctness lead. **Disposition:** queued, ANTS-6637.

- `src/remotecontrol_run_trace.cpp:97` and `src/runtraceverb.cpp:638-645` — A `session_id` of `agent-…` resolves to `<folder>/agent-….jsonl`, but session_cost looks under `<session>/subagents/` (sessioncostverb.cpp:258-264). run_trace cannot name a helper agent's transcript by id.

### Row 1424 — Info — `src/remotecontrol_session_message.cpp:146-150`

review-readability, lane 30-mcp-content-verbs-b, correctness lead. **Disposition:** queued, ANTS-6637.

The `unknown_project` message has no `session_message:` prefix on the preview and outbox paths.

### Row 1426 — Info — `src/docintegrity.cpp:875`

review-readability, lane 31-doc-engines, correctness lead. **Disposition:** queued, ANTS-6634.

Lines are split on `\n` with no `\r` chop (docfacts, docsymbols and doccitations all chop). On a CRLF document the `^<!-- MIRROR END -->$` marker never matches, and `trailHashRe` cannot strip a `##\r` tail.

### Row 1427 — Info — `src/doccitations.cpp:1120`

review-readability, lane 31-doc-engines, correctness lead. **Disposition:** queued, ANTS-6634.

`paragraphAttribution` walks up through table rows the quotation window treats as a separate block, so a prose quotation can be attributed from a table cell above it.

### Row 1428 — Info — `src/docfacts.cpp:294`

review-readability, lane 31-doc-engines, correctness lead. **Disposition:** queued under ANTS-5958 (also settled ANTS-6135).

- `src/docfacts.cpp:294` with `src/docintegrity.cpp:835` — Under `doc_lint`, one over-long list produces both a `count_mismatch` and a `list_count` finding for the same line.

### Row 1430 — Info — `src/specconformance.cpp:175`

review-readability, lane 32-spec-engines, correctness lead. **Disposition:** queued, ANTS-6652.

`taggedFences` counts `regex` fences with no engine or an unsupported one. A spec whose only fences are ```` ```regex python ```` reports `executable_fences > 0`, and the "checked nothing" hint never fires.

### Row 1431 — Info — `src/speclog.cpp:503`

review-readability, lane 32-spec-engines, correctness lead. **Disposition:** queued, ANTS-6652.

`createLoopLog` refuses `unrecognised_format` when the only table in the file is inside a fenced example.

### Row 1433 — Info — `src/debtsweepengine.cpp:434`

review-readability, lane 33-review-engines, correctness lead. **Disposition:** queued under ANTS-5979.

the message says `Q_UNUSED(x)` even when the marker found was `(void)x;`.

### Row 1434 — Info — `src/indiereviewengine.cpp:985`

review-readability, lane 33-review-engines, correctness lead. **Disposition:** queued, ANTS-6646.

`buildBasenameIndex` does not exclude git-ignored directories. A same-named file in an ignored tree makes a basename ambiguous and drops real citations.

### Row 1435 — Info — `src/indiereviewengine.cpp:904`

review-readability, lane 33-review-engines, correctness lead. **Disposition:** queued, ANTS-6646.

an absolute `sourcePaths` entry is joined onto `projectPath` and silently dropped, the case ANTS-1731 fixed in `BriefDispatch`. The override parser drops absolute paths (150), so this may not be reachable.

### Row 1436 — Info — `src/testauditengine.cpp:1936`

review-readability, lane 34-test-audit-verify, correctness lead. **Disposition:** queued, ANTS-6655.

hybrid mode `limit:-1` returns 3 chunks; the header says -1 means every chunk.

### Row 1437 — Info — `src/mutationjournal.cpp:53`

review-readability, lane 34-test-audit-verify, correctness lead. **Disposition:** queued, ANTS-6655.

the `setPermissions` result is ignored. The secureio policy says project content must be refused when it cannot be made owner-only.

### Row 1438 — Info — `src/verifyengine.cpp:359-360`

review-readability, lane 34-test-audit-verify, correctness lead. **Disposition:** queued, ANTS-6655.

`parseCtest` reads only the last `maxLogLines` lines (default 50). A long "The following tests FAILED" list is cut silently.

### Row 1439 — Info — `src/findsources.cpp:110`

review-readability, lane 35-code-index-search-a, correctness lead. **Disposition:** queued, ANTS-6628.

`.c` files are never scanned, so find_sources returns nothing for a plain C project.

### Row 1440 — Info — `src/findsources.cpp:167`

review-readability, lane 35-code-index-search-a, correctness lead. **Disposition:** queued, ANTS-6628.

files whose names start with `build` or `.` are skipped.

### Row 1441 — Info — `src/fileoutline.cpp:966`

review-readability, lane 35-code-index-search-a, correctness lead. **Disposition:** queued, ANTS-6628.

- `src/fileoutline.cpp:966` and `:986` — rxCppNameArgs and rxCppTypeOnly end with `\s*$` and are run on the raw line, so a trailing comment defeats them. This is the same defect ANTS-3738 fixed for three sibling patterns.

### Row 1442 — Info — `src/trace/languages.cpp:160`

review-readability, lane 35-code-index-search-a, correctness lead. **Disposition:** queued, ANTS-6628.

`#!/usr/bin/env -S bash` gives interpreter `-S`, so the file gets no language.

### Row 1443 — Info — `src/readregion.cpp:313`

review-readability, lane 35-code-index-search-a, correctness lead. **Disposition:** queued, ANTS-6628.

the end of a symbol's range is taken from the next symbol in list order. fileoutline.cpp:1251-1254 warns that typedef structs are added to that list out of line order.

### Row 1444 — Info — `src/commentclaims.cpp:490`

review-readability, lane 36-code-index-search-b, correctness lead. **Disposition:** queued, ANTS-6628.

`taken.insert(capturedStart(0) + 1)` assumes the digit sits one character after `~`. The `approx` regex allows `~\s?`, so "~ 5 files" puts the digit at +2, and the `plural` pass reports the same number a second time. The `+1` also has no comment (a Q4/Q5 gap)

### Row 1445 — Info — `src/indiereviewengine.cpp:62-76`

review-readability, lane 36-code-index-search-b, correctness lead. **Disposition:** queued, ANTS-6628.

`sourcePathsForLane` skips generated files by `moc_`/`ui_`/`qrc_` prefix only, so `*.spv.h` and the other suffix-shaped generated files reach a lane. This is the gap ANTS-4096 fixed in codewalk

### Row 1446 — Info — `src/trace/extract.cpp:66-67`

review-readability, lane 36-code-index-search-b, correctness lead. **Disposition:** queued, ANTS-6628.

- `src/trace/extract.cpp:66-67` and `src/trace/trace.cpp:161-162` — the second `chop` drops a complete trailing multi-byte character, not only a partial one. Signatures and `call_text` lose one character more than the 200-byte cap needs

### Row 1447 — Info — `src/sourcescrapes.cpp:988`

review-readability, lane 36-code-index-search-b, correctness lead. **Disposition:** queued, ANTS-6628.

`it.value().first()` on a `pathDefinitions` value. If that can be an empty list, this is undefined behaviour

### Row 1448 — Info — `src/mcpspill.cpp:427`

review-readability, lane 37-mcp-infra, correctness lead. **Disposition:** queued, ANTS-6638.

The `bytes` value in `rows_preview` is the size of the `{"v":row}` wrapper, so each row is reported 6 bytes too large. Tests only check lower bounds.

### Row 1449 — Info — `src/mcporientation.cpp:339-345`

review-readability, lane 37-mcp-infra, correctness lead. **Disposition:** queued under ANTS-6006.

Uninstalling against a settings.json that has no `hooks` key adds `"hooks":{"SessionStart":[]}`. The file then differs from the original, so it is rewritten, yet the function reports `Kept`.

### Row 1450 — Info — `src/modelnearmissledger.cpp:269-272`

review-readability, lane 38-model-switching, correctness lead. **Disposition:** queued, ANTS-6644.

`excluded_pre_epoch_count` counts pre-epoch records from **all** projects, even in project scope. `statsForScope` (modelswitchledger.cpp:612-617) counts only in-scope records.

### Row 1451 — Info — `src/aidialog.cpp:114-121`

review-readability, lane 38-model-switching, correctness lead. **Disposition:** queued under ANTS-6014.

the confirmation dialog says "byte(s)" for counts that are UTF-16 characters (`cmd.size()`, `stripped`), so a multi-byte payload is under-reported.

### Row 1452 — Info — `src/aidialog.cpp:371`

review-readability, lane 38-model-switching, correctness lead. **Disposition:** queued under ANTS-6015.

fence languages of 10 or more characters (`powershell`, `typescript`, `javascript`) are not stripped and become the first line of the inserted command.

### Row 1453 — Info — `src/modelnearmissledger.cpp:182-191`

review-readability, lane 38-model-switching, correctness lead. **Disposition:** queued, ANTS-6644.

a tie between two tokens not in `taxonomyOrder` is settled by QHash iteration order, which varies between runs.

### Row 1454 — Info — `src/config.cpp:687`

review-readability, lane 39-config-session-project, correctness lead. **Disposition:** queued, ANTS-6629.

`setClaudeMcpFeedbackRoot` takes a directory path but skips the `acceptablePathValue` gate (length cap, NUL rejection) that the other six path setters use.

### Row 1455 — Info — `src/luaengine.cpp:351-353`

review-readability, lane 40-plugins-lua, correctness lead. **Disposition:** queued, ANTS-6645.

`ants.get_output` needs no permission, yet the `Output`/`Line` events need `terminal.read` (`pluginmanager.cpp:691-693`). That would open the moment output is fed to plugins. According to the comment at `pluginmanager.cpp:711`, `setRecentOutput` has no caller yet.

### Row 1456 — Info — `src/pluginmanager.cpp:307`

review-readability, lane 40-plugins-lua, correctness lead. **Disposition:** queued, ANTS-6645.

`dispatchTo` skips the permission check. Not live with today's callers (`PaletteAction`, `Keybinding`).

### Row 1457 — Info — `src/pluginmanager.cpp:220`

review-readability, lane 40-plugins-lua, correctness lead. **Disposition:** queued, ANTS-6645.

`QObject::disconnect(engine, nullptr, this, nullptr)` runs before the `if (engine)` guard at line 227, which implies `engine` can be null there. Qt warns on a null sender.

### Row 1458 — Info — `src/changeloglog.cpp:541-548`

review-readability, lane 41-changelog-feedback, correctness lead. **Disposition:** queued, ANTS-6624.

A newly created category heading at the section end gets no blank line before it, so it can sit directly under the previous bullet. The dated merge guards against this at line 775.

### Row 1459 — Info — `src/changelogquery.cpp:506`

review-readability, lane 41-changelog-feedback, correctness lead. **Disposition:** queued, ANTS-6624.

An unterminated fence masks every later line for `releaseLint` (and `lint`), because `fenceMask` never closes it at `## [`. Citations after it are missed, which could produce false `shipped_not_cited` findings.

### Row 1460 — Info — `src/main.cpp:438`

review-readability, lane 42-app-entry-dialogs, correctness lead. **Disposition:** queued, ANTS-6620.

`while (!in.atEnd())` on a stdin pipe may stop before a slow writer has sent anything. The ants-helper reads until `read()` comes back empty instead. I have not confirmed this.

### Row 1461 — Info — `src/sshdialog.cpp:176`

review-readability, lane 42-app-entry-dialogs, correctness lead. **Disposition:** queued, ANTS-6620.

`sanitizeExtraArgs` passes through tokens that are not options, and `toSshCommand` puts extra args before `--`. A bookmark whose extra args contain a bare `other.host` could make ssh connect there and treat the real host as the remote command. Depends on OpenSSH's parsing; I have not confirmed it.

### Row 1462 — Info — `src/settingsdialog.cpp:1087`

review-readability, lane 42-app-entry-dialogs, correctness lead. **Disposition:** queued, ANTS-6620.

`loadSettings` uses `m_config` with no null check, while the constructor guards against a null `m_config` (lines 46, 132, 134, 621).

### Row 1463 — Info — `src/buildcache.cpp:87-89`

review-readability, lane 43-diagnostics-logging, correctness lead. **Disposition:** queued, ANTS-6632.

A `note:` after a warning, or after a gap, counts toward `warningsCount`.

### Row 1464 — Info — `src/debuglog.cpp:200-202`

review-readability, lane 43-diagnostics-logging, correctness lead. **Disposition:** queued, ANTS-6632.

If the log is still over the cap after a reopen (rotation failed), every later write closes and reopens it.

### Row 1465 — Info — `src/tooldetectionengine.cpp:53-55`

review-readability, lane 43-diagnostics-logging, correctness lead. **Disposition:** queued, ANTS-6632.

A found tool never expires, so a tool uninstalled mid-session keeps being returned. The comment states this on purpose; listed only for completeness.

### Row 1466 — Info — `src/hostexec.h:41-45`

review-readability, lane 44-shared-utilities, correctness lead. **Disposition:** queued under ANTS-6043.

Only variables the caller added or changed are forwarded. A variable the caller removed from `env` is not unset on the host, though the comment says "forward what the caller changed".

### Row 1467 — Info — `src/terminalwidget.cpp:3317`

review-readability, lane 44-shared-utilities, correctness lead. **Disposition:** queued, ANTS-6650.

`QApplication::clipboard()->setMimeData(...)` bypasses the clipboardguard funnel, so the NUL strip in `sanitize` does not apply to this rich-text copy. The funnel's header names only `setText` sites.

### Row 1468 — Info — `CMakeLists.txt:305`

review-readability, lane 45-build-system, correctness lead. **Disposition:** queued, ANTS-6623.

If `ProcessorCount` cannot detect the core count it returns 0. The cap is then 0 (`compile_pool=0`), which Ninja treats as an unlimited pool: the out-of-memory case this cap exists to prevent.

### Row 1469 — Info — `CMakeLists.txt:2088`

review-readability, lane 45-build-system, correctness lead. **Disposition:** queued, ANTS-6623.

`bench_session_save` links only `ants_core_lib ants_vt_lib` in its group, while 1996–1998 says core alone "is not closed" and fails to link on CI's GCC 13 + mold.

### Row 1470 — Info — `CMakeLists.txt:2150`

review-readability, lane 45-build-system, correctness lead. **Disposition:** queued, ANTS-6623.

`ants_add_widget_bench` leaves `ants_roadmapstore_lib` out of the group. test_claude's comment (2677–2680) says a link-only store dependency lands outside the group, so this may fail under a single-pass linker.

### Row 1471 — Info — `CMakeLists.txt:2716`

review-readability, lane 45-build-system, correctness lead. **Disposition:** queued, ANTS-6623.

A test_claude source line is mis-indented (column 4). That is harmless to CMake, but could trip text scanners of this file.

### Row 1472 — Info — `tools/local-ci.sh:369`

review-readability, lane 46-tools-ci-gate, correctness lead. **Disposition:** queued under ANTS-6050.

the build-asan job runs in the foreground, so its TERM/INT trap fires only after the job returns. If only the shell is signalled, the marker is written late and the sanitizer build keeps running.

### Row 1473 — Info — `tools/perf-report.sh:209-220`

review-readability, lane 46-tools-ci-gate, correctness lead. **Disposition:** queued under ANTS-6057.

in `--json` mode regressions are never computed, so the exit is 0 on a regression, against the header's "1 a regression".

### Row 1474 — Info — `tools/local-ci.sh:271-289`

review-readability, lane 46-tools-ci-gate, correctness lead. **Disposition:** queued, ANTS-6656.

a tree carrying the interrupted marker still gets a CMake regeneration run (line 275) before the marker check skips it.

### Row 1475 — Info — `tools/check-roadmap.sh:69`

review-readability, lane 46-tools-ci-gate, correctness lead. **Disposition:** queued, ANTS-6656.

the `--allow` escaping misses `(`, `)`, `+`, `?`, `{` and `|`.

### Row 1476 — Info — `tools/roadmap-conformance.py:128`

review-readability, lane 47-tools-roadmap-surveys, correctness lead. **Disposition:** queued, ANTS-6657.

`if "**" not in rest` looks unreachable once `head` is not None, since BULLET has already consumed the opening `**`.

### Row 1477 — Info — `tools/spec-header-survey.py:256`

review-readability, lane 47-tools-roadmap-surveys, correctness lead. **Disposition:** queued, ANTS-6657.

`--values` given before the directory becomes the directory path (`sys.argv[1]`).

### Row 1478 — Info — `tools/split-motion-check.py:1037-1038`

review-readability, lane 47-tools-roadmap-surveys, correctness lead. **Disposition:** queued, ANTS-6657.

If `matching()` returns -1, the site text silently becomes empty.

### Row 1479 — Info — `tools/measure/doc_symbols_locator_sample.py:37`

review-readability, lane 47-tools-roadmap-surveys, correctness lead. **Disposition:** queued, ANTS-6657.

If `doc_line` lacks the symbol, `dl` is None and `dl-1` raises.

### Row 1480 — Info — `tools/token-bench/project/tally/csvio.py:30`

review-readability, lane 47-tools-roadmap-surveys, correctness lead. **Disposition:** queued under ANTS-6083.

A weighted participant listed twice is silently merged, while `split_evenly` raises on duplicates. This may be the bench's deliberately planted bug.

### Row 1481 — Info — `tools/token-bench/project/tally/report.py:12`

review-readability, lane 47-tools-roadmap-surveys, correctness lead. **Disposition:** queued under ANTS-6084.

The width defaults to 4 only when there are no people. Names shorter than "Name" knock the header out of line.

### Row 1482 — Info — `packaging/flatpak/make-flathub-manifest.sh:44`

review-readability, lane 48-hooks-packaging, correctness lead. **Disposition:** queued, ANTS-6635.

The unanchored version regex picks up the first three-part `VERSION` anywhere in CMakeLists.txt.

### Row 1483 — Info — `packaging/obs/obs-submit.sh:142`

review-readability, lane 48-hooks-packaging, correctness lead. **Disposition:** queued, ANTS-6635.

`echo` prints the literal `%%%%...`, so the hint tells the user to write four percent signs. Line 117 correctly says to double (`%%check`).

### Row 1484 — Info — `hooks/ants-bash-veto.sh:131`

review-readability, lane 48-hooks-packaging, correctness lead. **Disposition:** queued, ANTS-6635.

A `case` stops at its first match. A command containing `git diff --stat` anywhere (say in an echo) that also greps ROADMAP.md matches the diff arm, gets no reason (`git_diff_stat=0`), and never reaches the ROADMAP arm.

### Row 1485 — Info — `packaging/obs/obs-setup.sh:19`

review-readability, lane 48-hooks-packaging, correctness lead. **Disposition:** queued, ANTS-6635.

`USER` is reassigned while exported, so `osc` children inherit `USER=milnet`.

### Row 1486 — Info — `hooks/ants-session-preamble.sh:32`

review-readability, lane 48-hooks-packaging, correctness lead. **Disposition:** queued under ANTS-6097.

It strips invalid characters, where the writer rejects them, so a malformed id maps to a different, stripped filename. Low impact today.

## Readability

### Row 471 — High — `src/ptyhandler.cpp:367`

review-readability, lane 01-vt-parser-pty, Q5. **Disposition:** queued, ANTS-6694.

The `8` is an exact count with no spare room. It covers the fixed slots below it: flatpak-spawn, --host, the version arg, the directory arg, the MCP-socket arg, --, the shell and the null terminator. Nothing says so. ANTS-1900 already added one forwarded `--env=` argument (the MCP socket) here, so adding another is a likely change. A stranger who adds one without raising the 8 writes past the end…

Quote: `const char *argv[std::size(kTermOverrides) + 8];`

Remedy: say beside the 8 that it counts those fixed slots, or derive the size from them.

### Row 472 — Medium — `src/ptyhandler.cpp:163-435`

review-readability, lane 01-vt-parser-pty, Q2. **Disposition:** queued, ANTS-6694.

The thread is lost at line 271, `if (m_childPid == 0) {`. The buffers the child uses are built 60–190 lines earlier: - `flatpakDirArg` and `flatpakMcpSocketArg` at 176–206 - `termProgramVerArg` at 210 - `argv0` at 240 - `homeBytesPre` at 232 The child branch itself spans 135 lines and mixes signal reset, fd closing, chdir and two exec paths. A stranger adding a Flatpak argument must find and edit…

Quote: `bool Pty::start(const QString &shell, const QString &workDir, int rows, int cols) {`

Remedy: build the pre-fork launch data in one struct-returning helper, and move the signal reset and fd closing into their own helpers.

### Row 480 — Low — `src/vtparser.cpp:375`

review-readability, lane 01-vt-parser-pty, Q5. **Disposition:** queued, ANTS-6694.

An empty parameter before `;` or `:` is pushed as 0 (361, 380), but an empty last parameter is dropped. So `CSI ;2J` gives [0,2] while `CSI 2;J` gives [2], and `CSI 4:m` loses its sub-parameter. No comment gives the reason. A stranger writing a consumer that checks `params.size()` (as `VtStream::onParsedAction` does for ED) cannot tell whether this is deliberate.

Quote: `if (m_currentParam >= 0) pushParam(m_currentParam);`

Remedy: one line saying whether a dropped trailing empty parameter is meant to read as "default".

### Row 481 — Low — `src/vtparser.h:113`

review-readability, lane 01-vt-parser-pty, Q3. **Disposition:** queued, ANTS-6694.

"reArm" suggests it is only used to re-enter a string from a peek state. It is also the first-time decoder for the plain Escape state (vtparser.cpp:293) and the 8-bit C1 forms (:273). A stranger reading `if (!reArmStringIntroducer(ch)) dispatchEsc(ch);` in the Escape branch would not guess that this is where OSC/DCS/APC start. The header comment at 106–112 does correct it.

Quote: `bool reArmStringIntroducer(uint32_t ch);`

Remedy: a name that says "enter a string if `ch` introduces one".

### Row 484 — Medium — `src/terminalgrid.h:355`

review-readability, lane 02-terminal-grid, Q3. **Disposition:** queued, ANTS-6689.

The name says history insertion pauses, but insertion carries on; only the doubling-guard suppression is turned off. A caller would expect lines to be held back or dropped. The setter's own comment had to be corrected once already (ANTS-5076). - Fix: rename to say what it does, for example "user is scrolled back". - Cost: pinned. `docs/specs/ANTS-1118.md` and `tests/features/scroll_snapshot_inten…

Quote: `void setScrollbackInsertPaused(bool paused) { m_scrollbackInsertPaused = paused; }`

### Row 488 — Medium — `src/terminalgrid.cpp:2786`

review-readability, lane 02-terminal-grid, Q2. **Disposition:** queued, ANTS-6689.

`resize` handles seven jobs in one 432-line body: history reflow, live-screen reflow, saved-screen reflow, scroll regions, the free pool, links and tab stops. - A stranger loses the thread here, where four more helper blocks are defined inside an `if` after three earlier ones. - One gap is easy to miss: the live screen's link table is only resized (3083), never reflowed. The saved screen's gap is…

Quote: `if (cols != m_cols && !m_scrollback.empty()) {`

### Row 489 — Medium — `src/terminalgrid.cpp:1226`

review-readability, lane 02-terminal-grid, Q2. **Disposition:** queued, ANTS-6689.

`handleOsc` is one 410-line `if/else` chain. A stranger loses the thread here, in a 150-line prompt-marker parser buried mid-chain. - The OSC 52 (1148-1215) and OSC 1337 user-variable (1384-1421) bodies are also long. - OSC 8 has already been split out as `handleOsc8`; these were not. - The chain's order matters: OSC 9;4 must come before plain OSC 9. A stranger adding a branch must find that rule…

Quote: `else if (oscNum == "133" && semi != std::string::npos && semi + 1 < payload.size()) {`

### Row 491 — Medium — `src/terminalgrid.cpp:1314`

review-readability, lane 02-terminal-grid, Q4. **Disposition:** queued, ANTS-6689.

This fills a 10-field `PromptRegion` by position alone. - Nothing says which `0` or `false` is which. - A stranger adding or reordering a field in `PromptRegion` (`terminalgrid.h:159`) would shift every later value with no compiler error, because the int, bool and qint64 values all convert silently. - Fix: use named-field initialisers. - Cost: one edit — the edit is within one statement, and no A…

Quote: `m_promptRegions.push_back({globalLine, globalLine, false, 0, 0, false, 0, 0, -1,`

### Row 495 — Low — `src/terminalgrid.cpp:218`

review-readability, lane 02-terminal-grid, Q5. **Disposition:** queued, ANTS-6689.

A D marker with no exit code is signed as `D|<id>`. That differs from the documented `D|<promptId>|<exitCode>` form (193), and no comment says the bare form is accepted. - A stranger writing a signer could not tell what to sign when there is no exit code. - Fix: add the bare-D form to the message-form comment at 189-193. - Cost: one edit (comment only).

Quote: `if (marker == 'D' && !extra.empty()) {`

### Row 496 — Low — `src/scrollbackexporter.cpp:204`

review-readability, lane 02-terminal-grid, Q5. **Disposition:** queued, ANTS-6689.

"not the first line" is tested as `m_next > 0`. That only works because a text export always starts at line 0. - A stranger adding a ranged text export (starting past 0) would get a stray leading newline. - Fix: a comment, or a separate first-line flag. - Cost: one edit — the change stays inside `writeLine`.

Quote: `if (m_next > 0) ++m_pendingNewlines;`

### Row 497 — Low — `src/scrollbackexporter.cpp:101`

review-readability, lane 02-terminal-grid, Q5. **Disposition:** queued, ANTS-6689.

The fallback output time of 0.1 s, used when the block has no end time, is unexplained. A stranger cannot tell whether it is required by the asciicast format or by the old behaviour (the spec's INV byte-identity). - Fix: one comment naming why 0.1. - Cost: one edit (comment only).

Quote: `double castDurationSec = 0.1;`

### Row 510 — Medium — `src/terminalwidget.cpp:1109`

review-readability, lane 03-terminal-widget-a, Q2. **Disposition:** queued under ANTS-6179.

`paintEvent` runs 986 lines. A reader loses the thread in the per-cell loop (1109-1341). In one body it: - picks the colour by layered priority (1131-1167); - merges background fills, with a wide-character rule (1169-1203); - builds text runs (1208-1249); - draws five underline styles inline (1265-1335). Then thirteen separate overlay passes follow, each saving and restoring the font. Someone cha…

Quote: `for (int col = 0; col < cols; ++col) {`

Remedy: pull the cell-colour choice and the underline drawing into helpers.

### Row 511 — Low — `src/terminalwidget.cpp:2481`

review-readability, lane 03-terminal-widget-a, Q2. **Disposition:** queued, ANTS-6690.

In `onVtBatch`, the screen snapshot taken during "synchronized output" is captured at 2340-2354, but released and timed about 130 lines later (2481-2511). Session logging, recording, title and diagnostics sit in between. Someone changing that snapshot handling has to hold both ends in mind across unrelated code.

Quote: `// Synchronized output (DEC 2026): unified snapshot path post-1148.`

Remedy: keep the capture and release next to each other, or name the two halves as helpers.

### Row 512 — Low — `src/terminalwidget.h:112`

review-readability, lane 03-terminal-widget-a, Q3. **Disposition:** queued, ANTS-6690.

The name says it changes the window's see-through level. It sets only the transparency of the terminal area's background, and the header comment says the name is "historical". Callers in other files (MainWindow) see only the name.

Quote: `void setWindowOpacityLevel(double opacity);`

Remedy: rename to something like `setBackgroundOpacity`, and `m_windowOpacity` to match.

### Row 520 — Medium — `src/terminalwidget.cpp:4215`

review-readability, lane 04-terminal-widget-b, Q3. **Disposition:** queued, ANTS-6690.

`invalidateSpanCaches()` is a `const` method whose name promises cache work only. It also uses up the grid's per-row dirty flags. Calling it from a new place silently wipes those flags. That is the exact bug the ANTS-5078 comment at 6097–6102 records.

Quote: `m_grid->clearAllScreenDirty();`

Remedy: rename it, or say in its declaration that it uses up the grid's screen dirty flags.

### Row 522 — Medium — `src/terminalwidget.cpp:5654`

review-readability, lane 04-terminal-widget-b, Q5. **Disposition:** queued, ANTS-6690.

Nothing says why the limit is 800 cells, or how it relates to the 200-character limit on the next line. In practice, a two-line selection on a 500-column pane (1,000 cells) never offers "Search Web", however short its text. A stranger can't tell whether 800 is tuned or arbitrary.

Quote: `QString sel = selCellBound <= 800 ? selectedText().trimmed() : QString();`

Remedy: a named constant, with a comment tying it to the 200-character limit.

### Row 523 — Medium — `src/terminalwidget.cpp:3532`

review-readability, lane 04-terminal-widget-b, Q3. **Disposition:** queued, ANTS-6690.

Cell positions are kept as `QPoint` with `x()` holding the line and `y()` the column. This holds for `pixelToCell`, `m_selStart`/`m_selEnd`, `m_lastClickCell` and `m_lastMouseCell`. A stranger writing new mouse code will read `cell.x()` as the column. The header declaration (`terminalwidget.h:439`) has no comment, and nothing else warns them.

Quote: `return QPoint(globalLine, col);`

Remedy: a one-line comment on the `pixelToCell` declaration and the selection members, or a named struct.

### Row 529 — Low — `src/stickycommandtext.h:27`

review-readability, lane 04-terminal-widget-b, Q5. **Disposition:** queued, ANTS-6690.

The `+ 1` allows for the trailing separator that the later trim removes. Nothing says so. Someone "simplifying" it to `> maxChars` would lose the ellipsis when reading stops right after a separator.

Quote: `if (cmdText.length() > maxChars + 1) break;`

Remedy: a short comment on the `+ 1`.

### Row 530 — Low — `src/terminalwidget.cpp:3354`

review-readability, lane 04-terminal-widget-b, Q5. **Disposition:** queued, ANTS-6690.

Nothing explains the 200-arrow cap on click-to-move. A stranger can't tell whether it is a safety limit or a bug.

Quote: `for (int i = 0; i < count && i < 200; ++i)`

Remedy: a named constant, with its reason.

### Row 531 — Low — `src/terminalwidget.cpp:4019`

review-readability, lane 04-terminal-widget-b, Q3. **Disposition:** queued, ANTS-6690.

The `a11y` prefix says screen-reader-only, but clipboard copy (`selectedText`, 3985) depends on it too. A screen-reader tweak would change what gets copied. The comment at 4017 does mention this, so the risk is low.

Quote: `QString TerminalWidget::a11yComposeCols(int globalLine, int beginCol, int endCol) const {`

Remedy: a neutral name such as `composeCols`.

### Row 535 — Medium — `src/mainwindow.cpp:970`

review-readability, lane 05-mainwindow-a, Q2. **Disposition:** queued under ANTS-6184.

The constructor runs 968 lines. A reader loses the thread here, where a 128-line focus-redirect lambda sits inline. It follows a 150-line plugin-setup block (492–642) that includes a whole permission dialog. The order rules that matter are scattered between those blocks: - `setupStatusBarChrome` must run before `newTab`. - `RemoteControl` must be built after the status-bar chrome (ANTS-5090). - T…

Quote: `connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {`

### Row 536 — Medium — `src/mainwindow.cpp:1663`

review-readability, lane 05-mainwindow-a, Q3. **Disposition:** queued, ANTS-6673.

The name says "status-bar widgets", but the function mostly does other work. It creates `ClaudeIntegration`, `ClaudeTabTracker`, the peer-usage file watcher and the MCP providers. Meanwhile the main status-bar widgets (branch, message, process) are built in the constructor *after* it runs (725–799). - **Wrong edit 1:** someone looking for where `ClaudeIntegration` is created will not look here. -…

Quote: `void MainWindow::setupStatusBarChrome() {`

### Row 547 — Medium — `src/mainwindow_mcp.cpp:136`

review-readability, lane 06-mainwindow-b, Q2. **Disposition:** queued, ANTS-6673.

`setupClaudeMcpProviders` is 386 lines. It mixes startup config, socket setup and hook setup with eight inline tool handlers. `get_scrollback` alone is a 105-line lambda containing two more lambdas. The reader loses the thread at line 179, where the `since_cursor` mode starts after a complete early-return path for the other mode.

Quote: `m_claudeIntegration->registerToolProvider("get_scrollback",`

Remedy: move each inline provider into its own named function and leave registration as a list. Cost: pinned. These anchors pin it: `registerToolProvider("get_scrollback"` (mcp_get_scrollback_cap:47, mcp_prompt_injection_hint:115, mcp_secret_redaction:59), `void MainWindow::setupClaudeMcpProviders()` (5 si…

### Row 549 — Low — `src/mainwindow_menus.cpp:938`

review-readability, lane 06-mainwindow-b, Q2. **Disposition:** queued, ANTS-6673.

The whole "apply Preferences" logic (theme, fonts, quake, tab list, plugins, MCP switch) is a 52-line lambda inside a menu builder. Someone looking for what happens on Apply would not look in `setupSettingsMenu`.

Quote: `connect(m_settingsDialog, &SettingsDialog::settingsChanged, this, [this]() {`

Remedy: move it into a named slot such as `onSettingsApplied`. Cost: pinned. Anchor `&SettingsDialog::settingsChanged, this, [this]()` (tab_list_panel:274).

### Row 563 — Low — `src/mainwindow_menus.cpp:121`

review-readability, lane 06-mainwindow-b, Q5. **Disposition:** queued, ANTS-6673.

Alt+9 jumps to the last tab, not tab 9. The comment at 116 says "jump to tab by index".

Quote: `int idx = (i == 9) ? m_tabWidget->count() - 1 : i - 1;`

Remedy: note the browser convention in the comment. Cost: one edit (comment only).

### Row 572 — Low — `src/coloredtabbar.cpp:403`

review-readability, lane 07-chrome-widgets, Q3. **Disposition:** queued, ANTS-6664.

`m_closeNormal` and `m_closeHover` are named for the × glyph, but they also serve as the tab name's resting and selected text colours, which set how strong the wash may be. A stranger giving the × its own hover colour would silently change every wash. Comments at 399-401 and in the header (182-184) do warn, hence LOW.

Quote: `const QColor selected = m_closeHover.isValid() ? m_closeHover : fallbackText;`

Remedy: name them for the text colours they are.

### Row 582 — Low — `src/selfupdate.cpp:113-116`

review-readability, lane 08-dialog-chrome-theme, Q5. **Disposition:** queued under ANTS-5845.

When a version part isn't a number, the code falls back to comparing text. The header contract (`selfupdate.h:25`, `"X.Y.Z"; a missing part is 0`) doesn't mention this, and no comment explains it. A stranger adding pre-release tags such as `-rc1` won't know how they will be ordered.

Quote: `const int c = QString::compare(as, bs);`

Remedy: document the text fallback in the header, or reject non-numeric parts.

### Row 585 — Low — `src/themedstylesheet.cpp:208-213`

review-readability, lane 08-dialog-chrome-theme, Q2. **Disposition:** queued, ANTS-6670.

The only key to what `%1`–`%6` mean sits at the end of a 180-line string. Someone editing a rule near line 125 has to scroll down to learn that `%2` is `bgSecondary` and `%5` is `accent`. A wrong placeholder still compiles and shows as a wrong colour.

Quote: `).arg(theme.bgPrimary.name(), // %1`

Remedy: put the `%1`–`%6` key at the top of the string.

### Row 588 — Medium — `src/auditdialog.h:182`

review-readability, lane 09-audit-dialog, Q3. **Disposition:** queued under ANTS-6194.

This sits beside the static `visibleSinceBaseline` (line 176), and the two names are the same words reordered. Only the member version stands down while `m_recentScopeError` is set. A stranger calling the static one from a new list would hide every finding that has a file whenever git failed. That is the ANTS-5084 bug again.

Quote: `bool sinceBaselineVisible(const Finding &f) const;`

Remedy: rename the member so it reads as "the filter as applied this run" (for example `passesSinceBaselineFilter`).

### Row 589 — Medium — `src/auditdialog.cpp:3245`

review-readability, lane 09-audit-dialog, Q2. **Disposition:** queued under ANTS-6195.

`renderResults` (510 lines) is where a stranger loses the thread. The name says "render", but it also: - changes the findings it shows (`highConfidence`, snippets, confidence) - queues git blame - reads trend.json and writes it, behind `m_snapshotPersisted` and `m_showNewOnly` checks It runs on every keystroke and pill click. Someone adding a re-render, or a filter mode, has no reason to expect t…

Quote: `const TrendSnapshot prev = loadLastSnapshot();`

Remedy: move the enrichment and the snapshot write into `finishRun`, so render only draws.

### Row 591 — Medium — `src/auditdialog_catalogue.cpp:771`

review-readability, lane 09-audit-dialog, Q3. **Disposition:** queued under ANTS-6305.

The OutputFilter and AuditCheck values are passed by position with no labels. The OutputFilter order is dropIfContains, dropIfMatches, keepOnlyIfContains, maxLines. The JSON schema's order is different (drop_if_contains, keep_only_if_contains, drop_if_matches). A `0` in the last slot means "uncapped, and exempt from the per-check cap" (auditdialog.cpp:3057). The trailing `true, true, nullptr` (au…

Quote: `CheckType::Info, Severity::Info, { {}, "", {}, 0 },`

Remedy: designated initialisers, or a `/*name*/` label on every slot as some rows already do.

### Row 604 — Low — `src/auditdialog.h:656`

review-readability, lane 09-audit-dialog, Q3. **Disposition:** queued, ANTS-6660.

It returns the Claude hand-off: up to six project docs, a five-phase workflow prompt and the findings. It is not a plain dump. A stranger reusing it for something like "copy as text" would ship all of that.

Quote: `QString plainTextResults() const;`

Remedy: rename it to say it is the review hand-off.

### Row 608 — Medium — `src/auditrunner.cpp:993`

review-readability, lane 10-audit-engine-a, Q5. **Disposition:** queued, ANTS-6661.

The raw count is capped at `kSarifFindingsMax` for JSON tools only. The line-based branch (`out.rawCount = located;`, 1106) is not capped, and nothing says why. That number is a count of findings, not of SARIF results. A stranger cannot tell which branch is right, so cannot safely make them agree.

Quote: `// Honour SARIF cap.`

Remedy: say in the comment why the JSON count is capped, or apply the same rule in both branches.

### Row 609 — Low — `src/auditrunner.cpp:914`

review-readability, lane 10-audit-engine-a, Q4. **Disposition:** queued, ANTS-6661.

This value is always overwritten at 992 (`arr.size() - emptyEntries`) before anything reads it. A stranger editing the raw count here sees no effect.

Quote: `out.rawCount = arr.size();`

Remedy: delete the line, or say why it stays.

### Row 610 — Medium — `src/auditrunner.cpp:2199`

review-readability, lane 10-audit-engine-a, Q2. **Disposition:** queued under ANTS-6200.

`runAudit` is 943 lines long. Its section banners make most of it followable, but the reader loses the thread in the scope-resolution block (2167–2267). That block is a bare `{}` scope holding a lambda, then an `else` whose 66-line body sits at the outer indentation and ends with two `}` in the same column (2265–2266). A stranger cannot see that the since-last-run and narrowing logic lives inside…

Quote: `} else {`

Remedy: move scope resolution into its own function returning the per-tool paths and the narrowed / since-last-run state.

### Row 621 — Low — `src/auditcache.cpp:324`

review-readability, lane 11-audit-engine-b, Q2. **Disposition:** queued, ANTS-6661.

`recordRun` (263 lines) does five jobs. A newcomer loses the thread here: a 22-line history comment, then a lambda declared inside a loop.

Quote: `// Reaper (INV-4): delete sarif/html named in dropped entries — only`

Remedy: move the reaper into its own function, called after the commit.

### Row 625 — Medium — `src/auditengine.cpp:829`

review-readability, lane 11-audit-engine-b, Q5. **Disposition:** queued, ANTS-6661.

Severity "error" maps to CRITICAL here, for foreign SARIF. The three native parsers map it to MAJOR (cppcheck 1133–1135, clang-tidy 1235, semgrep 1313–1315). No comment says why. A newcomer tidying this into one rule would silently change the severities `last_audit_summary` reports.

Quote: `if (level == QStringLiteral("error")) return QStringLiteral("CRITICAL");`

Remedy: a comment at `severityFromLevel` giving the reason for the difference.

### Row 627 — Medium — `src/auditengine.h:379`

review-readability, lane 11-audit-engine-b, Q3. **Disposition:** queued, ANTS-6661.

This field holds the XML, text or JSON path for the three non-SARIF formats too. Only `summariseCppcheckXml`'s doc admits the name is wrong. A newcomer reading a consumer would assume it points at a SARIF file, for example when deriving an html sibling.

Quote: `QString sarifPath;`

Remedy: rename it to `sourcePath`.

### Row 628 — Medium — `src/audithygiene.cpp:310`

review-readability, lane 11-audit-engine-b, Q2. **Disposition:** queued, ANTS-6661.

The lexer states are bare numbers 0–6, defined only by the legend at 237–239. `Syntax`, two lines further down, is an enum. Every `case 4:`, `state = 5` and `state = 6` sends the reader back to the legend. The code check at 283–299 also runs before the state changes, which is the only reason quote characters are excluded there, and that is not stated.

Quote: `switch (state) {`

Remedy: an `enum class` for the lexer state, plus one line saying the code check runs on the state before the transition.

### Row 629 — Low — `src/falseposledger.cpp:83`

review-readability, lane 11-audit-engine-b, Q3. **Disposition:** queued, ANTS-6661.

It strips every C0 control character and DEL, not just line breaks. At the call sites (140–142) a newcomer would think only CR/LF are removed. The comment on the definition corrects this.

Quote: `QString stripLineBreaks(const QString &in) {`

Remedy: rename it to `stripControlChars`.

### Row 630 — Low — `src/falseposledger.cpp:300`

review-readability, lane 11-audit-engine-b, Q5. **Disposition:** queued, ANTS-6661.

The entry-count cap drops the oldest entries (237–241). The byte cap does the opposite: it keeps the oldest in the window and drops every newer entry once the limit is hit. Nothing says so. A newcomer would expect the newest rejections to survive either way.

Quote: `if (outBytes + blockBytes > opts.maxBlockBytes) {`

Remedy: state the asymmetry, or trim from the oldest end for the byte cap as well.

### Row 633 — Low — `src/auditengine.cpp:1035`

review-readability, lane 11-audit-engine-b, Q2. **Disposition:** queued, ANTS-6661.

The last third of `summariseSarif` (165 lines) is a separate job. Its fallback matches only the GUI's `audit-YYYYMMDD-HHmmss` names, never AuditCache's `audit-<iso>-<sha>` names. "Fallback (a) failed" refers to a spec label that appears nowhere in the code. A newcomer loses the thread here.

Quote: `// Spec § 3.1 step 8 — derive htmlPath.`

Remedy: move the html lookup into its own named function and name both naming schemes.

### Row 634 — Low — `src/auditengine.cpp:534`

review-readability, lane 11-audit-engine-b, Q2. **Disposition:** queued, ANTS-6661.

Inside `applyFilter`'s line loop, the file-cache fill (539–555) is nested seven levels deep, and it holds its own size cap and file reading. A newcomer loses the filter logic here.

Quote: `if (hasContextFilter) {`

Remedy: move "load file lines for context, cached and capped" into a named helper.

### Row 635 — Low — `src/auditdelta.cpp:80`

review-readability, lane 11-audit-engine-b, Q5. **Disposition:** queued, ANTS-6661.

A negative `maxEntries` means "no cap". The header (auditdelta.h:39–44) does not say so.

Quote: `if (maxEntries < 0 || merged.size() <= maxEntries) return false;`

Remedy: one line in the header.

### Row 640 — Medium — `src/roadmapstore.cpp:766`

review-readability, lane 12-roadmap-store-a, Q2. **Disposition:** queued, ANTS-6686.

`createSchema` does four jobs in one body: 1. a read-only fast path 2. the creation race under BEGIN IMMEDIATE 3. sending older stores to `applyUpgrades` 4. creating the schema from scratch Between job 3 and the end of job 4 sit a 50-line rationale (717-765) and a 180-line SQL literal (766-947). A stranger loses the thread at 717: the control flow they were following, with its ROLLBACK and COMMIT…

Quote: `const QString ddl[] = {`

Remedy: move `ddl[]` and its "frozen schema text" rationale to a file-scope constant beside `kMessageTableDdl`, so `createSchema` keeps only its control flow.

### Row 642 — Low — `src/roadmapstore.cpp:2815`

review-readability, lane 12-roadmap-store-a, Q5. **Disposition:** queued, ANTS-6686.

The literal 5 must equal the number of `?` placeholders in the query above it. Nothing says so. The two sibling cascades (2222 and 2793) count the `?` instead. Adding one more EXISTS clause without changing the 5 makes every call return nullopt.

Quote: `for (int i = 0; i < 5; ++i)`

Remedy: count the placeholders, as `deleteItem` does.

### Row 643 — Low — `src/roadmapstore.cpp:1582`

review-readability, lane 12-roadmap-store-a, Q5. **Disposition:** queued, ANTS-6686.

A Dependencies id is handled differently depending on which project's prefix it carries: - Another project's prefix with no item: stored as a cross-project row. - The item's own prefix with no item: sent to `unconverted`. No comment and no line of the header contract (`.h:438-443`) gives the reason. A stranger who "fixes" unconverted same-project ids by storing them as cross-project rows cannot t…

Quote: `else if (t && !t->itemPk && t->projectId != self->projectId)`

Remedy: one line saying why a same-project unresolved id is not stored.

### Row 646 — Medium — `src/roadmapmigrateverb.cpp:820`

review-readability, lane 13-roadmap-store-b, Q3. **Disposition:** queued, ANTS-6686.

The reply key `store_backed` means two different things: - On success (399–402) it means "this file's format is one the database serves". - On the two re-run refusals (838–856) it means "this folder has a project record that is not deregistered". Someone reading or extending either reply would assume the meaning is the same. A github-task-list project that is already registered gets `store_backed…

Quote: `e[QStringLiteral("store_backed")] = !ownerDeregistered;`

Remedy: give the refusal's field its own name (e.g. `registered`), or compute it with the same format test.

### Row 648 — Medium — `src/knowhowstore.cpp:432`

review-readability, lane 13-roadmap-store-b, Q5. **Disposition:** queued, ANTS-6686.

`run()` sends every op it does not recognise to `opPurge`, the destructive op. The op set is kept by hand in three places: - `isOp` / `isWriteOp` (395–403) - this dispatch - the schema enum (the test at test_know_how_verb.cpp:253 checks only the schema) Someone adding an op to `isOp` / `isWriteOp` and forgetting `run()` gets purge behaviour. The header's "`op` must satisfy isOp()" does not warn o…

Quote: `return opPurge(args, author);`

Remedy: route `purge` explicitly and make the fall-through a refusal.

### Row 649 — Medium — `src/knowhowstore.cpp:352`

review-readability, lane 13-roadmap-store-b, Q5. **Disposition:** queued, ANTS-6686.

`Row` is filled by position, but the SELECT order (…, revision, retired_at) differs from `Row`'s member order (…, retiredAt, revision). So the column numbers are swapped here, with no comment. Someone adding a column to the SELECT or a member to `Row` shifts every later index, and the compiler cannot catch a misplaced string.

Quote: `q.value(7).toString(), q.value(6).toLongLong()};`

Remedy: assign the members by name, or make the SELECT order match `Row`.

### Row 650 — Medium — `src/knowhowstore.cpp:285`

review-readability, lane 13-roadmap-store-b, Q5. **Disposition:** queued, ANTS-6686.

The 8-word cap and the 2-character minimum (286) are bare numbers. Every other limit in this store is a named `kMax…` constant in the header. Someone tuning search cannot tell whether 8 is a promised limit (each word adds four SQL parameters) or a guess. They also cannot tell that the cap is applied before short words are dropped, so one-letter words use up slots.

Quote: `for (const QString &w : words.mid(0, 8)) {`

Remedy: named header constants with one line of reason, and an explicit choice of order.

### Row 651 — Medium — `src/roadmapmigrate.cpp:230`

review-readability, lane 13-roadmap-store-b, Q4. **Disposition:** queued, ANTS-6686.

The comment at 355–359 says the archive-misattribution bug (ANTS-4075) came from `sourceIndex` defaulting to 0, and says it "used to". It still does. I read all 21 `addNote` calls in the file and every one passes the argument, so the default's only effect is to let the next new call repeat the bug, and it will compile cleanly.

Quote: `int line, int sourceIndex = 0) {`

Remedy: remove the default.

### Row 654 — Medium — `src/roadmapmigrate.cpp:755`

review-readability, lane 13-roadmap-store-b, Q2. **Disposition:** queued, ANTS-6686.

This 200-line per-line loop runs its branches in an order that matters: reader record, then heading, then fence, then table, then blank, then text. Each branch tests "inside a code fence" for itself (767, 823, 872, 895, 949). The reader loses the thread on the question "which branch owns a line inside a fence that is not the fence's opening line?". Someone adding a branch above the fence branch w…

Quote: `while (ln <= n) {`

Remedy: classify each line's kind once, then dispatch to one handler per kind.

### Row 655 — Low — `src/roadmapmigrateverb.cpp:873`

review-readability, lane 13-roadmap-store-b, Q2. **Disposition:** queued, ANTS-6686.

Inside the 371-line `run()`, the three-step backup fallback (the given path, then the configured folder, then beside the store) shares five changing variables (`taken`, `backupErr`, `backupPath`, `backupSource`, `backupFallback`) across nested arms. Changing the fallback order means tracing all five.

Quote: `if (!req.dryRun && req.backup) {`

Remedy: move it into a function that returns one result struct.

### Row 657 — Low — `src/roadmapmigrateload.cpp:745`

review-readability, lane 13-roadmap-store-b, Q3. **Disposition:** queued, ANTS-6686.

A helper named for the Layman field is applied to the `source` column. Someone checking this would suspect the wrong function was called. Reading it (roadmaprender.cpp:128–130) shows it only drops one trailing `.`.

Quote: `&& RoadmapRender::laymanForStore(f.storedText) == f.planText)`

Remedy: say so in the comment above, or give the helper a neutral name in roadmaprender.

### Row 662 — Medium — `src/roadmapparse.cpp:472`

review-readability, lane 14-roadmap-parse-render-a, Q3. **Disposition:** queued, ANTS-6685.

**The name says "any trailer key", but the pattern leaves out `Source`.** It exists to end a `Source:` value. Its siblings are named `rxTrailerKeyAfterLayman` and `rxTrailerKeyAfterLanes`. A stranger reusing it as a general key stop, for example for `Evidence:`, would run straight through a following `Source:`. The comments at 478-481 and 524-526 have to warn about this twice. Fix: rename it to `…

Quote: `const QRegularExpression &rxTrailerKey() {`

### Row 663 — Medium — `src/passheadingwrite.cpp:74`

review-readability, lane 14-roadmap-parse-render-a, Q3. **Disposition:** queued, ANTS-6685.

**A file-local function with the same name as `RoadmapParse::normaliseHeadline`, which this file includes, but different behaviour.** This one does not drop trailing punctuation. A stranger reading `locatePass` would assume the pass locator folds headlines the way the ants-v1 locator does. It does not: `Fix the bug.` and `Fix the bug` match on one dialect and not the other. Fix: call `RoadmapPars…

Quote: `QString normaliseHeadline(const QString &s) {`

### Row 666 — Medium — `src/roadmapparse.cpp:1472`

review-readability, lane 14-roadmap-parse-render-a, Q2. **Disposition:** queued under ANTS-6212.

**`fillBulletRecord` is too long to follow** (432 lines, most of them dated history comments). It works out the id from four sources with one order of precedence, and `idToken` from three sources with a different order. Then it picks the headline through a cascade. A stranger loses the thread here: an empty branch uses "the headline is already set" as the only sign that the em-dash split at 1438 …

Quote: `if (!rec.headline.isEmpty()) {`

### Row 667 — Medium — `src/roadmapwrite.cpp:735`

review-readability, lane 14-roadmap-parse-render-a, Q2. **Disposition:** queued, ANTS-6685.

**`commitAndRender` runs to 337 lines and its step numbers are out of order.** "Steps 3–4" comes first, then the touched-bullets capture, then "Step 3a", then "Step 4b" (there is no 4a). The header promises *"begin → mutate → dry render → commit → real render"*, and a stranger checking that order loses it here. Two blocks that are not steps sit inline: the 25-line gate refusal message and the div…

Quote: `// Step 3a — ANTS-5286. On the AfterMutation path the drift is measured`

### Row 668 — Medium — `src/passheadingwrite.cpp:284`

review-readability, lane 14-roadmap-parse-render-a, Q3. **Disposition:** queued, ANTS-6685.

**`hasEmoji` is true for any leading run of symbols, `**` included.** So the fallback replaces an author's `**` decoration with a status glyph, which breaks the rule stated at 177-178. Also, the `hasEmoji && hasKeyword` branch at 287 can never run: a line with a status word would already have been rewritten by `rewriteStatusWord` above. A stranger "preserving the line's style" would end up mainta…

Quote: `const bool hasEmoji = !vm.captured(1).isEmpty();`

### Row 669 — Low — `src/passheadingwrite.cpp:260`

review-readability, lane 14-roadmap-parse-render-a, Q4. **Disposition:** queued, ANTS-6685.

**The code copies the whole document to find which line `rewriteStatusWord` changed, and no comment says why.** A stranger gets stuck on why it compares documents rather than asking the function. Fix: have `rewriteStatusWord` return the index of the line it rewrote.

Quote: `} else if (QStringList before = lines;`

### Row 670 — Low — `src/roadmapwrite.cpp:567`

review-readability, lane 14-roadmap-parse-render-a, Q3. **Disposition:** queued, ANTS-6685.

**The name `abort` says "failure", but the dry-run success path returns through it too** (`return abort(Result::Ok);` at 840). The comment at 837 does say so. Fix: rename it to `rollbackAndReturn`.

Quote: `const auto abort = [&store](Result r) {`

### Row 679 — Low — `src/roadmapparse.cpp:192`

review-readability, lane 14-roadmap-parse-render-a, Q3. **Disposition:** queued, ANTS-6685.

**`headLineEnd` (the end of the first line) differs from the field `headlineEnd` (the end of the consumed headline) by one capital letter**, and the two are passed side by side into `assignHeadline`. Fix: rename the function to `firstLineEnd`.

Quote: `static int headLineEnd(const QString &body) {`

### Row 687 — Medium — `src/roadmaprender.cpp:679-821`

review-readability, lane 15-roadmap-parse-render-b, Q2. **Disposition:** queued, ANTS-6685.

`render()` runs 354 lines and does six jobs: gate, route, assemble, compare, stage and commit. The per-file assembly loop nests the root-section branch, then the legend-validity refusal (699-730, about 30 lines of comment and code, six levels deep), then the INV-9 parent-level check, then the element walk. A stranger changing how a non-root section is emitted loses the thread at line 699. The leg…

Quote: `for (const QString &path : fileOrder) {`

Remedy: extract the per-file assembly (or at least the legend block) into a helper that returns the file text or a refusal.

### Row 691 — Low — `src/roadmapexport.cpp:463`

review-readability, lane 15-roadmap-parse-render-b, Q5. **Disposition:** queued, ANTS-6685.

The `15` is the index of `lanes` in the SELECT at 427-429, and nothing names it. A stranger adding a column to that SELECT ahead of `lanes` (where the scalar columns sit) shifts every JSON column. The `kJsonCols` comment at 42-43 covers the order but not the offset.

Quote: `if (!jsonColumn(q.value(15 + i).toString(), kJsonCols[i], &v, error))`

Remedy: name the offset beside the SELECT, or move the JSON columns into a SELECT built from `kJsonCols`.

### Row 698 — Medium — `src/roadmapdialog.cpp:2392`

review-readability, lane 16-roadmap-dialog, Q4. **Disposition:** queued, ANTS-6684.

The comment says the tab chain lists the controls in reading order. The code does not: - It leaves out the four buttons ANTS-4412 added (`m_tocToggleBtn`, `m_statusFilterBtn`, `m_kindFilterBtn`, `m_resetFiltersBtn`). - It still lists the status and Kind checkboxes, which now live inside popup menus. - It puts the density combo after the status boxes, although the combo now sits in the search row.…

Quote: `// whenever a widget is inserted. Chain the focusable controls in reading`

Remedy: rebuild the chain from the controls actually on screen, and correct the comment. Cost: one edit — Searched: anchors file for `tabChain|setTabOrder|Chain the` (no hit); `docs/` for `tabChain` (no hit).

### Row 699 — Medium — `src/roadmapdialog.cpp:1177`

review-readability, lane 16-roadmap-dialog, Q2. **Disposition:** queued under ANTS-6218.

This confirms the `renderCardsHtml` size candidate. A reader loses the thread at the 24-placeholder stylesheet (1069-1194): adding or reordering one CSS rule means renumbering `%N` by hand. The comment at 1491-1493 admits the duplicate-ID banner used inline styles just to avoid that renumbering. Then come four long lambdas and the main walk, all inside one function.

Quote: `.arg(QString::number(t.bodyPx), // %7`

Remedy: move the stylesheet builder, `emitSectionHeader` and `emitCard` out into their own functions. Cost: pinned — anchors `.rm-count{text-align:center;white-space:nowrap;` (line 10), `.rm-id{font-family:monospace;font-size:%15px;` (line 11) and `.rm-state-label{font-size:%20px;color:%6;` (line 14) pin t…

### Row 700 — Medium — `src/roadmapdialog.cpp:2489`

review-readability, lane 16-roadmap-dialog, Q2. **Disposition:** queued, ANTS-6684.

This confirms the constructor size candidate. After about 590 lines of building widgets, the constructor turns into saved-state logic (2486-2555): per-tab filters, the one-time migration from old global settings, open cards, scroll anchors. That logic is the counterpart of `saveViewState()`, but nothing points from one to the other.

Quote: `Preset persisted = Preset::Full;`

Remedy: move it into a `restoreViewState()` placed beside `saveViewState()`. Cost: pinned — anchors `!m_config->hasRoadmapViewStates()` (line 3) and `roadmapViewState(` (line 47).

### Row 708 — Low — `src/roadmapdialog.cpp:916`

review-readability, lane 16-roadmap-dialog, Q5. **Disposition:** queued, ANTS-6684.

The current-work match ignores signals, and bullet text (921), shorter than 6 characters. Nothing says why 6. A stranger cannot tell whether it is safe to change.

Quote: `if (f.size() >= 6) signalsFuzzy.append(f);`

Remedy: a named constant with a one-line reason (probably avoiding spurious substring hits). Cost: one edit — Searched: anchors file for `size\(\) >= 6` (no hit).

### Row 709 — Low — `src/roadmapdialog.h:521`

review-readability, lane 16-roadmap-dialog, Q3. **Disposition:** queued under ANTS-6220.

It returns CHANGELOG lines and commit subjects, not roadmap bullets, and the parameter it feeds is named `currentBullets` as well. The comment just above it corrects this, so a stranger recovers quickly.

Quote: `QStringList collectCurrentBullets();`

Remedy: rename to something like `collectCurrentWorkSignals`. Cost: pinned — `docs/specs/ANTS-3793-roadmap-consumer-cutover.md` (lines 81, 875) and `docs/plans/ANTS-1160.md` (line 1384) name `collectCurrentBullets`.

### Row 714 — Medium — `src/claudeintegration_terminal.cpp:186`

review-readability, lane 17-claude-integration-a, Q3. **Disposition:** queued, ANTS-6665.

The file header says this piece holds "the terminal and tab tool descriptors", and the function is `appendToolSchemasTerminal`. About two-thirds of it (185–1512) is `roadmap_query` and `model_switch_stats`. Neither is a terminal tool: both are registered as project-scoped in mcptoolregistry.cpp and are absent from `terminalScopedVerbNames()`. The declaration comment in claudeintegration_internal.…

Quote: `roadmapTool["name"] = "roadmap_query";`

Remedy: move both descriptors into the piece for their tool family.

### Row 715 — Medium — `src/claudeintegration.cpp:2396`

review-readability, lane 17-claude-integration-a, Q2. **Disposition:** queued, ANTS-6665.

`handleMcpRequest` is about 1720 lines. Its `tools/list` branch alone (1780–2874) makes four passes over `tools`: 1. Prefix, cost and `required` (2204). 2. Snapshot (2396) and key cache (2408). 3. Strip `detail` (2453). 4. Shorten descriptions (2765). Each pass must sit on the correct side of the snapshot, or `tool_info` serves the wire text instead of the full text. That ordering rule is spread …

Quote: `m_lastToolsList = tools;`

Remedy: lift the `tools/list` branch into its own method, with one named function per pass and the snapshot point stated once.

### Row 716 — Medium — `src/mcptoolregistry.cpp:435`

review-readability, lane 17-claude-integration-a, Q2. **Disposition:** queued, ANTS-6665.

The `audit_run` handler is a lambda of about 370 lines (240–611) inside `registerProjectScopedVerbs`. Two branches both dismiss the in-flight scope guard and hand the slot release to a completion slot. The async branch releases after `auditJobComplete`; the sync branch releases first. The guard's lifetime has to be traced through about 200 lines to see this.

Quote: `inFlightGuard.dismiss();`

Remedy: lift the handler into a named function in the anonymous namespace (part of ANTS-6221).

### Row 718 — Low — `src/claudeintegration.cpp:2791`

review-readability, lane 17-claude-integration-a, Q5. **Disposition:** queued, ANTS-6665.

Whether a `caller_cwd` description counts as "generic" depends on a phrase inside `makeCallerCwdReadProp`'s text, 1300 lines up (line 1496). Nothing at 1496 says the wording is load-bearing. Rewording it silently stops the summary. A test does catch this.

Quote: `.contains(QLatin1String("Required by the dispatcher"))`

Remedy: compare against a named constant that `makeCallerCwdReadProp` also uses.

### Row 739 — Medium — `src/claudeintegration_store.cpp:17-1708`

review-readability, lane 19-claude-integration-c, Q2. **Disposition:** queued, ANTS-6665.

The `roadmap_log` schema is one block of about 1,700 lines. Its properties are defined between lines 267 and 1431 but attached to the schema between lines 1432 and 1691, so each property is built far from where it is registered. For example, `lifecycleEnum` is defined at 304 and used again at 1020. A stranger adding an argument must edit two places about a thousand lines apart. Forgetting the sec…

Quote: `QJsonObject t;`

Remedy: build and register each property together, or split `roadmap_log`'s schema into its own function.

### Row 740 — Medium — `src/claudeintegration_sessions.cpp:842-845`

review-readability, lane 19-claude-integration-c, Q2. **Disposition:** queued, ANTS-6665.

`parseTranscriptTail` (344 lines) holds five separate readings: - the context size; - the current state; - `/compact`; - `/audit`; - plan mode. They do not share one window. The context reading widens by changing `window` itself (line 898). The state search widens a copy (`stateWindow`, 945). The compact, audit and plan-mode loops (1025, 1046, 1093) still walk the original narrow `events`. Someon…

Quote: `const QList<QJsonObject> events = parseEvents(tail);`

Remedy: split into one function per reading, each taking the events it should search.

### Row 748 — Low — `src/claudeintegration_callpolicy.cpp:890-898`

review-readability, lane 19-claude-integration-c, Q5. **Disposition:** queued, ANTS-6665.

The branch removes the bucket and then recreates it under the same key at once, so nothing is evicted. It also returns "accept" without checking the cap, so a cap of 0 (which the test override at line 731 allows) still lets the first call in every window through. The comment's "auto-evicts" does not say either thing, and a stranger cannot tell whether skipping the cap check is intended.

Quote: `// INV-6 — empty bucket auto-evicts. Insert nowMs into a fresh`

Remedy: state why an empty bucket skips the cap check, or let it fall through to the cap check.

### Row 755 — Medium — `src/claudestatuswidgets.cpp:2029-2052`

review-readability, lane 20-claude-session-widgets-a, Q2. **Disposition:** queued, ANTS-6666.

refreshAutoModelSwitch (320 lines) loses the reader while the Gate is being filled in. Twenty-four lines of layered ticket history (ANTS-1917, ANTS-1959 twice, an indie-review note on the old `if`) surround four lines of code, and the current rule ("idleElapsedMs is always set when idle; the ceiling alone is configurable") is stated only partway through. The same pattern runs across 1897-1930 and…

Quote: `// ANTS-1917 — idle end-of-session suppression. idleElapsedMs is how long`

Remedy: move the gate assembly (2004-2061) into a `buildGate(...)` helper with a one-line rule per field, and send the history to the commit log.

### Row 761 — Low — `src/claudestateresolver.cpp:39-42`

review-readability, lane 20-claude-session-widgets-a, Q5. **Disposition:** queued, ANTS-6666.

AskUserQuestion and ExitPlanMode are matched case-sensitively. The Bash check a few lines below is deliberately case-insensitive (INV-5, noted at `claudestatuswidgets.cpp:497-500`), and stateFace lower-cases tool names. Nothing says why these two differ. A stranger would not know which convention a new tool name should follow.

Quote: `if (r.base == ClaudeState::ToolUse`

Remedy: match case-insensitively like Bash, or comment why exact case is wanted.

### Row 766 — Low — `src/claudestatuswidgets.cpp:341`

review-readability, lane 20-claude-session-widgets-a, Q2. **Disposition:** queued, ANTS-6666.

The whole model-chip click behaviour (defer-or-send, handshake, hide) is a 65-line lambda inside the 227-line constructor. The other switch paths are named methods (`onUndoSwitchClicked`, `maybeFireDeferredChipSwitch`). A stranger looking for "what happens on chip click" finds no method by that name.

Quote: `connect(m_modelBtn, &QPushButton::clicked, this, [this]() {`

Remedy: move it into a named slot such as `onModelChipClicked`.

### Row 772 — Medium — `src/claudebgtasks.cpp:450`

review-readability, lane 21-claude-session-widgets-b, Q2. **Disposition:** queued under ANTS-6230.

`applyBgEvent` runs from line 336 to 496, nested up to seven deep. The reader loses the thread here. Inside the same per-block loop that starts tasks, a second, older completion path runs. It checks the event-level `trStatus` and `tur`, not the block's own. Its nested fallback scan (470–491) can end any tracked id. Only the header list at line 245 ("BashOutput's era") hints that this path is lega…

Quote: `// BashOutput / Task tool_result with a completion status.`

Remedy: split into `applyAssistantEvent`, `applyUserEvent` and a named legacy-completion function.

### Row 775 — Medium — `src/claudebgtasks.h:51`

review-readability, lane 21-claude-session-widgets-b, Q3. **Disposition:** queued, ANTS-6666.

Here "pending" means *running*, as the comment at line 50 says. In the same header `ClaudeBgPendingCall` uses "pending" to mean *awaiting a tool_result*. In the task list, `pendingCount()` means *not yet started*. A stranger reading `pendingCounts(tasks)` at a call site (`claudetabtracker.cpp:309`) would assume it counts tasks still waiting to start.

Quote: `struct ClaudeBgPendingCounts {`

Remedy: rename to `ClaudeBgRunningCounts` / `runningCounts`.

### Row 776 — Medium — `src/claudetasklist.cpp:107`

review-readability, lane 21-claude-session-widgets-b, Q3. **Disposition:** queued, ANTS-6666.

It returns `pendingCount()` only. "Unfinished" reads as pending plus in progress, and a caller (`claudestatuswidgets.cpp:1252`) never sees the header's inline correction.

Quote: `int ClaudeTaskListTracker::unfinishedCount() const {`

Remedy: rename to say what it counts, for example `actionableCount`, or have callers use `pendingCount()`.

### Row 780 — Low — `src/claudeallowlist.cpp:418`

review-readability, lane 21-claude-session-widgets-b, Q5. **Disposition:** queued, ANTS-6666.

The comment above says only commands that usually take arguments get ` *`. The code does the reverse: every command gets it except `make` and `env`, and no reason is given for those two. The presets list offers `Bash(make *)`. A stranger adding a command to this exclusion cannot tell what qualifies.

Quote: `if (cmd != "make" && cmd != "env") {`

Remedy: comment why `make` and `env` stay bare.

### Row 798 — Medium — `src/remotecontrol.cpp:68`

review-readability, lane 22-mcp-transport, Q4. **Disposition:** queued, ANTS-6680.

Neither overload of `resolveRootCanonical` is defined in this file. Both are in `remotecontrol_feedback.cpp:1396/1430`. Both declarations also repeat `remotecontrol_internal.h:520/525`, which this file includes at line 13. A stranger scrolls "below" for nothing.

Quote: `// post-bundle-A). Definition lives in the rcdetail namespace below`

Remedy: delete the two forward declarations and their comments.

### Row 803 — Medium — `src/mcpdmain.cpp:249`

review-readability, lane 22-mcp-transport, Q2. **Disposition:** queued, ANTS-6680.

`main` is easy to follow up to the `--call` return. From here a stranger loses the thread. The usage snapshot is built from five locals, three `[&]`-capturing lambdas and a timer, mixed in with the signal pipe and the end-of-input handling. Changing how snapshots are written means tracing captures across about 130 lines.

Quote: `// ANTS-5311 § 2.4 — this process's usage snapshot, so the terminal's`

Remedy: move the usage-snapshot state into a small class in this file.

### Row 822 — Medium — `src/remotecontrol_roadmap_log.cpp:1009`

review-readability, lane 23-mcp-roadmap-query-log-a, Q3. **Disposition:** queued, ANTS-6678.

By line 1039 this holds the maximum of three sources: the file, the committed corpus (CHANGELOG, `docs/roadmap`) and the store's allocation floor. The `id_taken` message built from it (1049–1053) still says *"highest existing … id in ROADMAP.md"*. A stranger, or a caller, will look in ROADMAP.md for an id that may only exist in the store.

Quote: `qint64 maxFileId =`

Remedy: rename it to something like `occupiedHighWater`, and drop "in ROADMAP.md" from the message.

### Row 827 — Medium — `src/remotecontrol_roadmap_log.cpp:2030`

review-readability, lane 23-mcp-roadmap-query-log-a, Q2. **Disposition:** queued, ANTS-6678.

Flip's numbered steps (1–14) lose the thread here. A 140-line lambda that mutates the captured `lines` is defined between step 6 and step 7. A whole unnumbered ants-v1 branch follows (2173–2300) with its own locate, suggestions and refusal. Step 7 then resumes as a GFM-only path, with the mixed-file fallback calling back into the lambda. A stranger cannot tell which format's path they are in.

Quote: `auto applyAntsV1FlipResult =`

Remedy: split into per-format functions (GFM, ants-v1, store) behind a small dispatcher.

### Row 829 — Medium — `src/remotecontrol_roadmap_log.cpp:3152`

review-readability, lane 23-mcp-roadmap-query-log-a, Q2. **Disposition:** queued under ANTS-6239.

This opens a 135-line block (to 3288) whose body is not indented. Only `} // !storeLocated` marks the end. A stranger reading 3153–3287 takes it for top-level code that always runs.

Quote: `if (!storeLocated) {`

Remedy: move the file-locate into its own function returning the located fields.

### Row 830 — Medium — `src/remotecontrol_roadmap_log.cpp:392`

review-readability, lane 23-mcp-roadmap-query-log-a, Q2. **Disposition:** queued under ANTS-6238.

From here, append runs a complete 330-line store implementation inside a bare brace block (398–726) that returns from deep inside. The markdown path then resumes using variables declared 400 lines up (`useStablePrefix`, `stableId`, `counterPath`, `projectRootDir`). A stranger loses which locals each path uses.

Quote: `// ANTS-3809 § 2.2 — the store path, AHEAD of the counter machinery below.`

Remedy: separate store-append and file-append functions after shared validation.

### Row 837 — Low — `src/remotecontrol_roadmap_log.cpp:2549`

review-readability, lane 23-mcp-roadmap-query-log-a, Q5. **Disposition:** queued, ANTS-6678.

The anchor prefix taken from the folder name (4 characters, with a `ROOT` fallback) skips the `rxPrefix` check that `prefix_hint` must pass (2552–2560). It is also a different rule from append's `rlResolveCounterPrefix`. No comment gives the reason. A stranger cannot tell whether a digit-leading folder name is meant to be allowed.

Quote: `prefix = leaf.left(4).toUpper();`

Remedy: a comment naming why this differs from append's rule and why it is not validated.

### Row 838 — Low — `src/remotecontrol_roadmap_log.cpp:503`

review-readability, lane 23-mcp-roadmap-query-log-a, Q5. **Disposition:** queued, ANTS-6678.

The >9999 branch (and its twin at 1069–1070) implies that 4-digit padding would truncate. `QString::arg`'s field width is a minimum, so both arms produce the same string. A stranger will copy the needless special case.

Quote: `idStr = allocated > 9999`

Remedy: drop the branch, or comment why it exists.

### Row 839 — Low — `src/remotecontrol_roadmap_log.cpp:3836`

review-readability, lane 23-mcp-roadmap-query-log-a, Q3. **Disposition:** queued, ANTS-6678.

It aliases a batch entry as `req`, so a stranger reads `req.value("value")` as the request. Its messages also say `op:"amend_field"` when the caller used amend_field_batch, against the rule at 2887–2889 (name the op the caller used).

Quote: `const QJsonObject &req = src;`

Remedy: use `src`, and pass in the op name.

### Row 840 — Low — `src/remotecontrol_roadmap_log.cpp:4096`

review-readability, lane 23-mcp-roadmap-query-log-a, Q3. **Disposition:** queued, ANTS-6678.

It wraps a value that is always present in an optional and then dereferences it everywhere. A stranger looks for the empty case.

Quote: `const std::optional<qint64> pk = pkValue;`

Remedy: use `pkValue` directly.

### Row 848 — Medium — `src/remotecontrol_roadmap_query_verb.cpp:2894`

review-readability, lane 24-mcp-roadmap-query-log-b, Q2. **Disposition:** queued under ANTS-6164.

`cmdRoadmapQuery` is 3,062 lines long. This is where a reader loses the thread. The `[&]` lambda reads around twenty locals declared 900-2,000 lines earlier, among them: - `filterEcho` (924), `queryArg` (1051), `sourceFilteredOut` (1254), `listBodyCap` (1428), `bulletFields` (1591), `includeBody` (1967) It also writes about fifteen keys into the shared `out`. Nobody can tell what it depends on or…

Quote: `const auto emitBulletPage = [&](QJsonArray &filtered, const ListCounts &n,`

Remedy: parse the arguments into a struct, then give each mode branch its own function. Cost: pinned — specs ANTS-1881, ANTS-1437 and ANTS-1922 name `cmdRoadmapQuery`; anchors 240, 244 and 334 name its locals.

### Row 849 — Medium — `src/remotecontrol_roadmap_query.cpp:1478`

review-readability, lane 24-mcp-roadmap-query-log-b, Q3. **Disposition:** queued, ANTS-6678.

`rcRoadmapWriteFields` reads as a pure "copy the render outcome into the reply" helper. It actually keeps a process-wide record of the last discarded text for each file. Calling it twice for one write, for example to build a second reply, wrongly sets `discard_repeated:true`. Nothing in the name warns of this.

Quote: `static QMutex mutex;`

Remedy: move the repeat check into its own named function, called once per real write. Cost: one edit — Searched: `docs/` for `rcRoadmapWriteFields` and `discard_repeated` (no hits); anchors for `discard` and `rcRoadmapWriteFields` (no hits).

### Row 857 — Low — `src/remotecontrol_roadmap_query.cpp:1`

review-readability, lane 24-mcp-roadmap-query-log-b, Q3. **Disposition:** queued, ANTS-6678.

Despite this header and the file name, most of the file is write code: the Pass-heading writers, the write-reply fields and the trailer-column derivation. A stranger looking for those would not open a file named "query".

Quote: `// ANTS-3833 TU 3/25 — Roadmap read ops.`

Remedy: correct the header to say what the file holds. Renaming the file is the bigger option. Cost: pinned for a rename — ANTS-3833-remotecontrol-decomposition.md:103 names the file in its file-split table; a header-only fix needs no search.

### Row 868 — Medium — `src/remotecontrol_roadmap_log_batch.cpp:335`

review-readability, lane 25-mcp-roadmap-batch, Q3. **Disposition:** queued under ANTS-6249.

On the database route, `firstLine` and `claimedFirstLines` hold the item's negated key minus one (501, 507, 716), not a line. The struct's own comment says nothing about this. The note is 150 lines away at 486–489. A newcomer adding `line = t.firstLine + 1` to the database reply, or trusting the sort at 797–800 ("returned in firstLine-ascending order"), would get negative lines and an order that …

Quote: `int firstLine = -1;`

Remedy: give the store-located target its own `itemPk` field (or rename the key `dedupKey`) and say so on the struct.

### Row 871 — Medium — `src/remotecontrol_roadmap_log_batch.cpp:690`

review-readability, lane 25-mcp-roadmap-batch, Q2. **Disposition:** queued under ANTS-6246.

`cmdRoadmapLogFlipBatch` (lines 136–1156) is easy to follow through numbered steps 1–6. Then a 245-line database route opens here, between step 6 and step "6.5", with its own locate, parts guard, transaction and reply, and returns. The reader loses track of whether "6.5"–"10" apply to both routes.

Quote: `if (writeTarget) {`

Remedy: move the database route into its own member, called after phase 1.

### Row 872 — Medium — `src/remotecontrol_roadmap_log_batch.cpp:2986`

review-readability, lane 25-mcp-roadmap-batch, Q2. **Disposition:** queued under ANTS-6176.

`cmdRoadmapLogAppendBatch` (lines 2229–3307) runs: - a call to itself (2262) - a nested early route that reads the roadmap a second time (2293–2331) - a `writeTarget` that four comments say is "consumed at four later points" (2401) - this database write block, between step INV-4 and step 7, which ends with its own reply tail A reader cannot tell which later steps and advisories run on which route…

Quote: `if (writeTarget) {`

Remedy: split into shared validation plus one write-and-reply member per route.

### Row 881 — Low — `src/remotecontrol_roadmap_repair.cpp:372-373`

review-readability, lane 25-mcp-roadmap-batch, Q5. **Disposition:** queued, ANTS-6677.

`runs_stripped` is worked out by subtracting from `bodyPlan`, which three different kinds of write feed. Only `repeatsStripped` is marked as a subset (192). Someone adding a fourth kind of body write will get a wrong count.

Quote: `int(bodyPlan.size()) - repeatsStripped - placeholderBodies;`

Remedy: count stripped runs directly where they are planned (285).

### Row 882 — Low — `src/remotecontrol_roadmap_backfill.cpp:119-120`

review-readability, lane 25-mcp-roadmap-batch, Q5. **Disposition:** queued, ANTS-6677.

A made-up heading is put in front of the line so the block parser will read a lone Status line. No comment says why, so a reader may "fix" it into a real pass id.

Quote: `{QStringLiteral("#### Pass 0.0"), line});`

Remedy: add a one-line comment saying the parser needs a heading.

### Row 885 — High — `src/remotecontrol_workspace.cpp:766`

review-readability, lane 26-mcp-state-workspace-a, Q5. **Disposition:** queued, ANTS-6679.

The order of precedence between the result modes is set in two places, and the two disagree: - In the line handler, `files_only` returns before the `matches_only` harvest at 778. - In the reply, `matches_only` (996) is checked before `count_only` (1039) and `files_only` (1066). The comment at 992–995 says count_only wins over matches_only and that files_only "already" does the same. It never says…

Quote: `if (filesOnly) {`

Remedy: settle the precedence once, before the scan, and state it in one comment.

### Row 886 — Medium — `src/remotecontrol_workspace.cpp:869`

review-readability, lane 26-mcp-state-workspace-a, Q3. **Disposition:** queued, ANTS-6679.

The local `hardKilled` also covers the output cap, and sits beside `run.hardKilled`, which does not. Line 909 must say "A REAL hard kill only" and switch back to `run.hardKilled`. A newcomer adding a branch keyed on `hardKilled` would report an output-capped run as a wall-budget timeout.

Quote: `const bool hardKilled = run.hardKilled || run.outputCapped;`

Remedy: rename it to something like `scanCutShort`.

### Row 887 — Medium — `src/remotecontrol_workspace.cpp:3767`

review-readability, lane 26-mcp-state-workspace-a, Q3. **Disposition:** queued, ANTS-6679.

In `resolveFeedbackPath`, `sharedRoot` is the parent of `caller_cwd`. In `feedbackSiblingCandidates` (3553) and `fbNotFound` (3624), `sharedRoot` is the configured `claude.mcp_feedback_root` — which this function calls `declared` (3891). A newcomer reading the feedback code would assume one meaning throughout, and could compare or swap the wrong directory.

Quote: `const QString sharedRoot = QFileInfo(rootCanonical).absolutePath();`

Remedy: rename the 3767 local to `callerParent` or similar.

### Row 889 — Medium — `src/remotecontrol_workspace.cpp:3877`

review-readability, lane 26-mcp-state-workspace-a, Q2. **Disposition:** queued, ANTS-6679.

`resolveFeedbackPath`'s no-`path` branch runs from 3748 to 3969 and makes seven attempts in sequence: 1. dot leaf; 2. origin-remote rescue; 3. conventional names; 4. normalised sibling; 5. configured-root redirect; 6. child-directory probe; 7. ancestor walk. Each attempt either returns or falls through with different out-parameters set. Here, nested seven deep, a newcomer loses track of which of …

Quote: `if (!existsOut) {`

Remedy: split each strategy into its own named function returning a small result struct.

### Row 890 — Medium — `src/remotecontrol_workspace.cpp:697`

review-readability, lane 26-mcp-state-workspace-a, Q2. **Disposition:** queued under ANTS-6254.

`cmdWorkspaceSearch` runs from 263 to 1355. The thread is lost at this line handler: it interleaves four output modes (files_only 715/766, matches_only 778, offset 809, normal rows 812), and each mode has a matching early return far below (996, 1039, 1066). Their conditions do not line up: see the precedence finding above. A newcomer adding a fifth mode cannot see every place that must change.

Quote: `const auto onLine = [&](const QByteArray &line) {`

Remedy: split each mode into a collector object with its own `onMatch` and `reply`, chosen once before the scan.

### Row 891 — Medium — `src/remotecontrol_workspace.cpp:2395`

review-readability, lane 26-mcp-state-workspace-a, Q2. **Disposition:** queued, ANTS-6679.

In `cmdMutationProbe` (2009–2697), the loop over mutations runs 220 lines and builds each result row `r` on seven different exits. Each exit sets its own subset of `applied`, `inert`, `outcome`, `occurrences` and `summary`; for example, `write_failed` at 2516 and 2524 has no `summary`. A newcomer adding a field every row should carry would miss some exits.

Quote: `for (qsizetype mi = 0; mi < muts.size(); ++mi) {`

Remedy: a small builder for the result row, with the per-outcome fields passed in.

### Row 901 — Low — `src/remotecontrol_workspace.cpp:2532`

review-readability, lane 26-mcp-state-workspace-a, Q5. **Disposition:** queued, ANTS-6679.

With no `build_command`, a dummy `RunOut` is marked started so that `buildSucceeded` passes. This relies on `exitCode` defaulting to 0, and no comment says so. A newcomer changing `RunOut`'s defaults (for example `exitCode = -1`, as at 2188) would mark every mutant `broken`.

Quote: `build.started = true;`

Remedy: test `buildArgv.isEmpty() || buildSucceeded(build)` explicitly.

### Row 908 — Medium — `src/remotecontrol_state.cpp:2639`

review-readability, lane 27-mcp-state-workspace-b, Q2. **Disposition:** queued, ANTS-6679.

`runScan` silently resets the shared `matched` list and the three `*Scanned` counters, which it reaches through captured references. It is called up to four times: once at the start, twice in the fallback passes and once in the near-miss pass. The near-miss pass (2742–2746) has to save `matched` and put it back, and it leaves `activeNeedles` holding the short forms afterwards. `totalScanned` is c…

Quote: `auto runScan = [&]() {`

Remedy: make the scan a function that takes the search strings and returns `{matched, counts}`. Cost: pinned. The anchor `QJsonDocument RemoteControl::cmdInvariantCheck(` (`test_mcp_invariant_check.cpp:71`) pins the body. Searched: anchors for `runScan|scanOneDir|tryTier` (no hit) and `docs/` for the same …

### Row 914 — Medium — `src/remotecontrol_state.cpp:2967`

review-readability, lane 27-mcp-state-workspace-b, Q5. **Disposition:** queued under ANTS-6256.

`task_priors` extracts only `ANTS-` ids. Yet the spec bucket's comment (3119–3120) says other projects' ids carry other prefixes, and `isValidSpecId` accepts any prefix. On a DOOM project, "DOOM-0009" in a description gets no id boost, and no comment says that is deliberate. A stranger assumes the boost works on every project.

Quote: `QStringLiteral("ANTS-[0-9]+"),`

Remedy: a comment that ids are ANTS-only by ANTS-1306 INV-2, or widen the pattern along with the spec. Cost: pinned. `docs/specs/ANTS-1306.md` INV-2 / table row 2 names `ANTS-[0-9]+`, and the anchor `ANTS-[0-9]+` (`test_mcp_task_priors.cpp:83`) pins it.

### Row 915 — Low — `src/remotecontrol_state.cpp:547`

review-readability, lane 27-mcp-state-workspace-b, Q5. **Disposition:** queued, ANTS-6679.

Zero distinct files (no top findings, for example `top_n:0`) falls through to `"broad"`, which also turns off the narrow-run warning. Nothing says why. The `5` is also unrelated to the separate `5` preview limit at 532, though they look linked.

Quote: `} else if (distinct >= 1 && distinct <= 5) {`

Remedy: name both constants and comment the zero case. Cost: pinned. The anchor `classifyAuditScope(` (`test_mcp_last_audit_summary.cpp:721`).

### Row 916 — Low — `src/remotecontrol_state.cpp:2167`

review-readability, lane 27-mcp-state-workspace-b, Q3. **Disposition:** queued, ANTS-6679.

Everywhere else in this file, `sq` means SymbolQuery (`sqArgErr`, `sqNoProject`, `sqLangEcho`, `sqOptions`, `sqAttachBody`). Here it means spec_query. A stranger will look for this helper among the symbol-query helpers, or assume it belongs there.

Quote: `static QString sqLastLoopDate(const QString &body) {`

Remedy: rename it, for example `specLastLoopDate`. Cost: one edit. Searched: `sqLastLoopDate` across the tree (only 2167 and 2222) and the anchors file (no hit).

### Row 919 — Medium — `src/remotecontrol_review.cpp:195`

review-readability, lane 28-mcp-review-verbs, Q2. **Disposition:** queued, ANTS-6676.

In `cmdIndieReviewPartition`, a stranger loses the thread here, for three reasons: - `unassigned` is filled in two places: at line 53, only when the computed walk ran, and again at 196. When the computed partition ran but lost, the first value is silently overwritten. - `derived` changes the outcome at 192, 195, 228 and 252. - The reply fields are spread over about 300 lines of separate hint sect…

Quote: `if (!derived) {`

Remedy: move the "files in no lane" coverage section into its own function that takes `derived` once.

### Row 920 — Medium — `src/remotecontrol_review.cpp:613`

review-readability, lane 28-mcp-review-verbs, Q2. **Disposition:** queued, ANTS-6676.

In `corroborateWithRoot`, the shared-symbol grouping (598–662) is a separate algorithm placed in the middle of building the reply. It has its own outline cache, a local struct and its own sorting. Here a stranger stops reading "the corroborate reply" and has to understand a second feature.

Quote: `struct SymGroup {`

Remedy: move the shared-symbols pass into a function of its own.

### Row 925 — Low — `src/remotecontrol_review.cpp:900`

review-readability, lane 28-mcp-review-verbs, Q4. **Disposition:** queued, ANTS-6676.

The outer condition does nothing, because the inner `if` repeats the only test that matters. A stranger would read the outer test as a real gate and wonder what a gate that ran with a skip reason is meant to do there.

Quote: `if (!r.ran || !r.skippedReason.isEmpty()) {`

Remedy: remove the outer condition.

### Row 927 — Low — `src/remotecontrol_review.cpp:1406`

review-readability, lane 28-mcp-review-verbs, Q3. **Disposition:** queued, ANTS-6676.

The `ce` (cold-eyes) prefix and the cold-eyes banner say "cold-eyes only", but `session_memory`'s `smErr` (coldeyes.cpp:387) uses it, and it is the intended home for the roadmap copies. A stranger needing echo hygiene in a new verb would write another copy.

Quote: `QString ceSanitiseEcho(const QString &raw) {`

Remedy: rename it to a general name.

### Row 934 — Medium — `src/remotecontrol_docs.cpp:221`

review-readability, lane 29-mcp-content-verbs-a, Q3. **Disposition:** queued, ANTS-6674.

The name says doc_integrity, but this is the shared document walk for five verbs (`doc_symbols`, `spec_lint`, `doc_dedup`, `doc_lint`, and `specLintSiblingInvNumbers`). The function's own comment (220) does not say it is shared. A stranger tuning it "for doc_integrity" would silently change four other verbs. `DocIntegrity::Options walk` (367, 948, 1418) has the same problem. Fix: rename it to a n…

Quote: `QStringList RemoteControl::docIntegrityEnumerate(const QString &rootCanonical,`

### Row 935 — Medium — `src/remotecontrol_docs.cpp:1068-1075`

review-readability, lane 29-mcp-content-verbs-a, Q2. **Disposition:** queued, ANTS-6674.

`cmdSpecLint` (216 lines) keeps 12 running totals. It then hands 11 positional arguments to a "pure" builder, including adjacent bools (`sectionsChecked, testCoverageChecked`) and `surfacesResolved, surfacesChecked`. After that it adds reply keys of its own and, in scoped mode, overwrites the builder's `counts`. The reader loses the thread here: where does each reply key come from, and in which o…

Quote: `QJsonObject out = specLintBuildResponse(scoped ? inDiff : findings,`

### Row 937 — Low — `src/quotationcheckverb.cpp:528-535`

review-readability, lane 29-mcp-content-verbs-a, Q2. **Disposition:** queued, ANTS-6674.

`run` is 201 lines. Its item loop goes through four `if (result.isEmpty())` stages. A final `else ++nNotRun` catch-all then counts any result not named. A stranger adding a new result kind would see it silently counted as `not_run`. Fix: move the per-item evaluation into a function, and count by an explicit switch over the kinds.

Quote: `++nNotRun;`

### Row 938 — Low — `src/remotecontrol_docs.cpp:1436`

review-readability, lane 29-mcp-content-verbs-a, Q3. **Disposition:** queued, ANTS-6674.

`gitErr` is used for refusals that involve no git: `exclude_headings` (1436, 2305) and the `since`/`staged` and `staged`/`fix` combinations (959, 963, 2240). A stranger would assume these replies come from git or carry its stderr. Fix: use `mcp::refusal` at the sites that do not involve git.

Quote: `return QJsonDocument(gitErr("bad_args",`

### Row 949 — Medium — `src/remotecontrol_feedback.cpp:1`

review-readability, lane 30-mcp-content-verbs-b, Q3. **Disposition:** queued, ANTS-6674.

Both the file name and its header say "feedback". It also holds: - spec_log - the whole git_state implementation (`gitFailure`, `runStatusOp`/`runLogOp`/`runDiffOp`/`runRenameSweepOp`) - `resolveRootCanonical` - the central caller_cwd resolver `ants::resolveCallerCwdRoot`, `requiredCallerCwd`, `ServerCwdRootProvider` and `expandGlobalConfigSentinel` A stranger looking for how every verb resolves …

Quote: `// ANTS-3833 TU 10/25 — Feedback and audit verbs.`

Remedy: make the header list each group the file holds. Moving the code out is the alternative, but it is pinned.

### Row 950 — Medium — `src/remotecontrol_changelog.cpp:1016-1134`

review-readability, lane 30-mcp-content-verbs-b, Q2. **Disposition:** queued, ANTS-6674.

`cmdChangelogLog` runs to about 590 lines. `amend` was pulled out into `clAmend`, but `normalize`, `release`, `add_subsection` and `add`/`add_from_roadmap` are still inline. The reader loses the thread in `release`, where a 24-line history comment and the byte-cap/UTF-8 back-off logic sit 150 lines below the op dispatch. A stranger changing one op cannot easily see where it ends and the shared ad…

Quote: `if (op == QStringLiteral("release")) {`

Remedy: extract each op into its own function, as `clAmend` was.

### Row 951 — Medium — `src/remotecontrol_feedback.cpp:308-514`

review-readability, lane 30-mcp-content-verbs-b, Q2. **Disposition:** queued, ANTS-6674.

Inside the 385-line `cmdFeedbackQuery`, this one 200-line block does five things in a row: - reads the caller's roadmap - resolves foreign ids - builds statuses - runs the stale-binary pass - does the split-parts lookup It has nesting depth 7 and long history comments. A stranger adding a per-id field loses track of which loop owns `statusArr` around 436.

Quote: `if (!pr.mappedIds.isEmpty()) {`

Remedy: extract the mapped-id status resolution into a helper that returns `statusArr` plus its flags.

### Row 954 — Low — `src/remotecontrol_changelog.cpp:1085-1088`

review-readability, lane 30-mcp-content-verbs-b, Q5. **Disposition:** queued, ANTS-6674.

Two clamps on `max_body_bytes` are silent: a request below 2000 is raised to 2000, and one above 1 MiB is capped. No comment gives the reason for either clamp, and the reply does not say the request was overridden. The 24-line comment above is all history (Vestige's 11,100 lines) and buries the current rule.

Quote: `else if (want < 2000) cap = 2000;`

Remedy: a one-line comment stating the default, floor, ceiling and 0 = off, and why the floor exists.

### Row 956 — Low — `src/runtraceverb.cpp:556`

review-readability, lane 30-mcp-content-verbs-b, Q5. **Disposition:** queued, ANTS-6674.

The 7 is the position of `row` in the hand-written skeleton list just above it. A stranger adding a line to the new-index skeleton (an intro sentence, say) gets a wrong `index_line` with no warning.

Quote: `rowLine = 7;`

Remedy: derive it from the list, for example the index of `row`.

### Row 966 — Medium — `src/doccitations.cpp:1692`

review-readability, lane 31-doc-engines, Q5. **Disposition:** queued, ANTS-6671.

In `check()` the status switch's `default` is in fact the `Target::Resolved` case: it reads `t.absPath` and computes `ok`/`out_of_range`. `OutOfRoot`/`Unresolvable` were filtered at 1622 and every other kind has its own case. A stranger adding a `Target::Kind` (as ANTS-4085 added `ForeignPath`) gets no `-Wswitch` warning, and the new kind is silently treated as a resolved file with an empty path.

Quote: `default: {`

Remedy: write `case Target::Resolved:` and drop `default`, so the compiler flags the next new kind.

### Row 967 — Medium — `src/doccitations.cpp:1571`

review-readability, lane 31-doc-engines, Q2. **Disposition:** queued, ANTS-6671.

`check()` runs from line 1468 to 1951. It does symbol-ref resolution (1496-1554), then a citation loop mixing antecedent reset, resolution, anchor, a 70-line `default` case (1692-1762) and antecedent update, then counts, the hint, two optional passes, filtering and byte-budget paging. A stranger changing the inherited-path rule (`ante.unresolvedSince`) must keep 1622-1786 in mind at once. This lo…

Quote: `for (const Citation &tok : citations) {`

Remedy: extract symbol-ref resolution, the per-citation verdict (the `Resolved` case) and the filter-and-page tail into named functions.

### Row 968 — Medium — `src/doccitations.cpp:1176`

review-readability, lane 31-doc-engines, Q2. **Disposition:** queued, ANTS-6671.

`dcQuotationPass` (1096-1383) nests window building, match-to-line mapping, three attribution strategies (cell, row, paragraph) and the read-and-fold verdict in one loop body. Each stage is wrapped in long ticket comments (1139-1175 is 37 lines before this condition).

Quote: `if (!tableRow && i > 0 && !prefix.at(i - 1).trimmed().isEmpty()`

Remedy: extract `attributionFor(...)` and `verdictFor(...)`, leaving the loop to harvest only.

### Row 976 — Low — `src/doccitations.cpp:1561`

review-readability, lane 31-doc-engines, Q3. **Disposition:** queued, ANTS-6671.

The cursor resets the antecedent on example regions, blank lines and ATX headings too (1585-1593), not only fences. A stranger adding a new break would look elsewhere.

Quote: `int fenceCursor = 0;`

Remedy: rename to `breakCursor` / `lineCursor`.

### Row 977 — Low — `src/docsymbols.h:131`

review-readability, lane 31-doc-engines, Q3. **Disposition:** queued, ANTS-6671.

The struct beside it has `resolved` (occurrences that resolved). `needlesResolved` counts needles looked up whether or not they resolved, and doclint.cpp:456 debits a budget by it. The trailing comment rescues the reader, hence LOW.

Quote: `int needlesResolved = 0; // needles attempted; the caller's budget debit`

Remedy: rename to `needlesLookedUp`.

### Row 978 — Low — `src/docfacts.cpp:22`

review-readability, lane 31-doc-engines, Q3. **Disposition:** queued under ANTS-6135.

In C++ `RowCount` reads as the usual "number of rows" sentinel. Here it is the `count_mismatch` row and sorts first.

Quote: `enum Row { RowCount = 0, RowInvariant, RowMarkup, RowVersion, RowVerbArg };`

Remedy: rename to `RowCountMismatch`.

### Row 979 — Low — `src/docsymbols.cpp:473`

review-readability, lane 31-doc-engines, Q4. **Disposition:** queued, ANTS-6671.

To take the innermost qualifier it uses `+ 1` and then strips the leftover `:`, which only works by accident of the `-1 + 1 = 0` case. A stranger "correcting" `+1` to `+2` breaks the unqualified-parent case.

Quote: `qual = qual.mid(qual.lastIndexOf(QLatin1String("::")) + 1).remove(QLatin1Char(':'));`

Remedy: use `section("::", -1)` or comment the trick.

### Row 986 — Medium — `src/speclog.cpp:377`

review-readability, lane 32-spec-engines, Q3. **Disposition:** queued, ANTS-6688.

This function also writes `### <label>` heading rows (lines 408-445, `rowShape = "heading"`). - Its comment (line 375) says "The loop log is bullet-shaped". - `speclog.h`'s append_loop contract (lines 71-92) never mentions heading rows. A stranger asked to change heading-row behaviour will not look here. The heading arm's line arithmetic (line 441, `(rowOrder == … ? at : at + 1) + 1`) is also whe…

Quote: `static EditResult appendBulletRow(QStringList lines, int hdr, int loopEnd,`

Remedy: split the heading arm into its own `appendHeadingRow`.

### Row 987 — Medium — `src/featurecoverage.cpp:453`

review-readability, lane 32-spec-engines, Q3. **Disposition:** queued, ANTS-6688.

The set is checked at every depth of the recursive walk (line 489), not only at the top. A stranger adding a name such as `docs` or `tools`, expecting a top-level-only effect, silently drops every nested directory of that name too.

Quote: `static const QSet<QString> kSkipTopDirs = [] {`

Remedy: rename to say it applies at any depth (e.g. `kSkipDirNames`).

### Row 988 — Medium — `src/specconformance.cpp:130`

review-readability, lane 32-spec-engines, Q3. **Disposition:** queued, ANTS-6688.

It is incremented for every `regex` fence (line 175), before the engine checks. So a bare ```` ```regex ```` fence and a ```` ```regex python ```` fence both count. It is then published as `executable_fences` (line 320). A stranger, or a caller, reads that number as "fences this verb could run". The "checked nothing" hint (line 323) is suppressed when every fence carries an unsupported engine.

Quote: `int taggedFences = 0;`

Remedy: count only after the `pcre2` check, or rename the variable and the key to "regex fences".

### Row 989 — Medium — `src/specparse.cpp:318`

review-readability, lane 32-spec-engines, Q2. **Disposition:** queued, ANTS-6688.

`parseSpecBody`'s bullet loop (lines 251-364) nests the `*Test:*` clause splice inside a per-bullet loop, inside the section block. The splice has a mask, two global-match scans and head/tail reassembly. A stranger changing where a test clause ends loses the thread at lines 318-355.

Quote: `const QVector<bool> quoted = inlineCodeMask(invBody);`

Remedy: extract "split the test clause out of an invariant body" into its own function.

### Row 1003 — Low — `src/speclint.cpp:634`

review-readability, lane 32-spec-engines, Q5. **Disposition:** queued, ANTS-6688.

Any header line mentioning "layman" anywhere, such as inside a Status value, satisfies the check. The header (speclint.h:127) promises a "Layman line". The looseness has no stated reason.

Quote: `if (lines[i].contains(QLatin1String("layman"), Qt::CaseInsensitive))`

Remedy: comment why, or match a `**Layman…:**` field.

### Row 1004 — Low — `src/speclog.cpp:504`

review-readability, lane 32-spec-engines, Q5. **Disposition:** queued, ANTS-6688.

`createLoopLog` refuses on any `|` line, fenced ones included. The rest of this file is fence-aware on purpose (ANTS-5100, line 577), and no comment says why this scan is not.

Quote: `if (!l.trimmed().startsWith(QLatin1Char('|')))`

Remedy: use the file's fence mask, or comment the choice.

### Row 1011 — Medium — `src/indiereviewdispatcher.cpp:326`

review-readability, lane 33-review-engines, Q5. **Disposition:** queued, ANTS-6683.

Nothing in the code says that 175000 is an assumed model context size in tokens. It is spelled out again in the error text (331). The default `maxTokens` is 64000 in the header. A stranger changing the model or `maxTokens` cannot tell what 175000 stands for, or whether it should change too.

Quote: `const qint64 budgetBytes = qint64(175000 - req.maxTokens) * 4;`

Remedy: a named constant with a one-line comment giving the context-window assumption.

### Row 1012 — Low — `src/indiereviewengine.cpp:1182`

review-readability, lane 33-review-engines, Q2. **Disposition:** queued, ANTS-6683.

Inside the 157-line `corroboratedFindings` is a 60-line lambda, nested four loops deep. It reads `claimed` by reference, and `claimed` changes between its two calls (1261 changes it before 1277). The second call's result depends on that hidden change. The comment at 1258–1259 explains it, but only after the lambda. A stranger who reorders or extracts the two calls will report groups twice.

Quote: `auto proximityGroups =`

Remedy: make it a named free function that takes `claimed` as an explicit argument.

### Row 1013 — Low — `src/indiereviewengine.cpp:1270`

review-readability, lane 33-review-engines, Q5. **Disposition:** queued under ANTS-6137.

Within a file, findings are sorted by line in *descending* order, while the near-miss sort at 1239 is ascending. No comment says why. A stranger will "fix" it as a typo, or will not know whether fold-in depends on bottom-up order.

Quote: `return a.line > b.line;`

Remedy: a one-line comment giving the reason, or make it ascending if there is none.

### Row 1022 — Low — `src/indiereviewengine.cpp:1015`

review-readability, lane 33-review-engines, Q3. **Disposition:** queued, ANTS-6683.

The constant is named and documented as bytes (`indiereviewengine.h:48–51`). Here it is applied to UTF-16 characters, while `corroboratedFindingsFromCanonicalDir` (1352) applies it to bytes. A stranger reasoning about the "64 KiB" limit will get non-ASCII reports wrong.

Quote: `if (scope.size() > kMaxScanBytes) scope = scope.left(kMaxScanBytes);`

Remedy: measure UTF-8 bytes here, or say "characters" at this use.

### Row 1023 — Low — `src/coldeyesengine.cpp:517`

review-readability, lane 33-review-engines, Q5. **Disposition:** queued under ANTS-6137.

Lane-name collisions are handled three ways in one function: - the spec-directory walk skips the later file (comment at 407–413) - the `spec.md` walk renames it to `@parent` (comment at 478–482) - the `docs/phases` walk silently drops it, with no comment A `docs/phases/audio.md` beside `docs/specs/audio.md` vanishes. A stranger cannot tell whether that is meant.

Quote: `if (seenLaneName.contains(laneName)) continue;`

Remedy: a comment giving the reason, or reuse the `@parent` renaming.

### Row 1026 — Medium — `src/testauditengine.cpp:954`

review-readability, lane 34-test-audit-verify, Q5. **Disposition:** queued, ANTS-6691.

`prePassFile` takes the compiled regexes as a parameter but reads each pattern's id, dimension and language gate from `g_kPrePatterns()` by the same index. Nothing says `rxs` must match that table one-for-one, in order. A plausible change, such as passing only the regexes for active dimensions, would silently label findings with the wrong `pattern_id` and dimension and apply the wrong language ga…

Quote: `for (int i = 0; i < rxs.size() && i < g_kPrePatterns().size(); ++i) {`

Remedy: comment the parallel-index rule at the signature, or pass the pattern structs alongside the regexes.

### Row 1029 — Medium — `src/testauditengine.cpp:1861`

review-readability, lane 34-test-audit-verify, Q2. **Disposition:** queued, ANTS-6691.

`synthesize` runs to about 380 lines: - Validation. - Pass 1, which reads every report. - Dimension ranking. - File-index dedup. - The histogram envelope. - A render lambda that captures a dozen locals. - Three mode branches. Here a stranger loses track of which locals (`topDims`, `fileIdx`, `sevEnv`, `reportsCapped`, `summaryCapped`, `md`) are final. Adding a mode or a summary field means tracin…

Quote: `auto renderSummaryBlock = [&]() {`

Remedy: split into collect-reports, rank, and render-by-mode functions that pass a struct.

### Row 1032 — Low — `src/testauditengine.cpp:1936`

review-readability, lane 34-test-audit-verify, Q5. **Disposition:** queued, ANTS-6691.

In hybrid mode, `limit:-1` becomes 3. The header (`testauditengine.h:187-190`) says "an explicit -1 = every chunk", and nothing explains why hybrid differs.

Quote: `if (topN <= 0) topN = 3;`

Remedy: comment the hybrid rule in the header, or honour -1.

### Row 1035 — Low — `src/testauditengine.cpp:2273`

review-readability, lane 34-test-audit-verify, Q3. **Disposition:** queued, ANTS-6691.

Everywhere else in this file, `canon` means the canonical project root. Here it is a dimension name.

Quote: `const QString canon = dimensionInHeader(text);`

Remedy: rename to `knownDim`.

### Row 1036 — Low — `src/testauditengine.cpp:1693`

review-readability, lane 34-test-audit-verify, Q3. **Disposition:** queued, ANTS-6691.

`md` also holds `.json` reports. Someone adding markdown-only handling in the loops below would misapply it.

Quote: `QStringList md = dir.entryList({QStringLiteral("*.md"),`

Remedy: rename to `reportFiles`.

### Row 1042 — Low — `src/testauditdialog.h:107`

review-readability, lane 34-test-audit-verify, Q4. **Disposition:** queued, ANTS-6691.

The constructor always switches this to Narrative (`testauditdialog.cpp:108-109`), unlike cold-eyes and indie-review, where PerFinding is the real default. Someone reading the header assumes per-finding fold-in by default.

Quote: `FoldInMode m_foldInMode = FoldInMode::PerFinding;`

Remedy: initialise to `Narrative`.

### Row 1045 — Low — `src/testauditengine.cpp:880`

review-readability, lane 34-test-audit-verify, Q2. **Disposition:** queued, ANTS-6691.

The raw-string close matcher counts `)` as step 1, so the delimiter index is `rawDelimMatched - 1`, and nothing explains that. The `"`-with-empty-delimiter branch at 886-896 is hard to check by eye. Someone fixing a raw-string bug here would need to work the offsets out on paper.

Quote: `} else if (rawDelimMatched >= 1 &&`

Remedy: one comment stating the meaning of `rawDelimMatched` (0 = idle, 1 = saw `)`, k+1 = matched k delimiter chars).

### Row 1057 — Medium — `src/readregion.cpp:686`

review-readability, lane 35-code-index-search-a, Q2. **Disposition:** queued, ANTS-6667.

`extract` (268 lines) checks the arguments, resolves a heading, resolves a symbol, applies two byte caps, slices the file, builds the reply, and then runs a separate call-sequence analysis over the result. A reader following "how is a region read" loses the thread at 686, where unrelated analysis starts inside the slicer.

Quote: `if (opts.callSequence) {`

Remedy: move section/symbol resolution and the call-sequence scan into their own functions.

### Row 1058 — Medium — `src/codebaseindex.cpp:492`

review-readability, lane 35-code-index-search-a, Q2. **Disposition:** queued, ANTS-6667.

the summary branch of `query` (181 lines) handles four truncation flags. At 492 one variable, `digestTruncated`, starts serving both the per-lane list and the flat fallback list, and it is sent under two names (lines 517 and 538). Someone changing either list's cap will lose track of which flag reports it.

Quote: `if (params.laneFiles && digestEmitted == 0) {`

Remedy: pull the summary branch, and the fallback inside it, into its own function.

### Row 1059 — Medium — `src/duplicateblocks.cpp:371`

review-readability, lane 35-code-index-search-a, Q2. **Disposition:** queued, ANTS-6667.

`run` (145 lines) builds groups, merges sites, sorts and emits. At 371, `Group::sites` changes meaning: before, it holds positions in `g.sites()`; after, it holds positions in `merged`. Code added after line 373 that uses `g.sites().at(...)` would quietly read the wrong site.

Quote: `gr.sites = kept;`

Remedy: give the merged positions their own field, or split the merge step out.

### Row 1060 — Low — `src/applyedits.cpp:81`

review-readability, lane 35-code-index-search-a, Q3. **Disposition:** queued, ANTS-6667.

it also reports punctuation and wrapped-line near misses (kinds "punctuation" and "wrapped"). Someone adding a new kind would look for a broader function.

Quote: `void findWhitespaceNearMiss(const QString &contents, const QString &oldStr,`

Remedy: rename to `findNearMiss`.

### Row 1061 — Low — `src/duplicateblocks.cpp:30`

review-readability, lane 35-code-index-search-a, Q3. **Disposition:** queued, ANTS-6667.

it is the only constant in the block with no comment. It caps how many changed lines are reported, but it reads as a limit on how different two copies may be.

Quote: `inline constexpr int kMaxDiff = 20;`

Remedy: rename to `kMaxDiffLinesReported`.

### Row 1062 — Low — `src/readregion.cpp:176`

review-readability, lane 35-code-index-search-a, Q3. **Disposition:** queued, ANTS-6667.

a private overload with the same name as the public scanner. It only reads the file and forwards, yet it carries the scanner's description (lines 170-175). Someone fixing the scan would find the comment here, above code that does no scanning.

Quote: `int braceBalancedEndLine(const QString &absPath, int startLine) {`

Remedy: rename it (`braceEndLineInFile`) and move the description to the scanner at line 186.

### Row 1073 — Low — `src/fileoutline.cpp:966`

review-readability, lane 35-code-index-search-a, Q5. **Disposition:** queued, ANTS-6667.

some branches of the matching chain test the raw line, others the comment-stripped `codeLine`. The ANTS-3738 comment explains the choice for three of them. rxCppNameArgs and rxCppTypeOnly also end with `\s*$`, yet they get the raw line. Someone adding a branch cannot tell which text to pass.

Quote: `&& (m = rxCppNameArgs().match(line)).hasMatch()) {`

Remedy: say per branch why it gets the raw line, or pass `codeLine` to every pattern anchored at the end of the line.

### Row 1081 — Medium — `src/sourcescrapes.cpp:687-714`

review-readability, lane 36-code-index-search-b, Q2. **Disposition:** queued, ANTS-6667.

`scanPython` (181 lines) builds 13 regexes from concatenated fragments (`base`, `part`, `upward`, `osHere`). It then defines five capturing lambdas and runs seven match loops in one body. A reader loses the thread at `join`/`whole`/`walkCall` (692-711): which capture group means what is only knowable by rebuilding the pattern by hand. Adding one more Python path form needs all of it in your head.

Quote: `const QString upward = QStringLiteral(`

Remedy: split into a small class with one named method per form (Path join, os.path join, glob/walk, getsource, assignment).

### Row 1088 — Medium — `src/mcpprojection.cpp:297`

review-readability, lane 37-mcp-infra, Q2. **Disposition:** queued, ANTS-6675.

`projectFields` (208-351) has about 45 lines of code inside about 145 lines. Most of the comment text is incident history: ANTS-4930's block at 266-290 is 24 lines. A reader loses the thread here. The "floors" are numbered by the date they were added, not by where they sit: the "fourth" floor comes before the refusal floor at 312, and before the "third" floor at 329. The refusal test is also writ…

Quote: `// ANTS-4877 — a fourth floor, and the narrowest: when NOT ONE requested`

Remedy: reuse `refusal` at 321, number the floors in code order, and keep the incident history in the commit log; the header contract already states the rules.

### Row 1089 — Medium — `src/mcpspill.cpp:410`

review-readability, lane 37-mcp-infra, Q3. **Disposition:** queued, ANTS-6675.

The name says it holds each row's bytes. It actually holds each row wrapped as `{"v":<row>}`. Two things depend on that wrapper without saying so. The `bytes` emitted at line 427 is the row's size plus 6. The head at line 434 strips the wrapper with the unexplained numbers `mid(5, size - 6)`. A stranger reusing `elBytes` as "the row's size", or changing the wrapper key, gets wrong sizes or a wron…

Quote: `QList<QByteArray> elBytesByRow;`

Remedy: store the row's own JSON once (strip the wrapper when building the list), or rename the list to say it is wrapped and name the 5/6 offsets.

### Row 1097 — Low — `src/tokenusageengine.cpp:305`

review-readability, lane 37-mcp-infra, Q5. **Disposition:** queued, ANTS-6675.

The `case` numbers must match the order of `kCounterKeys` (line 299), and nothing says so. If a key is inserted mid-list, values land in the wrong fields with no error. If a key is added at the end without a case, `default` returns `nullptr`, which is dereferenced at lines 383 and 428.

Quote: `qint64 *counterField(ToolCounter &c, int i) {`

Remedy: one line at `kCounterKeys` saying the index is `counterField`'s case number, or keep name and member together in one table.

### Row 1099 — Low — `src/mcporientation.cpp:262`

review-readability, lane 37-mcp-infra, Q3. **Disposition:** queued, ANTS-6675.

When the file cannot be opened, the code returns `ParseFail`, which is not a parse failure. The comment at line 246 lists four outcomes ("kept, added, removed, parse_fail"), but the enum has six: `WriteFail` and `Created` are missing. A stranger adding a caller that branches on the outcome would get the meanings wrong.

Quote: `return MergeOutcome::ParseFail;`

Remedy: list all six outcomes at line 246, and say that `ParseFail` also covers an unreadable file.

### Row 1100 — Low — `src/mcporientation.cpp:482`

review-readability, lane 37-mcp-infra, Q4. **Disposition:** queued, ANTS-6675.

Nothing reads the variable. The tests check the file, not this variable. A stranger would go looking for where it is used.

Quote: `(void)scriptWritten; // used by tests via file inspection`

Remedy: delete the variable and its two assignments.

### Row 1114 — Medium — `src/aidialog.cpp:371`

review-readability, lane 38-model-switching, Q5. **Disposition:** queued under ANTS-6142; ANTS-6015.

The fence's language-id strip only fires when the first line is under 10 characters. That length cap has no reason in any comment; the comment above explains only spaces and tabs. `powershell`, `typescript` and `javascript` are exactly 10 characters, so they are not stripped and become the first line of the inserted command. A stranger cannot tell whether 10 is deliberate.

Quote: `if (nl > 0 && nl < 10) {`

Remedy: name the bound as a constant with its reason, or drop it in favour of the no-whitespace test.

### Row 1119 — Low — `src/modelautoswitch.cpp:302-303`

review-readability, lane 38-model-switching, Q5. **Disposition:** queued, ANTS-6681.

The comments above (:274-281) say the title is anchored to "a known tier token" and list Sonnet and Opus. "Default" is not a tier, and neither comment nor header (:179-185) says why it is accepted. A stranger tidying the list to tiers would drop it.

Quote: `QStringLiteral("Set model to Default"), Qt::CaseInsensitive);`

Remedy: one line saying CC prints "Set model to Default" for `/model default`.

### Row 1125 — Low — `src/modelswitchledger.h:63`

review-readability, lane 38-model-switching, Q3. **Disposition:** queued under ANTS-6142.

The name says it looks within 5 turns. `computeOutcome` checks the first post-switch user turn only (cpp:314-323, header :136), and the hard-coded 5 would lie if `kOutcomeWindowTurns` changed. A stranger would expect a 5-turn scan.

Quote: `bool correctionSignalWithin5 = false;`

Remedy: rename the C++ field (for example `correctionSignalFirstTurn`), keeping the on-disk key.

### Row 1126 — Low — `src/llmdispatcher.cpp:9`

review-readability, lane 38-model-switching, Q5. **Disposition:** queued, ANTS-6681.

The upper bound of 4 is stated nowhere in the header (:6-7 says it "runs at most `maxConcurrent`"). A caller passing 8 silently gets 4.

Quote: `: QObject(parent), m_max(std::clamp(maxConcurrent, 1, 4)) {}`

Remedy: one line on the constructor naming the [1,4] clamp and its reason.

### Row 1127 — Low — `src/llmclient.cpp:468-478`

review-readability, lane 38-model-switching, Q2. **Disposition:** queued under ANTS-6142.

In `onFinished`, `hadError` is set or cleared in 5 places and `result.error` is assigned in 5. The tail has two fallbacks to `errorString()` (:468-469 and :477-478), and the second already covers the first. A stranger changing the fallback message would edit one.

Quote: `if (hadError && m_text.isEmpty() && result.error.isEmpty())`

Remedy: keep only the final fallback.

### Row 1128 — Low — `src/aidialog.cpp:73`

review-readability, lane 38-model-switching, Q2. **Disposition:** queued, ANTS-6681.

A 56-line Insert-command confirmation flow (:73-129) is hidden as a lambda inside the constructor, among the layout code. A stranger looking for the insert flow next to `onSend`/`onLlmFinished` would not find a named slot.

Quote: `connect(m_insertBtn, &QPushButton::clicked, this, [this]() {`

Remedy: move it to a private slot `onInsertCommand`.

### Row 1144 — Medium — `src/projectsettings.cpp:394`

review-readability, lane 39-config-session-project, Q5. **Disposition:** queued, ANTS-6668.

Root-level files are counted only when the subdirectory walk left budget. They also do not spend the budget. When the walk used it all, `rootLevel` stays 0, so the whole-root `["."]` branch (446) cannot fire and the subdirectory list is suggested instead. No comment says whether that is intended. A stranger debugging "large repo with root sources got a subdir suggestion" would have to work it out…

Quote: `if (budget > 0) {`

Remedy: one comment stating the intent (or count root files regardless).

### Row 1149 — Medium — `src/pluginmanager.cpp:519`

review-readability, lane 40-plugins-lua, Q2. **Disposition:** queued, ANTS-6682.

`scanAndLoad` runs 142 lines and nests 6 deep. A reader loses the thread in the manifest block (lines 500–532): loop → `exists` → `open` → size check → `else` → parse result, with the size-cap rationale in the middle.

Quote: `} else {`

Remedy: move "read and parse one manifest, size-capped" into its own function, beside `parseManifestInto`.

### Row 1150 — Low — `src/pluginmanager.cpp:609`

review-readability, lane 40-plugins-lua, Q5. **Disposition:** queued, ANTS-6682.

This branch runs in two cases: the grants already cover the request, or a prompt is needed but none is wired. Its comment explains only the "manifest shrank" filtering. Line 583 says *"Deny by default if no prompt wired"*, but with no prompt the previously saved grants still in the manifest are honoured. A stranger would think a missing prompt grants nothing.

Quote: `} else {`

Remedy: state the no-prompt case in the comment at line 583 or at line 609.

### Row 1151 — Low — `src/pluginmanager.cpp:471`

review-readability, lane 40-plugins-lua, Q5. **Disposition:** queued, ANTS-6682.

The check accepts an entry that resolves to the plugin root itself. That contradicts the comment above it (*"anchored INSIDE the canonical plugin root"*) and nothing says why. If it ever happened, the root itself would load as a plugin. A stranger cannot tell if this is deliberate.

Quote: `!(canonicalPlugin == canonicalRoot ||`

Remedy: drop the equality, or comment why it is allowed.

### Row 1156 — Low — `src/luaengine.cpp:254`

review-readability, lane 40-plugins-lua, Q5. **Disposition:** queued under ANTS-6026.

With no engine in the registry, the hook reports a *timeout*. Nothing says why a missing engine should look like a timeout. Anyone debugging that message would chase the wrong cause.

Quote: `luaL_error(L, "Script execution timeout exceeded");`

Remedy: comment the reason, or give the error a message that names the missing engine.

### Row 1159 — Low — `src/luaengine.cpp:1214`

review-readability, lane 40-plugins-lua, Q2. **Disposition:** queued, ANTS-6682.

In `lua_project_read`, the `NotRegular` check opens an `else` whose body (lines 1215–1228) is not indented. The second `}` at line 1229 closes a block the layout hides, and the nesting reaches 5.

Quote: `} else {`

Remedy: make the `isFile` test one more `else if` in the refusal chain.

### Row 1165 — Medium — `src/feedbackfile.cpp:306`

review-readability, lane 41-changelog-feedback, Q2. **Disposition:** queued under ANTS-6145.

`parse` puts the boundary pass, the tracking-table row parser (lines 253–300), the v2 delta, and the v1 delta in one function. A reader loses the thread here. The v2 block runs about 120 lines, holds two lambdas, and ends in an early `return r;` at line 425. You only reach the v1 rule (line 428) by spotting that return. Meanwhile `v1Ids` and `lastMaintainerIdx`, built above for v1, are carried ac…

Quote: `if (v2) {`

Remedy: split into a tracking-row function, a v2-delta function and a v1-delta function.

### Row 1166 — Low — `src/changeloglog.cpp:119`

review-readability, lane 41-changelog-feedback, Q3. **Disposition:** queued, ANTS-6663.

This returns true for any column-0 `### ` line, including dated topics and non-canonical names. Callers must test the name afterwards (lines 289–293 and 329–331). Someone using it to count categories would also count dated topics. `parseCategoryHeading` (changelogquery.cpp:125) has the same shape.

Quote: `bool isCategoryHeading(const QString &line) {`

Remedy: rename to say it detects an H3 heading, not a category.

### Row 1172 — Medium — `src/diffviewer.cpp:416`

review-readability, lane 42-app-entry-dialogs, Q2. **Disposition:** queued, ANTS-6659.

`finalize` (about 400 lines) and `runAsync` are defined **inside** the `runProbes` lambda, but they are indented as if they sat beside it. The only sign of the nesting is `}; // close runProbes lambda` at line 871. A stranger loses the thread at line 414. They will read `finalize` as built once per dialog rather than once per refresh round, around its own `state`, and will put per-dialog state, o…

Quote: `auto finalize = [state, dlgGuard, viewerGuard, copyGuard,`

### Row 1178 — Medium — `src/settingsdialog.cpp:1339`

review-readability, lane 42-app-entry-dialogs, Q5. **Disposition:** queued, ANTS-6659.

The colour buttons' labels "Default" and "None" (set at 1026 and 1039) double as the "no colour" value that `applySettings` reads back (1339, 1341). No comment ties them together. Rewording or translating a label means it gets saved as the colour string. Fix: keep the colour in the button's property or data, not its label, or name the shared strings as constants.

Quote: `obj["fg"] = (fgBtn && fgBtn->text() != "Default") ? fgBtn->text() : "";`

### Row 1188 — Low — `src/claudesetup.cpp:625`

review-readability, lane 42-app-entry-dialogs, Q3. **Disposition:** queued under ANTS-6146.

A field named `unavailable` holds `State::Missing` whenever the plan *can* run (635). Callers have to test `.state == State::Unavailable`. A stranger reading `p.unavailable` assumes it is set only on failure. The comment beside it helps, hence LOW. Fix: rename it to something like `refusal` or `availability`.

Quote: `Status unavailable; // state Unavailable when the plan cannot run`

### Row 1194 — Medium — `src/readlog.cpp:265`

review-readability, lane 43-diagnostics-logging, Q2. **Disposition:** queued, ANTS-6669.

`filter` runs 203 lines (155-357): regex checks, caps, cursor, an oversized-line drain, filters, a drop-oldest deque, redaction with a second byte cap, then the envelope. The reader loses the thread here. After the oversized branch, `complete` can only be false at end of file, so `if (complete) raw.chop(1)` at line 272 is always true. Seeing that means rebuilding the branch logic above.

Quote: `const bool complete = raw.endsWith('\n');`

Remedy: extract "read one line, or drain and skip an oversized one" and the post-scan redaction/re-cap stage into named helpers.

### Row 1195 — Low — `src/readlog.cpp:238`

review-readability, lane 43-diagnostics-logging, Q5. **Disposition:** queued, ANTS-6669.

The 256-line interval is an unnamed bit-mask, and a mask only works for powers of two. A stranger changing it to "every 1000 lines" would write `& 999` and get an irregular check.

Quote: `if (scanned > 0 && (scanned & 255) == 0 && clock.elapsed() > budgetMs) {`

Remedy: a named constant and a modulo, or a comment that it must be 2ⁿ−1.

### Row 1199 — Low — `src/debuglog.cpp:81-82`

review-readability, lane 43-diagnostics-logging, Q5. **Disposition:** queued, ANTS-6669.

`all`, `none` and `off` end parsing early. So `paint,off` gives 0 and `paint,all` gives everything. The header (debuglog.h:47-49) mentions "all" but not `none`/`off`, the order rule, or the numeric legacy form.

Quote: `if (t == "all") return All;`

Remedy: state the short-circuit and the accepted tokens in the header doc.

### Row 1204 — Low — `src/gitwrap.h:39`

review-readability, lane 44-shared-utilities, Q3. **Disposition:** queued, ANTS-6687.

The name says SIGKILL was sent. It is actually set whenever the 5 s timeout fires (`gitwrap.cpp:31`), even when the SIGTERM alone ended git. `kHardKillMs` is likewise the wait before SIGTERM, not before the hard kill. A caller reading `hardKilled` as "git ignored SIGTERM" would report the wrong thing.

Quote: `bool hardKilled = false;`

Remedy: rename the field to `timedOut`.

### Row 1208 — Low — `src/gitwrap.cpp:154`

review-readability, lane 44-shared-utilities, Q3. **Disposition:** queued, ANTS-6687.

In this file `out` names the result being built (`parseDiffHunks`, `addedLines`). Here, and in `parseGrepZ` at line 183, it names the input bytes. A stranger reads it as an output parameter.

Quote: `QVector<LogRecord> parseLogNumstat(const QByteArray &out) {`

Remedy: rename the parameter to `gitOutput` in both functions and in `gitwrap.h:109,118`.

### Row 1211 — Low — `src/subsystemmap.cpp:152`

review-readability, lane 44-shared-utilities, Q3. **Disposition:** queued under ANTS-6148.

The header declares this parameter `sourcePath` (`subsystemmap.h:35`), and callers pass `resolveSource(...)`, which is usually docs/subsystems.md. The definition's `claudeMdPath` tells a stranger editing the body that the file is always CLAUDE.md.

Quote: `QVector<Lane> cachedLanes(const QString &claudeMdPath) {`

Remedy: use `sourcePath` in the definition.

### Row 1214 — Low — `src/prunedwalk.h:22`

review-readability, lane 44-shared-utilities, Q5. **Disposition:** queued, ANTS-6687.

Without `QDir::Hidden`, hidden *directories* are skipped too (line 37-38), so `pruneDir` and `onDir` are never called for `.github`, `.git` and similar. The comment mentions only hidden files. A stranger adding `.github` handling to a `pruneDir` callback would find it never fires.

Quote: `// Calls onFile(path) for each non-hidden file under \root\, never listing a`

Remedy: say "non-hidden files and directories".

### Row 1220 — Medium — `CMakeLists.txt:4623`

review-readability, lane 45-build-system, Q4. **Disposition:** queued, ANTS-6662.

Each bundle's ANTS-4797 comment says "a NEW define goes here", so readers will assume the per-test define blocks are grouped by bundle. Several sit under the wrong bundle: | Test | Its bundle | Where its define block sits | |---|---|---| | claude_context_meter (4623) | test_claude | test_core's section | | envelope_close_tag_scrub (4643) | test_claude | test_core's | | parked_feature_doc_truth (4…

Quote: `set_property(SOURCE tests/features/claude_context_meter/test_claude_context_meter.cpp`

Remedy: move each `set_property` block under the bundle that compiles its source.

### Row 1223 — Medium — `CMakeLists.txt:2550`

review-readability, lane 45-build-system, Q3. **Disposition:** queued, ANTS-6662.

One file often has several define names: - `terminalwidget.cpp`: `SRC_TERMINALWIDGET_PATH`, `_CPP_PATH`, `_IMPL_PATH`, `TERMINALWIDGET_CPP`. - `terminalwidget.h`: `_H_PATH`, `_HEADER_PATH`, `TERMINALWIDGET_H`. - `mainwindow.h`: `SRC_MAINWINDOW_H_PATH`, `MAINWINDOW_H`, `SRC_MAINWINDOW_H`. - `CMakeLists.txt`: `SRC_CMAKELISTS_PATH`, `CMAKELISTS_PATH`, `SRC_CMAKELISTS`, `CMAKELISTS_TXT`, `ANTS_QT_GUA…

Quote: `SRC_TERMINALWIDGET_IMPL_PATH="${CMAKE_SOURCE_DIR}/src/terminalwidget.cpp"`

Remedy: settle on one define name per file and say so beside the first block.

### Row 1231 — Low — `CMakeLists.txt:306`

review-readability, lane 45-build-system, Q5. **Disposition:** queued, ANTS-6662.

Under 4 cores the cap is nproc itself (1 on a 1-core host). The comment (268) and the cache help text (319) both promise `max(2, nproc/4)`. Nothing says why.

Quote: `if(_ANTS_NPROC LESS 4)`

Remedy: give the reason for the branch in a line, or make the help text state the real formula.

### Row 1238 — Medium — `tools/local-ci.sh:289`

review-readability, lane 46-tools-ci-gate, Q2. **Disposition:** queued, ANTS-6692.

The sanitizer leg (lines 228-380) is one block nested four deep. It mixes measurement, a CMake regeneration, an inline "how long ago" formatter (lines 296-310), a lock and a trap. The thread breaks here: the check that skips a marked tree comes after all the measuring, including actually re-running CMake at line 275. A stranger cannot tell whether that order is required. Five skip paths each prin…

Quote: `if [[ -f "$asan_marker" ]]; then`

Remedy: split it into named functions (measure edges, describe marker age, run leg), with the marker check first. Cost: unsure — `test_ci_workflow_runner.py:212` pins `ANTS_PUSH_GATE=1 python3 "$runner" run build-test` (outside this block), and `prepush_asan_gate` checks this leg's printed output, which a …

### Row 1239 — Low — `tools/qt62-guard.sh:341`

review-readability, lane 46-tools-ci-gate, Q2. **Disposition:** queued, ANTS-6692.

The script has two modes (compile-only, and `--run-job`, which runs the whole CI job), told apart by `run_job` checks at lines 105, 125, 146, 216, 341, 344, 381 and 388. A stranger changing the run-job path has to find all eight.

Quote: `[[ "$run_job" == 1 ]] && qt62_ccache="$PWD/.ccache-guard/$qt62_prefix-job"`

Remedy: group the per-mode settings into one block, as lines 112-131 already do for `job`. Cost: pinned — the ANCHORS pin `podman run --rm --security-opt label=disable`, `rc=$?` and `trap -` in this region, and `test_ci_guard_parity.py` reads the code lines.

### Row 1240 — Medium — `tools/qt62-guard.sh:213`

review-readability, lane 46-tools-ci-gate, Q3. **Disposition:** queued, ANTS-6692.

Every variable is named `qt62_*` and every image `*-baseline`, even under `--job build-test`, which uses Ubuntu 24.04 with Qt 6.4 and, with `--run-job`, runs the full test suite. The header still opens "the Qt 6.2 floor guard". A stranger reading `qt62_base`, or the image name `ants-ubuntu24-baseline`, would assume the value is Qt-6.2-specific and could change it for one job only.

Quote: `qt62_image="localhost/$qt62_prefix-baseline:$qt62_tag"`

Remedy: rename the variables to job-neutral names (for example `guard_image`) and retitle the header. Cost: pinned — ANCHORS `podman image exists "$qt62_image"` and `podman volume exists "$qt62_volume"`. `docs/specs/ANTS-3756-roadmap-store-schema.md` names `qt62_image`.

### Row 1241 — Low — `tools/perf-report.sh:205`

review-readability, lane 46-tools-ci-gate, Q3. **Disposition:** queued under ANTS-6150.

`COMPARABLE` is a three-way code: 0 = no baseline, 1 = compared, 2 = baseline from a different machine. No comment says so, and `--json` emits it as `"comparable": 2`. A stranger testing `COMPARABLE != 0` to mean "comparable", or a JSON consumer reading 2 as truthy, gets the opposite meaning.

Quote: `COMPARABLE=2`

Remedy: name the states (for example `compare_state=none|ok|other_machine`) or comment the values. Cost: one edit for the shell variable — Searched: `COMPARABLE` in docs/: none. Renaming the JSON key would be another matter; I did not identify its consumers.

### Row 1245 — Medium — `tools/ci_workflow.py:75`

review-readability, lane 46-tools-ci-gate, Q5. **Disposition:** queued, ANTS-6692.

The comment above says "The command runs unbounded", yet a `timeout` call whose first argument is an option still runs the real, time-limited `timeout`. No comment gives the reason. Today every ci.yml use is `timeout 25m …` (searched ci.yml), so the branch never fires. A stranger adding `timeout -k 10s 25m …` to ci.yml would get a bounded step locally, against the stated design (a timeout that fi…

Quote: ` PREAMBLE = ('timeout() { if [[ "${1:-}" == -* ]]; then command timeout "$@"; ' `

Remedy: comment why option-led calls keep the real `timeout`, or drop the branch. Cost: one edit — Searched: `timeout` in .github/workflows/ci.yml; no ANCHORS entry.

### Row 1247 — Low — `tools/ci_workflow.py:82`

review-readability, lane 46-tools-ci-gate, Q5. **Disposition:** queued, ANTS-6692.

The `True` key is there because YAML 1.1 reads `on:` as a boolean. Neither this line nor `docs_only` (line 250, `load().get(True)`) says so. A stranger would take `True` for a typo and delete it.

Quote: `WORKFLOW_KEYS = {"name", "on", True, "permissions", "concurrency", "env", "jobs"}`

Remedy: add a one-line comment naming the YAML `on:` quirk. Cost: one edit (comment only).

### Row 1256 — Medium — `tools/split-motion-check.py:136`

review-readability, lane 47-tools-roadmap-surveys, Q5. **Disposition:** queued, ANTS-6693.

The docstring (line 11) describes the allowed added lines in general terms ("a carved function's signature, braces or call"), and `--stem` accepts auditdialog and claudeintegration. In fact only MainWindow's two carved function families pass (here and `CARVED_CALL`, line 138). A stranger checking a split of another class would get every carved signature reported as "neither motion nor an allowed …

Quote: `r"void\s+(?:MainWindow::)?(?:appendToolSchemas|registerMcpProviders)\w*\s*\([^;{}]*\)\s*\{?")`

Remedy: a comment saying these are mainwindow's carves and another class's carve must add its names (or key them per stem in `STEMS`).

### Row 1257 — Medium — `tools/split-motion-check.py:977`

review-readability, lane 47-tools-roadmap-surveys, Q2. **Disposition:** queued, ANTS-6693.

For each block, `check_scrapes` does four jobs at nesting up to 9: 1. collects lambda callables (968-974); 2. runs a taint fixpoint (977-993); 3. evaluates windows (999-1024); 4. flags windows inside callables that are handed class text (1026-1040). The stranger loses the thread here. Nothing says what "tainted" means: a name whose binding reads the class text, directly or through another tainted…

Quote: `tainted: set[str] = set()`

Remedy: pull the taint fixpoint and the callable pass out into named functions.

### Row 1264 — Low — `tools/roadmap-slug-oracle.py:98`

review-readability, lane 47-tools-roadmap-surveys, Q5. **Disposition:** queued, ANTS-6693.

`archives` is explained below; `discovery` is not explained anywhere. A stranger adding a fixture could not tell whether it belongs in that skip list.

Quote: `if not os.path.isdir(root) or name in ("discovery", "archives"):`

Remedy: one comment.

### Row 1265 — Low — `tools/rc-namespace-scan.py:200`

review-readability, lane 47-tools-roadmap-surveys, Q5. **Disposition:** queued under ANTS-6152.

A seam on a block's opening line counts as open code, and one on its closing line counts as inside. That only makes sense if a seam is the first line of the next piece, and neither the `--seams` help nor the docstring says so.

Quote: `inside = [(a, b, nm) for a, b, nm in blocks if a < s <= b]`

Remedy: say what a seam line number denotes.

### Row 1266 — Low — `tools/measure/filemap_prototype.py:266`

review-readability, lane 47-tools-roadmap-surveys, Q5. **Disposition:** queued, ANTS-6693.

The collapse thresholds 0.6 and 0.8 (line 282) and the 12-file listing cap have no stated reason. A stranger tuning the collapse rule has nothing to go on.

Quote: `if n >= 0.6 * len(subs):`

Remedy: one comment each.

### Row 1267 — Low — `tools/split-motion-check.py:297`

review-readability, lane 47-tools-roadmap-surveys, Q5. **Disposition:** queued under ANTS-6153.

The exemption is for C++14 digit separators (`1'000`), but nothing says so. A stranger "simplifying" it would break masking.

Quote: `elif c == '"' or (c == "'" and not (i > 0 and src[i - 1].isalnum() and nxt.isalnum())):`

Remedy: one comment.

### Row 1270 — Low — `tools/measure/doc_symbols_locator_sample.py:21`

review-readability, lane 47-tools-roadmap-surveys, Q3. **Disposition:** queued, ANTS-6693.

`L` (cached lines of a file) and `R` (root plus slash, line 9, from an undocumented `ANTS_ROOT`) tell a stranger nothing.

Quote: `def L(path):`

Remedy: name them for what they hold, and mention `ANTS_ROOT` in the docstring.

### Row 1279 — Medium — `hooks/ants-bash-veto.sh:135`

review-readability, lane 48-hooks-packaging, Q5. **Disposition:** queued, ANTS-6672.

The comment at lines 107-110 argues that anchoring to the start of the whole command is "the obvious over-correction", because later stages of a pipeline run git too. On that ground the diff branch checks every segment, but the status/log branch still matches only at the start (`cd x && git status` passes). Nothing says whether this is deliberate. Someone would either "fix" it to match the diff b…

Quote: `"git status"|"git status "*|"git log --oneline"*|"git log -n"*"--oneline"*)`

Remedy: one line saying why status/log stay start-anchored, or segment-scan them too.

### Row 1280 — Medium — `packaging/release.sh:239`

review-readability, lane 48-hooks-packaging, Q5. **Disposition:** queued, ANTS-6672.

In `merge_unreleased`, `emit_section` prints the existing section's lead (`b` = "T") before the new [Unreleased] lead (`a` = "U"). `emit_cat` (lines 233-234) prints new before existing. The header promises only "new bullets first", so it is unclear whether the reversed lead order (old Theme line on top) is intended. The `a`/`b` and `"T"`/`"U"` naming is never explained. Someone tidying the awk wo…

Quote: `if (b != "") { print b; print "" }`

Remedy: one comment on why the lead keeps the existing text first, and name what `"T"`/`"U"` stand for.

### Row 1281 — Low — `packaging/obs/obs-submit.sh:219`

review-readability, lane 48-hooks-packaging, Q3. **Disposition:** queued under ANTS-6158.

Until this line `REV` holds `_service`'s pinned tag. From here on it is a display label used in the commit message (line 256) and an error message (line 225). Someone adding a git or tag operation on `$REV` below line 219 would pass it "sha as ver".

Quote: `REV="$STAGING_SHA as $STAGING_VERSION"`

Remedy: put the label in its own variable.

### Row 1282 — Low — `hooks/_common.sh:246`

review-readability, lane 48-hooks-packaging, Q3. **Disposition:** queued, ANTS-6672.

`ants_grep_nudge_throttled`, `_record`, `_dir`, `ANTS_GREP_NUDGE_THROTTLE_SEC` and `ANTS_GREP_NUDGE_KEY` also serve the cat/head/tail read-dump nudge (`ants-bash-veto.sh:60-66`). One throttle window covers both nudges. Someone tuning "the grep nudge" would also change the read nudge.

Quote: `ants_grep_nudge_throttled() {`

Remedy: rename to a source-nudge prefix, or say at line 243 that both nudge classes share it.

### Row 1291 — Low — `packaging/obs/obs-setup.sh:19`

review-readability, lane 48-hooks-packaging, Q3. **Disposition:** queued, ANTS-6672.

This overwrites the shell's exported `$USER`, so every `osc` child sees `USER=milnet`. Someone reading `$USER` later in the script, or in osc's environment, would expect the login user.

Quote: `USER="${OBS_USER:-milnet}"`

Remedy: rename to `OBS_MAINTAINER` or similar.

### Row 1292 — Low — `packaging/cc-job/cc-job:146`

review-readability, lane 48-hooks-packaging, Q3. **Disposition:** queued under ANTS-6159.

Inside `status_one`, the local `state` (a status word) sits beside the global `STATE` (the log directory) used three lines later. One mistyped case swaps a path for a word.

Quote: `local name=$1 state`

Remedy: rename the global to `JOB_DIR` or the local to `status`.
