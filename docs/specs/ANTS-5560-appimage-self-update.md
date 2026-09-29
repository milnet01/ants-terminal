# ANTS-5560 — The AppImage updates itself

**Status:** draft (2026-09-29).
**Kind:** feature.
**Source:** ROADMAP.md ANTS-5560 (user-request-2026-09-29; decisions and the finbreak research recorded in the item's 2026-09-29 notes).
**Composes with:** `tests/features/update_available_menubar/spec.md` (the menu-bar indicator this reuses), ANTS-1318 (release channels in `release.yml`), finbreak's `docs/specs/FIBR-0054.md` (the design this follows).

**Layman:** When a new version is out, Ants tells you. One click downloads it, checks it really came from the project, and swaps it in. You then restart now, or keep working and get the new version the next time Ants opens.

## 1. Problem

`MainWindow::checkForUpdates` finds a newer release and shows "↗ Update vX available" on the menu bar. `MainWindow::handleUpdateClicked` then either opens the release page or hands `$APPIMAGE` to an external AppImageUpdate tool the user rarely has. Nothing in Ants downloads, verifies or installs an update, and no release is signed, so nothing could verify one.

## 2. Surface

### 2.1 What updates, and what does not

Only an AppImage updates itself: `$APPIMAGE` is set and names a regular file. Every other install (RPM, deb, Arch, Flatpak, a source build) keeps today's behaviour: the indicator opens the release page, and the package manager does the update.

### 2.2 The signed manifest

Each release uploads, beside `Ants_Terminal-<version>-x86_64.AppImage`, two files:

- `Ants_Terminal-<version>-x86_64.AppImage.manifest` — compact JSON:
  `{"format":1,"version":"<version>","asset":"<AppImage file name>","size":<bytes>,"sha256":"<64 hex>"}`.
- `….manifest.sig` — the raw 64-byte Ed25519 signature over the manifest file's exact bytes.

The signature covers the version, so an old signed build re-published under a newer tag is refused (§ 2.4). That is the gap finbreak left open (its FIBR-0169).

**Keys.** The public key is committed at `packaging/update-signing/ants-update.pub.pem`, and the build embeds that file, so the two cannot differ. Its DER form is the fixed 12-byte Ed25519 prefix `302a300506032b6570032100` followed by the 32-byte key; the verifier checks the prefix and uses the 32 bytes. The private key exists only as the GitHub secret `ANTS_UPDATE_SIGNING_KEY` (PEM).

### 2.3 Signing in the release pipeline

`release.yml` runs `tools/sign-update-manifest.sh <appimage> <version> [<public-key.pem>]` after the AppImage smoke test and before upload. The private key PEM comes from the environment variable `ANTS_UPDATE_SIGNING_KEY`; the public key defaults to the committed file. The script writes the manifest beside the AppImage, signs it with `openssl pkeyutl -sign -rawin`, and verifies the signature against the public key with `openssl pkeyutl -verify -pubin -rawin`. `-rawin` needs OpenSSL 3.0 or later; the release job's `ubuntu-22.04` runner has it. A missing secret, or a failed verify, fails the job: a release never ships unsigned. The upload step adds both files to the release, for RCs and stable alike. The `.zsync` files and `UPDATE_INFORMATION` stay as they are.

**Rollout.** The secret must be set before the first release after this lands. Builds older than this feature cannot update themselves; their users update once by hand.

### 2.4 The update flow

1. **Check.** As today, except it respects `update.check_on_startup` (default true; a Settings → General checkbox) and `update.skipped_version`. The startup check shows nothing for the skipped version; Help → Check for Updates ignores the skip. While a swapped update waits for a restart, no check replaces the "Restart to finish" indicator. The check keeps the release's notes and asset list for the dialog.
2. **Click.** On a self-updatable install, the indicator opens `UpdateDialog`, non-modal: "Version X is available (you have Y)", the release notes as plain text, and **Update now**, **Skip this version**, **Later**. Elsewhere it opens the release page.
3. **Download.** Update now fetches the release's `.manifest` and `.manifest.sig` assets first, verifies them (step 4's first three checks), and only then fetches the asset whose name equals `manifest.asset`. A stable release also carries a version-less copy, `Ants_Terminal-x86_64.AppImage`, which is never the one fetched. Everything travels over HTTPS only (`QNetworkRequest::NoLessSafeRedirectPolicy`, and any non-`https` URL is refused). A progress bar shows the AppImage download.
4. **Verify,** in this order, stopping at the first failure:
   - the signature verifies against the embedded key;
   - `manifest.version` equals the release tag without its `v`, and is newer than the running version;
   - `manifest.asset` is the asset downloaded;
   - the bytes received equal `manifest.size` and hash to `manifest.sha256`.
