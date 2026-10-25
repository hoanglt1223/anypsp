// SPDX-License-Identifier: MIT
// anyui: the in-game menu toolkit shared by AnyGBA and AnyPSP (identical copies in both
// repositories; change both). Software rendering into 8-bit RGBA pictures: anti-aliased
// rounded rectangles, circles, lines and filled outlines, image blits, resizing and a box blur.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace anyui {

// Straight (not premultiplied) colour, sRGB components 0-1.
struct Color {
    float r = 0, g = 0, b = 0, a = 1;

    static constexpr Color rgb(std::uint32_t hex, float alpha = 1) {
        return {static_cast<float>((hex >> 16) & 0xFF) / 255.f, static_cast<float>((hex >> 8) & 0xFF) / 255.f,
                static_cast<float>(hex & 0xFF) / 255.f, alpha};
    }
    constexpr Color alpha(float a2) const { return {r, g, b, a * a2}; }
};

inline Color mix(Color x, Color y, float t) {
    return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t};
}

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
};

struct Point {
    float x = 0, y = 0;
};

// One pixel is R, G, B, A bytes in memory order (0xAABBGGRR on little-endian hosts), as
// SDL_PIXELFORMAT_RGBA32 and GL_RGBA/GL_UNSIGNED_BYTE expect.
inline std::uint32_t pack(float r, float g, float b, float a = 1) {
    const auto c = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

// Lerps `dst` toward `c` by c.a * coverage; the result is opaque when dst was.
inline void blend_pixel(std::uint32_t& dst, Color c, float coverage) {
    const float a = c.a * coverage;
    if (a <= 0.002f) return;
    const unsigned w = static_cast<unsigned>(std::min(a, 1.f) * 256.f + 0.5f);
    const unsigned v = 256 - w;
    const unsigned sr = static_cast<unsigned>(std::clamp(c.r, 0.f, 1.f) * 255.f + 0.5f);
    const unsigned sg = static_cast<unsigned>(std::clamp(c.g, 0.f, 1.f) * 255.f + 0.5f);
    const unsigned sb = static_cast<unsigned>(std::clamp(c.b, 0.f, 1.f) * 255.f + 0.5f);
    const std::uint32_t d = dst;
    const unsigned r = ((d & 0xFF) * v + sr * w) >> 8;
    const unsigned g = (((d >> 8) & 0xFF) * v + sg * w) >> 8;
    const unsigned b = (((d >> 16) & 0xFF) * v + sb * w) >> 8;
    const unsigned da = (d >> 24) & 0xFF;
    const unsigned outa = da + (((255 - da) * w) >> 8);
    dst = r | (g << 8) | (b << 16) | (outa << 24);
}

class Canvas {
public:
    Canvas() = default;
    Canvas(int w, int h) { resize(w, h); }

    void resize(int w, int h) {
        width_ = std::max(w, 0);
        height_ = std::max(h, 0);
        pixels_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0xFF000000u);
        clip_ = {0, 0, width_, height_};
        clips_.clear();
    }
    int width() const { return width_; }
    int height() const { return height_; }
    std::uint32_t* data() { return pixels_.data(); }
    const std::uint32_t* data() const { return pixels_.data(); }
    std::vector<std::uint32_t>& pixels() { return pixels_; }
    const std::vector<std::uint32_t>& pixels() const { return pixels_; }
    std::uint32_t* row(int y) { return pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_); }
    const std::uint32_t* row(int y) const {
        return pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
    }

    void clear(Color c) { std::fill(pixels_.begin(), pixels_.end(), pack(c.r, c.g, c.b, c.a)); }

    // Drawing is limited to the intersection of the pushed rectangles.
    void push_clip(Rect r) {
        clips_.push_back(clip_);
        const int x0 = std::max(clip_.x0, static_cast<int>(std::floor(r.x)));
        const int y0 = std::max(clip_.y0, static_cast<int>(std::floor(r.y)));
        const int x1 = std::min(clip_.x1, static_cast<int>(std::ceil(r.right())));
        const int y1 = std::min(clip_.y1, static_cast<int>(std::ceil(r.bottom())));
        clip_ = {x0, y0, std::max(x0, x1), std::max(y0, y1)};
    }
    void pop_clip() {
        if (clips_.empty()) return;
        clip_ = clips_.back();
        clips_.pop_back();
    }

    void blend(int x, int y, Color c, float coverage) {
        if (x < clip_.x0 || y < clip_.y0 || x >= clip_.x1 || y >= clip_.y1) return;
        blend_pixel(pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)], c,
                    coverage);
    }

    // A rounded rectangle; `radius` is clamped to half the shorter side. `feather` widens the
    // anti-aliased edge (soft shadows use several pixels).
    void fill_round_rect(Rect r, float radius, Color c, float feather = 1) {
        if (r.w <= 0 || r.h <= 0 || c.a <= 0) return;
        radius = std::clamp(radius, 0.f, std::min(r.w, r.h) / 2);
        const float half = feather / 2;
        const int y0 = std::max(clip_.y0, static_cast<int>(std::floor(r.y - half)));
        const int y1 = std::min(clip_.y1, static_cast<int>(std::ceil(r.bottom() + half)));
        const int x0 = std::max(clip_.x0, static_cast<int>(std::floor(r.x - half)));
        const int x1 = std::min(clip_.x1, static_cast<int>(std::ceil(r.right() + half)));
        const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        const float hx = r.w / 2 - radius, hy = r.h / 2 - radius;
        for (int y = y0; y < y1; ++y) {
            const float py = static_cast<float>(y) + 0.5f - cy;
            const float qy = std::abs(py) - hy;
            std::uint32_t* line = row(y);
            // Between the corner columns only the vertical distance matters.
            const float inner_cov = std::clamp(0.5f - (qy - radius) / feather, 0.f, 1.f);
            const int ix0 = std::max(x0, static_cast<int>(std::ceil(cx - hx)));
            const int ix1 = std::min(x1, static_cast<int>(std::floor(cx + hx)));
            for (int x = x0; x < x1; ++x) {
                float cov;
                if (x >= ix0 && x < ix1) {
                    cov = inner_cov;
                } else {
                    const float px = static_cast<float>(x) + 0.5f - cx;
                    const float qx = std::abs(px) - hx;
                    const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
                    const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - radius;
                    cov = std::clamp(0.5f - d / feather, 0.f, 1.f);
                }
                if (cov > 0) blend_pixel(line[x], c, cov);
            }
        }
    }

    void fill_rect(Rect r, Color c) { fill_round_rect(r, 0, c); }

    // An outline `width` wide inside the rectangle's edge.
    void stroke_round_rect(Rect r, float radius, float width, Color c) {
        if (r.w <= 0 || r.h <= 0 || c.a <= 0) return;
        radius = std::clamp(radius, 0.f, std::min(r.w, r.h) / 2);
        const float inner_radius = std::max(radius - width, 0.f);
        const Rect in = r.inset(width);
        const int y0 = std::max(clip_.y0, static_cast<int>(std::floor(r.y - 1)));
        const int y1 = std::min(clip_.y1, static_cast<int>(std::ceil(r.bottom() + 1)));
        const int x0 = std::max(clip_.x0, static_cast<int>(std::floor(r.x - 1)));
        const int x1 = std::min(clip_.x1, static_cast<int>(std::ceil(r.right() + 1)));
        for (int y = y0; y < y1; ++y) {
            std::uint32_t* line = row(y);
            for (int x = x0; x < x1; ++x) {
                const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                const float outer = std::clamp(0.5f - sd_round_rect(px, py, r, radius), 0.f, 1.f);
                if (outer <= 0) continue;
                const float inner = in.w > 0 && in.h > 0 ? std::clamp(0.5f - sd_round_rect(px, py, in, inner_radius), 0.f, 1.f) : 0.f;
                const float cov = outer - inner;
                if (cov > 0) blend_pixel(line[x], c, cov);
            }
        }
    }

    void fill_circle(float cx, float cy, float radius, Color c) {
        fill_round_rect({cx - radius, cy - radius, radius * 2, radius * 2}, radius, c);
    }

    void stroke_circle(float cx, float cy, float radius, float width, Color c) {
        stroke_round_rect({cx - radius, cy - radius, radius * 2, radius * 2}, radius, width, c);
    }

    // A line `width` wide with round caps.
    void line(float ax, float ay, float bx, float by, float width, Color c) {
        const float r = width / 2;
        const int x0 = std::max(clip_.x0, static_cast<int>(std::floor(std::min(ax, bx) - r - 1)));
        const int x1 = std::min(clip_.x1, static_cast<int>(std::ceil(std::max(ax, bx) + r + 1)));
        const int y0 = std::max(clip_.y0, static_cast<int>(std::floor(std::min(ay, by) - r - 1)));
        const int y1 = std::min(clip_.y1, static_cast<int>(std::ceil(std::max(ay, by) + r + 1)));
        const float dx = bx - ax, dy = by - ay;
        const float len2 = std::max(dx * dx + dy * dy, 1e-6f);
        for (int y = y0; y < y1; ++y) {
            std::uint32_t* l = row(y);
            for (int x = x0; x < x1; ++x) {
                const float px = static_cast<float>(x) + 0.5f - ax, py = static_cast<float>(y) + 0.5f - ay;
                const float t = std::clamp((px * dx + py * dy) / len2, 0.f, 1.f);
                const float ex = px - dx * t, ey = py - dy * t;
                const float d = std::sqrt(ex * ex + ey * ey) - r;
                const float cov = std::clamp(0.5f - d, 0.f, 1.f);
                if (cov > 0) blend_pixel(l[x], c, cov);
            }
        }
    }

    // Draws an 8-bit coverage mask (glyphs) with its top-left corner at (x, y).
    void mask(const std::uint8_t* m, int mw, int mh, int x, int y, Color c) {
        const int sx0 = std::max(0, clip_.x0 - x), sy0 = std::max(0, clip_.y0 - y);
        const int sx1 = std::min(mw, clip_.x1 - x), sy1 = std::min(mh, clip_.y1 - y);
        for (int sy = sy0; sy < sy1; ++sy) {
            std::uint32_t* l = row(y + sy);
            const std::uint8_t* src = m + static_cast<std::size_t>(sy) * static_cast<std::size_t>(mw);
            for (int sx = sx0; sx < sx1; ++sx) {
                if (src[sx]) blend_pixel(l[x + sx], c, static_cast<float>(src[sx]) / 255.f);
            }
        }
    }

    // Copies `src` (w x h pixels) scaled bilinearly into `dst`, with rounded corners.
    void image(const std::uint32_t* src, int sw, int sh, Rect dst, float radius = 0, float opacity = 1) {
        if (!src || sw <= 0 || sh <= 0 || dst.w <= 0 || dst.h <= 0) return;
        radius = std::clamp(radius, 0.f, std::min(dst.w, dst.h) / 2);
        const int x0 = std::max(clip_.x0, static_cast<int>(std::floor(dst.x)));
        const int x1 = std::min(clip_.x1, static_cast<int>(std::ceil(dst.right())));
        const int y0 = std::max(clip_.y0, static_cast<int>(std::floor(dst.y)));
        const int y1 = std::min(clip_.y1, static_cast<int>(std::ceil(dst.bottom())));
        const float sx = static_cast<float>(sw) / dst.w, sy = static_cast<float>(sh) / dst.h;
        for (int y = y0; y < y1; ++y) {
            std::uint32_t* l = row(y);
            const float fy = std::clamp((static_cast<float>(y) + 0.5f - dst.y) * sy - 0.5f, 0.f, static_cast<float>(sh - 1));
            const int iy = static_cast<int>(fy);
            const int iy2 = std::min(iy + 1, sh - 1);
            const float ty = fy - static_cast<float>(iy);
            for (int x = x0; x < x1; ++x) {
                const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                float cov = 1;
                if (radius > 0) cov = std::clamp(0.5f - sd_round_rect(px, py, dst, radius), 0.f, 1.f);
                if (cov <= 0) continue;
                const float fx = std::clamp((px - dst.x) * sx - 0.5f, 0.f, static_cast<float>(sw - 1));
                const int ix = static_cast<int>(fx);
                const int ix2 = std::min(ix + 1, sw - 1);
                const float tx = fx - static_cast<float>(ix);
                const std::uint32_t* r0 = src + static_cast<std::size_t>(iy) * static_cast<std::size_t>(sw);
                const std::uint32_t* r1 = src + static_cast<std::size_t>(iy2) * static_cast<std::size_t>(sw);
                Color c;
                const auto ch = [&](int shift) {
                    const auto v = [&](std::uint32_t p) { return static_cast<float>((p >> shift) & 0xFF); };
                    const float top = v(r0[ix]) + (v(r0[ix2]) - v(r0[ix])) * tx;
                    const float bot = v(r1[ix]) + (v(r1[ix2]) - v(r1[ix])) * tx;
                    return (top + (bot - top) * ty) / 255.f;
                };
                c.r = ch(0);
                c.g = ch(8);
                c.b = ch(16);
                c.a = opacity;
                blend_pixel(l[x], c, cov);
            }
        }
    }

    // Fills closed outlines (each a polygon) with the non-zero rule: signed area accumulated
    // per pixel, so edges are exactly anti-aliased.
    void fill_polygons(const std::vector<std::vector<Point>>& polys, Color c) {
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
        for (const auto& p : polys)
            for (const Point& q : p) {
                minx = std::min(minx, q.x), miny = std::min(miny, q.y);
                maxx = std::max(maxx, q.x), maxy = std::max(maxy, q.y);
            }
        if (minx > maxx) return;
        const int ox = static_cast<int>(std::floor(minx)), oy = static_cast<int>(std::floor(miny));
        const int w = static_cast<int>(std::ceil(maxx)) - ox + 1, h = static_cast<int>(std::ceil(maxy)) - oy + 1;
        std::vector<float> acc;
        accumulate(polys, static_cast<float>(-ox), static_cast<float>(-oy), w, h, acc);
        for (int y = 0; y < h; ++y) {
            const int ty = oy + y;
            if (ty < clip_.y0 || ty >= clip_.y1) continue;
            float sum = 0;
            std::uint32_t* l = row(ty);
            const float* a = acc.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w + 2);
            for (int x = 0; x < w; ++x) {
                sum += a[x];
                const int tx = ox + x;
                const float cov = std::min(std::abs(sum), 1.f);
                if (cov > 0.002f && tx >= clip_.x0 && tx < clip_.x1) blend_pixel(l[tx], c, cov);
            }
        }
    }

    // Signed-area accumulation of the polygons' edges, offset by (dx, dy), into a
    // (w + 2) x h buffer; a running sum along each row gives the coverage.
    static void accumulate(const std::vector<std::vector<Point>>& polys, float dx, float dy, int w, int h,
                           std::vector<float>& acc) {
        const int stride = w + 2;
        acc.assign(static_cast<std::size_t>(stride) * static_cast<std::size_t>(std::max(h, 0)), 0.f);
        for (const auto& p : polys) {
            const std::size_t n = p.size();
            for (std::size_t i = 0; i < n; ++i) {
                const Point a = p[i], b = p[(i + 1) % n];
                edge(acc, stride, w, h, a.x + dx, a.y + dy, b.x + dx, b.y + dy);
            }
        }
    }

