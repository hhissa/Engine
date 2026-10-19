#!/bin/bash
# Generates controller mode's toolbar button art into bin/assets/ui/toolbar/.
# Needs ImageMagick 7 (`magick`). The PNGs are committed, so the tool itself
# doesn't need this; re-run after changing a style here.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="$ROOT/bin/assets/ui/toolbar"
mkdir -p "$OUT"
S=256 # drawn at 2x, then halved for smooth edges

# Button backgrounds: a translucent dark disc with a ring.
bg() { # name fill ring ring_width
  magick -size ${S}x${S} xc:none -fill "$2" -stroke "$3" -strokewidth "$4" \
    -draw "circle 128,128 128,$((8 + $4 / 2))" -resize 128x128 "$OUT/$1.png"
}
bg button        '#1b1f26c0' '#d8dde5a0' 6
bg button_hover  '#2b313bd8' '#ffffff'   10
bg button_active '#3a2a14e0' '#ff8c1a'   12

# Group: two overlapping rounded squares joined by a link bar.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 14 \
  -draw "roundrectangle 52,52 150,150 18,18" \
  -draw "roundrectangle 106,106 204,204 18,18" -resize 128x128 "$OUT/group.png"
# Scope in: a magnifying glass.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 16 \
  -draw "circle 112,112 112,52" -strokewidth 22 -draw "line 156,156 206,206" \
  -resize 128x128 "$OUT/scope.png"
# Delete: a bin with a lid.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 14 \
  -draw "line 56,72 200,72" -draw "line 104,72 104,52" -draw "line 104,52 152,52" \
  -draw "line 152,52 152,72" -draw "path 'M 72,88 L 84,208 L 172,208 L 184,88'" \
  -strokewidth 10 -draw "line 112,112 114,184" -draw "line 144,112 142,184" \
  -resize 128x128 "$OUT/delete.png"
# Top bar. Tools: a wrench (handle + round head with the jaw cut out).
magick -size ${S}x${S} xc:none -fill white -stroke white -strokewidth 26 \
  -draw "line 64,192 150,106" -stroke none -draw "circle 164,92 164,40" \
  -draw "circle 64,192 64,176" \
  \( -size ${S}x${S} xc:none -fill white -draw "polygon 178,106 150,78 220,8 248,36" \) \
  -compose DstOut -composite -resize 128x128 "$OUT/tools.png"
# Modes: three stacked layers.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 12 \
  -draw "polygon 128,52 208,92 128,132 48,92" \
  -draw "polyline 48,128 128,168 208,128" -draw "polyline 48,164 128,204 208,164" \
  -resize 128x128 "$OUT/modes.png"
# Sculpt mode: a lump of clay with a thumb-press dent.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 12 \
  -draw "path 'M 48,196 C 40,120 80,60 128,60 C 176,60 216,120 208,196 Z'" \
  -draw "path 'M 100,96 Q 128,132 156,96'" -resize 128x128 "$OUT/sculpt.png"
# Paint mode: a paintbrush.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 16 \
  -draw "line 200,56 128,128" -fill white -stroke none \
  -draw "path 'M 112,116 L 140,144 C 124,176 96,196 52,204 C 60,160 80,132 112,116 Z'" \
  -resize 128x128 "$OUT/paint.png"
# Coat mode: a drop over a coated surface.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 12 \
  -draw "path 'M 128,40 C 152,84 180,112 180,144 A 52,52 0 0 1 76,144 C 76,112 104,84 128,40 Z'" \
  -strokewidth 14 -draw "line 68,214 188,214" -resize 128x128 "$OUT/coat.png"
# Style mode: three flowing lines.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 12 \
  -draw "path 'M 44,84 C 88,44 128,124 212,76'" \
  -draw "path 'M 44,132 C 88,92 128,172 212,124'" \
  -draw "path 'M 44,180 C 88,140 128,220 212,172'" -resize 128x128 "$OUT/style.png"
# Effects mode: sparkles.
magick -size ${S}x${S} xc:none -fill white \
  -draw "path 'M 104,40 C 112,96 128,112 184,120 C 128,128 112,144 104,200 C 96,144 80,128 24,120 C 80,112 96,96 104,40 Z'" \
  -draw "path 'M 184,148 C 188,172 196,180 220,184 C 196,188 188,196 184,220 C 180,196 172,188 148,184 C 172,180 180,172 184,148 Z'" \
  -resize 128x128 "$OUT/effects.png"

# Coat mode. Add / subtract.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 26 \
  -draw "line 128,56 128,200" -draw "line 56,128 200,128" -resize 128x128 "$OUT/plus.png"
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 26 \
  -draw "line 56,128 200,128" -resize 128x128 "$OUT/minus.png"
# Tint: a drop, half filled.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 12 \
  -draw "path 'M 128,40 C 156,88 188,116 188,152 A 60,60 0 0 1 68,152 C 68,116 100,88 128,40 Z'" \
  -fill white -stroke none -draw "path 'M 70,160 L 186,160 A 58,58 0 0 1 70,160 Z'" \
  -resize 128x128 "$OUT/tint.png"
# Finish: a sphere outline with a highlight.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 12 \
  -draw "circle 128,128 128,48" -fill white -stroke none -draw "ellipse 100,98 22,14 0,360" \
  -resize 128x128 "$OUT/finish.png"
# Glow: a sun.
magick -size ${S}x${S} xc:none -fill white -draw "circle 128,128 128,92" -stroke white -strokewidth 14 \
  -draw "line 128,30 128,62" -draw "line 128,194 128,226" -draw "line 30,128 62,128" \
  -draw "line 194,128 226,128" -draw "line 59,59 81,81" -draw "line 175,175 197,197" \
  -draw "line 59,197 81,175" -draw "line 175,81 197,59" -resize 128x128 "$OUT/glow.png"
# Revert: an anticlockwise arrow.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 16 \
  -draw "arc 56,56 200,200 200,500" -fill white -stroke none \
  -draw "polygon 40,92 92,92 66,140" -resize 128x128 "$OUT/revert.png"

# Placeholder for buttons that do nothing yet: three dots.
magick -size ${S}x${S} xc:none -fill white \
  -draw "circle 78,128 78,112" -draw "circle 128,128 128,112" -draw "circle 178,128 178,112" \
  -resize 128x128 "$OUT/dummy.png"
echo "Toolbar art written to $OUT"
