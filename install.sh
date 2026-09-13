#!/bin/bash
set -euo pipefail

# --- Salume Studio: build & (re)start on unraid --------------------------
# Fetches the latest source, builds the single image (nginx + the recorder
# backend baked in), and recreates the container. One container, one data
# volume - the backend's history.db and monitor-config.json live in $DATA
# alongside curing.json, so everything persists across rebuilds.
#
# Run it again any time to update: it re-pulls main, rebuilds, and recreates.
SRC=/mnt/user/appdata/salume-studio-src           # build context (throwaway)
DATA=/mnt/user/appdata/salume-studio              # persistent data (curing + history + config)
NAME=SalumeStudio
IMAGE=salume-studio:latest
PORT=8083                                         # host port -> container :80  (8080/8082 were taken)
REPO=zacharyd3/Salume-Studio
BRANCH=main
# ------------------------------------------------------------------------

# Persistent data dir must exist and be writable by the nginx worker AND the
# backend (it writes history.db / monitor-config.json here).
mkdir -p "$DATA"
chmod 777 "$DATA"

# Start the build context clean so files removed upstream don't linger from a
# previous run and get baked into the image.
rm -rf "$SRC"
mkdir -p "$SRC"
cd "$SRC"

echo "==> Fetching latest source ($REPO @ $BRANCH)"
curl -fL "https://github.com/$REPO/archive/refs/heads/$BRANCH.tar.gz" \
  | tar xz --strip-components=1

echo "==> Building image ($IMAGE)"
docker build -t "$IMAGE" .

echo "==> Recreating container ($NAME)"
docker stop "$NAME" 2>/dev/null || true
docker rm   "$NAME" 2>/dev/null || true
docker run -d \
  --name "$NAME" \
  -p "${PORT}:80" \
  -v "$DATA:/usr/share/nginx/html/data" \
  --restart unless-stopped \
  "$IMAGE"

echo "==> Done. Salume Studio is up on http://192.168.250.4:${PORT}/"
echo "    App + history API in one container; data persists at $DATA"
echo "    Health check: curl -s http://192.168.250.4:${PORT}/api/health"
