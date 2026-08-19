# read_regions_alias_conflict — two alias arrays refuse rather than one winning (ANTS-4512)

## Problem

`items`, `requests`, `paths` and `regions` are all accepted for the batch
key (ANTS-3500). When more than one was sent, the first in preference
order won **silently**.

A Games Hub session sent `paths` as bare filename strings and `regions`
as properly shaped `{path, start_line, end_line}` objects. `paths` won.
The reply was three copies of `bad_args: item missing "path"` — a message
that is *true of the array the verb chose and false of the array the
caller meant*, and which names neither. The natural next move is to
re-inspect `regions`, where nothing is wrong.

## Surface

- `src/remotecontrol_workspace.cpp` — `cmdReadRegions`: the alias
  resolution now records which key won, refuses when two or more arrays
  are supplied, and echoes `items_key` on success.

## Cases

| # | Asserts |
|---|---------|
| A1 | The reported call (`paths` bare strings + `regions` objects) refuses `bad_op_combo`, and the error names **both** keys. |
| A2 | A single alias still works, and the reply echoes `items_key: "regions"`. |
| A3 | The canonical key echoes as `items`, not as an alias. |

**Behavioural, not a source-grep.** The sibling `ReadRegions.ItemsKeyAliases`
(RR-7) is a windowed grep proving the alias *names* appear within 1500
chars of `cmdReadRegions`. That can never show which array a call
resolved to — which is the whole defect. Both tests are kept: RR-7 guards
the wiring, these guard the behaviour.

**Run red before trusting these.** Verified 2026-08-19 against the
pre-fix verb: A1 failed on the absent refusal, A2 and A3 on the absent
`items_key`.

## Would break this

- Naming only the winning key in the refusal. That repeats the original
  sin in a new place — A1 asserts both.
- Refusing but keeping preference order as a "fallback" when the shapes
  happen to agree. Then the refusal is conditional on data rather than on
  the call, and the same call succeeds or fails depending on file
  contents.
- Adding code ABOVE the alias loop that pushes `"requests"` / `"paths"` /
  `"regions"` past RR-7's 1500-char window from `RemoteControl::cmdReadRegions`.
  RR-7 then fails for a reason that has nothing to do with what changed —
  the byte-window trap this project has hit before.
- Dropping `items_key` as redundant once the refusal exists. The refusal
  covers the two-array case; `items_key` is what makes a per-item shape
  error interpretable in the ordinary one-array case.
