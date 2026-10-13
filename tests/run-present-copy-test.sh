#!/usr/bin/env bash
# Requires NVIDIA plus an AMD/Intel render node; tests actual copy and sampling.
# PRESENT_TEST_SOCKET optionally exercises the built vtest server transport.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
"${CC:-cc}" -O2 -Wall -Wextra -Werror -I"$here/../src" \
  "$here/present-copy.c" -ldl -lpthread -lEGL -lGLESv2 -lgbm -o "$out/test"
"$out/test"
