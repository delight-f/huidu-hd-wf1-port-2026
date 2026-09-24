// Measure what libwebp allocates inside a single WebPDecode() call, and how
// that changes with decoder options (cropping, scaling). This is how the
// decoder's transient heap requirement was established, and how the option
// dead ends (cropping buys nothing, scaling costs more) were closed off.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <webp/decode.h>
#include <webp/demux.h>

void memtrack_reset(void);
long memtrack_peak(void);

static uint8_t *readfile(const char *p, size_t *n) {
  FILE *f = fopen(p, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long s = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(s);
  if (fread(b, 1, s, f) != (size_t)s) { fclose(f); free(b); return NULL; }
  fclose(f);
  *n = (size_t)s;
  return b;
}

// Decode one fragment with the given options; report peak transient heap.
static void measure(const char *label, const uint8_t *frag, size_t frag_len,
                    size_t out_stride, size_t out_size, int channels,
                    int use_crop, int crop_x, int crop_y, int crop_w, int crop_h,
                    int use_scale, int sw, int sh) {
  uint8_t *out = calloc(1, out_size ? out_size : 1);
  WebPDecoderConfig cfg;
  WebPInitDecoderConfig(&cfg);
  cfg.output.colorspace = channels == 3 ? MODE_RGB : MODE_RGBA;
  cfg.output.is_external_memory = 1;
  cfg.output.u.RGBA.rgba = out;
  cfg.output.u.RGBA.stride = (int)out_stride;
  cfg.output.u.RGBA.size = out_size;
  cfg.options.no_fancy_upsampling = 1;
  if (use_crop) {
    cfg.options.use_cropping = 1;
    cfg.options.crop_left = crop_x;
    cfg.options.crop_top = crop_y;
    cfg.options.crop_width = crop_w;
    cfg.options.crop_height = crop_h;
  }
  if (use_scale) {
    cfg.options.use_scaling = 1;
    cfg.options.scaled_width = sw;
    cfg.options.scaled_height = sh;
  }
  memtrack_reset();
  VP8StatusCode s = WebPDecode(frag, frag_len, &cfg);
  long peak = memtrack_peak();
  printf("  %-38s %-14s peak %6ld B\n", label,
         s == VP8_STATUS_OK ? "OK" : "FAIL", peak);
  free(out);
}

int main(int argc, char **argv) {
  for (int a = 1; a < argc; a++) {
    size_t len;
    uint8_t *buf = readfile(argv[a], &len);
    if (!buf) { printf("cannot read %s\n", argv[a]); continue; }
    int w, h;
    WebPGetInfo(buf, len, &w, &h);
    printf("\n== %s (%dx%d, %zu B) ==\n", argv[a], w, h, len);

    WebPData d = {buf, len};
    WebPDemuxer *dm = WebPDemux(&d);
    WebPIterator it;
    if (!dm || !WebPDemuxGetFrame(dm, 1, &it)) { printf("demux failed\n"); continue; }
    const uint8_t *frag = it.fragment.bytes;
    const size_t flen = it.fragment.size;
    printf("  frame 1: %dx%d, fragment %zu B\n", it.width, it.height, flen);

    const size_t canvas_rgb = (size_t)w * h * 3;
    const size_t canvas_rgba = (size_t)w * h * 4;

    measure("baseline MODE_RGB (canvas-sized)", frag, flen, (size_t)w * 3,
            canvas_rgb, 3, 0,0,0,0,0, 0,0,0);
    measure("baseline MODE_RGBA (canvas-sized)", frag, flen, (size_t)w * 4,
            canvas_rgba, 4, 0,0,0,0,0, 0,0,0);
    // Cropping: decode top and bottom halves separately.
    measure("crop top half 64x16 MODE_RGB", frag, flen, (size_t)w * 3,
            (size_t)w * 16 * 3, 3, 1, 0, 0, w, h / 2, 0,0,0);
    measure("crop bottom half 64x16 MODE_RGB", frag, flen, (size_t)w * 3,
            (size_t)w * 16 * 3, 3, 1, 0, h / 2, w, h / 2, 0,0,0);
    // Scaling: decode to half size.
    measure("scale to half (32x16) MODE_RGB", frag, flen, (size_t)(w / 2) * 3,
            (size_t)(w / 2) * (h / 2) * 3, 3, 0,0,0,0,0, 1, w / 2, h / 2);
    measure("scale to half (32x16) MODE_RGBA", frag, flen, (size_t)(w / 2) * 4,
            (size_t)(w / 2) * (h / 2) * 4, 4, 0,0,0,0,0, 1, w / 2, h / 2);

    WebPDemuxReleaseIterator(&it);
    WebPDemuxDelete(dm);
    free(buf);
  }
  return 0;
}
