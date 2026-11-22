#!/bin/sh
# LVGL fonts with French accents + a few FontAwesome symbols.
set -e
cd "$(dirname "$0")"
OUT=../components/radar_app/fonts
TXT=0x20-0x7E,0xA0-0xFF,0x2013,0x2014,0x2019,0x2022,0x2026,0x2192,0x2212,0x2191,0x2193
SYM=0xF1EB,0xF00B,0xF00D,0xF053,0xF054,0xF078,0xF021,0xF06E,0xF070,0xF55A,0xF00C,0xF071,0xF023,0xF072
gen() { # nom police taille avec_symboles
  if [ "$4" = 1 ]; then EXTRA="--font FontAwesome5-Solid+Brands+Regular.woff -r $SYM"; else EXTRA=""; fi
  npx -y lv_font_conv@1.5.2 --no-compress --bpp 4 --size $3 --format lvgl --lv-include lvgl.h \
    --font $2 -r $TXT $EXTRA -o $OUT/$1.c
}
gen font_m12 Montserrat-Medium.ttf 12 0
gen font_m14 Montserrat-Medium.ttf 14 1
gen font_s16 Montserrat-SemiBold.ttf 16 1
gen font_s20 Montserrat-SemiBold.ttf 20 1
gen font_b24 Montserrat-Bold.ttf 24 0
# Screen saver: big rounded clock digits (Nunito Black, OFL)
npx -y lv_font_conv@1.5.2 --no-compress --bpp 4 --size 340 --format lvgl --lv-include lvgl.h \
  --font Nunito-Black.woff -r 0x30-0x39 -o $OUT/font_clock.c
