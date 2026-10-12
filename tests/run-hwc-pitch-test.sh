#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror "$here/hwc-pitch.c" -o "$out/test"
"$out/test"
echo "PASS: byte-pitch admission"
