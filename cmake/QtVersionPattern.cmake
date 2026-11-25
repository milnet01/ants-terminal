# ANTS-4625 — the line a Qt header carries its version on. CMakeLists.txt's
# configure-time probe and QtVersionGuard.cmake's build-time read both include
# this file, so "found here" and "readable there" cannot disagree. Group 1 is
# the version.
set(ANTS_QT_VERSION_STR_REGEX
    "#[ \t]*define[ \t]+QT_VERSION_STR[ \t]+\"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
