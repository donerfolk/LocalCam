#pragma once
#include <cstdint>

// A decoded NV12 picture: Y plane, then interleaved half-resolution UV plane, both with the same pitch.
struct Nv12 {
    const uint8_t* y;
    const uint8_t* uv;
    int pitch;
    int width, height;
    bool bt601;      // else BT.709
    bool fullRange;  // else 16-235 video range
};

// Draws src into dst (top-down BGR24, dw x dh, no row padding): rotated clockwise by rotate * 90 degrees,
// then mirrored horizontally if asked, scaled to fit and centered on black.
void drawNv12(const Nv12& src, uint8_t* dst, int dw, int dh, int rotate, bool mirror);
