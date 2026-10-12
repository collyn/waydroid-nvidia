#!/bin/sh
# Requires depot_tools on PATH. Use the same pinned dependencies locally and in CI.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$repo/packaging/ci/pins.env"
mkdir -p "${1:?usage: checkout.sh WORKSPACE}"
cd "$1"
cat > .gclient <<'EOF'
solutions = [{
  "name": "angle",
  "url": "https://chromium.googlesource.com/angle/angle",
  "managed": False,
  "custom_vars": {
    "checkout_angle_cl_deps": False,
    "checkout_angle_dawn_deps": False,
    "checkout_angle_restricted_traces": False,
    "checkout_angle_mesa": False,
    "checkout_angle_partition_alloc": False
  }
}]
target_os = ["android"]
EOF
gclient sync --revision "angle@$ANGLE_SHA" --nohooks --shallow --no-history --jobs 8
gclient runhooks
