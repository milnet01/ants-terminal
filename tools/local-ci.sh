#!/usr/bin/env bash
# tools/local-ci.sh — Ants Terminal's push gate (ANTS-3580, ANTS-5542).
#
# The machine-wide hook (~/.claude/githooks/pre-push) runs this, through the
# shim in tools/hooks/pre-push. That hook owns everything about the PUSH: the
# secret scan, which files it changes, whether it is documentation-only, and
# whether this runs in the real checkout. This script owns what Ants checks.
# tools/setup-git-hooks.sh sets the hook's per-repo options.
#
#   tools/local-ci.sh           the full gate
#   tools/local-ci.sh --docs    the document checks only; nothing builds
#
# The hook passes the changed files in ANTS_PUSH_CHANGED, one per line. Unset
# means the range could not be worked out, so every leg runs.
#
# ANTS-5322 — the full gate RUNS ci.yml, through tools/ci_workflow.py, rather
# than a copy of it (local-gate.md § 3). The build-test job always: build, the
# whole suite, the packaging and shell lints, with the job's own env. The
# build-asan job when a sanitizer tree already exists and is warm enough to
# build inside a caller's timeout (ANTS-4118's cost gate, below). Then the two
# container compile guards over a warm cache. Not here: ci.yml's cppcheck job,
# which is informational, and a cold container leg. `tools/ci-parity.sh --full`
# runs every job.
#
# Documentation-only is decided by the hook, from ci.yml's push `paths-ignore`
# (ants.gate.docsCommand runs `tools/ci_workflow.py docs-only`). There is no
# second list here to drift. That decision is local: `paths-ignore` binds the
# `push` trigger only, so the same change opened as a PR runs CI in full.
set -uo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root" || exit 1

mode=full
case "${1:-}" in
    --docs) mode=docs ;;
    "") ;;
    *) echo "local-ci: unknown argument '$1' (expected --docs or nothing)" >&2; exit 2 ;;
esac

# ANTS-5322 — the pushed repo's own runner and ci.yml. PyYAML is its one
# dependency; without it there is no gate, and that is said, not skipped.
runner="$repo_root/tools/ci_workflow.py"
if [[ ! -f "$runner" ]] || ! python3 -c 'import yaml' 2>/dev/null; then
    echo "pre-push: cannot run ci.yml locally — $runner or PyYAML is missing." >&2
    echo "          Install python3-yaml." >&2
    exit 1
fi

# The files this push changes. Unset is "unknown", never "nothing": the
# compilable-source legs below then run rather than skip (prepush_range).
if [[ -n "${ANTS_PUSH_CHANGED+set}" ]]; then
    changed="$(printf '%s\n' "$ANTS_PUSH_CHANGED" | sed '/^$/d' | sort -u)"
    run_gate=0
else
    changed=""
    run_gate=1
fi

# A docs run on an unknown change set cannot know which checks apply, so it
# is not a docs run. The hook never asks for one; this is the backstop.
if [[ "$mode" == docs && "$run_gate" -eq 1 ]]; then
    echo "pre-push: --docs without ANTS_PUSH_CHANGED — running the full gate."
    mode=full
fi

# ANTS-4584 — README claim check, in both modes. A README-only push is
# docs-only, so leaving it to the full gate would exempt exactly the push most
# likely to introduce drift. Three greps against the code.
if [[ -x tools/check-readme-claims.sh ]]; then
    if ! readme_out="$(tools/check-readme-claims.sh 2>&1)"; then
        echo "$readme_out"
        echo "pre-push: README.md contradicts the code — fix it."
        exit 1
    fi
fi

# ANTS-4584 — the PROSE half, which no script can judge: does the README still
# read for someone who does not write software? Raised every 10th push rather
# than every one, because the answer changes slowly and a prompt that fires
# constantly stops being read. Advisory — it never blocks.
push_count_file="$(git rev-parse --git-common-dir)/ants-push-count"
push_n=$(( $(cat "$push_count_file" 2>/dev/null || echo 0) + 1 ))
echo "$push_n" > "$push_count_file" 2>/dev/null || true
if (( push_n % 10 == 0 )); then
    echo "pre-push: push #${push_n} — re-read README.md as a NEW USER."
    echo "          The numbers are checked above; this is about the prose:"
    echo "          jargon that crept in, a feature described but not shipped,"
    echo "          install steps nobody has run lately."
