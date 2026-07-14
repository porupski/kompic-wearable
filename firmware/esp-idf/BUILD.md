# Kompic Mk I firmware — build & flash

Target chip: **ESP32-S3** (WROOM-1U-N16R8, 16 MB flash / 8 MB octal PSRAM).

## Once per new terminal

```
get_idf
cd ~/Projekti/Elektronika/Kompic-Wearable/kompic-wearable/firmware/esp-idf
```

`get_idf` is an alias for `. $HOME/.espressif/v5.5.2/esp-idf/export.sh` — set it up once in `~/.bashrc`.

## Common commands

```
idf.py menuconfig                        # same as the VS Code cog icon
idf.py build                             # compile
idf.py -p /dev/ttyACM0 flash monitor     # flash + serial; Ctrl-] to exit monitor
idf.py fullclean                         # wipe build/ and managed_components/
idf.py set-target esp32s3                # only if switching chip family
```

## VS Code ESP-IDF toolbar missing

Open **this folder** (`firmware/esp-idf/`) as the workspace root:
`File → Open Folder → firmware/esp-idf/`. The extension only detects the project
when `CMakeLists.txt` + `sdkconfig` sit at the workspace root.

## Required menuconfig settings

Set once after `set-target esp32s3`:

- **Serial flasher config → Flash size** → `16 MB`, mode `QIO`, speed `80 MHz`
- **Partition Table** → Custom → `partitions.csv`
- **Component config → ESP System Settings → CPU frequency** → `240 MHz`
- **Component config → ESP PSRAM** → SPI RAM support ON, mode `Octal`, speed `80 MHz`
- **Component config → FreeRTOS → Kernel** → `configTICK_RATE_HZ = 1000`
- **Component config → LVGL configuration → Font Usage → Enable built-in fonts → `from 8 to 48`, why not**
- **Component config → FAT Filesystem support → Long filename support → `Long filename buffer in heap`** (required for `s0058_r0001_annot.wav`-style names; the default is 8.3-only and fopen returns ENOENT for anything longer)

