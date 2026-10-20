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

# Exit mode: a door with an arrow leaving it.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 14 \
  -draw "polyline 128,64 128,44 52,44 52,212 128,212 128,192" \
  -strokewidth 16 -draw "line 92,128 196,128" -fill white -stroke none \
  -draw "polygon 222,128 182,96 182,160" -resize 128x128 "$OUT/exit.png"

# Style mode tools. Fleck style: three overlapping flecks.
magick -size ${S}x${S} xc:none -fill white -stroke '#2b313b' -strokewidth 8 \
  -draw "circle 96,150 96,96" -draw "circle 160,150 160,96" -draw "circle 128,96 128,44" \
  -resize 128x128 "$OUT/fleckstyle.png"
# Looseness: flecks growing bigger and sparser.
magick -size ${S}x${S} xc:none -fill white \
  -draw "circle 52,184 52,172" -draw "circle 84,150 84,134" -draw "circle 64,112 64,100" \
  -draw "circle 130,120 130,98" -draw "circle 108,196 108,180" -draw "circle 182,82 182,50" \
  -draw "circle 186,176 186,148" -resize 128x128 "$OUT/looseness.png"
# Comb: a comb.
magick -size ${S}x${S} xc:none -fill white -draw "roundrectangle 36,52 220,96 14,14" \
  -draw "rectangle 48,92 64,204" -draw "rectangle 84,92 100,204" -draw "rectangle 120,92 136,204" \
  -draw "rectangle 156,92 172,204" -draw "rectangle 192,92 208,204" -resize 128x128 "$OUT/comb.png"
# Ruffle: short strokes scattered at random angles.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 18 \
  -draw "line 48,70 96,52" -draw "line 150,40 176,88" -draw "line 60,140 72,190" \
  -draw "line 112,118 160,136" -draw "line 186,140 210,192" -draw "line 108,196 150,176" \
  -resize 128x128 "$OUT/ruffle.png"
# Impasto: a paint stroke attached to the surface at one end, its tip
# lifted off it, thick.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 14 \
  -draw "line 28,208 228,208" -stroke none -fill white \
  -draw "path 'M 40,200 C 90,200 140,180 176,124 C 196,92 214,64 226,70 C 240,80 224,124 206,148 C 170,192 110,206 40,206 Z'" \
  -resize 128x128 "$OUT/impasto.png"
# Neatness: flecks in neat rows, every other row shifted half along.
magick -size ${S}x${S} xc:none -fill white \
  -draw "roundrectangle 34,52 86,84 12,12" -draw "roundrectangle 102,52 154,84 12,12" -draw "roundrectangle 170,52 222,84 12,12" \
  -draw "roundrectangle 0,112 52,144 12,12" -draw "roundrectangle 68,112 120,144 12,12" -draw "roundrectangle 136,112 188,144 12,12" -draw "roundrectangle 204,112 256,144 12,12" \
  -draw "roundrectangle 34,172 86,204 12,12" -draw "roundrectangle 102,172 154,204 12,12" -draw "roundrectangle 170,172 222,204 12,12" \
  -resize 128x128 "$OUT/neatness.png"

# Effects mode tools. Boil: bubbles rising off a surface.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 14 \
  -draw "line 28,212 228,212" -strokewidth 12 \
  -draw "circle 80,168 80,140" -draw "circle 150,150 150,106" -draw "circle 108,84 108,66" \
  -draw "circle 186,64 186,52" -resize 128x128 "$OUT/boil.png"
# Flow: streaming current lines with an arrow.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 16 \
  -draw "path 'M 28,88 C 76,60 116,116 164,88'" -draw "path 'M 28,140 C 76,112 116,168 164,140'" \
  -draw "path 'M 28,192 C 76,164 116,220 164,192'" -stroke none -fill white \
  -draw "polygon 234,140 186,104 186,176" -resize 128x128 "$OUT/flow.png"
# Wave: grass blades bending in the wind.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 14 \
  -draw "line 28,220 228,220" \
  -draw "path 'M 64,214 C 66,160 84,110 120,74'" -draw "path 'M 116,214 C 118,166 138,124 176,96'" \
  -draw "path 'M 168,214 C 170,176 188,142 222,124'" -resize 128x128 "$OUT/wave.png"
# Evaporate: wisps of steam rising from a surface.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 14 \
  -draw "line 28,220 228,220" \
  -draw "path 'M 80,196 C 56,170 104,146 80,120 C 56,94 104,70 80,44'" \
  -draw "path 'M 128,196 C 104,170 152,146 128,120 C 104,94 152,70 128,44'" \
  -draw "path 'M 176,196 C 152,170 200,146 176,120 C 152,94 200,70 176,44'" \
  -resize 128x128 "$OUT/evaporate.png"
# Throb: a heartbeat trace.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 14 \
  -draw "polyline 20,136 82,136 104,84 132,192 156,108 172,136 236,136" \
  -resize 128x128 "$OUT/throb.png"

# Placeholder for buttons that do nothing yet: three dots.
magick -size ${S}x${S} xc:none -fill white \
  -draw "circle 78,128 78,112" -draw "circle 128,128 128,112" -draw "circle 178,128 178,112" \
  -resize 128x128 "$OUT/dummy.png"
echo "Toolbar art written to $OUT"
