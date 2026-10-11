#!/bin/sh
# Lays out the game data the way the Vita build looks for it: copy the
# resulting data/srb2 folder to ux0:data/srb2 on the console.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZIP="$ROOT/tools/cache/SRB2-v2215-Full.zip"
URL="https://github.com/STJr/SRB2/releases/download/SRB2_release_2.2.15/SRB2-v2215-Full.zip"
DEST="$ROOT/data/srb2"

if [ ! -f "$ZIP" ]; then
	mkdir -p "$(dirname "$ZIP")"
	curl -L -o "$ZIP" "$URL"
fi

mkdir -p "$DEST"
unzip -o -q "$ZIP" \
	srb2.pk3 zones.pk3 characters.pk3 music.pk3 models.dat 'models/*' \
	LICENSE.txt LICENSE-3RD-PARTY.txt README.txt \
	-d "$DEST"

mkdir -p "$DEST/addons"

md5sum "$DEST"/*.pk3
