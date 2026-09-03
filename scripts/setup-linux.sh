#!/usr/bin/env bash
set -euo pipefail

APP_DIR="${APP_DIR:-/opt/jh-server}"
SUDO=""
if [[ "$(id -u)" -ne 0 ]]; then
  SUDO="sudo"
fi

$SUDO mkdir -p "$APP_DIR"
cd "$APP_DIR"

if command -v dnf >/dev/null 2>&1; then
  $SUDO dnf install -y gcc-c++ make cmake cmake3 || $SUDO dnf install -y gcc-c++ make cmake
elif command -v yum >/dev/null 2>&1; then
  $SUDO yum install -y gcc-c++ make cmake cmake3 || $SUDO yum install -y gcc-c++ make cmake
elif command -v apt-get >/dev/null 2>&1; then
  export DEBIAN_FRONTEND=noninteractive
  $SUDO apt-get update -y
  $SUDO apt-get install -y build-essential cmake
else
  echo "未识别的包管理器，请先手动安装 g++ / cmake" >&2
  exit 1
fi

if [[ ! -f config.json && -f config.prod.json ]]; then
  cp config.prod.json config.json
fi

CMAKE_BIN="cmake"
if ! command -v cmake >/dev/null 2>&1 && command -v cmake3 >/dev/null 2>&1; then
  CMAKE_BIN="cmake3"
fi

$CMAKE_BIN -S . -B build -DCMAKE_BUILD_TYPE=Release
$CMAKE_BIN --build build -j"$(nproc)"

if command -v firewall-cmd >/dev/null 2>&1; then
  $SUDO firewall-cmd --permanent --add-port=8888/tcp || true
  $SUDO firewall-cmd --reload || true
fi

$SUDO install -m 644 deploy/jh-server.service /etc/systemd/system/jh-server.service
$SUDO systemctl daemon-reload
$SUDO systemctl enable --now jh-server
$SUDO systemctl --no-pager --full status jh-server || true

echo
echo "部署完成。健康检查: curl -sS http://127.0.0.1:8888/health"
echo "管理后台: http://$(hostname -I | awk '{print $1}'):8888/admin"
