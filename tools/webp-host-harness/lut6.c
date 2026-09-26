// The 6-bit CIE table, in its own translation unit: cie_luts.h only defines the
// table matching PIXEL_COLOR_DEPTH_BITS, so each depth needs a separate include.
#define PIXEL_COLOR_DEPTH_BITS 6
#include "cie_luts.h"

const uint8_t *lut6_ptr(void) { return lumConvTab_6bit; }
