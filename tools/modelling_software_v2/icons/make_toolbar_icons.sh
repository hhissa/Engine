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

# Sculpt mode tools. Stamp: a rubber stamp (knob, neck, block) over its mark.
magick -size ${S}x${S} xc:none -fill white -stroke none \
  -draw "circle 128,60 128,30" -draw "rectangle 112,84 144,128" \
  -draw "roundrectangle 60,128 196,170 12,12" -fill none -stroke white -strokewidth 12 \
  -draw "line 60,210 196,210" -resize 128x128 "$OUT/stamp.png"
# Smear: a blob dragged along, trailing off.
magick -size ${S}x${S} xc:none -fill white -stroke none \
  -draw "circle 184,96 184,48" -fill none -stroke white -strokewidth 52 \
  -draw "path 'M 184,96 C 140,120 112,168 56,190'" -stroke none -fill white \
  -draw "circle 56,190 56,176" -resize 128x128 "$OUT/smear.png"
# Spray paint: a can with its nozzle and a cloud of dots.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 12 \
  -draw "roundrectangle 104,96 184,224 14,14" -fill white -stroke none \
  -draw "rectangle 124,60 164,92" -draw "rectangle 134,44 154,60" \
  -draw "circle 92,52 92,44" -draw "circle 64,40 64,32" -draw "circle 68,72 68,64" \
  -draw "circle 40,60 40,52" -draw "circle 40,92 40,84" -resize 128x128 "$OUT/spray.png"
# Crop: two corner brackets framing a square.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 16 \
  -draw "polyline 72,28 72,184 228,184" -draw "polyline 28,72 184,72 184,228" \
  -resize 128x128 "$OUT/crop.png"
# Cut out: scissors.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 14 \
  -draw "circle 72,184 72,148" -draw "circle 184,184 184,148" \
  -strokewidth 16 -draw "line 96,158 196,40" -draw "line 160,158 60,40" \
  -resize 128x128 "$OUT/cutout.png"

# Placeholder for buttons that do nothing yet: three dots.
magick -size ${S}x${S} xc:none -fill white \
  -draw "circle 78,128 78,112" -draw "circle 128,128 128,112" -draw "circle 178,128 178,112" \
  -resize 128x128 "$OUT/dummy.png"
echo "Toolbar art written to $OUT"

# --- Guides (top bar, default mode) ---
# Guides: a set square with ruler ticks.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 12 \
  -draw "polygon 52,52 52,204 204,204" -draw "polygon 84,124 84,172 132,172" \
  -strokewidth 8 -draw "line 52,84 70,84" -draw "line 52,116 64,116" \
  -draw "line 52,148 70,148" -draw "line 84,204 84,186" -draw "line 116,204 116,192" \
  -draw "line 148,204 148,186" -resize 128x128 "$OUT/guides.png"
# Grid snap: 3x3 lattice of corner crosses, the middle one filled.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 10 \
  -draw "line 40,56 72,56" -draw "line 56,40 56,72" \
  -draw "line 112,56 144,56" -draw "line 128,40 128,72" \
  -draw "line 184,56 216,56" -draw "line 200,40 200,72" \
  -draw "line 40,128 72,128" -draw "line 56,112 56,144" \
  -draw "line 184,128 216,128" -draw "line 200,112 200,144" \
  -draw "line 40,200 72,200" -draw "line 56,184 56,216" \
  -draw "line 112,200 144,200" -draw "line 128,184 128,216" \
  -draw "line 184,200 216,200" -draw "line 200,184 200,216" \
  -fill white -stroke none -draw "circle 128,128 128,104" \
  -resize 128x128 "$OUT/grid.png"
# Grid size: a cell with a diagonal two-headed arrow.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 12 \
  -draw "rectangle 48,48 208,208" -strokewidth 12 -draw "line 88,168 168,88" \
  -fill white -stroke none -draw "polygon 70,186 70,136 120,186" \
  -draw "polygon 186,70 136,70 186,120" -resize 128x128 "$OUT/grid_size.png"
