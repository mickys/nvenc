#!/bin/bash
# Fetch NVIDIA's public headers into vendor/ (gitignored), at the versions this was written against.
#   open-gpu-kernel-modules 610.57.04  RM API (objects, controls, channels, memory); the RM inside nullmoth's driver 2.3.x
#   open-gpu-doc                       class docs: clc9b7.h (Ada NVENC), nvenc_drv.h (engine data structures)
set -euo pipefail
cd "$(dirname "$0")/.."; mkdir -p vendor
clone() {   # clone <dir> <url> <rev> [sparse paths...]
  local d=vendor/$1 url=$2 rev=$3; shift 3
  if [ ! -e "$d" ]; then
    git clone -q --filter=blob:none --no-checkout "$url" "$d"
    [ $# -gt 0 ] && git -C "$d" sparse-checkout set "$@"
  fi
  [ -L "$d" ] || { git -C "$d" fetch -q --tags origin; git -C "$d" checkout -q "$rev"; }
  echo "$1 at $(git -C "$d" describe --tags --always)"
}
clone open-gpu-kernel-modules https://github.com/NVIDIA/open-gpu-kernel-modules.git 610.57.04 \
  src/common/sdk/nvidia/inc src/nvidia/arch/nvalloc/unix/include
clone open-gpu-doc https://github.com/NVIDIA/open-gpu-doc.git 9fdf5c4062007929d9f4e6cbad9c9771fe61b880
