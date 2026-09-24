// malloc interception so we can measure how much libwebp allocates inside
// WebPDecode() - i.e. the transient heap it needs *on top of* our canvas.
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>

static long g_cur = 0;
static long g_peak = 0;
static int g_trace = 0;

void memtrack_trace(int on) { g_trace = on; }

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);

static void acc(void *p) {
  if (p) {
    long sz = (long)malloc_usable_size(p);
    g_cur += sz;
    if (g_cur > g_peak) g_peak = g_cur;
    if (g_trace) fprintf(stderr, "    +%6ld -> live %6ld\n", sz, g_cur);
  }
}

void *__wrap_malloc(size_t n) { void *p = __real_malloc(n); acc(p); return p; }
void *__wrap_calloc(size_t a, size_t b) { void *p = __real_calloc(a, b); acc(p); return p; }
void *__wrap_realloc(void *q, size_t n) {
  if (q) g_cur -= (long)malloc_usable_size(q);
  void *p = __real_realloc(q, n); acc(p); return p;
}
void __wrap_free(void *p) {
  if (p) {
    long sz = (long)malloc_usable_size(p);
    g_cur -= sz;
    if (g_trace) fprintf(stderr, "    -%6ld -> live %6ld\n", sz, g_cur);
  }
  __real_free(p);
}

void memtrack_reset(void) { g_peak = g_cur; }
long memtrack_peak(void) { return g_peak - g_cur; }
long memtrack_cur(void) { return g_cur; }
