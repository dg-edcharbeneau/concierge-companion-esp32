#!/bin/sh
# Exercise main/echo.c on a laptop: the sequence growing a step a round, the
# show's timing and its tones, the wait for the level callout and its timeout,
# game over and the best level, the same seed giving the same game, the lead
# held under agent speech, a resume after the panel, and a game played out to
# ECHO_MAX.
#
#   ./echo.sh
#
# The module is plain C with the time passed in, so it compiles as is.
set -e
cd "$(dirname "$0")"

mkdir -p build
cc -O2 -std=gnu11 -Wall -Wextra -I../main \
   -o build/echo_test echo_test.c ../main/echo.c
./build/echo_test
