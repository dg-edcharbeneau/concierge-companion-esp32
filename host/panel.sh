#!/bin/sh
# Exercise main/panel_geom.c on a laptop: every touch-panel control is hit where
# it is drawn, the gaps and rounded corners hit nothing, and the colour picker's
# dial holds exactly main/orb_colors.c's catalog, linked in with host stubs.
#
#   ./panel.sh            run the checks
#   ./panel.sh DIR        ... and write each page's shapes to DIR as PGM images
#
# panel_geom.c is plain C over libm; orb_colors.c takes voices_stubs/ for NVS,
# logging and cJSON.
set -e
cd "$(dirname "$0")"

mkdir -p build
cc -O2 -std=gnu11 -Wall -Wextra -Ivoices_stubs -I../main \
   -o build/panel_test panel_test.c ../main/panel_geom.c ../main/orb_colors.c -lm
./build/panel_test "$@"
