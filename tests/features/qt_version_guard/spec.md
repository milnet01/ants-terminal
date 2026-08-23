# qt_version_guard — the Qt version a build was compiled against is checked

**ANTS-4625.** Contract for the configure-time stamp and build-time
comparison that stops a Qt upgrade silently poisoning an incremental
build tree.

## Why this exists

Measured 2026-08-23. `zypper` installed Qt 6.11.2 at 10:20:34. RPM
preserves upstream mtimes, so the headers it wrote carry stamps of
`2026-05-11` and `2026-08-18` — **older than the object files from the
previous night's build**. Ninja decides staleness by mtime, so it saw
nothing to do and recompiled nothing, then linked 6.11.2 libraries
against objects compiled from 6.11.1 headers. The few TUs edited for
unrelated reasons *did* recompile, against the new headers.

That mix is the defect. Our own types embed `QString` / `QVector`, so a
layout or inline change across the patch release gives one TU a
different idea of a type's size than another. The result was 859 of
3838 tests failing with heap corruption and crashes inside the copy
constructors of our own record types — a shape that reads as a
catastrophic code regression rather than a build-tree problem.

Neither a reconfigure nor ccache helps. `cmake -B build` regenerates
`build.ninja` (which fixes the separate stale-`.so`-path failure) but
does not invalidate objects; ccache never runs at all, because ninja
never invokes the compiler.

## Invariants

- **INV-1 — The guard script exists and is invocable.**
  *Breaks when:* the script is renamed or removed and the CMake wiring
  is left pointing at nothing. Without this case, INV-2 and INV-3 pass
  for the wrong reason: `cmake -P` on a missing file also exits
  non-zero, so an absent guard is indistinguishable from a working one
  that refused.
  *Test:* invoke the guard with a matching version pair; assert exit 0.

- **INV-2 — A matching version passes.**
  *Breaks when:* the comparison is inverted, or the version is parsed
  into an empty string that compares unequal to itself.
  *Test:* `EXPECTED_QT_VERSION=6.11.2` against the 6.11.2 fixture;
  assert exit 0.

- **INV-3 — A differing version REFUSES.**
  *Breaks when:* the guard warns instead of failing, or compares only
  the major.minor and lets a patch bump through — which is exactly the
  bump that caused the incident.
  *Test:* `EXPECTED_QT_VERSION=6.11.1` against the 6.11.2 fixture;
  assert non-zero exit.

- **INV-4 — The refusal names both versions and the remedy.**
  *Breaks when:* the message says only "Qt changed". The whole cost of
  the incident was the 25 minutes spent not knowing what to do; a
  refusal that does not name `-t clean` sends the reader back to the
  same guessing.
  *Test:* assert the failing output contains both version strings and
  the literal `-t clean`.

- **INV-5 — An unreadable or version-less header REFUSES, never passes.**
  *Breaks when:* the regex misses and the guard treats "found nothing"
  as "found no problem". Zero findings and zero confidence must not
  look identical.
  *Test:* run against `qconfig_no_version.h` and against a path that
  does not exist; assert non-zero for both.

- **INV-6 — The guard is wired into the build as an ALL target.**
  *Breaks when:* the script is correct and nothing invokes it. A guard
  that is never run is the failure mode this whole item exists to
  prevent, and it is invisible from the script's own tests.
  *Test:* scrape `CMakeLists.txt` for the guard target declared with
  `ALL` and for the `-P` invocation naming the script.
