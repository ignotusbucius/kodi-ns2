#!/usr/bin/env bash
# One-zip SD-card install for the Kodi Switch 2 port.
# Usage: ./release.sh <tag>      (after ./build.sh nro; reads out/switch/)
# Writes dist/kodi-ns2-<tag>.zip with the SD layout  switch/kodi.nro  switch/kodi/{addons,media,system,userdata}
# plus INSTALL.txt and SHA256SUMS. User data (switch/kodi/.kodi/) is never part of the zip, so unpacking a
# new release over an old one keeps settings, sources and databases.
set -euo pipefail
cd "$(dirname "$0")"
TAG=${1:?usage: ./release.sh <tag>}
OUT=out/switch
[ -f "$OUT/kodi.nro" ] && [ -d "$OUT/kodi/system" ] || { echo "no $OUT/kodi.nro / $OUT/kodi: run ./build.sh nro"; exit 1; }
if [ -e "$OUT/kodi/.kodi" ]; then echo "refusing: $OUT/kodi/.kodi (user data) must not ship"; exit 1; fi
if find "$OUT/kodi/addons" -maxdepth 1 -name 'service.switch2.*' | grep -q .; then
  echo "refusing: a service.switch2.* test add-on is in $OUT/kodi/addons"; exit 1
fi

STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/switch"
cp "$OUT/kodi.nro" "$STAGE/switch/"
cp -r "$OUT/kodi" "$STAGE/switch/"
# __pycache__ written by host-side checks must not ship (the console writes its own).
find "$STAGE" -name __pycache__ -type d -prune -exec rm -rf {} +

NRO_SHA=$(sha256sum "$OUT/kodi.nro" | cut -d' ' -f1)
cat > "$STAGE/INSTALL.txt" <<EOF
Kodi for Nintendo Switch 2 ($TAG) - unofficial port, needs kaiHEN + hbmenu.

1. Copy the "switch" folder from this zip to the root of the SD card (merge with the existing one).
   You end up with  sdmc:/switch/kodi.nro  and  sdmc:/switch/kodi/
2. Start Kodi from hbmenu. The first start takes longer (databases, add-on scan).
3. Add media: Videos > Files > Add videos...  (SD card paths start with /, e.g. /switch/kodi-media/;
   network: http:// sources on your LAN).

Upgrading: unpack over the old install. Settings and libraries live in sdmc:/switch/kodi/.kodi/ and
are kept. Log: sdmc:/switch/kodi/.kodi/temp/kodi.log

kodi.nro sha256: $NRO_SHA
EOF
(cd "$STAGE" && find switch -type f -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)

mkdir -p dist
ZIP="$PWD/dist/kodi-ns2-$TAG.zip"
rm -f "$ZIP"
(cd "$STAGE" && zip -qr -X "$ZIP" switch INSTALL.txt SHA256SUMS)
echo "$ZIP"
sha256sum "$ZIP"
du -h "$ZIP" | cut -f1
