// Renders an animated WebP as a PPM montage under several decode strategies,
// so the visual difference between them can be compared. Uses libwebp's own
// WebPAnimDecoder as the reference.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/mux_types.h>

#define MAXF 20
#define SCALE 3
#define COLS 5

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

// ---- montage helpers ----
static int g_frames = 0;
static uint8_t g_rgb[MAXF][64 * 32 * 3];  // up to 64x32 frames
static int g_w = 0, g_h = 0;

static void push_frame_rgba(const uint8_t *rgba, int w, int h) {
  if (g_frames >= MAXF) return;
  for (int i = 0; i < w * h; i++) {
    const int a = rgba[4 * i + 3];
    for (int c = 0; c < 3; c++) {
      const int s = rgba[4 * i + c];
      g_rgb[g_frames][3 * i + c] = (uint8_t)((s * a + 255 * (255 - a)) / 255);
    }
  }
  g_frames++;
}

static void push_frame_rgb(const uint8_t *rgb, int w, int h) {
  if (g_frames >= MAXF) return;
  memcpy(g_rgb[g_frames], rgb, (size_t)w * h * 3);
  g_frames++;
}

static void write_ppm(const char *path, int w, int h) {
  if (g_frames == 0) return;
  const int cols = g_frames < COLS ? g_frames : COLS;
  const int rows = (g_frames + COLS - 1) / COLS;
  const int cw = w * SCALE, ch = h * SCALE;
  const int W = cols * cw + (cols + 1) * 2;
  const int H = rows * ch + (rows + 1) * 2;
  FILE *f = fopen(path, "wb");
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  uint8_t *row = malloc((size_t)W * 3);
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      int gi = -1, sx = 0, sy = 0;
      for (int gy = 0; gy < rows && gi < 0; gy++) {
        for (int gx = 0; gx < cols; gx++) {
          const int idx = gy * COLS + gx;
          if (idx >= g_frames) continue;
          const int ox = gx * cw + (gx + 1) * 2;
          const int oy = gy * ch + (gy + 1) * 2;
          if (x >= ox && x < ox + cw && y >= oy && y < oy + ch) {
            gi = idx; sx = (x - ox) / SCALE; sy = (y - oy) / SCALE;
          }
        }
      }
      uint8_t *p = row + 3 * x;
      if (gi < 0) { p[0] = p[1] = p[2] = 40; }
      else { memcpy(p, &g_rgb[gi][3 * (sy * w + sx)], 3); }
    }
    fwrite(row, 1, (size_t)W * 3, f);
  }
  free(row);
  fclose(f);
  printf("  wrote %s  (%d frames, %dx%d each)\n", path, g_frames, w, h);
}

// ---- strategies ----
static void do_reference(const uint8_t *buf, size_t len) {
  WebPAnimDecoderOptions o;
  WebPAnimDecoderOptionsInit(&o);
  o.color_mode = MODE_RGBA;
  WebPData d = {buf, len};
  WebPAnimDecoder *dec = WebPAnimDecoderNew(&d, &o);
  if (!dec) { printf("  WebPAnimDecoderNew failed\n"); return; }
  WebPAnimInfo info;
  WebPAnimDecoderGetInfo(dec, &info);
  uint8_t *fb; int ts;
  while (g_frames < MAXF && WebPAnimDecoderGetNext(dec, &fb, &ts))
    push_frame_rgba(fb, info.canvas_width, info.canvas_height);
  WebPAnimDecoderDelete(dec);
}

// variant: 0 = naive (decode at origin, canvas stride - what gfx.c does today)
//          1 = offset-aware (decode at frame x/y, canvas stride, no blend)
static void do_variant(const uint8_t *buf, size_t len, int variant) {
  int w, h;
  WebPGetInfo(buf, len, &w, &h);
  uint8_t *canvas = calloc(1, (size_t)w * h * 3);
  WebPData d = {buf, len};
  WebPDemuxer *dm = WebPDemux(&d);
  WebPIterator it;
  if (dm && WebPDemuxGetFrame(dm, 1, &it)) {
    do {
      if (g_frames >= MAXF) break;
      WebPDecoderConfig cfg;
      WebPInitDecoderConfig(&cfg);
      cfg.output.colorspace = MODE_RGB;
      cfg.output.is_external_memory = 1;
      cfg.output.u.RGBA.stride = w * 3;
      cfg.output.u.RGBA.size = (size_t)w * h * 3;
      cfg.options.no_fancy_upsampling = 1;
      if (variant == 0) {
        cfg.output.u.RGBA.rgba = canvas;
      } else {
        cfg.output.u.RGBA.rgba =
            canvas + (size_t)it.y_offset * w * 3 + (size_t)it.x_offset * 3;
        cfg.output.u.RGBA.size = (size_t)it.height * w * 3;
      }
      if (WebPDecode(it.fragment.bytes, it.fragment.size, &cfg) == VP8_STATUS_OK)
        push_frame_rgb(canvas, w, h);
      WebPFreeDecBuffer(&cfg.output);
    } while (WebPDemuxNextFrame(&it));
    WebPDemuxReleaseIterator(&it);
  }
  if (dm) WebPDemuxDelete(dm);
  free(canvas);
}

