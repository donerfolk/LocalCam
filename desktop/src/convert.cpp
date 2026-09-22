#include "convert.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

struct Coef { int y, yOffset, rv, gu, gv, bu; };  // 16.16 fixed point

Coef coefficients(bool bt601, bool full) {
    const double kr = bt601 ? 0.299 : 0.2126, kb = bt601 ? 0.114 : 0.0722, kg = 1 - kr - kb;
    const double ys = full ? 1.0 : 255.0 / 219, cs = full ? 1.0 : 255.0 / 224;
    auto fx = [](double v) { return int(v * 65536 + (v < 0 ? -0.5 : 0.5)); };
    return {fx(ys), full ? 0 : 16, fx(2 * (1 - kr) * cs), fx(-2 * (1 - kb) * kb / kg * cs),
            fx(-2 * (1 - kr) * kr / kg * cs), fx(2 * (1 - kb) * cs)};
}

inline uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

inline void putPixel(uint8_t* out, const Coef& k, int y, int u, int v) {
    const int yy = (y - k.yOffset) * k.y + 32768;
    out[0] = clamp8((yy + k.bu * u) >> 16);
    out[1] = clamp8((yy + k.gu * u + k.gv * v) >> 16);
    out[2] = clamp8((yy + k.rv * v) >> 16);
}

}  // namespace

void drawNv12(const Nv12& s, uint8_t* dst, int dw, int dh, int rotate, bool mirror) {
    const Coef k = coefficients(s.bt601, s.fullRange);
    rotate &= 3;

    // Same size, upright: straight row-by-row conversion (the common case).
    if (rotate == 0 && s.width == dw && s.height == dh) {
        for (int y = 0; y < dh; y++) {
            const uint8_t* yr = s.y + size_t(y) * s.pitch;
            const uint8_t* cr = s.uv + size_t(y / 2) * s.pitch;
            uint8_t* out = dst + size_t(y) * dw * 3;
            for (int x = 0; x < dw; x++) {
                const int sx = mirror ? dw - 1 - x : x;
                putPixel(out + x * 3, k, yr[sx], cr[sx & ~1] - 128, cr[(sx & ~1) + 1] - 128);
            }
        }
        return;
    }

    // General case: fit the rotated picture into dst, bilinear luma, nearest chroma.
    const int rw = rotate & 1 ? s.height : s.width, rh = rotate & 1 ? s.width : s.height;
    int ow = dw, oh = dh;
    if (int64_t(rw) * dh > int64_t(rh) * dw) oh = int(int64_t(rh) * dw / rw);
    else ow = int(int64_t(rw) * dh / rh);
    const int ox = (dw - ow) / 2, oy = (dh - oh) / 2;
    std::memset(dst, 0, size_t(dw) * dh * 3);
    if (ow <= 0 || oh <= 0) return;

    // Source position (16.16) = column term + row term, whatever the rotation.
    const int64_t w1 = int64_t(s.width - 1) << 16, h1 = int64_t(s.height - 1) << 16;
    std::vector<int64_t> cx(ow), cy(ow), rx(oh), ry(oh);
    for (int x = 0; x < ow; x++) {
        int64_t u = int64_t(((x + 0.5) * rw / ow - 0.5) * 65536);
        if (mirror) u = (int64_t(rw - 1) << 16) - u;
        cx[x] = rotate == 0 ? u : rotate == 2 ? w1 - u : 0;
        cy[x] = rotate == 1 ? h1 - u : rotate == 3 ? u : 0;
    }
    for (int y = 0; y < oh; y++) {
        const int64_t v = int64_t(((y + 0.5) * rh / oh - 0.5) * 65536);
        rx[y] = rotate == 1 ? v : rotate == 3 ? w1 - v : 0;
        ry[y] = rotate == 0 ? v : rotate == 2 ? h1 - v : 0;
    }
    for (int y = 0; y < oh; y++) {
        uint8_t* out = dst + (size_t(oy + y) * dw + ox) * 3;
        for (int x = 0; x < ow; x++, out += 3) {
            const int64_t fx = std::clamp<int64_t>(cx[x] + rx[y], 0, w1);
            const int64_t fy = std::clamp<int64_t>(cy[x] + ry[y], 0, h1);
            const int x0 = int(fx >> 16), y0 = int(fy >> 16);
            const int ax = int(fx >> 8) & 255, ay = int(fy >> 8) & 255;
            const int x1 = std::min(x0 + 1, s.width - 1), y1 = std::min(y0 + 1, s.height - 1);
            const uint8_t* r0 = s.y + size_t(y0) * s.pitch;
            const uint8_t* r1 = s.y + size_t(y1) * s.pitch;
            const int top = r0[x0] * (256 - ax) + r0[x1] * ax, bottom = r1[x0] * (256 - ax) + r1[x1] * ax;
            const uint8_t* c = s.uv + size_t(y0 / 2) * s.pitch + (x0 & ~1);
            putPixel(out, k, (top * (256 - ay) + bottom * ay + 32768) >> 16, c[0] - 128, c[1] - 128);
        }
    }
}
