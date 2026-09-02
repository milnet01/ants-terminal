# No workflow expands `${{ }}` into a shell script body

**Why this exists.** GitHub's template engine substitutes `${{ ... }}` into the
`run:` text *before* any shell parses it. A value carrying `$(...)`, a
backtick or a `;` therefore stops being data and becomes script — in
`release.yml`'s case, inside a job holding `contents: write`.

**This class has now bitten twice, and the second time was the first fix's own
residue.** ANTS-4772 stopped `inputs.tag` being interpolated directly, routing
it through `env:`. But that step writes the value to `$GITHUB_OUTPUT`, and
eight later sites read it back as `${{ steps.tag.outputs.ref }}` — straight
into the script body. The injection was not closed, only laundered one hop:
zizmor graded the survivors *info* because its taint analysis does not follow a
value out through a step output and back in, so the severity signal pointed
away from the live path. ANTS-4792 closed all thirteen.

**Nothing was watching.** `zizmor`, `actionlint` and `yamllint` are all
available, and neither `.github/workflows/ci.yml` nor `tools/ci-parity.sh`
runs any of them — so between ANTS-4772 and ANTS-4792 there was no run, local
or remote, in which the surviving sites appeared. A guard belongs in the test
suite rather than in CI: the suite is what the pre-push hook gates on, and
adding a Rust binary to CI would oblige every packaging carrier that runs the
suite to install it, which is the ANTS-4391 / ANTS-4717 trap.

## The invariant

**INV-1** — in every workflow under `.github/workflows/`, no `${{` appears
inside a `run:` body, whether a block scalar (`run: |`) or the single-line
form. Values reach the shell through `env:`, where the runner passes them as
environment variables and no substitution into script text occurs.

**The check enumerates the directory, not a list of files.** ANTS-4717's
lesson is that guarding named carriers turns a class defect into a queue of
surfaces, each found by a later build. A workflow added tomorrow is covered on
the day it lands.

## Two deliberate strictnesses

**A comment inside a `run:` body is not exempt.** `# see ${{ inputs.tag }}`
looks inert, and is — until the expanded value contains a newline, at which
point everything after it is on a fresh line and is script. The engine expands
before the shell tokenises, so the shell's idea of a comment cannot protect
anything.

**`env:` and `with:` positions are not flagged.** There the substitution lands
in a YAML value the runner hands to the process as data; nothing parses it as
source. That is the whole reason the `env:` indirection is the fix, so
flagging it would forbid the remedy.

## Not asserted

**Whether the value is actually attacker-influenced.** That is per-site
judgement and it is what let thirteen live sites be graded *info*. The
invariant is the shape, not the provenance — a rule that no reader has to
adjudicate is a rule that still holds in five years.

**Anything else zizmor reports.** `excessive-permissions` is open against
`release.yml` and is a different finding with a different remedy.
