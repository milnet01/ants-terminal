# Feature spec: `stable_id` accepts a digit-led project ID (ANTS-4849)

Status: shipped
Kind: fix
Source: cc-feedback-2026-09-03 (Vestige)

## Problem

`roadmap_log` with `id_strategy:"stable_prefix"` refused `stable_id:"3D_E-0682"`
as `bad_args`. The shape demanded a leading LETTER.

The defect is the ASYMMETRY, not the regex. On the same verb, the `id` locator
accepts `3D_E-0629` — `op:"flip"` uses it — and `id_prefix` deliberately allows
a leading digit so long as a letter appears somewhere; its own refusal names
`3D_E` as a valid example, one screen above. So one argument was stricter than
the two either side of it, for no reason visible from the schema, and the
project it locked out has used that dialect for hundreds of items.

The workaround left open was the counter strategy, which silently depends on
`.roadmap-counter` being in sync — where `stable_id` is the argument that
exists precisely to let the caller name the ID. A project whose counter had
drifted would get a colliding or skipped ID from the only route left.

## Surface

`rcdetail::rlStableIdShape()` — the shape in one place, declared in
`remotecontrol_internal.h`. Three sites carried a private copy: `op:"append"`'s
validator, `op:"append_batch"`'s, and `rlDetectStablePrefixId` (the sniffer
behind the missing-counter hint). Rule of Three, and the third copy is why the
hint was withheld from exactly the projects it was written for.

The guard's real purpose is kept: a value with no letter at all is still
refused, because no locator could tell a bare number from a counter ID. Both
refusal messages now say that, and name a leading digit as fine, instead of
printing a regex.

## Invariants

- **INV-1 append.** `op:"append"` accepts a digit-led `stable_id`, and the id
  the caller named is the id that lands.
- **INV-2 bare number refused.** A `stable_id` with no letter is still
  `bad_args`. The control: without it, "accept everything" passes INV-1.
  Passes in both states.
- **INV-3 batch agrees.** `op:"append_batch"` takes the digit-led bullet and
  still skips the bare-number one. The two ops carried separate copies of the
  shape, which is how they drift.
- **INV-4 the sniffer agrees**, checked through the behaviour it exists to
  produce: with no `.roadmap-counter` and a digit-led roadmap, the refusal
  names the id it found, so the caller is pointed at the stable_prefix
  strategy. Driven through the public verb rather than by calling the sniffer
  — `remotecontrol_internal.h` is off limits to a test (`RcTuSplit` INV-5
  keeps it to the `ANTS_RC_SOURCES` list), and the public path is the better
  subject anyway, being what a caller actually meets.

## Deliberately not done

`id_prefix`'s 16-character cap is not imported. That bounds a PREFIX; a
`stable_id` is a whole ID and is legitimately longer.