fi

if [[ "$mode" == docs ]]; then
    # ANTS-4726 — the evidence a wrong docs-only verdict needs, printed at the
    # moment it is acted on.
    echo "pre-push: documentation-only push — document checks only, nothing builds."
    echo "          decided on these paths:"
    printf '%s\n' "$changed" | head -20 | sed 's/^/            /'
    [[ "$(printf '%s\n' "$changed" | wc -l)" -gt 20 ]] \
        && echo "            … and more"
    # Each check runs only when the push touches what it checks.
    doc_rc=0
    if grep -qx 'ROADMAP.md' <<<"$changed"; then
        tools/check-roadmap.sh || doc_rc=1
    fi
    if grep -q '^docs/standards/' <<<"$changed"; then
        tools/check-standard-mirrors.sh || doc_rc=1
        tools/check-standards-index.sh || doc_rc=1
    fi
    # These tests read CLAUDE.md's text. Run them against the existing build/ —
    # they read the file at run time, so no rebuild is needed.
    if grep -qx 'CLAUDE.md' <<<"$changed"; then
        if [[ -f build/CTestTestfile.cmake ]]; then
            if ! ctest_out="$(ctest --test-dir build --output-on-failure \
                -R '^(McpSubsystem\.WiringContract|McpOutputSanitisationWiring\.ConventionDocumentedInClaudeMd)$' 2>&1)"; then
                doc_rc=1
            fi
            printf '%s\n' "$ctest_out" | tail -15
        else
            echo "pre-push: no build/ — CLAUDE.md content tests skipped."
        fi
    fi
    if [[ "$doc_rc" -ne 0 ]]; then
        echo "pre-push: a document check failed — fix it."
        exit 1
    fi
    echo "pre-push: document checks green."
    exit 0
fi

# ANTS-5322 — ci.yml's build-test job, as written: configure, BUILD, the whole
# suite (e2e and perf included, as CI runs them), and the lints. The hook used
# to test the existing build/ without building, because a build-minute stamp
# recompiled the two largest TUs every time; ANTS-3582 moved that stamp into
# its own TU, so an incremental build is the cost of what changed.
# ANTS-5375 — tells ci_workflow.py this is the push gate; ci.yml's perf step
# then skips here and runs on GitHub only (user ruling, 2026-09-26).
#
# The job runs in CI's own image (ubuntu 24.04: its GCC, mold and Qt). The
# image is the match: this box's newer Qt passed ANTS-5479's button test while
# every CI run failed it. A cold image never runs inside a push
# (local-gate.md § 9), and falling back to this machine let three CI-only
# failures through (ANTS-5614). So a cold image BLOCKS the push and names the
# warm-up; with no podman at all the job runs here and the image leg is
# declared skipped (local-gate.md § 7.1).
# ANTS_PREPUSH_NO_UBUNTU24=1 keeps the job on this machine for one push.
job_in_image=0
if [[ -z "${ANTS_PREPUSH_NO_UBUNTU24:-}" && -x tools/qt62-guard.sh ]] \
   && command -v podman >/dev/null 2>&1; then
    if ! tools/qt62-guard.sh --job build-test --run-job --check-warm; then
        echo >&2
        echo "pre-push: CI's build-test image is cold or stale — push blocked." >&2
        echo "          Warm it (about 20 min), then push again:" >&2
        echo "            tools/qt62-guard.sh --job build-test --run-job" >&2
        echo "          ('git push --no-verify' to override.)" >&2
        exit 1
    fi
    job_in_image=1
