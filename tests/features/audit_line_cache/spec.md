# Feature: the audit dialog's file-line cache is bounded (ANTS-6520)

## Problem

An audit run reads each file a finding names, for its snippet and its
inline-suppression markers, and caches the lines for the rest of the run.
Each file was capped at 4 MiB, but the cache had no limit across files.

## Contract

`AuditLineCache` (`src/auditlinecache.h`) holds the lines of each file read.
`AuditDialog` keeps one, cleared at the start of each run and each render.

- **INV-1** A file read twice is read from disk once, and both reads return
  the same lines.
- **INV-2** The cache never holds more than `AuditLineCache::kMaxChars`
  characters. A file whose lines would take it past that clears the cache
  first, so the file just read stays cached.
- **INV-3** A file of `kMaxFileBytes` or more reads as one empty line; a file
  that cannot be opened reads as no lines.

## Tests

`test_audit_line_cache.cpp` drives the class on files in a temporary
folder. INV-2 reads twelve 3 MB files, 36 million characters in all.
