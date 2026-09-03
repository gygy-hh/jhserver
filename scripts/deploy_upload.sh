#!/usr/bin/env bash
# Usage: ./scripts/deploy_upload.sh user@your-server-ip
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 user@server-ip"
  echo "Example: $0 admin@1.2.3.4"
  exit 1
fi

TARGET="$1"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REMOTE_DIR="/opt/jh-server"

cd "$ROOT"

if [[ ! -f build-linux/jh_server ]]; then
  echo "Missing build-linux/jh_server. Run scripts/build_linux.sh first."
  exit 1
fi

echo "[deploy] create remote dir: $REMOTE_DIR"
ssh "$TARGET" "sudo mkdir -p $REMOTE_DIR/{data/saves,web} && sudo chown -R \$(whoami):\$(whoami) $REMOTE_DIR"

echo "[deploy] upload binary and config"
scp build-linux/jh_server "$TARGET:$REMOTE_DIR/jh_server"
scp config.prod.json "$TARGET:$REMOTE_DIR/config.json"
scp -r web/admin.html "$TARGET:$REMOTE_DIR/web/"

if [[ -d data/saves ]]; then
  echo "[deploy] upload existing saves (if any)"
  scp -r data/saves/* "$TARGET:$REMOTE_DIR/data/saves/" 2>/dev/null || true
fi

echo "[deploy] set executable"
ssh "$TARGET" "chmod +x $REMOTE_DIR/jh_server"

cat <<EOF

Upload complete.

On server run:
  cd $REMOTE_DIR
  ./jh_server --config config.json

Or background:
  nohup ./jh_server --config config.json > jh_server.log 2>&1 &

Health check:
  curl http://127.0.0.1:18080/health

EOF
