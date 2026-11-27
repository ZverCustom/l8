#!/usr/bin/env bash
# Builds the panel receive test process from the real lib/panel_rx sources (ASan/UBSan).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .pio/panel_rx_node
c++ -std=gnu++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Ilib/panel_rx lib/panel_rx/panel_rx.cpp tools/panel_rx_node.cpp -o .pio/panel_rx_node/panel_rx_node
echo "$(pwd)/.pio/panel_rx_node/panel_rx_node"
