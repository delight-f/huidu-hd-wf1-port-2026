#include "diag.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DIAG_LOG_CAP 1024

static const char *TAG = "heap";

static char s_log[DIAG_LOG_CAP];
static size_t s_len = 0;
static vprintf_like_t s_prev = NULL;

// Boot heap trace. Kept outside the log ring because the ring is overwritten by
// steady-state decode-failure spam within seconds, and the boot sequence is
// exactly the part worth keeping.
typedef struct {
  const char *stage;
  uint32_t free_int;
  uint32_t largest_int;
  uint32_t free_dma;
} heap_mark_t;

static heap_mark_t s_marks[DIAG_HEAP_TRACE_SLOTS];
static size_t s_marks_n = 0;

// Append to the buffer, dropping the oldest bytes once it is full so the most
// recent output (the error that matters) is always retained.
static void append(const char *data, size_t n) {
  if (n >= DIAG_LOG_CAP) {
    memcpy(s_log, data + (n - (DIAG_LOG_CAP - 1)), DIAG_LOG_CAP - 1);
    s_len = DIAG_LOG_CAP - 1;
    return;
  }
  if (s_len + n > DIAG_LOG_CAP - 1) {
    size_t drop = s_len + n - (DIAG_LOG_CAP - 1);
    memmove(s_log, s_log + drop, s_len - drop);
    s_len -= drop;
  }
  memcpy(s_log + s_len, data, n);
  s_len += n;
}

static int diag_vprintf(const char *fmt, va_list ap) {
  va_list copy;
  va_copy(copy, ap);

  char line[256];
  int n = vsnprintf(line, sizeof(line), fmt, copy);
  va_end(copy);
  if (n > 0) {
    size_t l = (size_t)n;
    if (l > sizeof(line) - 1) {
      l = sizeof(line) - 1;
    }
    append(line, l);
  }

  if (s_prev != NULL) {
    return s_prev(fmt, ap);
  }
  return vprintf(fmt, ap);
}

void diag_init(void) { s_prev = esp_log_set_vprintf(diag_vprintf); }

size_t diag_log_copy(char *out, size_t max) {
  if (out == NULL || max == 0) {
    return 0;
  }
  size_t n = (s_len < max - 1) ? s_len : (max - 1);
  memcpy(out, s_log, n);
  out[n] = '\0';
  return n;
}

void diag_log_heap(const char *stage) {
  if (stage == NULL) {
    stage = "?";
  }

  const uint32_t free_int =
      (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t largest_int =
      (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

  // Keep the milestone even if the log ring drops it within seconds.
  if (s_marks_n < DIAG_HEAP_TRACE_SLOTS) {
    heap_mark_t *m = &s_marks[s_marks_n++];
    m->stage = stage;
    m->free_int = free_int;
    m->largest_int = largest_int;
    m->free_dma = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
  }

  multi_heap_info_t info;
  heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
  ESP_LOGI(TAG,
           "%s: int free=%u largest=%u blocks=%u/%u | dma free=%u largest=%u",
           stage, (unsigned)free_int, (unsigned)largest_int,
           (unsigned)info.free_blocks, (unsigned)info.total_blocks,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

size_t diag_heap_trace_report(char *out, size_t max) {
  if (out == NULL || max == 0) {
    return 0;
  }

  size_t off = 0;
  for (size_t i = 0; i < s_marks_n; i++) {
    int n = snprintf(out + off, max - off, "%-22s free=%-6u largest=%-6u dma=%-6u\n",
                     s_marks[i].stage, (unsigned)s_marks[i].free_int,
                     (unsigned)s_marks[i].largest_int,
                     (unsigned)s_marks[i].free_dma);
    if (n < 0 || (size_t)n >= max - off) {
      break;
    }
    off += (size_t)n;
  }
  out[off] = '\0';
  return off;
}

void diag_dump_heap_layout(void) {
  // Report the free blocks themselves, largest first.
  //
  // heap_caps_get_largest_free_block() answers "how big is the biggest" but not
  // "how many are there", and on this board the difference is the whole problem:
  // libwebp wants two ~12.5 KB blocks live at once, so 48 KB free in one piece
  // decodes and the same 48 KB in twenty pieces does not. The aggregate hides
  // that, and inferring the layout from totals has already sent this bring-up
  // down several blind alleys.
  //
  // heap_caps_malloc() cannot be given a size larger than the largest free
  // block, so repeatedly taking the largest available drains the heap from the
  // top down and the sequence of sizes *is* the block list. Everything is freed
  // again before returning, and the drain stops short of exhausting the heap so
  // the HTTP response can still be allocated afterwards.
  enum { MAX_BLOCKS = 20, MIN_REPORT = 512, RESERVE = 8 * 1024 };
  size_t sizes[MAX_BLOCKS];
  void *held[MAX_BLOCKS];
  int n = 0;
  size_t total = 0;

  const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t budget = free_now > RESERVE ? free_now - RESERVE : 0;

  while (n < MAX_BLOCKS) {
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest < MIN_REPORT || total + largest > budget) {
      break;
    }
    void *p = heap_caps_malloc(largest, MALLOC_CAP_INTERNAL);
    if (p == NULL) {
      break;
    }
    held[n] = p;
    sizes[n] = largest;
    n++;
    total += largest;
  }

  multi_heap_info_t info;
  heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);

  char line[256];
  size_t off = 0;
  line[0] = '\0';
  for (int i = 0; i < n; i++) {
    int w = snprintf(line + off, sizeof(line) - off, "%u ",
                     (unsigned)sizes[i]);
    if (w < 0 || (size_t)w >= sizeof(line) - off) {
      break;
    }
    off += (size_t)w;
  }

  for (int i = 0; i < n; i++) {
    free(held[i]);
  }

  ESP_LOGI(TAG, "blocklist: free=%u largest=%u blocks=%u/%u",
           (unsigned)info.total_free_bytes, (unsigned)info.largest_free_block,
           (unsigned)info.free_blocks, (unsigned)info.total_blocks);
  ESP_LOGI(TAG, "blocklist (largest first): %s", line);
}
