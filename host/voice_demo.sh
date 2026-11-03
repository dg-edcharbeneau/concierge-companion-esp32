#!/bin/sh
# Talk the board through language changes out loud. See voice_demo.py.
#
#   ./voice_demo.sh                       Bulgarian and Spanish, from a reset
#   ./voice_demo.sh --languages fr,ja     any of es fr de hi ru pt ja it nl bg
#   ./voice_demo.sh --no-reset            the session already running
#
# Needs a Python with pyserial: python3 if it has it, otherwise the newest
# ESP-IDF environment under ~/.espressif, which always does. Nothing is built
# or flashed.
set -e
cd "$(dirname "$0")"

PY=""
if python3 -c "import serial" 2>/dev/null; then
    PY=python3
else
    for p in $(ls -d "$HOME"/.espressif/python_env/*/bin/python 2>/dev/null | sort -r); do
        if "$p" -c "import serial" 2>/dev/null; then PY="$p"; break; fi
    done
fi
if [ -z "$PY" ]; then
    echo "no Python with pyserial: install ESP-IDF, or pip install pyserial" >&2
    exit 1
fi

exec "$PY" voice_demo.py "$@"