elif [[ -z "${ANTS_PREPUSH_NO_UBUNTU24:-}" && -n "${ANTS_GATE_SKIPPED:-}" ]]; then
    echo "build-test in CI's image (podman or tools/qt62-guard.sh missing)" \
        >> "$ANTS_GATE_SKIPPED"
fi

# Only a push that changes something COMPILABLE can break a container compile
# guard, so a docs- or packaging-only push skips both guard legs below.
# run_gate=1 means we could not compute a precise range, so we cannot rule
# out a source change — run them rather than skip on uncertainty.
compilable=""
if [[ "$run_gate" -eq 0 && -n "$changed" ]]; then
    # `.cpp.in` / `.h.in` count: a template that generates a compiled TU can
    # break the floor exactly as the generated file would.
    compilable="$(grep -E '\.(c|cc|cpp|cxx|h|hpp|hxx)(\.in)?$|(^|/)CMakeLists\.txt$|\.cmake$' \
                  <<<"$changed" || true)"
else
    compilable="(range unknown)"
fi

# A cold Qt 6.2 cache BLOCKS the push, as a cold build-test image does above.
# Skipping it let ANTS-5381's Qt 6.3-only QCryptographicHash::addData overload
# through three CI-red pushes (2026-10-03 to 2026-10-05). Checked here, before
# the long build-test job, so a cold cache costs seconds, not that job.
if [[ -n "$compilable" && -z "${ANTS_PREPUSH_NO_QT62:-}" && -x tools/qt62-guard.sh ]]; then
    if command -v podman >/dev/null 2>&1; then
        if ! tools/qt62-guard.sh --check-warm; then
            echo >&2
            echo "pre-push: the Qt 6.2 floor cache is cold or interrupted — push blocked." >&2
            echo "          Warm it (about 11 min), then push again:" >&2
            echo "            tools/qt62-guard.sh" >&2
            echo "          ('git push --no-verify' to override.)" >&2
            exit 1
        fi
    elif [[ -n "${ANTS_GATE_SKIPPED:-}" ]]; then
        echo "qt62-baseline compile guard (podman missing)" >> "$ANTS_GATE_SKIPPED"
    fi
fi

if [[ "$job_in_image" == 1 ]]; then
    echo "pre-push: running ci.yml's build-test job in CI's image (tools/qt62-guard.sh --run-job)…"
    echo "          bypass with 'git push --no-verify'; every job: tools/ci-parity.sh --full"
    ANTS_PUSH_GATE=1 tools/qt62-guard.sh --job build-test --run-job
else
    echo "pre-push: running ci.yml's build-test job on this machine (tools/ci_workflow.py)…"
    echo "          CI's image is not used, so this is this box's Qt and compiler, not CI's."
    echo "          bypass with 'git push --no-verify'; every job: tools/ci-parity.sh --full"
    ANTS_PUSH_GATE=1 python3 "$runner" run build-test
fi
job_rc=$?
if (( job_rc != 0 )); then
    echo >&2
    echo "pre-push: ci.yml's build-test job FAILED locally — push blocked." >&2
    echo "          Fix the failure above (or 'git push --no-verify' to override)." >&2
    exit 1
fi

# --- build-asan leg (ANTS-3761 follow-up) -----------------------------------
# The Release suite above cannot see a sanitizer-only failure, and one reached
# GitHub: CI run 30587366963 went red on an ASan-only RSS measurement while
# build-test stayed green. So mirror ci.yml's build-asan job too — but only
# against a sanitizer tree that ALREADY exists. Configuring one from cold is a
# 20-minute build, which is how a hook gets bypassed; an incremental build over
# a warm tree is the cost of the files you actually touched.
#
# ANTS-5322 — the job runs as ci.yml writes it: its build, its sanitized
# suite with its own flags and env, and its smoke step.
#
# Opt out for one push with ANTS_PREPUSH_NO_ASAN=1.
# ci.yml's build-asan job builds build-asan/, so that is the tree measured.
asan_dir=""
if [[ -f build-asan/CMakeCache.txt ]] &&
   grep -q '^ANTS_SANITIZERS:BOOL=ON$' build-asan/CMakeCache.txt 2>/dev/null; then
    asan_dir="build-asan"