// variant 2: one RGB canvas + a frame-sized RGBA scratch, with libwebp's
// keyframe / blend / dispose semantics. This is the design intended for the
// device: it never holds a second full canvas.
static int is_key_frame(const WebPIterator *cur, const WebPIterator *prev,
                        int prev_was_key, int cw, int ch) {
  if (cur->frame_num == 1) return 1;
  if ((!cur->has_alpha || cur->blend_method == WEBP_MUX_NO_BLEND) &&
      cur->width == cw && cur->height == ch) return 1;
  return prev->dispose_method == WEBP_MUX_DISPOSE_BACKGROUND &&
         ((prev->width == cw && prev->height == ch) || prev_was_key);
}

static void do_scratch(const uint8_t *buf, size_t len) {
  int cw, ch;
  WebPGetInfo(buf, len, &cw, &ch);
  uint8_t *canvas = calloc(1, (size_t)cw * ch * 3);
  uint8_t *scratch = malloc((size_t)cw * ch * 4);
  WebPData d = {buf, len};
  WebPDemuxer *dm = WebPDemux(&d);
  WebPIterator it;
  WebPIterator prev;
  int prev_was_key = 0, have_prev = 0;
  memset(&prev, 0, sizeof(prev));
  if (dm && WebPDemuxGetFrame(dm, 1, &it)) {
    do {
      if (g_frames >= MAXF) break;
      const int key = is_key_frame(&it, &prev, prev_was_key, cw, ch);
      if (key) memset(canvas, 0, (size_t)cw * ch * 3);

      WebPDecoderConfig cfg;
      WebPInitDecoderConfig(&cfg);
      cfg.output.colorspace = MODE_RGBA;
      cfg.output.is_external_memory = 1;
      cfg.output.u.RGBA.rgba = scratch;
      cfg.output.u.RGBA.stride = it.width * 4;
      cfg.output.u.RGBA.size = (size_t)it.height * it.width * 4;
      cfg.options.no_fancy_upsampling = getenv("FANCY") ? 0 : 1;
      if (WebPDecode(it.fragment.bytes, it.fragment.size, &cfg) != VP8_STATUS_OK) {
        WebPFreeDecBuffer(&cfg.output);
        break;
      }
      WebPFreeDecBuffer(&cfg.output);

      const int do_blend =
          (it.frame_num > 1 && it.blend_method == WEBP_MUX_BLEND && !key);
      for (int y = 0; y < it.height; y++) {
        const int cy = it.y_offset + y;
        if (cy < 0 || cy >= ch) continue;
        for (int x = 0; x < it.width; x++) {
          const int cx = it.x_offset + x;
          if (cx < 0 || cx >= cw) continue;
          const uint8_t *s = scratch + ((size_t)y * it.width + x) * 4;
          uint8_t *dst = canvas + ((size_t)cy * cw + cx) * 3;
          const int a = s[3];
          if (!do_blend) {
            dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2];
          } else if (a == 0) {
            /* keep */
          } else if (a == 255) {
            dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2];
          } else {
            for (int c = 0; c < 3; c++)
              dst[c] = (uint8_t)((s[c] * a + dst[c] * (255 - a)) / 255);
          }
        }
      }
      if (it.dispose_method == WEBP_MUX_DISPOSE_BACKGROUND) {
        for (int y = 0; y < it.height; y++) {
          const int cy = it.y_offset + y;
          if (cy < 0 || cy >= ch) continue;
          for (int x = 0; x < it.width; x++) {
            const int cx = it.x_offset + x;
            if (cx < 0 || cx >= cw) continue;
            memset(canvas + ((size_t)cy * cw + cx) * 3, 0, 3);
          }
        }
      }
      push_frame_rgb(canvas, cw, ch);
      WebPDemuxReleaseIterator(&prev);
      prev = it;
      prev_was_key = key;
      have_prev = 1;
    } while (WebPDemuxNextFrame(&it));
    if (have_prev) WebPDemuxReleaseIterator(&prev);
    else WebPDemuxReleaseIterator(&it);
  }
  if (dm) WebPDemuxDelete(dm);
  free(canvas);
  free(scratch);
}

int main(int argc, char **argv) {
  const char *file = argv[1];
  const char *tag = argv[2];
  size_t len;
  uint8_t *buf = readfile(file, &len);
  if (!buf) { printf("cannot read %s\n", file); return 1; }
  int w, h;
  WebPGetInfo(buf, len, &w, &h);
  g_w = w; g_h = h;
  char path[512];

  g_frames = 0; do_reference(buf, len);
  snprintf(path, sizeof(path), "render_%s_ref.ppm", tag);
  write_ppm(path, w, h);

  g_frames = 0; do_variant(buf, len, 0);
  snprintf(path, sizeof(path), "render_%s_naive.ppm", tag);
  write_ppm(path, w, h);

  g_frames = 0; do_variant(buf, len, 1);
  snprintf(path, sizeof(path), "render_%s_offset.ppm", tag);
  write_ppm(path, w, h);

  g_frames = 0; do_scratch(buf, len);
  snprintf(path, sizeof(path), "render_%s_scratch.ppm", tag);
  write_ppm(path, w, h);

  free(buf);
  return 0;
}
