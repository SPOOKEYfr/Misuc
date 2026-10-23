#!/bin/bash
# Installer branding art for Misuc (NSIS wizard bitmaps + Windows icon).
# Sources stay PNG; the .bmp files are the exact bitmaps CPack/NSIS consume.
set -euo pipefail
cd "$(dirname "$0")/../.."

VER="${1:-0.3.1}"

# ─── 1. multi-resolution app icon (.ico) straight from the hicolor set ───────
convert assets/hicolor/16x16/apps/misuc.png \
        assets/hicolor/24x24/apps/misuc.png \
        assets/hicolor/32x32/apps/misuc.png \
        assets/hicolor/48x48/apps/misuc.png \
        assets/hicolor/64x64/apps/misuc.png \
        assets/hicolor/128x128/apps/misuc.png \
        assets/hicolor/256x256/apps/misuc.png \
        assets/nsis/misuc.ico

# ─── 2. welcome / finish sidebar 164x314 (dark, golden, flame accents) ────────
# vertical gradient panel: deep navy → near black (top→bottom)
convert -size 164x314 gradient:'#26262e'-'#141419' assets/nsis/_bg.png

# subtle horizontal glow behind the logo (warm halo, edges fully transparent)
convert -size 164x84 radial-gradient:'rgba(247,183,51,0.34)'-'rgba(247,183,51,0)' assets/nsis/_glow.png

# panel = bg + glow
composite -geometry +0+50 assets/nsis/_glow.png assets/nsis/_bg.png assets/nsis/_panel.png

# logo centered (256 → 120 px)
convert docs/assets/misuc-logo.png -resize 120x120 assets/nsis/_logo120.png
composite -geometry +22+58 assets/nsis/_logo120.png assets/nsis/_panel.png assets/nsis/_panel.png

# "MISUC" wordmark: gold, with a soft flame-colored under-glow
convert -background none -font DejaVu-Sans-Bold -pointsize 34 \
        -fill '#f0b429' label:MISUC assets/nsis/_wordmark.png
convert assets/nsis/_wordmark.png -channel A -blur 0x5 -fill '#e8590c' -colorize 55 assets/nsis/_glowword.png
convert assets/nsis/_glowword.png assets/nsis/_wordmark.png -compose over -composite assets/nsis/_wordmark.png

wm_size=$(identify -format %wx%h assets/nsis/_wordmark.png)
token=$(echo $wm_size | tr 'x' ' ')
set -- $token
composite -geometry +$(( (164 - $1) / 2 ))+205 assets/nsis/_wordmark.png assets/nsis/_panel.png assets/nsis/_panel.png

# accent flame-gradient bar + thin separator
convert -size 164x6 gradient:'#f7b733'-'#e8590c' assets/nsis/_accentbar.png
composite -geometry +0+250 assets/nsis/_accentbar.png assets/nsis/_panel.png assets/nsis/_panel.png

# subtitle
convert -background none -font DejaVu-Sans -pointsize 10 \
        -fill '#8a8a96' label:"DIGITAL AUDIO WORKSTATION" assets/nsis/_subtitle.png
sub=$(identify -format %w assets/nsis/_subtitle.png)
composite -geometry +$(( (164 - sub) / 2 ))+266 assets/nsis/_subtitle.png assets/nsis/_panel.png assets/nsis/_panel.png

# version strip
convert -background none -font DejaVu-Sans -pointsize 9 \
        -fill '#6b6b74' label:"v${VER}" assets/nsis/_version.png
ver=$(identify -format %w assets/nsis/_version.png)
composite -geometry +$(( (164 - ver) / 2 ))+288 assets/nsis/_version.png assets/nsis/_panel.png assets/nsis/_panel.png

# final: keep PNG for review, emit the exact 24-bit BMP NSIS consumes
convert assets/nsis/_panel.png assets/nsis/welcome.png
convert assets/nsis/_panel.png BMP3:assets/nsis/welcome.bmp

# ─── 3. page header 150x57 (light, matches the standard wizard chrome) ───────
convert -size 150x57 xc:'#f0f0f0' assets/nsis/_header.png
convert docs/assets/misuc-logo.png -resize 40x40 assets/nsis/_logo40.png
convert docs/assets/misuc-logo.png -resize 56x56 -roll +0+8 assets/nsis/_logoClip.png 2>/dev/null || true
composite -geometry +8+8 assets/nsis/_logo40.png assets/nsis/_header.png assets/nsis/_header.png
convert -background none -font DejaVu-Sans-Bold -pointsize 20 \
        -fill '#c8871a' label:MISUC assets/nsis/_htext.png
composite -geometry +58+17 assets/nsis/_htext.png assets/nsis/_header.png assets/nsis/_header.png
convert -background none -font DejaVu-Sans -pointsize 8 \
        -fill '#7d7d85' label:"MIDI WORKSTATION" assets/nsis/_htag.png
composite -geometry +58+38 assets/nsis/_htag.png assets/nsis/_header.png assets/nsis/_header.png
convert assets/nsis/_header.png assets/nsis/header.png
convert assets/nsis/_header.png BMP3:assets/nsis/header.bmp

rm -f assets/nsis/_bg.png assets/nsis/_glow.png assets/nsis/_panel.png assets/nsis/_logo120.png \
      assets/nsis/_logo40.png assets/nsis/_logoClip.png assets/nsis/_wordmark.png assets/nsis/_glowword.png \
      assets/nsis/_accentbar.png assets/nsis/_subtitle.png assets/nsis/_htext.png assets/nsis/_htag.png \
      assets/nsis/_version.png assets/nsis/_header.png
identify assets/nsis/welcome.bmp assets/nsis/header.bmp assets/nsis/misuc.ico
echo "nsis assets done"