private:
    struct Clip {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };

    static float sd_round_rect(float px, float py, Rect r, float radius) {
        const float qx = std::abs(px - (r.x + r.w / 2)) - (r.w / 2 - radius);
        const float qy = std::abs(py - (r.y + r.h / 2)) - (r.h / 2 - radius);
        const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
        return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - radius;
    }

    static void edge(std::vector<float>& acc, int stride, int w, int h, float x0, float y0, float x1, float y1) {
        if (y0 == y1) return;
        float dir = 1;
        if (y0 > y1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
            dir = -1;
        }
        const float dxdy = (x1 - x0) / (y1 - y0);
        float x = x0;
        if (y0 < 0) {
            x -= y0 * dxdy;
            y0 = 0;
        }
        y1 = std::min(y1, static_cast<float>(h));
        const float maxx = static_cast<float>(w);
        for (int y = static_cast<int>(y0); static_cast<float>(y) < y1; ++y) {
            float* line = acc.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
            const float dy = std::min(static_cast<float>(y + 1), y1) - std::max(static_cast<float>(y), y0);
            const float xnext = x + dxdy * dy;
            const float d = dy * dir;
            float xa = std::clamp(std::min(x, xnext), 0.f, maxx), xb = std::clamp(std::max(x, xnext), 0.f, maxx);
            const float xa_floor = std::floor(xa);
            const int xai = static_cast<int>(xa_floor);
            const float xb_ceil = std::ceil(xb);
            const int xbi = static_cast<int>(xb_ceil);
            if (xbi <= xai + 1) {
                const float xmf = 0.5f * (xa + xb) - xa_floor;
                line[xai] += d - d * xmf;
                line[xai + 1] += d * xmf;
            } else {
                const float s = 1.f / (xb - xa);
                const float xaf = xa - xa_floor;
                const float a0 = 0.5f * s * (1 - xaf) * (1 - xaf);
                const float xbf = xb - xb_ceil + 1;
                const float am = 0.5f * s * xbf * xbf;
                line[xai] += d * a0;
                if (xbi == xai + 2) {
                    line[xai + 1] += d * (1 - a0 - am);
                } else {
                    const float a1 = s * (1.5f - xaf);
                    line[xai + 1] += d * (a1 - a0);
                    for (int xi = xai + 2; xi < xbi - 1; ++xi) line[xi] += d * s;
                    const float a2 = a1 + static_cast<float>(xbi - xai - 3) * s;
                    line[xbi - 1] += d * (1 - a2 - am);
                }
                line[xbi] += d * am;
            }
            x = xnext;
        }
    }

    int width_ = 0, height_ = 0;
    std::vector<std::uint32_t> pixels_;
    Clip clip_;
    std::vector<Clip> clips_;
};

