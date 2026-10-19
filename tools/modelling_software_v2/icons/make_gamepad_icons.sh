#!/bin/bash
# Generates the gamepad button icons controller mode's tooltips draw, into
# bin/assets/ui/gamepad/. Needs ImageMagick 7 (`magick`). Re-run after
# changing a style here; the PNGs are committed so the tool doesn't need it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="$ROOT/bin/assets/ui/gamepad"
FONT="$ROOT/bin/assets/fonts/DejaVuSans.ttf"
mkdir -p "$OUT"
S=128 # drawn at 2x, then halved for smooth edges

# Face button: a filled circle in the button's colour with its letter.
face() { # name letter colour
  magick -size ${S}x${S} xc:none -fill "$3" -stroke '#00000080' -strokewidth 4 \
    -draw "circle 64,64 64,6" -font "$FONT" -fill white -stroke none \
    -pointsize 76 -gravity center -annotate +0+2 "$2" -resize 64x64 "$OUT/$1.png"
}
# Shoulder button: a dark rounded bar (bumper) or a taller rounded block
# (trigger) with its label.
shoulder() { # name label height
  local h=$3 top=$(( (S - $3) / 2 ))
  magick -size ${S}x${S} xc:none -fill '#3a3f47' -stroke '#d8dde5' -strokewidth 5 \
    -draw "roundrectangle 6,$top 121,$((top + h)) 22,22" -font "$FONT" -fill white \
    -stroke none -pointsize 54 -gravity center -annotate +0+0 "$2" \
    -resize 64x64 "$OUT/$1.png"
}
# Stick: a ring with a dark cap and its label.
stick() { # name label
  magick -size ${S}x${S} xc:none -fill '#3a3f47' -stroke '#d8dde5' -strokewidth 6 \
    -draw "circle 64,64 64,8" -fill '#23272d' -stroke none -draw "circle 64,64 64,26" \
    -font "$FONT" -fill white -pointsize 44 -gravity center -annotate +0+0 "$2" \
    -resize 64x64 "$OUT/$1.png"
}
# D-pad: a cross with some arms lit (any of up/down/left/right).
dpad() { # name arms...
  local name=$1; shift
  local draws=()
  for arm in "$@"; do
    case $arm in
      up)    draws+=(-draw "polygon 64,14 78,36 50,36") ;;
      down)  draws+=(-draw "polygon 64,114 78,92 50,92") ;;
      left)  draws+=(-draw "polygon 14,64 36,50 36,78") ;;
      right) draws+=(-draw "polygon 114,64 92,50 92,78") ;;
    esac
  done
  magick -size ${S}x${S} xc:none -fill '#3a3f47' -stroke '#d8dde5' -strokewidth 5 \
    -draw "path 'M 44,6 L 84,6 L 84,44 L 122,44 L 122,84 L 84,84 L 84,122 L 44,122 L 44,84 L 6,84 L 6,44 L 44,44 Z'" \
    -fill white -stroke none "${draws[@]}" -resize 64x64 "$OUT/$name.png"
}

face a A '#4caf50'
face b B '#e53935'
face x X '#1e88e5'
face y Y '#fbc02d'
shoulder l1 L1 64
shoulder r1 R1 64
shoulder l2 L2 100
shoulder r2 R2 100
stick ls LS
stick rs RS
stick l3 L3
dpad dpad_updown up down
dpad dpad_left left
dpad dpad_right right
echo "Icons written to $OUT"
