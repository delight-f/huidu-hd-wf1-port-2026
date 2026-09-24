#pragma once

#include <stddef.h>

// Capture ESP-IDF log output into a fixed RAM ring buffer so it can be read
// back over the config portal (/diag). On boards with no usable serial console
// this is the only way to see why init failed.
void diag_init(void);

// Copy the captured log (oldest-first, most recent retained if it wrapped) into
// out. Always NUL-terminates. Returns the number of bytes written.
size_t diag_log_copy(char *out, size_t max);
