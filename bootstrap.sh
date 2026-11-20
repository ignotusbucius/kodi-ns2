#!/usr/bin/env bash
# Fetch Kodi 21.3-Omega into ./xbmc and apply the Switch 2 port patch.
set -euo pipefail
cd "$(dirname "$0")"
if [ ! -d xbmc ]; then
  git clone --depth 1 --branch 21.3-Omega https://github.com/xbmc/xbmc.git xbmc
fi
git -C xbmc apply --check ../patches/kodi-21.3-switch2.patch && git -C xbmc apply ../patches/kodi-21.3-switch2.patch
echo "patched: $(git -C xbmc status --short | wc -l) files changed in xbmc/"
