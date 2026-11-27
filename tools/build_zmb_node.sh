#!/usr/bin/env bash
# Builds the integration test module process from the real lib/zmb sources (ASan/UBSan).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .pio/zmb_node
c++ -std=gnu++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Ilib/zmb lib/zmb/zmb.cpp lib/zmb/zmb_disc.cpp tools/zmb_node.cpp -o .pio/zmb_node/zmb_node
echo "$(pwd)/.pio/zmb_node/zmb_node"
