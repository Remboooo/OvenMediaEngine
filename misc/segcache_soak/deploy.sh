#!/usr/bin/env bash
# Deploy latest build as OvenMediaEngine-0.21.0-segcacheN and restart.
set -euo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)"
SRC="${ROOT}/build/Release/bin/OvenMediaEngine"
DEST_DIR=/mnt/zfs/opt/ovenmediaengine
NAME="${1:-OvenMediaEngine-0.21.0-segcache6}"

if [[ ! -x "$SRC" ]]; then
	echo "missing $SRC — build first" >&2
	exit 1
fi

echo "Install $SRC -> $DEST_DIR/$NAME and point symlink"
sudo cp -a "$SRC" "$DEST_DIR/$NAME"
sudo ln -sfn "$NAME" "$DEST_DIR/OvenMediaEngine"
sudo systemctl restart ovenmediaengine
sleep 2
systemctl is-active ovenmediaengine
readlink -f "$DEST_DIR/OvenMediaEngine"
