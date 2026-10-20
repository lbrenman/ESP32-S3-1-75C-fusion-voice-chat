/* Compiles the minimp3 decoder (https://github.com/lieff/minimp3, CC0 public domain).
 * MINIMP3_NO_SIMD: the ESP32-S3 has no SSE/NEON, so use the portable C path.
 * MINIMP3_ONLY_MP3: skip MPEG layer 1/2 support to keep the code small. */
#define MINIMP3_NO_SIMD
#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
