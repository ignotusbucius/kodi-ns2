#!/usr/bin/env bash
# Recreate addons-bundle/addons/ (the pre-installed Python add-ons) from MANIFEST.txt.
# Each zip is downloaded from the official Kodi Omega mirror listed in the manifest, checked against the
# manifest's sha256, kept in zips/ and unpacked to addons/<id>/. package.sh copies addons/* into the build.
# Usage: ./addons-bundle/fetch.sh        (run from anywhere; needs curl, sha256sum, unzip)
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p zips addons
trim() { local v="$1"; v="${v#"${v%%[![:space:]]*}"}"; printf '%s' "${v%"${v##*[![:space:]]}"}"; }
n=0
while IFS='|' read -r id ver kind sha size url rest; do
  id=$(trim "$id"); kind=$(trim "$kind"); sha=$(trim "$sha"); url=$(trim "$url")
  case "$id" in ''|\#*) continue ;; esac
  case "$kind" in root|dependency) ;; *) continue ;; esac
  zip="zips/$(basename "$url")"
  if [ ! -f "$zip" ] || [ "$(sha256sum "$zip" | cut -d' ' -f1)" != "$sha" ]; then
    curl -sfL -o "$zip.part" "$url"
    got=$(sha256sum "$zip.part" | cut -d' ' -f1)
    [ "$got" = "$sha" ] || { echo "sha256 mismatch for $id: $got != $sha" >&2; rm -f "$zip.part"; exit 1; }
    mv "$zip.part" "$zip"
  fi
  rm -rf "addons/$id"
  unzip -q "$zip" -d addons
  [ -f "addons/$id/addon.xml" ] || { echo "$zip did not unpack to addons/$id/" >&2; exit 1; }
  n=$((n + 1))
done < MANIFEST.txt
echo "add-ons ready: $n in $(pwd)/addons"
