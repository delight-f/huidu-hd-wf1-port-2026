#!/bin/sh
# Build the WebP host harnesses against this repo's vendored libwebp, so the
# decoder can be measured and its output inspected on a workstation instead of
# by flashing the board.
#
#   ./build.sh          -> ./measure, ./render and ./rgb565_check
#   ./measure FILE...   decode peak transient heap per frame, under several
#                       decoder options
#   ./render FILE TAG   write render_TAG_{ref,naive,offset,scratch}.ppm montages
#                       (ref = libwebp's own WebPAnimDecoder)
#   ./rgb565_check FILE prove the MODE_RGB_565 byte layout against an RGBA decode
#
# __SSE2__ is undefined for the host build on purpose: the ESP-IDF component
# compiles libwebp without SIMD, and the _sse2.c files are not in the source
# list, so leaving it defined on an x86 host produces undefined references.
set -e
cd "$(dirname "$0")"

LW=../../managed_components/libwebp
if [ ! -d "$LW/src" ]; then
  echo "vendored libwebp not found at $LW/src" >&2
  echo "run 'idf.py build' once so the component manager fetches it" >&2
  exit 1
fi

SRC="$LW/src/dec/alpha_dec.c $LW/src/dec/buffer_dec.c $LW/src/dec/frame_dec.c \
$LW/src/dec/idec_dec.c $LW/src/dec/io_dec.c $LW/src/dec/quant_dec.c \
$LW/src/dec/tree_dec.c $LW/src/dec/vp8_dec.c $LW/src/dec/vp8l_dec.c \
$LW/src/dec/webp_dec.c $LW/src/demux/anim_decode.c $LW/src/demux/demux.c \
$LW/src/dsp/alpha_processing.c $LW/src/dsp/cpu.c $LW/src/dsp/dec.c \
$LW/src/dsp/dec_clip_tables.c $LW/src/dsp/filters.c $LW/src/dsp/lossless.c \
$LW/src/dsp/rescaler.c $LW/src/dsp/upsampling.c $LW/src/dsp/yuv.c \
$LW/src/utils/bit_reader_utils.c $LW/src/utils/color_cache_utils.c \
$LW/src/utils/filters_utils.c $LW/src/utils/huffman_utils.c \
$LW/src/utils/palette.c $LW/src/utils/quant_levels_dec_utils.c \
$LW/src/utils/rescaler_utils.c $LW/src/utils/random_utils.c \
$LW/src/utils/thread_utils.c $LW/src/utils/utils.c"

CFLAGS="-O1 -g -U__SSE2__ -U__SSE4_1__ -U__AVX2__ -I $LW/src -I $LW"

cc $CFLAGS -o measure measure.c memtrack.c $SRC -lm \
  -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free
cc $CFLAGS -o render render.c $SRC -lm
cc $CFLAGS -o rgb565_check rgb565_check.c $SRC -lm

echo "built: ./measure ./render ./rgb565_check"
echo
echo "sample assets to try:"
echo "  components/assets/tronbyt.webp      (360 frames, lossless)"
echo "  components/assets/tronconfig.webp   (still)"
echo "  tools/config.webp                   (856 B still, lossless)"
