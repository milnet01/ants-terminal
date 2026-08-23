# ANTS-4625 — refuse a build whose objects were compiled against a different
# Qt than the one now installed.
#
# THE FAILURE THIS EXISTS FOR, measured 2026-08-23. zypper installed Qt 6.11.2
# at 10:20:34. RPM preserves UPSTREAM mtimes, so the headers it wrote carry
# stamps of 2026-05-11 and 2026-08-18 — older than the objects from the
# previous night's build. Ninja decides staleness by mtime, so it recompiled
# nothing and linked 6.11.2 libraries against objects built from 6.11.1
# headers. The handful of TUs edited for unrelated reasons DID recompile,
# against the new headers, and that MIX is the defect: our own types embed
# QString / QVector, so one TU ends up with a different idea of a type's size
# than another.
#
# The result was 859 of 3838 tests failing with heap corruption and crashes
# inside the copy constructors of our own record types — a shape that reads as
# a catastrophic code regression rather than a build-tree problem.
#
# WHY NEITHER EXISTING MECHANISM CATCHES IT. `cmake -B build` regenerates
# build.ninja (which is what fixes the separate, loud "libQt6*.so.6.11.1
# missing" link failure) but does not invalidate a single object. ccache never
# gets a chance, because ninja never invokes the compiler at all.
#
# And note the loud failure MASKS the quiet one: the missing-.so link error is
# self-explaining, so the reconfigure that clears it feels like the whole fix.
#
# Required -D vars:
#   EXPECTED_QT_VERSION — the Qt version recorded when this tree was configured
#   QCONFIG_FILE        — path to the installed Qt's qconfig.h

if(NOT DEFINED EXPECTED_QT_VERSION OR NOT DEFINED QCONFIG_FILE)
    message(FATAL_ERROR
        "QtVersionGuard.cmake: missing required -D vars "
        "(EXPECTED_QT_VERSION, QCONFIG_FILE)")
endif()

# A guard that cannot read its input must REFUSE. Reporting OK here would make
# "found nothing wrong" and "could not look" byte-identical to the caller,
# which is the one outcome a guard may never produce.
if(NOT EXISTS "${QCONFIG_FILE}")
    message(FATAL_ERROR
        "Qt version guard: cannot read '${QCONFIG_FILE}'.\n"
        "This build was configured against Qt ${EXPECTED_QT_VERSION}. The "
        "guard cannot confirm the installed Qt still matches, so it refuses "
        "rather than pass silently.\n"
        "If Qt moved, re-run cmake. If the tree is stale, run:\n"
        "    ninja -C <builddir> -t clean && cmake --build <builddir>")
endif()

file(READ "${QCONFIG_FILE}" _qconfig)
string(REGEX MATCH "#[ \t]*define[ \t]+QT_VERSION_STR[ \t]+\"([0-9]+\\.[0-9]+\\.[0-9]+)\""
       _matched "${_qconfig}")

if(NOT _matched OR "${CMAKE_MATCH_1}" STREQUAL "")
    message(FATAL_ERROR
        "Qt version guard: no QT_VERSION_STR in '${QCONFIG_FILE}'.\n"
        "The guard could not determine the installed Qt version, so it "
        "refuses rather than pass silently.\n"
        "If this tree is stale, run:\n"
        "    ninja -C <builddir> -t clean && cmake --build <builddir>")
endif()

set(_installed "${CMAKE_MATCH_1}")

# Compared in FULL, patch included. The incident was a patch bump (6.11.1 ->
# 6.11.2); a major.minor comparison would have waved through the exact change
# that caused it.
if(NOT _installed STREQUAL EXPECTED_QT_VERSION)
    message(FATAL_ERROR
        "Qt changed under this build tree: configured against "
        "${EXPECTED_QT_VERSION}, but ${_installed} is installed now.\n"
        "\n"
        "Ninja cannot see this on its own — RPM preserves upstream header "
        "mtimes, so the new headers can be OLDER than your objects and "
        "nothing is rebuilt. Linking new Qt against stale objects corrupts "
        "the heap at run time (ANTS-4625).\n"
        "\n"
        "Clean the tree and rebuild:\n"
        "    ninja -C <builddir> -t clean\n"
        "    cmake --build <builddir>\n"
        "\n"
        "A reconfigure alone is NOT sufficient: it regenerates build.ninja "
        "but invalidates no objects.")
endif()
