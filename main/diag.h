#pragma once

#include <stddef.h>

// Capture ESP-IDF log output into a fixed RAM ring buffer so it can be read
// back over the config portal (/diag). On boards with no usable serial console
// this is the only way to see why init failed.
void diag_init(void);

// Copy the captured log (oldest-first, most recent retained if it wrapped) into
// out. Always NUL-terminates. Returns the number of bytes written.
size_t diag_log_copy(char *out, size_t max);

// Slots in the boot heap trace, and the buffer a caller needs to render it.
#define DIAG_HEAP_TRACE_SLOTS 16
#define DIAG_HEAP_TRACE_MAX 768

// Record (and log) the heap at a boot milestone: free, largest free
// *contiguous* block, and the DMA figures. The largest block is the number that
// decides whether a WebP decode can fit - a 64x32 lossless frame needs one
// 12,544-byte run - and total free heap has misled this bring-up three times.
//
// Marks are kept in a small fixed array that /diag always serves, because the
// 1 KB log ring is overwritten by steady-state decode-failure spam within
// seconds of boot: the boot sequence is exactly the part that gets lost, and it
// is the part that says which stage fragments the heap.
void diag_log_heap(const char *stage);

// Render the recorded marks, one line each. Always NUL-terminates. Returns the
// number of bytes written.
size_t diag_heap_trace_report(char *out, size_t max);

// Dump the current free-block *distribution*. heap_caps_print_heap_info() and
// heap_caps_dump() are not usable here: both write with printf(), which goes
// nowhere on a board with no console. This probes instead - it asks the heap
// how many blocks of each size class it can actually satisfy - so the answer
// comes back through the ESP-IDF logger, which /diag can read.
// Called by /diag?heap=1.
void diag_dump_heap_layout(void);