// Box-blurs `c` three times (close to a Gaussian of about radius / 1.4), on a picture
// shrunk `down` times and scaled back up, then darkens it toward `tint` by `amount`.
inline void frosted(Canvas& c, int radius, int down, Color tint, float amount) {
    const int w = c.width(), h = c.height();
    if (w <= 0 || h <= 0) return;
    down = std::max(down, 1);
    const int sw = std::max(1, w / down), sh = std::max(1, h / down);
    std::vector<float> img(static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh) * 3, 0.f);
    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            float r = 0, g = 0, b = 0;
            int n = 0;
            for (int yy = y * down; yy < std::min(h, (y + 1) * down); ++yy) {
                const std::uint32_t* l = c.row(yy);
                for (int xx = x * down; xx < std::min(w, (x + 1) * down); ++xx) {
                    r += static_cast<float>(l[xx] & 0xFF), g += static_cast<float>((l[xx] >> 8) & 0xFF);
                    b += static_cast<float>((l[xx] >> 16) & 0xFF);
                    ++n;
                }
            }
            float* p = &img[(static_cast<std::size_t>(y) * static_cast<std::size_t>(sw) + static_cast<std::size_t>(x)) * 3];
            p[0] = r / static_cast<float>(std::max(n, 1)), p[1] = g / static_cast<float>(std::max(n, 1)),
            p[2] = b / static_cast<float>(std::max(n, 1));
        }
    }
    const int rad = std::max(1, radius / down);
    std::vector<float> tmp(img.size());
    const auto pass = [&](bool horizontal) {
        const int len = horizontal ? sw : sh, lines = horizontal ? sh : sw;
        for (int li = 0; li < lines; ++li) {
            const auto at = [&](int i) -> std::size_t {
                const int cl = std::clamp(i, 0, len - 1);
                return horizontal ? (static_cast<std::size_t>(li) * static_cast<std::size_t>(sw) + static_cast<std::size_t>(cl)) * 3
                                  : (static_cast<std::size_t>(cl) * static_cast<std::size_t>(sw) + static_cast<std::size_t>(li)) * 3;
            };
            for (int ch = 0; ch < 3; ++ch) {
                float sum = 0;
                for (int i = -rad; i <= rad; ++i) sum += img[at(i) + static_cast<std::size_t>(ch)];
                for (int i = 0; i < len; ++i) {
                    tmp[at(i) + static_cast<std::size_t>(ch)] = sum / static_cast<float>(2 * rad + 1);
                    sum += img[at(i + rad + 1) + static_cast<std::size_t>(ch)] - img[at(i - rad) + static_cast<std::size_t>(ch)];
                }
            }
        }
        img.swap(tmp);
    };
    for (int i = 0; i < 3; ++i) {
        pass(true);
        pass(false);
    }
    for (int y = 0; y < h; ++y) {
        std::uint32_t* l = c.row(y);
        const float fy = std::clamp((static_cast<float>(y) + 0.5f) / static_cast<float>(down) - 0.5f, 0.f, static_cast<float>(sh - 1));
        const int iy = static_cast<int>(fy), iy2 = std::min(iy + 1, sh - 1);
        const float ty = fy - static_cast<float>(iy);
        for (int x = 0; x < w; ++x) {
            const float fx = std::clamp((static_cast<float>(x) + 0.5f) / static_cast<float>(down) - 0.5f, 0.f, static_cast<float>(sw - 1));
            const int ix = static_cast<int>(fx), ix2 = std::min(ix + 1, sw - 1);
            const float tx = fx - static_cast<float>(ix);
            float out[3];
            for (int ch = 0; ch < 3; ++ch) {
                const auto v = [&](int xx, int yy) {
                    return img[(static_cast<std::size_t>(yy) * static_cast<std::size_t>(sw) + static_cast<std::size_t>(xx)) * 3 +
                               static_cast<std::size_t>(ch)];
                };
                const float top = v(ix, iy) + (v(ix2, iy) - v(ix, iy)) * tx;
                const float bot = v(ix, iy2) + (v(ix2, iy2) - v(ix, iy2)) * tx;
                out[ch] = (top + (bot - top) * ty) / 255.f;
            }
            l[x] = pack(out[0] + (tint.r - out[0]) * amount, out[1] + (tint.g - out[1]) * amount,
                        out[2] + (tint.b - out[2]) * amount);
        }
    }
}