fi

if [[ -n "${ANTS_PREPUSH_NO_ASAN:-}" ]]; then
    echo "pre-push: build-asan leg skipped (ANTS_PREPUSH_NO_ASAN set)."
elif [[ -z "$asan_dir" ]]; then
    echo "pre-push: ⊘ no sanitizer build tree — the build-asan gate did NOT run."
    echo "          CI still runs it. To cover it locally, once:"
    echo "            cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DANTS_SANITIZERS=ON"
    echo "          or run the complete mirror: tools/ci-parity.sh --full"
else
    # ANTS-4118 — cost gate. "A tree that already exists" is not the same as
    # "a tree that is warm": after a pull or a header touch the incremental
    # build is minutes long, and a caller with a command timeout (an agent
    # harness caps at 600 s) gets SIGTERMed mid-ninja. That costs twice — the
    # push fails, AND a killed ninja leaves the tree in the state this project
    # treats as untrustworthy. So measure the pending work and refuse a build
    # that will not fit, rather than starting one that may be killed. A warm
    # tree — the common case while iterating — is a handful of edges and runs.
    asan_marker="$asan_dir/.ants-prepush-interrupted"
    asan_max_edges="${ANTS_PREPUSH_ASAN_MAX_EDGES:-25}"

    # `ninja -n` lists the edges it WOULD run, in `[i/n] …` form, and costs
    # milliseconds. Unmeasurable (no ninja, no build.ninja) counts as unsafe:
    # an unmeasured build is exactly the one that gets killed. stderr is kept —
    # ninja reports a damaged deps file there, and that is a reading too.
    asan_dry=""
    asan_edges=""
    if command -v ninja >/dev/null 2>&1 && [[ -f "$asan_dir/build.ninja" ]]; then
        asan_dry="$(ninja -C "$asan_dir" -n 2>&1 || true)"
        asan_edges="$(grep -cE '^\[[0-9]+/[0-9]+\]' <<<"$asan_dry" || true)"

        # ANTS-4536 — a pending regen hides every real edge behind it, so the
        # dry run reads `[0/1] Re-running CMake...` for what may be a full
        # rebuild. Treating that as unmeasurable stood the leg down on EVERY
        # push touching CMakeLists.txt — the change most likely to introduce a
        # sanitizer-visible defect, and the same shape ANTS-4131's Qt-floor
        # guard exists to close. The regen is a CMake re-run, not a build, so
        # run it and measure what it reveals. Only a regen that fails, or one
        # that does not clear, is genuinely unmeasurable.
        if grep -q 'Re-running CMake' <<<"$asan_dry"; then
            echo "pre-push: build-asan has a pending CMake regeneration, which"
            echo "          hides the real edge count. Running the regen to"
            echo "          measure it (a CMake re-run, not a build)."
            if ninja -C "$asan_dir" build.ninja >/dev/null 2>&1; then
                asan_dry="$(ninja -C "$asan_dir" -n 2>&1 || true)"
                asan_edges="$(grep -cE '^\[[0-9]+/[0-9]+\]' <<<"$asan_dry" || true)"
                if grep -q 'Re-running CMake' <<<"$asan_dry"; then
                    asan_edges=""
                fi
            else
                echo "pre-push: the regen itself failed — treating the tree as"
                echo "          unmeasurable."
                asan_edges=""
            fi
        fi
    fi

    if [[ -f "$asan_marker" ]]; then
        # ANTS-4943 — the marker never expires, and this skip is one line
        # inside a long run that callers commonly tail. A marker written days
        # ago printed identically to one written this session, so the leg
        # stayed dark across every push in between while each still ended
        # "push allowed". Expiring it is deliberately NOT the repair — healing
        # the tree stays the caller's call — but the age has to be visible.
        asan_marker_age="age unknown"
        marker_epoch="$(stat -c %Y "$asan_marker" 2>/dev/null || true)"
        if [[ -n "$marker_epoch" ]]; then
            marker_secs=$(( $(date +%s) - marker_epoch ))
            (( marker_secs < 0 )) && marker_secs=0
            if (( marker_secs >= 86400 )); then
                marker_n=$(( marker_secs / 86400 )); marker_unit=days
            elif (( marker_secs >= 3600 )); then
                marker_n=$(( marker_secs / 3600 )); marker_unit=hours
            else
                marker_n=$(( marker_secs / 60 )); marker_unit=minutes
            fi
            (( marker_n == 1 )) && marker_unit="${marker_unit%s}"
            asan_marker_age="$marker_n $marker_unit ago"
        fi
        echo "pre-push: ⊘ build-asan gate SKIPPED — an earlier run was killed"
        echo "          mid-build in $asan_dir $asan_marker_age, so an"
        echo "          incremental result over it"
        echo "          is a false pass, not a cheap one. Heal the tree and clear"
        echo "          the marker in one go:"
        echo "            cmake --build $asan_dir --clean-first && rm $asan_marker"
        echo "          CI runs build-asan nightly, not on push (ANTS-5343): until"
        echo "          then nothing sanitizes this push. To run it on GitHub now:"
        echo "            gh workflow run CI --ref main"
    elif [[ -z "$asan_edges" ]]; then
        # No ninja, no build.ninja, or a regen that would not resolve. An
        # unmeasured build is the one a caller timeout kills.
        echo "pre-push: ⊘ build-asan gate SKIPPED — cannot measure pending work"
        echo "          in $asan_dir (no ninja / no build.ninja, or a CMake"
        echo "          regeneration that would not resolve), and an unmeasured"
        echo "          build is the one a caller timeout kills."
        echo "          Run it deliberately: tools/ci-parity.sh --full."
    elif (( asan_edges > asan_max_edges )); then
        echo "pre-push: ⊘ build-asan gate SKIPPED — $asan_dir has $asan_edges"
        echo "          pending build steps, over the $asan_max_edges cap"
        echo "          (ANTS_PREPUSH_ASAN_MAX_EDGES). That is a cold or stale"
        echo "          tree: minutes of build, which a caller timeout would kill"
        echo "          mid-ninja. Run it yourself (cmake --build $asan_dir) or"
        echo "          tools/ci-parity.sh --full. CI runs build-asan nightly, not"
        echo "          on push (ANTS-5343): until then nothing sanitizes this"
        echo "          push. To run it on GitHub now: gh workflow run CI --ref main"
    else
        echo "pre-push: build-asan gate ($asan_edges pending build steps +"
        echo "          sanitized suite in $asan_dir) — expect a few minutes."
        echo "          Skip just this leg with ANTS_PREPUSH_NO_ASAN=1."
        # ANTS-4118 — ninja's "premature end of file; recovering" on the deps
        # log is reported, NOT gated on. Measured 2026-08-12: it survived a
        # full `--clean-first` rebuild of this repo's build-asan, so it is a
        # property of the on-disk log rather than a per-run signal, and a gate
        # keyed to it would never clear — permanently disabling the very leg
        # this cost gate exists to keep running. Recovery also errs toward
        # rebuilding more (a dropped dep record reads as dirty), not less.
        if grep -q 'premature end of file' <<<"$asan_dry"; then
            echo "          note: ninja is recovering a truncated deps log in"
            echo "          $asan_dir. Harmless (it rebuilds more, not less);"
            echo "          'cmake --build $asan_dir --clean-first' clears it."
        fi
        # If the caller's timeout kills us anyway, mark the tree so the NEXT
        # run refuses to trust an incremental build over a killed ninja.
        trap 'touch "$asan_marker"
              echo >&2
              echo "pre-push: interrupted during the sanitizer build — $asan_dir" >&2
              echo "          marked untrusted; the next run will say how to heal it." >&2
              exit 143' TERM INT
        if ! ANTS_PUSH_GATE=1 python3 "$runner" run build-asan; then
            echo >&2
            echo "pre-push: ci.yml's build-asan job FAILED locally — push blocked." >&2
            echo "          This is the class that reached CI as run 30587366963." >&2
            echo "          ('git push --no-verify' to override.)" >&2
            exit 1
        fi
        trap - TERM INT
    fi
