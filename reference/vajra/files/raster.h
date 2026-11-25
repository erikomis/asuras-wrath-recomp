#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

struct rgb {
    float r, g, b;
};

inline rgb mix(rgb a, rgb b, float t)
{
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

struct grid {
    int w = 0, h = 0;
    std::vector<float> v;
    grid() = default;
    grid(int w, int h, float fill) : w(w), h(h), v((size_t)w * h, fill) {}
    float& at(int x, int y) { return v[(size_t)y * w + x]; }
    float at(int x, int y) const { return v[(size_t)y * w + x]; }
};

template <class F> float area(F&& sd, float x, float y, int n)
{
    const float d = sd(x, y);
    if (d >= 0.75f) return 0;
    if (d <= -0.75f) return 1;
    float acc = 0;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) acc += std::clamp(0.5f - n * sd(x + (i + 0.5f) / n - 0.5f, y + (j + 0.5f) / n - 0.5f), 0.f, 1.f);
    return acc / (float)(n * n);
}

template <class F> grid mask(int w, int h, F&& f)
{
    grid g(w, h, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) g.at(x, y) = f(x + 0.5f, y + 0.5f);
    return g;
}

inline float sd_round_rect(float x, float y, float cx, float cy, float hw, float hh, float r)
{
    r = std::min({ r, hw, hh });
    const float qx = std::fabs(x - cx) - hw + r, qy = std::fabs(y - cy) - hh + r;
    return std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - r;
}

inline float sd_polygon(float x, float y, const float* p, int n)
{
    float d = 1e30f, s = 1;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const float ex = p[2 * j] - p[2 * i], ey = p[2 * j + 1] - p[2 * i + 1], wx = x - p[2 * i], wy = y - p[2 * i + 1];
        const float t = std::clamp((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.f, 1.f);
        const float bx = wx - ex * t, by = wy - ey * t;
        d = std::min(d, bx * bx + by * by);
        const bool c0 = y >= p[2 * i + 1], c1 = y < p[2 * j + 1], c2 = ex * wy > ey * wx;
        if ((c0 && c1 && c2) || (!c0 && !c1 && !c2)) s = -s;
    }
    return s * std::sqrt(d);
}

inline grid blur(const grid& src, float sigma)
{
    grid a = src, b(src.w, src.h, 0);
    const float ideal = std::sqrt(12 * sigma * sigma / 3 + 1);
    int lo = (int)ideal;
    if (!(lo & 1)) lo--;
    const int m = (int)std::lround((12 * sigma * sigma - 3 * lo * lo - 12 * lo - 9) / (-4.f * lo - 4));
    for (int pass = 0; pass < 3; pass++) {
        const int r = ((pass < m ? lo : lo + 2) - 1) / 2;
        if (r <= 0) continue;
        const float inv = 1.f / (2 * r + 1);
        for (int dir = 0; dir < 2; dir++) {
            const int n = dir ? a.h : a.w, lines = dir ? a.w : a.h;
            for (int l = 0; l < lines; l++) {
                auto get = [&](int i) { return i < 0 || i >= n ? 0.f : dir ? a.at(l, i) : a.at(i, l); };
                float acc = 0;
                for (int i = -r; i <= r; i++) acc += get(i);
                for (int i = 0; i < n; i++) {
                    (dir ? b.at(l, i) : b.at(i, l)) = acc * inv;
                    acc += get(i + r + 1) - get(i - r);
                }
            }
            std::swap(a, b);
        }
    }
    return a;
}

inline float grain(int x, int y, uint32_t seed)
{
    uint32_t h = (uint32_t)x * 0x8DA6B343u ^ (uint32_t)y * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h = (h ^ (h >> 15)) * 0x2C1B3C6Du;
    h = (h ^ (h >> 12)) * 0x297A2D39u;
    return (float)((h ^ (h >> 15)) & 0xFFFF) / 32767.5f - 1;
}

struct canvas {
    int w, h;
    std::vector<float> px;
    canvas(int w, int h) : w(w), h(h), px((size_t)w * h * 4, 0) {}

    template <class C> void paint(const grid& mask, C&& color)
    {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float a = mask.at(x, y);
                if (a <= 0) continue;
                const auto [c, opacity] = color(x, y);
                a = std::min(a * opacity, 1.f);
                float* p = &px[((size_t)y * w + x) * 4];
                p[0] = c.r * a + p[0] * (1 - a);
                p[1] = c.g * a + p[1] * (1 - a);
                p[2] = c.b * a + p[2] * (1 - a);
                p[3] = a + p[3] * (1 - a);
            }
    }

    void store(uint8_t* out) const
    {
        grid alpha(w, h, 0), bleed[3] = { alpha, alpha, alpha };
        for (size_t i = 0; i < (size_t)w * h; i++) {
            alpha.v[i] = px[i * 4 + 3];
            for (int c = 0; c < 3; c++) bleed[c].v[i] = px[i * 4 + c];
        }
        const grid soft = blur(alpha, 1.5f);
        for (int c = 0; c < 3; c++) bleed[c] = blur(bleed[c], 1.5f);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            const float a = px[i * 4 + 3];
            for (int c = 0; c < 3; c++) {
                const float v = a > 1.f / 512 ? px[i * 4 + c] / a : soft.v[i] > 0 ? bleed[c].v[i] / soft.v[i] : 0;
                out[i * 4 + c] = (uint8_t)std::lround(std::clamp(v, 0.f, 1.f) * 255);
            }
            out[i * 4 + 3] = (uint8_t)std::lround(std::clamp(a, 0.f, 1.f) * 255);
        }
    }
};