# Grid cell sizes (the Grid Size sub-row): the size as a fraction.
for spec in "16:1/16" "8:1/8" "4:1/4" "2:1/2" "1:1" "0:2"; do
  name=${spec%%:*}; text=${spec#*:}
  pts=120; [ ${#text} -ge 4 ] && pts=84; [ ${#text} -eq 3 ] && pts=100
  magick -size ${S}x${S} xc:none -fill white -font Adwaita-Sans-Bold \
    -pointsize $pts -gravity center -annotate +0+0 "$text" \
    -resize 128x128 "$OUT/grid_$name.png"
done
# Upright: an arrow standing on the ground.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 14 \
  -draw "line 128,196 128,84" -draw "line 52,208 204,208" \
  -fill white -stroke none -draw "polygon 128,36 168,96 88,96" \
  -resize 128x128 "$OUT/upright.png"
# 90 degree angles: a right angle with its corner square.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 14 \
  -draw "polyline 64,40 64,192 216,192" -strokewidth 10 \
  -draw "polyline 64,140 116,140 116,192" -resize 128x128 "$OUT/angle90.png"
# Precise move: three axis arrows from one corner.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 12 \
  -draw "line 88,168 200,168" -draw "line 88,168 88,56" -draw "line 88,168 40,216" \
  -fill white -stroke none -draw "polygon 224,168 192,148 192,188" \
  -draw "polygon 88,32 68,64 108,64" -draw "polygon 28,228 36,190 66,220" \
  -resize 128x128 "$OUT/precise.png"

# --- Shape options (sculpt mode, Stamp tool) ---
# Side button: three sliders.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 12 \
  -draw "line 48,72 208,72" -draw "line 48,128 208,128" -draw "line 48,184 208,184" \
  -fill white -stroke none -draw "circle 92,72 92,50" -draw "circle 160,128 160,106" \
  -draw "circle 116,184 116,162" -resize 128x128 "$OUT/shape_options.png"
# Precise scale: a box with a face being pulled out by an arrow.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 12 \
  -draw "rectangle 40,72 140,184" -strokewidth 8 -draw "line 148,72 160,72" \
  -draw "line 170,72 182,72" -draw "line 148,184 160,184" -draw "line 170,184 182,184" \
  -strokewidth 12 -draw "line 150,128 196,128" -fill white -stroke none \
  -draw "polygon 228,128 192,104 192,152" -resize 128x128 "$OUT/precise_scale.png"
# Smoothness (the blend between shapes): two discs melted together -- drawn
# apart, blurred, then cut at half brightness, so the wider the blur the
# thicker the neck between them (a metaball).
blend() { # name blur
  if [ "$2" = 0 ]; then
    magick -size ${S}x${S} xc:black -fill white -draw "circle 76,128 76,82" \
      -draw "circle 180,128 180,82" -alpha copy -fill white -colorize 100 \
      -resize 128x128 "$OUT/$1.png"
  else
    magick -size ${S}x${S} xc:black -fill white -draw "circle 76,128 76,82" \
      -draw "circle 180,128 180,82" -blur 0x$2 -threshold 42% \
      -alpha copy -fill white -colorize 100 -resize 128x128 "$OUT/$1.png"
  fi
}
i=0
for b in 0 4 8 13 18 24; do
  blend blend_$i $b
  i=$((i + 1))
done
cp "$OUT/blend_3.png" "$OUT/smoothness.png"
# Sections: a disc, then a half, quarter and eighth of it (the rest faint).
pie() { # name end_angle
  magick -size ${S}x${S} xc:none -fill '#ffffff40' -draw "circle 128,128 128,40" \
    -fill white -draw "path 'M 128,128 L 216,128 A 88,88 0 $([ $2 -gt 180 ] && echo 1 || echo 0) 0 $(python3 -c "import math;a=math.radians($2);print(f'{128+88*math.cos(a):.1f},{128-88*math.sin(a):.1f}')") Z'" \
    -resize 128x128 "$OUT/$1.png"
}
magick -size ${S}x${S} xc:none -fill white -draw "circle 128,128 128,40" \
  -resize 128x128 "$OUT/section_0.png"
pie section_1 180
pie section_2 90
pie section_3 45

# --- Search (top bar, default mode) ---
# Search: a magnifying glass over a small grid of saved things.
magick -size ${S}x${S} xc:none -fill white -stroke none \
  -draw "roundrectangle 36,36 84,84 8,8" -draw "roundrectangle 100,36 148,84 8,8" \
  -draw "roundrectangle 36,100 84,148 8,8" \
  -fill none -stroke white -strokewidth 14 -draw "circle 156,156 156,112" \
  -strokewidth 22 -draw "line 188,188 222,222" -resize 128x128 "$OUT/search.png"
# Save: a box with an arrow going down into it.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 14 \
  -draw "polyline 48,128 48,208 208,208 208,128" -draw "line 128,40 128,150" \
  -fill white -stroke none -draw "polygon 128,180 88,128 168,128" \
  -resize 128x128 "$OUT/save.png"
# Opacity (Spray Paint): a disc, solid on the left, sprayed dots on the right.
dots=""
for p in "150,70" "176,92" "150,110" "196,124" "168,136" "150,160" "186,162" \
         "160,190" "140,90" "140,136" "172,114" "140,184" "200,146"; do
  dots="$dots -draw \"circle $p ${p%,*},$(( ${p#*,} + 7 ))\""
done
eval magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 12 \
  -draw \"circle 128,128 128,40\" -stroke none -fill white \
  -draw \"path \'M 128,40 A 88,88 0 0,0 128,216 Z\'\" $dots \
  -resize 128x128 "$OUT/opacity.png"
# Colour Variation (Smear): Off a single disc, Switch three separate discs,
# Blend a bar fading from solid to empty.
magick -size ${S}x${S} xc:none -fill white -draw "circle 128,128 128,72" \
  -resize 128x128 "$OUT/variation_off.png"
magick -size ${S}x${S} xc:none -fill white -draw "circle 60,128 60,102" \
  -draw "circle 128,128 128,102" -draw "circle 196,128 196,102" \
  -resize 128x128 "$OUT/variation_switch.png"
magick -size ${S}x${S} xc:none \
  \( -size 176x72 -define gradient:direction=east gradient:white-none \) \
  -geometry +40+92 -composite \
  -fill none -stroke white -strokewidth 10 -draw "roundrectangle 40,92 216,164 20,20" \
  -resize 128x128 "$OUT/variation_blend.png"
# Move (every mode's Tools): a cross with an arrowhead on each arm.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 16 \
  -draw "line 128,56 128,200" -draw "line 56,128 200,128" \
  -fill white -stroke none \
  -draw "polygon 128,24 92,68 164,68" -draw "polygon 128,232 92,188 164,188" \
  -draw "polygon 24,128 68,92 68,164" -draw "polygon 232,128 188,92 188,164" \
  -resize 128x128 "$OUT/move.png"
# Surface Snap: a ball sitting on a surface, an arrow pressing it down.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 14 \
  -draw "line 32,200 224,200" -fill white -stroke none \
  -draw "circle 128,150 128,108" \
  -fill none -stroke white -strokewidth 12 -draw "line 128,30 128,82" \
  -fill white -stroke none -draw "polygon 128,100 104,72 152,72" \
  -resize 128x128 "$OUT/surface_snap.png"
# Tentacle Snap: a curling stroke rising off a surface, ending in a dot.
magick -size ${S}x${S} xc:none -fill none -stroke white -strokewidth 14 \
  -draw "line 32,212 224,212" -strokewidth 18 \
  -draw "path 'M 96,212 C 96,150 170,150 160,100 C 152,62 104,70 110,44'" \
  -fill white -stroke none -draw "circle 110,40 110,24" \
  -resize 128x128 "$OUT/tentacle_snap.png"
# Mirror: a dashed line between a shape and its reflection.
magick -size ${S}x${S} xc:none -fill white -stroke none \
  -draw "polygon 108,64 108,192 36,192" -draw "polygon 148,64 148,192 220,192" \
  -draw "rectangle 122,28 134,58" -draw "rectangle 122,82 134,112" \
  -draw "rectangle 122,136 134,166" -draw "rectangle 122,190 134,220" \
  -resize 128x128 "$OUT/mirror.png"
# Kaleidoscope: copies in a ring about a point.
k="-draw \"circle 128,128 128,116\""
for a in 0 60 120 180 240 300; do
  x=$(python3 -c "import math;print(round(128+78*math.cos(math.radians($a))))")
  y=$(python3 -c "import math;print(round(128+78*math.sin(math.radians($a))))")
  k="$k -draw \"circle $x,$y $x,$((y + 24))\""
done
eval magick -size ${S}x${S} xc:none -fill white -stroke none $k \
  -resize 128x128 "$OUT/kaleidoscope.png"
# Brush looseness levels (sculpt mode): a solid disc breaking up into ever
# more scattered flecks, 0% to 100%.
for k in 0 1 2 3 4; do
  draw=$(python3 - "$k" <<'PY'
import math, random, sys
k = int(sys.argv[1])
if k == 0:
    print('-draw "circle 128,128 128,48"')
    sys.exit()
r = random.Random(5)
out = []
spread = 40 + 16 * k   # how far the flecks reach
size = 26 - 3 * k      # fleck radius
for i in range(14 + 4 * k):
    a = r.uniform(0, 2 * math.pi)
    d = spread * math.sqrt(r.uniform(0, 1))
    x, y = 128 + d * math.cos(a), 128 + d * math.sin(a)
    out.append('-draw "circle %d,%d %d,%d"' % (x, y, x, y + size))
print(" ".join(out))
PY
)
  eval magick -size ${S}x${S} xc:none -fill white -stroke none $draw \
    -resize 128x128 "$OUT/looseness_$k.png"
done
