# roadmap_store_locate — feature-conformance contract (ANTS-4485)

The contract is `docs/specs/ANTS-4485-store-backed-locate.md` § 5. This file
names the cases in `test_store_locate.cpp` and how each was shown to bite.

Every store-backed case migrates a small ants-v1 fixture into a store under the
case's own `XDG_DATA_HOME`. "In the store, absent from the file" is made the way
it happens in use: the bullet is taken out of `ROADMAP.md` by hand after
migration.

| Case | Invariant | Asserts |
|---|---|---|
| `queryableIsWritable` | INV-1 | With `DEMO-0007` out of the file, `flip`, `flip_batch`, `set_body` and `amend_field` each succeed, and the item ends `shipped`. |
| `fileOnlyRefusesWithCause` | INV-2 | A bullet added to the file by hand and absent from the store refuses `bullet_not_found` with a message naming the store and `roadmap_migrate`; the file is unchanged. |
| `locatorPrecedenceHolds` | INV-3 | A `flip_batch` locator carrying `DEMO-0007`'s id and `DEMO-0003`'s headline flips `DEMO-0007` only. |
| `markdownUnchanged` | INV-4 | On a project the store does not serve, `flip`, `flip_batch`, `amend_body` and `amend_field` reply exactly as the golden files under `golden/` record, with the sandbox path replaced by `<ROOT>`. |
| `ambiguousHeadlineRefuses` | INV-5 | With both `Twin.` items out of the file, a headline locator refuses `bullet_ambiguous` and neither item changes. |
| `lineRangeRefusedEverywhere` | INV-6 | A lone `line_range` refuses `locator_unsupported` on `flip`, `flip_batch` and `amend_body`, `amend_field` refuses it, and the file is unchanged. |

## Verifying RED

INV-1, INV-2, INV-5 and INV-6 failed against the pre-change build. INV-3 and
INV-4 describe behaviour the change keeps, so they passed on both sides. Each
was shown to fail under a deliberate break instead: checking the headline
before the id reddens `locatorPrecedenceHolds`, and changing one field of a
markdown flip's reply reddens `markdownUnchanged`.

The golden files were recorded from the pre-change build, with
`ANTS_STORE_LOCATE_RECORD=1`. Recording them from a build that contains the
change would make INV-4 pass whatever the change did, so that variable is never
set when checking one.
