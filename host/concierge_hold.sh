#!/bin/sh
# Exercise components/concierge/concierge_hold.c on a laptop: when the device
# fills a desk wait, when it stays quiet, the no-repeat picker, the "promised to
# check" matcher, and the recovery nudge.
#
#   ./concierge_hold.sh
#
# The module is plain C over libc, so it compiles as is.
set -e
cd "$(dirname "$0")"

mkdir -p build
cc -O2 -std=gnu11 -Wall -Wextra -Werror -I../components/concierge \
   -o build/concierge_hold_test concierge_hold_test.c ../components/concierge/concierge_hold.c
./build/concierge_hold_test
