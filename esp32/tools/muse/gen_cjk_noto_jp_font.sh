#!/bin/sh
# museStopWatch: regenerates components/muse/fonts/muse_font_cjk_noto_jp_16.c,
# Noto Sans JP Medium at 16 px, 4 bpp: CJK punctuation, kana, the fullwidth
# forms and every character of JIS X 0208 (its symbols and both kanji levels,
# 6355 kanji). Made like gen_cjk_noto_font.sh. Needs curl, python3 (or $PYTHON)
# and npx.
set -eu

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HERE/components/muse/fonts/muse_font_cjk_noto_jp_16.c"
PYTHON="${PYTHON:-python3}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

curl -fsSL -o "$TMP/noto.otf" \
    "https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/JP/NotoSansJP-Medium.otf"
# JIS X 0208 through EUC-JP's two-byte codes; ASCII is Montserrat's. Passed
# as characters, not code points: Windows caps a command line at 32K.
"$PYTHON" -c '
cps = set()
for hi in range(0xA1, 0xF5):
    for lo in range(0xA1, 0xFF):
        try:
            cps.add(ord(bytes([hi, lo]).decode("euc_jp")))
        except UnicodeDecodeError:
            pass
cps = sorted(c for c in cps if c > 0x7F and not 0x3000 <= c <= 0x30FF and not 0xFF00 <= c <= 0xFFEF)
import sys
sys.stdout.buffer.write("".join(chr(c) for c in cps).encode("utf-8"))' > "$TMP/jis.txt"
npx -y lv_font_conv@1.5.3 --font "$TMP/noto.otf" --size 16 --bpp 4 \
    --format lvgl --lv-font-name muse_font_cjk_noto_jp_16 --no-compress \
    -r 0x3000-0x30FF -r 0xFF00-0xFFEF --symbols "$(cat "$TMP/jis.txt")" -o "$TMP/font.c"
sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TMP/||g" "$TMP/font.c" > "$OUT"
echo "wrote $OUT"
