#!/bin/sh
# Host test for the IDE model (no Pi needed). From the repo root:
#   sh platforms/atari/ide_test/run.sh
set -e
cd "$(dirname "$0")/../../.."
cc -O1 -w -I. -Igpio -o /tmp/ide_range_test platforms/atari/ide_test/ide_range_test.c \
   platforms/atari/idedriver.c platforms/atari/disk_order.c
/tmp/ide_range_test
