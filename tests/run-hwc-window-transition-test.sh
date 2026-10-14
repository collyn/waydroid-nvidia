#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror "$here/hwc-window-transition.cpp" -o "$out/test"
"$out/test"
echo 'PASS: resize window handoff, ambiguity, close and timeout'
