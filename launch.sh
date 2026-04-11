#!/bin/bash
exec /mnt/Storage/Scripts/Linux/Ants/build/ants-terminal "$@" 2>"${XDG_RUNTIME_DIR:-/tmp}/ants-terminal-$(id -u).log"
