#!/bin/sh
# Check main/voices.c's catalog on a laptop: unique names, model ids that match
# their language, languages Flux STT can hear, and text that fits its buffers.
#
#   ./voices.sh             run the checks
#   HOST_LOG=1 ./voices.sh  ... and show what the module logs while doing it
#
# The real module is compiled; only NVS, logging and cJSON are stubbed.
set -e
cd "$(dirname "$0")"

mkdir -p build
cc -O2 -std=gnu11 -Wall -Wextra -Ivoices_stubs -I../main -include voices_stubs/sdkconfig.h \
   -o build/voices_test voices_test.c
./build/voices_test
