# Feature spec: a saved tab loads when it is first needed (ANTS-5225)

The contract is
[`docs/specs/ANTS-5225-lazy-tab-restore.md`](../../../docs/specs/ANTS-5225-lazy-tab-restore.md)
§ 5. This test checks each invariant there, as its § 7 assigns:

- **INV-1** and **INV-2** — behavioural: `SessionManager::peek` and
  `SessionManager::restore` run over the same corpus, one case per refusal
  in `restore`, plus V1, V2 and V3 streams it accepts. `peekSession` and
  `loadSession` are compared once on a saved file.
- **INV-6** — behavioural: two session files 40 days old, one named in
  `tab_order.txt`, under a temporary `XDG_DATA_HOME`.
- **INV-3**, **INV-4**, **INV-5** and **INV-7** — source scrapes, with
  comments stripped, over the `MainWindow` and `RemoteControl` sources.

Not covered: the 100 MiB file-size refusal, which needs a file that large.
