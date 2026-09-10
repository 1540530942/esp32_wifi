#!/usr/bin/env bash
# Build (and optionally flash) the ESP32-S3 firmware from WSL.
#   tools/build.sh          -> build only, log in build/build.log
#   tools/build.sh flash    -> build + flash over /dev/ttyACM0, log in build/flash.log
# Lives in the repo because WSL's /tmp does not survive a host sleep/restart.
set -u
export IDF_PATH=/home/archer/esp-idf
export IDF_PYTHON_ENV_PATH=/home/archer/.espressif/python_env/idf5.5_py3.12_env
export PATH=$IDF_PYTHON_ENV_PATH/bin:/home/archer/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin:$IDF_PATH/tools:$PATH
export ESP_ROM_ELF_DIR=/home/archer/.espressif/tools/esp-rom-elfs/20250809/
cd "$(dirname "$0")/../esp32_wifi_impl" || exit 2
mkdir -p build
if [ "${1:-}" = "flash" ]; then
  python "$IDF_PATH/tools/idf.py" -p /dev/ttyACM0 build flash > build/flash.log 2>&1
  echo "FLASH_EXIT=$?" >> build/flash.log
else
  python "$IDF_PATH/tools/idf.py" build > build/build.log 2>&1
  echo "BUILD_EXIT=$?" >> build/build.log
fi
