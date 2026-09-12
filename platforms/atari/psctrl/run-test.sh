#!/bin/sh
# Host test for the PSCTRL settings surface. Needs nothing from SDL,
# libdrm, mpg123 or the JIT - it runs on the build machine.
#
#   sh platforms/atari/psctrl/run-test.sh
#
# config_file_save.c is compiled with the C compiler on purpose: that is
# how the real build compiles it, and the extern "C" linkage between it
# and psctrl_settings.cpp is part of what this checks.
set -e
cd "$(dirname "$0")/../../.."
INC="-I. -Iinclude -Igpio -Ithreaddep -Isoftfloat -Ijit"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

${CC:-cc}   -std=gnu11  -Wall -D_GNU_SOURCE $INC -c -o "$TMP/save.o" \
            config_file/config_file_save.c
${CC:-cc}   -std=gnu11  -Wall -D_GNU_SOURCE $INC -c -o "$TMP/tun.o" \
            platforms/atari/psctrl/psctrl_tunables.c
${CXX:-c++} -std=gnu++17 -Wall -D_GNU_SOURCE $INC -c -o "$TMP/set.o" \
            platforms/atari/psctrl/psctrl_settings.cpp
${CXX:-c++} -std=gnu++17 -Wall -D_GNU_SOURCE $INC -c -o "$TMP/test.o" \
            platforms/atari/psctrl/test_psctrl_settings.cpp
${CXX:-c++} -o "$TMP/pstest" "$TMP/test.o" "$TMP/set.o" "$TMP/tun.o" "$TMP/save.o"
"$TMP/pstest"
