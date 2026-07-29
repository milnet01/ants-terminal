# Publishing Ants Terminal on the openSUSE Build Service (OBS)

OBS builds the RPM on openSUSE's servers and hosts a repository, so anyone can
`zypper install ants-terminal` and get updates automatically. You need a free
[build.opensuse.org](https://build.opensuse.org) account.

**Where it lives:** `home:milnet:ants-terminal` — its own sub-project, the same
shape finbreak uses (`home:milnet:finbreak`). That matters: each project owns its
own list of distributions, so adding a distro here cannot disturb OneUp
(`home:milnet`) or finbreak.

> Account note: the OBS project is under **`milnet`** (your OBS username), while
> the source is under **`milnet01`** on GitHub. Different accounts, on purpose.

## How the source gets there

You never upload a tarball. `_service` tells OBS to do it:

1. **`obs_scm`** runs on OBS's servers (they have network) and clones the tag
   pinned in `_service`.
2. **`tar` / `recompress` / `set_version`** run *inside the build VM* at build
   time, packing that clone into the tarball `Source0` expects.

`set_version` is the important one. The spec's `Version:` field tracks
`CMakeLists.txt`, which — because of the RC cadence — always names the *next*,
unreleased version. Left alone it would point at a tag that doesn't exist yet.
`set_version` overwrites it from the pinned tag, so the version is never typed by
hand and can't drift.

## The three scripts

| Script | When | What it does |
|---|---|---|
| `obs-setup.sh` | Once, or when changing distros | Creates/updates the project + package. **This is where the distro list lives.** |
| `obs-submit.sh` | Every release | Copies the recipe + spec into an `osc` checkout and commits, which triggers a rebuild. |
| `obs-status.sh` | After submitting | Polls until every build finishes, then prints the tail of any failing log. |

Each takes overrides via environment variables (`OBS_PROJECT`, `OBS_PACKAGE`, …)
and needs `osc` installed and logged in — run any `osc` command once and enter
your password.

The spec itself is **not** kept here. It lives at
`packaging/opensuse/ants-terminal.spec` — one file, also usable with plain
`rpmbuild`. `obs-submit.sh` copies it at submit time, which is what stops an
OBS-only copy quietly drifting from the repo's.

## Releasing a new version

1. Land the release and its tag as usual (`packaging/cut-rc.sh promote`).
2. Point `_service`'s `<revision>` at the new tag.
3. Run `packaging/obs/obs-submit.sh`, then `obs-status.sh`.

`obs-submit.sh` refuses to run if the tag it's pinned to doesn't exist yet —
otherwise the build fails at source fetch, which is an unpleasant thing to
diagnose from a build log.

## Adding a distribution

Add a `<repository>` block in `obs-setup.sh` and re-run it. Finding out whether a
distro *can* work is free: an unresolvable repository reports the exact missing
package immediately, without consuming any build time.

Two things in the spec currently block everything except openSUSE
(**ANTS-3727**):

- `BuildRequires: cmake(LayerShellQt) >= 6.0` is a hard requirement, even though
  `CMakeLists.txt` treats it as optional (`find_package(... QUIET)`, with a
  fallback for Wayland Quake-mode). Any distro without `layer-shell-qt6` fails on
  that line alone.
- `pkgconfig(lua5.4)` is openSUSE's spelling. Fedora ships `pkgconfig(lua)` at
  version 5.4, so it cannot resolve there.

Both need `%if 0%{?suse_version}` / `%else` arms before Fedora, Leap or Mageia
will build. See the
[cross-distribution howto](https://en.opensuse.org/openSUSE:Build_Service_cross_distribution_howto).

**Debian and Ubuntu are a bigger job.** OBS cannot build a `.deb` from a `.spec`;
it needs `debian.control` and `debian.rules` alongside it, which OBS's
`debtransform` turns into a source package. The repo has a `packaging/debian/`
tree already, but wiring it up is real work, not a checkbox — finbreak does this
with a committed `debian.obscpio`.

## Fully hands-off rebuilds (not set up)

OBS can rebuild automatically when you push a tag, via an `.obs/workflows.yml`
file. OneUp is set up this way. It is **not** wired up here, because it needs
credentials only you can create. The exact flow, from the OBS
[SCM/CI integration guide](https://openbuildservice.org/help/manuals/obs-user-guide/cha.obs.scm_ci_workflow_integration.html):

1. Create a GitHub personal access token — OBS uses it to report build status
   back onto commits. (This is the token OneUp needed.)
2. On OBS, mint a workflow token that wraps it:
   `osc token --create --operation workflow --scm-token <github-pat>`
   It returns a numeric **ID** and a **secret**; both are needed next.
3. In the GitHub repo, add a webhook under Settings → Webhooks:
   - Payload URL: `https://build.opensuse.org/trigger/workflow?id=<TOKEN_ID>`
   - Content type: `application/json`
   - Secret: the token secret from step 2
   - Events: Pushes (and Pull requests, if you want PR builds)
4. Commit `.obs/workflows.yml` with a `trigger_services` step filtered on
   `event: tag_push`.

One trap worth knowing before enabling it: `trigger_services` re-runs the
**existing** `_service`, which clones whatever `<revision>` that file pins. So a
tag push only rebuilds the right version if `_service` was updated to that tag
in the same push. Pushing a tag by hand without bumping `<revision>` silently
rebuilds the previous release.

## Once it's green

The repository is published at:

```
https://download.opensuse.org/repositories/home:/milnet:/ants-terminal/openSUSE_Tumbleweed/
```
