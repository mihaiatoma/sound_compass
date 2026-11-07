#!/bin/sh
# Renders the UI to docs/*.png. Runs inside the ESP-IDF Docker image, from the
# project root, after "idf.py reconfigure" has fetched LVGL into managed_components.
set -e
B=build_host/preview
L=managed_components/lvgl__lvgl
mkdir -p "$B" build_host/frames docs
FLAGS="-O1 -std=gnu11 -w -DLV_CONF_INCLUDE_SIMPLE -Itools/preview -I$L -I$L/src -Imain"
find "$L/src" -name '*.c' | while read -r f; do
  o="$B/$(echo "$f" | tr '/' '_').o"
  [ -f "$o" ] || echo "$f $o"
done | xargs -r -n2 -P8 sh -c 'gcc '"$FLAGS"' -c "$0" -o "$1"'
gcc $FLAGS tools/preview/preview.c main/ui.c main/fonts/font_digits_124.c main/fonts/font_word_28.c \
   main/fonts/font_small_21.c "$B"/*.o -lm -o build_host/preview_app
rm -f build_host/frames/*
build_host/preview_app
python3 -I tools/preview/to_png.py build_host/frames docs
