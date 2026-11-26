#!/usr/bin/env bash
# Fetches the Vimium version the extension work was tested against into the given directory (default: ./vimium-src),
# ready for --load-extension. Vimium's MV3 source loads unpacked as is; it needs no build step.
set -euo pipefail

VIMIUM_COMMIT=4046615ae4128d485fce3335fc1032e6f87c630a # 2.4.2
DEST="${1:-vimium-src}"

if [ ! -d "$DEST/.git" ]; then
    git clone --quiet https://github.com/philc/vimium.git "$DEST"
fi
git -C "$DEST" fetch --quiet origin "$VIMIUM_COMMIT"
git -C "$DEST" -c advice.detachedHead=false checkout --quiet "$VIMIUM_COMMIT"
echo "Vimium $(git -C "$DEST" rev-parse --short HEAD) is in $DEST"
