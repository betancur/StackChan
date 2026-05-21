---
name: firmware-build
description: Build, flash, and monitor StackChan ESP-IDF firmware on a connected CoreS3. Auto-detects the serial port. Run from the firmware/ directory.
disable-model-invocation: true
---

All commands must be run from the `firmware/` directory.

## First-time setup

Fetch git-based component dependencies before the first build:

```bash
cd /Users/betancur/Developer/StackChan/firmware
python3 ./fetch_repos.py
```

Requires ESP-IDF v5.5.4 with IDF_PATH set. If `idf.py` is not in PATH, source the IDF export script first:
```bash
source $IDF_PATH/export.sh
```

## Build

```bash
cd /Users/betancur/Developer/StackChan/firmware
idf.py build
```

## Detect connected port

```bash
ls /dev/cu.usbserial* /dev/cu.SLAB* /dev/cu.wchusbserial* /dev/cu.usbmodem* 2>/dev/null
```

## Flash

```bash
cd /Users/betancur/Developer/StackChan/firmware
idf.py -p /dev/cu.<YOUR_PORT> flash
```

## Monitor serial output

```bash
cd /Users/betancur/Developer/StackChan/firmware
idf.py -p /dev/cu.<YOUR_PORT> monitor
```

Press `Ctrl+]` to exit monitor.

## Flash and monitor in one step

```bash
cd /Users/betancur/Developer/StackChan/firmware
idf.py -p /dev/cu.<YOUR_PORT> flash monitor
```

## Firmware version

Defined in `CMakeLists.txt` as `PROJECT_VER`. Currently **1.4.1**.
