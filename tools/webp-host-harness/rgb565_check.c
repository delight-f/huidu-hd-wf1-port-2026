// Proves the RGB565 byte layout libwebp actually emits, and which of the two
// candidate extractions matches a known-good RGBA decode.
//
//   ./rgb565_check FILE
//
// Decodes every frame twice: once to RGBA (the reference - this is the path the
// firmware used before the 16-bit canvas, and it is known to render correctly),
// once to MODE_RGB_565 into a raw byte buffer. Then compares, per pixel, the RGB
// recovered from the 565 bytes by:
//
//   "bytes" - byte 0 = rg, byte 1 = gb            (what the driver now assumes)
//   "word"  - the two bytes read as a native u16  (what the driver assumed first)
//
// against the RGBA reference. Fully transparent pixels are skipped, because
// libwebp leaves arbitrary colour in them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <webp/decode.h>
#include <webp/demux.h>

static uint8_t expand5(uint8_t v5) { return (uint8_t)((v5 << 3) | (v5 >> 2)); }
static uint8_t expand6(uint8_t v6) { return (uint8_t)((v6 << 2) | (v6 >> 4)); }

static void from_bytes(const uint8_t *b, uint8_t *r, uint8_t *g, uint8_t *bl) {
  const uint8_t rg = b[0], gb = b[1];
  *r = expand5((uint8_t)(rg >> 3));
  *g = expand6((uint8_t)(((rg & 0x07) << 3) | (gb >> 5)));
  *bl = expand5((uint8_t)(gb & 0x1F));
}

static void from_word(const uint8_t *b, uint8_t *r, uint8_t *g, uint8_t *bl) {
  const uint16_t p = (uint16_t)(b[0] | (b[1] << 8));  // little-endian read
  *r = expand5((uint8_t)((p >> 11) & 0x1F));
  *g = expand6((uint8_t)((p >> 5) & 0x3F));
  *bl = expand5((uint8_t)(p & 0x1F));
}

static int worst(uint8_t a, uint8_t b) {
  int d = (int)a - (int)b;
  return d < 0 ? -d : d;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s FILE\n", argv[0]); return 2; }
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror("open"); return 2; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *data = malloc((size_t)sz);
  if (fread(data, 1, (size_t)sz, f) != (size_t)sz) { perror("read"); return 2; }
  fclose(f);

  WebPData wd = {data, (size_t)sz};
  WebPDemuxer *dm = WebPDemux(&wd);
  if (dm == NULL) { fprintf(stderr, "demux failed\n"); return 2; }

  const int nframes = (int)WebPDemuxGetI(dm, WEBP_FF_FRAME_COUNT);
  const int cw = (int)WebPDemuxGetI(dm, WEBP_FF_CANVAS_WIDTH);
  const int ch = (int)WebPDemuxGetI(dm, WEBP_FF_CANVAS_HEIGHT);
  printf("%s: %d frame(s), canvas %dx%d\n", argv[1], nframes, cw, ch);

  long bytes_bad = 0, word_bad = 0, compared = 0, skipped = 0;
  int bytes_max = 0, word_max = 0;
  uint8_t shown = 0;

  WebPIterator it;
  for (int ok = WebPDemuxGetFrame(dm, 1, &it); ok;
       ok = WebPDemuxNextFrame(&it)) {
    const int w = it.width, h = it.height;
    uint8_t *ref = malloc((size_t)w * h * 4);
    uint8_t *s565 = malloc((size_t)w * h * 2);
    if (!ref || !s565) { fprintf(stderr, "oom\n"); return 2; }

    WebPDecoderConfig rc;
    WebPInitDecoderConfig(&rc);
    rc.output.colorspace = MODE_RGBA;
    rc.output.is_external_memory = 1;
    rc.output.u.RGBA.rgba = ref;
    rc.output.u.RGBA.stride = w * 4;
    rc.output.u.RGBA.size = (size_t)w * h * 4;
    if (WebPDecode(it.fragment.bytes, it.fragment.size, &rc) != VP8_STATUS_OK) {
      fprintf(stderr, "frame %d RGBA decode failed\n", it.frame_num);
      return 2;
    }

    WebPDecoderConfig sc;
    WebPInitDecoderConfig(&sc);
    sc.output.colorspace = MODE_RGB_565;
    sc.output.is_external_memory = 1;
    sc.output.u.RGBA.rgba = s565;
    sc.output.u.RGBA.stride = w * 2;
    sc.output.u.RGBA.size = (size_t)w * h * 2;
    if (WebPDecode(it.fragment.bytes, it.fragment.size, &sc) != VP8_STATUS_OK) {
      fprintf(stderr, "frame %d 565 decode failed\n", it.frame_num);
      return 2;
    }

    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const uint8_t *p = &ref[((size_t)y * w + x) * 4];
        if (p[3] != 255) { skipped++; continue; }
        const uint8_t *b = &s565[((size_t)y * w + x) * 2];
        uint8_t br, bg, bb, wr, wg, wb;
        from_bytes(b, &br, &bg, &bb);
        from_word(b, &wr, &wg, &wb);

        const int db = worst(br, p[0]) + worst(bg, p[1]) + worst(bb, p[2]);
        const int dw = worst(wr, p[0]) + worst(wg, p[1]) + worst(wb, p[2]);
        if (db > bytes_max) bytes_max = db;
        if (dw > word_max) word_max = dw;
        if (db > 24) bytes_bad++;
        if (dw > 24) word_bad++;
        compared++;

        // Show the first near-white opaque pixel of the first frame - the case
        // the board owner reported, where white came out blue or green.
        if (!shown && it.frame_num == 1 && p[0] > 200 && p[1] > 200 &&
            p[2] > 200) {
          printf("  frame 1 near-white pixel (%d,%d): reference rgb(%d,%d,%d)\n",
                 x, y, p[0], p[1], p[2]);
          printf("    565 bytes = 0x%02x 0x%02x\n", b[0], b[1]);
          printf("    'bytes' extraction -> rgb(%d,%d,%d)\n", br, bg, bb);
          printf("    'word'  extraction -> rgb(%d,%d,%d)\n", wr, wg, wb);
          shown = 1;
        }
      }
    }
    free(ref);
    free(s565);
  }
  WebPDemuxReleaseIterator(&it);
  WebPDemuxDelete(dm);
  free(data);

  printf("compared %ld opaque pixels (%ld transparent skipped)\n", compared,
         skipped);
  printf("  'bytes' (byte0=rg, byte1=gb): %ld pixels off by >24, worst sum %d\n",
         bytes_bad, bytes_max);
  printf("  'word'  (native uint16_t)  : %ld pixels off by >24, worst sum %d\n",
         word_bad, word_max);
  printf("%s\n", bytes_max < 24 && word_max > bytes_max
                     ? "=> the byte-wise layout is the correct one"
                     : "=> inconclusive, inspect the numbers above");
  return 0;
}