enum class Resample : std::uint8_t {
    Nearest,  // the source pixel under each output pixel's centre: pixel art stays sharp
    Linear,   // bilinear between the four nearest source pixels (as a GPU's linear filter)
    Area,     // shrinking: the mean of the source pixels each output pixel covers (no moire
              // on scanlines or grids); enlarging: nearest
};

// Resizes a sw x sh picture into dst (dw x dh), opaque. Separable: each output pixel is a
// weighted sum of source pixels along x, then along y.
inline void resample(const std::uint32_t* src, int sw, int sh, std::uint32_t* dst, int dw, int dh, Resample mode) {
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    struct Tap {
        int at;
        float weight;
    };
    // taps[i]: the source pixels (and weights summing to 1) for output index i.
    const auto taps_for = [mode](int s, int d) {
        std::vector<std::vector<Tap>> taps(static_cast<std::size_t>(d));
        const float ratio = static_cast<float>(s) / static_cast<float>(d);
        for (int i = 0; i < d; ++i) {
            auto& t = taps[static_cast<std::size_t>(i)];
            if (mode == Resample::Area && ratio > 1) {
                const float a = static_cast<float>(i) * ratio, b = a + ratio;
                for (int k = static_cast<int>(a); k < std::min(s, static_cast<int>(std::ceil(b))); ++k) {
                    const float w = std::min(b, static_cast<float>(k + 1)) - std::max(a, static_cast<float>(k));
                    if (w > 0) t.push_back({k, w / ratio});
                }
            } else if (mode == Resample::Linear) {
                const float f = std::clamp((static_cast<float>(i) + 0.5f) * ratio - 0.5f, 0.f, static_cast<float>(s - 1));
                const int k = static_cast<int>(f);
                const float frac = f - static_cast<float>(k);
                t.push_back({k, 1 - frac});
                if (frac > 0) t.push_back({std::min(k + 1, s - 1), frac});
            } else {
                t.push_back({std::min(s - 1, static_cast<int>((static_cast<float>(i) + 0.5f) * ratio)), 1.f});
            }
        }
        return taps;
    };
    const auto tx = taps_for(sw, dw), ty = taps_for(sh, dh);
    std::vector<float> rows(static_cast<std::size_t>(dw) * static_cast<std::size_t>(sh) * 3);
    for (int y = 0; y < sh; ++y) {
        const std::uint32_t* l = src + static_cast<std::size_t>(y) * static_cast<std::size_t>(sw);
        float* o = &rows[static_cast<std::size_t>(y) * static_cast<std::size_t>(dw) * 3];
        for (int x = 0; x < dw; ++x) {
            float r = 0, g = 0, b = 0;
            for (const Tap& t : tx[static_cast<std::size_t>(x)]) {
                const std::uint32_t p = l[t.at];
                r += static_cast<float>(p & 0xFF) * t.weight;
                g += static_cast<float>((p >> 8) & 0xFF) * t.weight;
                b += static_cast<float>((p >> 16) & 0xFF) * t.weight;
            }
            o[3 * x] = r, o[3 * x + 1] = g, o[3 * x + 2] = b;
        }
    }
    const auto byte = [](float v) { return static_cast<std::uint32_t>(std::clamp(v + 0.5f, 0.f, 255.f)); };
    for (int y = 0; y < dh; ++y) {
        std::uint32_t* l = dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(dw);
        for (int x = 0; x < dw; ++x) {
            float r = 0, g = 0, b = 0;
            for (const Tap& t : ty[static_cast<std::size_t>(y)]) {
                const float* p = &rows[(static_cast<std::size_t>(t.at) * static_cast<std::size_t>(dw) + static_cast<std::size_t>(x)) * 3];
                r += p[0] * t.weight, g += p[1] * t.weight, b += p[2] * t.weight;
            }
            l[x] = byte(r) | byte(g) << 8 | byte(b) << 16 | 0xFF000000u;
        }
    }
}

}  // namespace anyui
