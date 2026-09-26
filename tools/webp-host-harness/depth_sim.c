// Simulates the HUB75 driver's colour chain on the host, to show what the panel
// actually receives at each colour depth, and how far each option is from truth.
//
// The driver (ESP32-HUB75-MatrixPanel-I2S-DMA.cpp, updateMatrixDMABuffer) does:
//
//   red_val = lumConvTab[red];                       // CIE table, chosen at
//                                                    // COMPILE time by
//                                                    // PIXEL_COLOR_DEPTH_BITS
//   for (i = depth-1; i >= 0; i--)
//     bitplane[i] = (red_val & (1 << i)) ? 1 : 0;    // the LOW depth bits
//
// which is only correct when the compile-time macro and the run-time depth agree,
// because that is what makes the table's output range match the bits being read.
// The ESP-IDF build sets the macro to 8; the port passes 5 at run time.
#include <stdio.h>
#include <stdlib.h>

#define PIXEL_COLOR_DEPTH_BITS 8  // what the ESP-IDF build actually uses
#include "cie_luts.h"

extern const uint8_t *lut6_ptr(void);

#include <webp/decode.h>
#include <webp/demux.h>

static int absd(int a, int b) { return a > b ? a - b : b - a; }

static int panel_bits(int v, int depth, const uint8_t *lut) {
  return lut[v] & ((1 << depth) - 1);  // the driver's low-bit read
}

static int expand(int bits, int depth) {
  return bits * 255 / ((1 << depth) - 1);
}

struct cfg {
  const char *name;
  int depth;
  const uint8_t *lut;
};

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s FILE\n", argv[0]); return 2; }
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror("open"); return 2; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *data = malloc((size_t)sz);
  if (fread(data, 1, (size_t)sz, f) != (size_t)sz) { perror("read"); return 2; }
  fclose(f);

  const uint8_t *lut6 = lut6_ptr();

  struct cfg cfgs[] = {
      {"depth 5, 8-bit LUT  (the board today)", 5, lumConvTab_8bit},
      {"depth 6, 8-bit LUT  (before the trim)", 6, lumConvTab_8bit},
      {"depth 6, 6-bit LUT  (depth 6, matched)", 6, lut6},
      {"depth 8, 8-bit LUT  (depth 8, matched)", 8, lumConvTab_8bit},
  };
  const int ncfg = (int)(sizeof(cfgs) / sizeof(*cfgs));

  // Is the per-channel mapping even monotonic? That is the whole question.
  const int probes[] = {19, 60, 69, 120, 139, 160, 200, 240, 255};
  const int nprobe = (int)(sizeof(probes) / sizeof(*probes));
  printf("=== what the driver maps an input channel to ===\n");
  printf("%-38s", "input byte:");
  for (int i = 0; i < nprobe; i++) printf(" %4d", probes[i]);
  printf("\n");
  for (int c = 0; c < ncfg; c++) {
    printf("%-38s", cfgs[c].name);
    for (int i = 0; i < nprobe; i++)
      printf(" %4d", expand(panel_bits(probes[i], cfgs[c].depth, cfgs[c].lut),
                            cfgs[c].depth));
    printf("\n");
  }

  // Score each against the true image, over every frame.
  WebPData wd = {data, (size_t)sz};
  WebPDemuxer *dm = WebPDemux(&wd);
  if (!dm) { fprintf(stderr, "demux failed\n"); return 2; }

  long bad[8] = {0}, total = 0;
  long sumerr[8] = {0};
  int maxerr[8] = {0};

  WebPIterator it;
  for (int ok = WebPDemuxGetFrame(dm, 1, &it); ok; ok = WebPDemuxNextFrame(&it)) {
    const int w = it.width, h = it.height;
    uint8_t *ref = malloc((size_t)w * h * 4);
    WebPDecoderConfig rc;
    WebPInitDecoderConfig(&rc);
    rc.output.colorspace = MODE_RGBA;
    rc.output.is_external_memory = 1;
    rc.output.u.RGBA.rgba = ref;
    rc.output.u.RGBA.stride = w * 4;
    rc.output.u.RGBA.size = (size_t)w * h * 4;
    if (WebPDecode(it.fragment.bytes, it.fragment.size, &rc) != VP8_STATUS_OK) {
      free(ref);
      continue;
    }

    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const uint8_t *p = &ref[((size_t)y * w + x) * 4];
        if (p[3] != 255) continue;
        total++;
        for (int c = 0; c < ncfg; c++) {
          int e = 0;
          for (int k = 0; k < 3; k++) {
            const int shown =
                expand(panel_bits(p[k], cfgs[c].depth, cfgs[c].lut),
                       cfgs[c].depth);
            const int d = absd(shown, p[k]);
            e += d;
            if (d > 64) bad[c]++;
          }
          sumerr[c] += e;
          if (e > maxerr[c]) maxerr[c] = e;
        }
      }
    }
    free(ref);
  }
  WebPDemuxReleaseIterator(&it);
  WebPDemuxDelete(dm);
  free(data);

  printf("\n=== per-pixel error against the true image (%ld opaque pixels) ===\n",
         total);
  for (int c = 0; c < ncfg; c++) {
    printf("  %-38s mean/px %6.1f  worst %3d  channels off >64: %ld (%.1f%%)\n",
           cfgs[c].name, (double)sumerr[c] / (double)(total ? total : 1),
           maxerr[c], bad[c], 100.0 * (double)bad[c] / (double)(total * 3));
  }
  return 0;
}