fi

# --- qt62-baseline leg (ANTS-4131) ------------------------------------------
# The Qt 6.2 floor guard. This dev box runs a much newer Qt, so an API newer
# than the floor compiles here, passes the full suite here, and passes both
# legs above — and breaks only in CI. ANTS-4108 shipped
# QRegularExpressionMatch::hasCaptured() (Qt 6.3+) and went red on three
# consecutive pushes for exactly that reason.
#
# The leg used to be excluded because it cost ~25 min. ANTS-4131 caches the apt
# layer as an image and the build tree as a podman volume, and measured on this
# host 2026-08-12 a warm run is 5-7 s, with a floor violation rejected in ~1 s.
# At that price it belongs here.
#
# Scope: `compilable`, computed above. --warm-only guarantees the hook never
# pays the ~11 min cold build; a cold cache has already blocked the push above,
# so here it can only skip when podman is missing.
#
# Opt out for one push with ANTS_PREPUSH_NO_QT62=1.

if [[ -n "${ANTS_PREPUSH_NO_QT62:-}" ]]; then
    echo "pre-push: qt62-baseline leg skipped (ANTS_PREPUSH_NO_QT62 set)."
elif [[ ! -x tools/qt62-guard.sh ]]; then
    echo "pre-push: ⊘ tools/qt62-guard.sh missing — Qt-floor guard did NOT run."