5. **Swap.** The AppImage streams to a temporary file in `$APPIMAGE`'s own directory, hashed as it arrives. After step 4 it is made `0755` and renamed over `$APPIMAGE`, which keeps the user's file name. A directory that is not writable is found before the download starts, and the dialog says so. Any failure removes the temporary file and leaves `$APPIMAGE` untouched.
6. **Restart.** The dialog then offers:
   - **Restart now** — warns that every tab and Claude Code session will close, then quits through the normal path (so the session is saved) and starts the new AppImage once this process has exited.
   - **Restart later** — nothing more happens. The running copy keeps its mounted image, and the new version starts at the next launch. Meanwhile anything started from the `$APPIMAGE` path, such as the `--mcpd` helper Claude Code launches, is the new version: the same state as a rebuilt `ants-mcpd` (ANTS-4932), which the About dialog already reports. The indicator reads "↻ Restart to finish updating to vX" until then, and clicking it offers Restart now again.

**The relaunch.** A detached `/bin/sh -c 'while kill -0 "$1" 2>/dev/null; do sleep 0.1; done; exec "$2"' sh <pid> <path>`, with paths as arguments, never spliced into the script. The environment drops `APPDIR`, `APPIMAGE`, `ARGV0` and `OWD`. In a colon-separated variable such as `PATH`, it drops only the entries under the old `$APPDIR`; any other variable whose value names it is dropped whole. That mount disappears when this process exits.

### 2.5 Where it lives

`src/selfupdate.{h,cpp}` holds the pure parts, which tests call directly: manifest parsing and verification (with the public key as a parameter), the install-kind check, the relaunch command and its environment. The download and the swap live there too, reached through `SelfUpdate::Session`. `src/updatedialog.{h,cpp}` is the dialog. `MainWindow` keeps the check and the indicator. `compareSemver` moves from `mainwindow.cpp` into `selfupdate`, so the check and the verifier compare versions one way. Ed25519 verification uses libsodium's `crypto_sign_verify_detached`, found with `pkg-config` (`libsodium`).

### 2.6 Reload

The dialog and the update settings are read when used, so they need no relaunch. Installing the new version does, which is why Restart later is always offered and never automatic.

### 2.7 Alternatives

- **Keep delegating to AppImageUpdate.** Rejected: few users have it, and it checks only the `.zsync` hashes, which prove the download matches the release, not that the release came from the project.
- **OpenSSL's libcrypto.** Rejected: Qt loads OpenSSL at run time for TLS, and an AppImage bundling the build machine's libcrypto could hand the host's newer libssl an older libcrypto under the same soname, breaking HTTPS — the channel the update check itself uses. Not measured; avoided rather than tested. libsodium is not loaded by Qt, so bundling it collides with nothing.
- **A vendored Ed25519.** Rejected: a vendored crypto routine is code this project would have to keep correct.
- **Sign only the AppImage, as finbreak does.** Rejected: that signature does not bind the version, so the downgrade in § 2.2 stays open.

## 3. Invariants

