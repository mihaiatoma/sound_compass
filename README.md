# Sound Compass

Points to a sound using the two microphones on a [Waveshare ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm).

![The six screens](docs/ui_preview.png)

## Caveats

- Keep the USB port at the bottom. With the cable at the side, the mics sit one above the other and it can't tell left from right.
- It can't tell front from back, or how far away a sound is.
- To zero it, press and hold the screen, then clap a few times about 30 cm past the top edge. Clapping in front of the screen won't work.

## How it works

A sound off to one side reaches one mic first, by up to 66 µs at the 22.7 mm spacing. That delay appears as a phase slope across frequency, which `main/doa.c` finds with noise-gated GCC-PHAT at 24 kHz, using 500 Hz to 7.2 kHz. Steady background sound, frames with no clear direction and single-frame outliers are rejected.

The 22.7 mm is the distance between the mic bodies in Waveshare's 3D model. The sound ports may sit slightly differently.

## Build and flash

ESP-IDF runs from Docker. Flashing runs on the computer, because Docker on macOS can't reach USB. Check the port with `ls /dev/cu.usbmodem*`.

```sh
tools/idf.sh idf.py build
tools/idf.sh idf.py merge-bin -o sound_compass_full.bin

# Once, before the first flash: back up the board
mkdir -p backup
uvx esptool --port /dev/cu.usbmodem1101 read-flash 0 ALL backup/original_firmware.bin

uvx esptool --port /dev/cu.usbmodem1101 write-flash 0x0 build/sound_compass_full.bin
```

Flashing the full image wipes the saved zero. To keep it, flash only the app with `write-flash 0x10000 build/sound_compass.bin`. To restore the backup, flash `backup/original_firmware.bin` at `0x0`.

## Live readings and tests

```sh
uv run --no-project --with pyserial tools/monitor.py   # live readings, saved to logs/

mkdir -p build_host
cc -O2 -std=c11 -Imain test/test_doa.c main/doa.c -lm -o build_host/test_doa && build_host/test_doa
cc -O2 -std=c11 -Imain test/test_turn.c main/turn.c main/doa.c -lm -o build_host/test_turn && build_host/test_turn
tools/idf.sh sh tools/preview/run.sh                   # renders the UI to docs/*.png
```

Made with [Claude Code](https://claude.com/claude-code).

Font: [Outfit](https://fonts.google.com/specimen/Outfit), SIL Open Font License 1.1.
