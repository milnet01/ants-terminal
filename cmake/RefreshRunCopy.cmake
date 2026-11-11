# ANTS-6728 — after ants-mcpd links, refresh the copy launch.sh installs
# under ${XDG_DATA_HOME:-~/.local/share}/ants-terminal/bin, so a program
# outside Claude that runs `ants-mcpd --call` gets the new build without a
# terminal relaunch. Run as: cmake -DSRC=<built binary> -P RefreshRunCopy.cmake
#
# Only an existing copy is refreshed: a CI, OBS or packaging build has none,
# so this does nothing there. A failure warns and never fails the build.

set(base "$ENV{XDG_DATA_HOME}")
if(base STREQUAL "")
    set(base "$ENV{HOME}/.local/share")
endif()
get_filename_component(name "${SRC}" NAME)
set(dst "${base}/ants-terminal/bin/${name}")
if(NOT EXISTS "${dst}")
    return()
endif()

# cp to a staging name, then rename over: a running copy keeps its inode.
set(staging "${base}/ants-terminal/bin/.${name}.staging")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${SRC}" "${staging}"
                RESULT_VARIABLE rc)
if(rc EQUAL 0)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E rename "${staging}" "${dst}"
                    RESULT_VARIABLE rc)
endif()
if(NOT rc EQUAL 0)
    file(REMOVE "${staging}")
    message(WARNING "ANTS-6728: could not refresh ${dst}")
endif()
