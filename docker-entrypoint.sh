#!/bin/sh
# Salume Studio container entrypoint.
#
# The image runs two processes: nginx (serves the app, proxies /api/ and /mqtt)
# and the Python recorder/API backend. nginx is the container's main process -
# if it dies the container should die so Docker's `--restart unless-stopped`
# recreates it. The backend is supervised by a small loop so a transient crash
# (e.g. a bad broker hiccup) restarts it without taking the app down.
set -eu

# Keep the recorder alive independently of nginx.
(
  while true; do
    /opt/venv/bin/python /app/monitor.py || \
      echo "[entrypoint] monitor.py exited ($?); restarting in 5s"
    sleep 5
  done
) &

# nginx in the foreground = the container's lifecycle.
exec nginx -g 'daemon off;'
