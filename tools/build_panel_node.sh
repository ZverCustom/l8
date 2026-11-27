#!/usr/bin/env bash
# Builds the panel mode test process: lib/panel_core + lib/zmb + lib/panel_rx + the real
# l8proto/settings over the host test stubs (ASan/UBSan). ArduinoJson from PlatformIO libdeps.
set -euo pipefail
cd "$(dirname "$0")/.."
aj=.pio/libdeps/native_settings/ArduinoJson/src
if [ ! -d "$aj" ]; then ~/.platformio/penv/bin/pio pkg install -e native_settings >/dev/null; fi
mkdir -p .pio/panel_node
c++ -std=gnu++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Itest/support -Isrc -Ilib/v3_safety -Ilib/strict_json -Ilib/line_framer -I"$aj" \
  -Ilib/zmb -Ilib/panel_rx -Ilib/panel_core \
  lib/zmb/zmb.cpp lib/zmb/zmb_disc.cpp lib/panel_rx/panel_rx.cpp lib/panel_core/panel_core.cpp \
  tools/panel_node.cpp -o .pio/panel_node/panel_node
echo "$(pwd)/.pio/panel_node/panel_node"
