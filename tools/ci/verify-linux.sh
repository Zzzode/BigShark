#!/usr/bin/env bash
set -euo pipefail

node --version
npm --version
cmake --version | head -n 1
ninja --version
g++ --version | head -n 1
clang-format --version

npm run proto:check
export BIGSHARK_ALLOW_CFR_FALLBACK=1

cmake --preset linux-release
cmake --build --preset linux-release --target format-check
cmake --build --preset linux-release
BIGSHARK_ENGINE_BINARY="$PWD/build/linux-release/apps/engine-host/bigshark-engine" \
  npm run check
ctest --preset linux-release --parallel "$(nproc)" -LE benchmark
ctest --preset linux-release --verbose -L benchmark

if [[ -d sessions ]]; then
  BIGSHARK_ENGINE_BINARY="$PWD/build/linux-release/apps/engine-host/bigshark-engine" \
    node bin/replay.mjs
fi

ctest --test-dir build/linux-release --verbose -R '^gto$' \
  | tee build/linux-release/gto-backend.log
grep -F '(exact LP unavailable, bounded CFR only)' \
  build/linux-release/gto-backend.log

ldd build/linux-release/apps/engine-host/bigshark-engine \
  | tee build/linux-release/engine-ldd.log
if grep -Fq 'not found' build/linux-release/engine-ldd.log; then
  echo 'Linux engine has unresolved shared-library dependencies.' >&2
  exit 1
fi

if readelf -d build/linux-release/apps/engine-host/bigshark-engine \
  | grep -qi CoreFoundation; then
  echo 'Linux engine unexpectedly links CoreFoundation.' >&2
  exit 1
fi
