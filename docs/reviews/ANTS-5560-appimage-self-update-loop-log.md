# ANTS-5560 — review loop log

The `review-contract` rows for [`docs/specs/ANTS-5560-appimage-self-update.md`](../specs/ANTS-5560-appimage-self-update.md).

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-09-29 | 2 lanes, each holding every question, headless via neutral-lane | 1 | 1 | 4 | 2 | 8 verified / 0 dismissed, all 8 fixed. Both lanes: the signing script's key inputs were unpinned, so the pipeline-round-trip test could never pass (the private key is `ANTS_UPDATE_SIGNING_KEY`, the public key an optional third argument); and the client had no rule for which AppImage to fetch while stable releases carry a version-less copy (now: verify the manifest first, fetch exactly `manifest.asset`). Also: a manual check during Restart later overwrote the "Restart to finish" indicator; `SwapInPlace` could not see where the temporary file sat. From settling open questions: linking libcrypto risked an AppImage handing the host's libssl an older libcrypto, breaking the HTTPS the check uses, so verification moves to libsodium (not measured; avoided); dropping every variable naming `$APPDIR` would have emptied `PATH`, now only its mount entries go; the `--mcpd` version mix during Restart later is stated. Resolved clean: the release runner is ubuntu-22.04, whose OpenSSL 3 has `-rawin`; the Ed25519 public key DER is a fixed 12-byte prefix plus 32 bytes (measured). Loop 2 dispatched. |
