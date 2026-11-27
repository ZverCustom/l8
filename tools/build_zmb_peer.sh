#!/usr/bin/env bash
# Builds the interop peer from the real lib/zmb sources (with ASan/UBSan).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .pio/zmb_peer
c++ -std=gnu++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Ilib/zmb lib/zmb/zmb.cpp tools/zmb_peer.cpp -o .pio/zmb_peer/zmb_peer
echo "$(pwd)/.pio/zmb_peer/zmb_peer"