else
    if [[ -z "$compilable" ]]; then
        echo "pre-push: qt62-baseline leg skipped — no compilable source in this push."
    elif ! tools/qt62-guard.sh --warm-only; then
        echo >&2
        echo "pre-push: Qt 6.2 FLOOR violation — push blocked." >&2
        echo "          The code compiles on this box's Qt but not on the 6.2 floor" >&2
        echo "          (dependencies.md § 4). This is the class that broke CI three" >&2
        echo "          times on ANTS-4108; only this guard sees it locally." >&2
        echo "          ('git push --no-verify' to override.)" >&2
        exit 1
    fi
fi

# --- build-test toolchain leg -------------------------------------------------
# The same guard, pointed at build-test's runner: ubuntu 24.04, GCC 13, mold.
# This box's GCC 16 links targets that GCC 13 + mold cannot — an under-linked
# static-archive consumer links here and fails there. bench_partition_walk went
# red on three consecutive CI pushes that way while every local leg was green.
# Same scope and --warm-only contract as the leg above.
#
# Opt out for one push with ANTS_PREPUSH_NO_UBUNTU24=1.
if [[ "$job_in_image" == 1 ]]; then
    echo "pre-push: build-test toolchain leg already covered — the job ran in CI's image."
elif [[ -n "${ANTS_PREPUSH_NO_UBUNTU24:-}" ]]; then
    echo "pre-push: build-test toolchain leg skipped (ANTS_PREPUSH_NO_UBUNTU24 set)."
elif [[ ! -x tools/qt62-guard.sh ]]; then
    echo "pre-push: ⊘ tools/qt62-guard.sh missing — build-test toolchain guard did NOT run."
elif [[ -z "$compilable" ]]; then
    echo "pre-push: build-test toolchain leg skipped — no compilable source in this push."
elif ! tools/qt62-guard.sh --job build-test --warm-only; then
    echo >&2
    echo "pre-push: build FAILED under CI's build-test toolchain — push blocked." >&2
    echo "          It builds with this box's compiler but not with ubuntu 24.04's" >&2
    echo "          GCC 13 + mold. Check the error above; a link error usually" >&2
    echo "          means a target needs the full --start-group library set." >&2
    echo "          ('git push --no-verify' to override.)" >&2
    exit 1
fi

echo "pre-push: correctness suite green — push allowed."
exit 0
