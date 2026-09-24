#include "diag.h"

#include <esp_log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DIAG_LOG_CAP 4096

static char s_log[DIAG_LOG_CAP];
static size_t s_len = 0;
static vprintf_like_t s_prev = NULL;

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