- **INV-1** — A manifest is accepted only if its signature verifies against the given key, its version equals the release tag's and is newer than the running version, and its asset names the file offered. Broken by skipping any one check. *Test:* `SelfUpdate.ManifestVerification` with a key pair made in the test: a correct manifest passes; a one-byte change to the manifest, a one-byte change to the signature, a version different from the tag, a version not newer, and a different asset name each fail with their own reason.
- **INV-2** — A download whose size or SHA-256 differs from the manifest is refused, its temporary file removed, and `$APPIMAGE` left byte-identical. Broken by trusting the transport. *Test:* `SelfUpdate.DownloadMismatchLeavesOriginal` feeds `Session` a fixture AppImage, a signed manifest and bytes with one flipped, then a short body, and compares the target before and after.
- **INV-3** — A verified update replaces `$APPIMAGE` in place, mode `0755`, from a temporary file in the same directory. Broken by writing elsewhere or copying across filesystems. *Test:* `SelfUpdate.SwapInPlace` asserts the target's new bytes and mode, that the temporary path `Session` reports sits in the target's directory, and that no temporary file remains.
- **INV-4** — Only an AppImage self-updates, and a non-writable directory is refused before any download. Broken by treating a package install as updatable. *Test:* `SelfUpdate.InstallKind` with `APPIMAGE` unset, set to a missing file, set to a file in a read-only directory, and set correctly.
- **INV-5** — Every URL the updater fetches is `https`, and redirects use `NoLessSafeRedirectPolicy`. Broken by following a redirect to `http`. *Test:* `SelfUpdate.HttpsOnly` asserts an `http` asset URL is refused before a request is made, and a source check asserts the redirect policy on the request the updater builds.
- **INV-6** — The relaunch waits for this process to exit, passes paths as arguments, and drops `APPDIR`, `APPIMAGE`, `ARGV0`, `OWD` and every variable naming the old `$APPDIR`. Broken by splicing a path into the script or inheriting the dead mount. *Test:* `SelfUpdate.RelaunchCommand` builds the command for a path holding a space and a quote, and an environment whose `PATH` holds a mount entry beside `/usr/bin`, and asserts the argv, the dropped variables, and a `PATH` of `/usr/bin` alone.
- **INV-7** — A skipped version shows nothing on the startup check and is still reported by Help → Check for Updates, and `update.check_on_startup` false stops the startup check. Broken by the skip hiding a manual check, or the setting being ignored. *Test:* `SelfUpdate.SkipAndStartupSetting` calls the decision function with each combination.
- **INV-8** — The committed public key is the one the build embeds, and `tools/sign-update-manifest.sh` produces a manifest the C++ verifier accepts. Broken by a key or format mismatch between the pipeline and the app. *Test:* `SelfUpdate.PipelineSignatureRoundTrip` generates a throwaway key pair with the `openssl` CLI, runs the script on a fixture file with `ANTS_UPDATE_SIGNING_KEY` holding the private key and the throwaway public key as its third argument, and verifies its output with the verifier given that public key; it skips with a message when `openssl` is absent or lacks `-rawin`. A second arm asserts the embedded key's bytes equal the committed file's.
- **INV-9** — `release.yml` signs and verifies before it uploads, fails without the secret, and uploads the manifest and signature. Broken by a release step order or upload list that ships an unsigned AppImage. *Test:* `SelfUpdate.ReleaseWorkflowSigns` reads `release.yml` and asserts the signing step sits between the smoke test and the upload, references `ANTS_UPDATE_SIGNING_KEY`, and that the upload list carries both new files.

## 4. RAM / build cost

The AppImage streams to disk and is hashed as it arrives, so memory holds one network buffer, not the file. The build gains two translation units and a link to libsodium; CI's images and the RPM, deb, Arch and Flatpak recipes gain its development package.

## 5. Out of scope

- Key rotation, and more than one trusted key.
- Updating an RC build onto the next RC: an RC build reports the same version as its stable release, so it is offered only a newer version.
- Rolling back after a bad update.
- Updating a package-manager install.

## 6. Tests

`tests/features/self_update/` holds this spec's tests.

| INV | Covered by |
|---|---|
| INV-1 | `SelfUpdate.ManifestVerification` |
| INV-2 | `SelfUpdate.DownloadMismatchLeavesOriginal` |
| INV-3 | `SelfUpdate.SwapInPlace` |
| INV-4 | `SelfUpdate.InstallKind` |
| INV-5 | `SelfUpdate.HttpsOnly` |
| INV-6 | `SelfUpdate.RelaunchCommand` |
| INV-7 | `SelfUpdate.SkipAndStartupSetting` |
| INV-8 | `SelfUpdate.PipelineSignatureRoundTrip` |
| INV-9 | `SelfUpdate.ReleaseWorkflowSigns` |

## 7. Cross-doc impact

- `tests/features/update_available_menubar/spec.md` keeps its invariants; the indicator gains the "Restart to finish" text.
- `docs/standards/dependencies.md` § 4 gains a floor row for libsodium, set to the version the Qt 6.2 floor image (Ubuntu 22.04) ships.
- `README.md` and the CHANGELOG describe updating; `packaging/README.md` names the signing secret.
- `docs/subsystems.md` and `.indie-review/partition.json` list the new files.

## Cold-eyes loop log

The rows are in [`../reviews/ANTS-5560-appimage-self-update-loop-log.md`](../reviews/ANTS-5560-appimage-self-update-loop-log.md).
