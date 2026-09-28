#!/bin/bash
set -euo pipefail

# --- Salume Studio: unraid update bootstrap -------------------------------
# Paste this into the unraid User Scripts plugin (or run it from a shell).
# It downloads the latest install.sh from GitHub and runs it, which re-pulls
# the source, rebuilds the image, and recreates the container. Because the
# real deploy logic lives in the repo, this stub never needs editing - and if
# it ever gets overwritten again, these few lines are all you need back.
#
# Data in /mnt/user/appdata/salume-studio (curing.json, history.db,
# monitor-config.json) is a mounted volume and is left untouched.
REPO=zacharyd3/Salume-Studio
BRANCH=main
# ------------------------------------------------------------------------

TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT

echo "==> Downloading install.sh ($REPO @ $BRANCH)"
curl -fsSL "https://raw.githubusercontent.com/$REPO/$BRANCH/install.sh" -o "$TMP"

bash "$TMP"
