#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "[build] project: $ROOT"

if ! command -v cmake >/dev/null; then
  sudo apt-get update
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential cmake pkg-config libmariadb-dev || \
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential cmake pkg-config default-libmysqlclient-dev
fi

rm -rf build-linux
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j"$(nproc)"

echo "[build] done:"
ls -lh build-linux/jh_server
file build-linux/jh_server
ldd build-linux/jh_server | grep -E 'mariadb|mysql|not found' || true
