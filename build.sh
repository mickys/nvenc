#!/bin/bash
# Build in the macOS guest (needs the Command Line Tools) over ssh; the binaries stay in the guest's ~/nvenc-dev.
#   ./build.sh [tool ...]          tools are tools/<name>.c, each linked with src/*.c (default: all)
#   NVENC_GUEST=<ssh host>         the guest (default: tahoe)
# Headers come from vendor/ (scripts/fetch-vendor.sh).
set -euo pipefail
cd "$(dirname "$0")"
G=${NVENC_GUEST:-tahoe}
[ -d vendor/open-gpu-kernel-modules ] && [ -d vendor/open-gpu-doc ] || { echo "run scripts/fetch-vendor.sh first"; exit 1; }
tools=${*:-$(ls tools/*.c tools/*.m | xargs -n1 basename | sed 's/\.[cm]$//')}
tar -C vendor/open-gpu-kernel-modules -chf - src/common/sdk/nvidia/inc src/nvidia/arch/nvalloc/unix/include \
  | ssh "$G" 'rm -rf ~/nvenc-dev/inc && mkdir -p ~/nvenc-dev/inc/ogkm && tar -xf - -C ~/nvenc-dev/inc/ogkm'
tar -C vendor/open-gpu-doc -chf - classes/video | ssh "$G" 'mkdir -p ~/nvenc-dev/inc/ogd && tar -xf - -C ~/nvenc-dev/inc/ogd'
tar -cf - src tools | ssh "$G" "cd ~/nvenc-dev && rm -rf src tools && tar -xf - && for t in $tools; do
  if [ -f tools/\$t.m ]; then
    clang -O2 -fobjc-arc -Wall -o \$t tools/\$t.m -framework Foundation -framework VideoToolbox -framework CoreMedia -framework CoreVideo 2>&1 | grep -E 'error|warning'
  else
    clang -O2 -Wall -Wno-unused-function -o \$t tools/\$t.c src/*.c -I src \
      -I inc/ogkm/src/common/sdk/nvidia/inc -I inc/ogkm/src/nvidia/arch/nvalloc/unix/include -I inc/ogd/classes/video \
      -framework IOKit -framework CoreFoundation 2>&1 | grep -E 'error|warning'
  fi; echo \"built ~/nvenc-dev/\$t\"; done"
