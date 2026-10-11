#!/bin/sh
# Builds the LiveArea assets from the artwork shipped with the SRB2 sources.
# The Vita shell only accepts 8-bit indexed PNGs here.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ART="$ROOT/SRB2-master"
OUT="$ROOT/sce_sys"

mkdir -p "$OUT/livearea/contents"

magick "$ART/srb2.png" -resize 128x128 -background black -flatten "$OUT/icon0.png"

magick -size 840x500 xc:'#1c2c8c' \
	\( "$ART/srb2banner.png" -resize 760x \) -gravity center -composite \
	-flatten "$OUT/livearea/contents/bg.png"

magick "$ART/srb2banner.png" -resize 280x158 -background black -gravity center -extent 280x158 \
	-flatten "$OUT/livearea/contents/startup.png"

for f in "$OUT/icon0.png" "$OUT/livearea/contents/bg.png" "$OUT/livearea/contents/startup.png"; do
	pngquant --force --strip --output "$f" 256 "$f"
done

cat > "$OUT/livearea/contents/template.xml" <<'EOF'
<?xml version="1.0" encoding="utf-8"?>

<livearea style="a1" format-ver="01.00" content-rev="1">
  <livearea-background>
    <image>bg.png</image>
  </livearea-background>

  <gate>
    <startup-image>startup.png</startup-image>
  </gate>
</livearea>
EOF
