#!/bin/sh
# Run ESP-IDF from Docker, so nothing is installed on the computer. The container
# sees only this project folder. Run from the project root:
#
#   tools/idf.sh idf.py build
#
# It cannot flash the board, because Docker on macOS has no access to USB ports.
exec docker run --rm -v "$PWD":/project -w /project -e HOME=/project/build_host/home \
    espressif/idf:v5.5.6 "$@"
