// SPDX-License-Identifier: MIT
// anyui (shared by AnyGBA and AnyPSP; keep the copies identical): TrueType outlines
// (glyf/loca, cmap formats 4 and 12, composite glyphs) rasterised with exact area coverage
// and cached per size, and UTF-8 text measured and drawn with them.
#pragma once

#include "canvas.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace anyui {

// Next code point of UTF-8 text at `i` (advanced); malformed bytes give U+FFFD.
inline char32_t next_code_point(std::string_view s, std::size_t& i) {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = byte(i);
    int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (extra < 0 || i + static_cast<std::size_t>(extra) >= s.size() + (extra == 0 ? 1 : 0)) {
        ++i;
        return 0xFFFD;
    }
    char32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k <= extra; ++k) {
        const unsigned char cc = byte(i + static_cast<std::size_t>(k));
        if ((cc & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += static_cast<std::size_t>(extra) + 1;
    return cp;
}

class Font {
public:
    // `data` must outlive the font. Throws std::runtime_error for a file it cannot read.
    explicit Font(std::span<const std::uint8_t> data) : d_(data) {
        if (d_.size() < 12) fail("too short");
        const int tables = u16(4);
        for (int i = 0; i < tables; ++i) {
            const std::size_t rec = 12 + static_cast<std::size_t>(i) * 16;
            if (rec + 16 > d_.size()) fail("table directory");
            const std::string tag(reinterpret_cast<const char*>(d_.data() + rec), 4);
            const std::uint32_t off = u32(rec + 8), len = u32(rec + 12);
            if (static_cast<std::size_t>(off) + len > d_.size()) fail("table " + tag + " out of range");
            if (tag == "head") head_ = off;
            else if (tag == "hhea") hhea_ = off;
            else if (tag == "hmtx") hmtx_ = off;
            else if (tag == "loca") loca_ = off;
            else if (tag == "glyf") glyf_ = off, glyf_len_ = len;
            else if (tag == "cmap") cmap_ = off;
            else if (tag == "maxp") maxp_ = off;
        }
        if (!head_ || !hhea_ || !hmtx_ || !loca_ || !glyf_ || !cmap_ || !maxp_) fail("missing a required table");
        units_ = u16(head_ + 18);
        long_loca_ = s16(head_ + 50) != 0;
        glyphs_ = u16(maxp_ + 4);
        ascent_ = s16(hhea_ + 4);
        descent_ = s16(hhea_ + 6);
        gap_ = s16(hhea_ + 8);
        hmetrics_ = u16(hhea_ + 34);
        pick_cmap();
    }

    // Line metrics in pixels for a font size (em height) in pixels.
    float ascent(float px) const { return static_cast<float>(ascent_) * px / static_cast<float>(units_); }
    float descent(float px) const { return static_cast<float>(-descent_) * px / static_cast<float>(units_); }
    float line_height(float px) const { return static_cast<float>(ascent_ - descent_ + gap_) * px / static_cast<float>(units_); }

    int glyph(char32_t cp) const {
        if (cmap_format_ == 12) {
            const std::uint32_t groups = u32(cmap_sub_ + 12);
            for (std::uint32_t g = 0; g < groups; ++g) {
                const std::size_t at = cmap_sub_ + 16 + static_cast<std::size_t>(g) * 12;
                const std::uint32_t start = u32(at), end = u32(at + 4);
                if (cp >= start && cp <= end) return static_cast<int>(u32(at + 8) + (cp - start));
            }
            return 0;
        }
        if (cmap_format_ == 4 && cp <= 0xFFFF) {
            const int segs = u16(cmap_sub_ + 6) / 2;
            const std::size_t ends = cmap_sub_ + 14, starts = ends + static_cast<std::size_t>(segs) * 2 + 2;
            const std::size_t deltas = starts + static_cast<std::size_t>(segs) * 2, ranges = deltas + static_cast<std::size_t>(segs) * 2;
            for (int s = 0; s < segs; ++s) {
                const std::size_t k = static_cast<std::size_t>(s) * 2;
                if (cp > u16(ends + k)) continue;
                const unsigned start = u16(starts + k);
                if (cp < start) return 0;
                const unsigned range = u16(ranges + k);
                if (range == 0) return static_cast<int>((cp + static_cast<unsigned>(u16(deltas + k))) & 0xFFFF);
                const std::size_t at = ranges + k + range + (cp - start) * 2;
                if (at + 2 > d_.size()) return 0;
                const unsigned g = u16(at);
                return g == 0 ? 0 : static_cast<int>((g + static_cast<unsigned>(u16(deltas + k))) & 0xFFFF);
            }
        }
        return 0;
    }

    float advance(int g, float px) const {
        const int i = std::min(g, hmetrics_ - 1);
        return static_cast<float>(u16(hmtx_ + static_cast<std::size_t>(i) * 4)) * px / static_cast<float>(units_);
    }

    // A glyph rendered at `px`: coverage bitmap and its offset from the pen (baseline).
    struct Bitmap {
        int width = 0, height = 0, left = 0, top = 0;
        std::vector<std::uint8_t> alpha;
    };

    const Bitmap& bitmap(int g, float px) const {
        const int key_size = static_cast<int>(px * 4 + 0.5f);
        const auto key = std::make_pair(g, key_size);
        if (auto it = cache_.find(key); it != cache_.end()) return it->second;
        if (cache_.size() > 4096) cache_.clear();
        Bitmap& b = cache_[key];
        std::vector<std::vector<Point>> polys;
        outline(g, 1, 0, 0, 1, 0, 0, polys, 0);
        const float scale = static_cast<float>(key_size) / 4.f / static_cast<float>(units_);
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
        for (auto& p : polys)
            for (Point& q : p) {
                q.x *= scale;
                q.y *= -scale;  // y grows downward from the baseline
                minx = std::min(minx, q.x), miny = std::min(miny, q.y), maxx = std::max(maxx, q.x), maxy = std::max(maxy, q.y);
            }
        if (polys.empty() || minx > maxx) return b;
        b.left = static_cast<int>(std::floor(minx));
        b.top = static_cast<int>(std::floor(miny));
        b.width = static_cast<int>(std::ceil(maxx)) - b.left + 1;
        b.height = static_cast<int>(std::ceil(maxy)) - b.top + 1;
        std::vector<float> acc;
        Canvas::accumulate(polys, static_cast<float>(-b.left), static_cast<float>(-b.top), b.width, b.height, acc);
        b.alpha.resize(static_cast<std::size_t>(b.width) * static_cast<std::size_t>(b.height));
        for (int y = 0; y < b.height; ++y) {
            float sum = 0;
            for (int x = 0; x < b.width; ++x) {
                sum += acc[static_cast<std::size_t>(y) * static_cast<std::size_t>(b.width + 2) + static_cast<std::size_t>(x)];
                b.alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(b.width) + static_cast<std::size_t>(x)] =
                    static_cast<std::uint8_t>(std::min(std::abs(sum), 1.f) * 255.f + 0.5f);
            }
        }
        return b;
    }

    float measure(std::string_view text, float px) const {
        float w = 0;
        for (std::size_t i = 0; i < text.size();) w += advance(glyph_or_fallback(next_code_point(text, i)), px);
        return w;
    }

    // Draws `text` with its baseline at y, starting at x. Returns the pen position after it.
    float draw(Canvas& c, float x, float baseline, std::string_view text, float px, Color color) const {
        const int by = static_cast<int>(std::lround(baseline));
        for (std::size_t i = 0; i < text.size();) {
            const int g = glyph_or_fallback(next_code_point(text, i));
            const Bitmap& b = bitmap(g, px);
            if (!b.alpha.empty()) {
                c.mask(b.alpha.data(), b.width, b.height, static_cast<int>(std::lround(x)) + b.left, by + b.top, color);
            }
            x += advance(g, px);
        }
        return x;
    }

    // `text` cut to fit `max_width`, with "…" at the end when cut.
    std::string fit(std::string_view text, float px, float max_width) const {
        if (measure(text, px) <= max_width) return std::string(text);
        const std::string ellipsis = "\xE2\x80\xA6";
        const float room = max_width - measure(ellipsis, px);
        float w = 0;
        std::size_t cut = 0;
        for (std::size_t i = 0; i < text.size();) {
            const std::size_t at = i;
            const float a = advance(glyph_or_fallback(next_code_point(text, i)), px);
            if (w + a > room) break;
            w += a;
            cut = i;
            (void)at;
        }
        return std::string(text.substr(0, cut)) + ellipsis;
    }

    // The end of `text` that fits, with "…" in front (paths: the end tells them apart).
    std::string fit_end(std::string_view text, float px, float max_width) const {
        if (measure(text, px) <= max_width) return std::string(text);
        const std::string ellipsis = "\xE2\x80\xA6";
        std::vector<std::size_t> starts;
        for (std::size_t i = 0; i < text.size();) {
            starts.push_back(i);
            next_code_point(text, i);
        }
        for (std::size_t k = 0; k < starts.size(); ++k) {
            const std::string candidate = ellipsis + std::string(text.substr(starts[k]));
            if (measure(candidate, px) <= max_width) return candidate;
        }
        return ellipsis;
    }

private:
    [[noreturn]] static void fail(const std::string& why) { throw std::runtime_error("font: " + why); }

    unsigned u16(std::size_t at) const {
        if (at + 2 > d_.size()) return 0;
        return static_cast<unsigned>(d_[at] << 8 | d_[at + 1]);
    }
    int s16(std::size_t at) const { return static_cast<std::int16_t>(u16(at)); }
    std::uint32_t u32(std::size_t at) const {
        if (at + 4 > d_.size()) return 0;
        return static_cast<std::uint32_t>(d_[at]) << 24 | static_cast<std::uint32_t>(d_[at + 1]) << 16 |
               static_cast<std::uint32_t>(d_[at + 2]) << 8 | d_[at + 3];
    }

    int glyph_or_fallback(char32_t cp) const {
        const int g = glyph(cp);
        return g != 0 ? g : glyph(U'?');
    }

    void pick_cmap() {
        const int n = static_cast<int>(u16(cmap_ + 2));
        std::size_t best4 = 0, best12 = 0;
        for (int i = 0; i < n; ++i) {
            const std::size_t rec = cmap_ + 4 + static_cast<std::size_t>(i) * 8;
            const unsigned platform = u16(rec), encoding = u16(rec + 2);
            const std::size_t sub = cmap_ + u32(rec + 4);
            const unsigned format = u16(sub);
            const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
            if (!unicode) continue;
            if (format == 12) best12 = sub;
            if (format == 4) best4 = sub;
        }
        if (best12) {
            cmap_sub_ = best12;
            cmap_format_ = 12;
        } else if (best4) {
            cmap_sub_ = best4;
            cmap_format_ = 4;
        } else {
            fail("no Unicode cmap");
        }
    }

    std::pair<std::size_t, std::size_t> glyph_range(int g) const {
        if (g < 0 || g >= glyphs_) return {0, 0};
        const auto ug = static_cast<std::size_t>(g);
        std::size_t a, b;
        if (long_loca_) {
            a = u32(loca_ + ug * 4), b = u32(loca_ + ug * 4 + 4);
        } else {
            a = static_cast<std::size_t>(u16(loca_ + ug * 2)) * 2, b = static_cast<std::size_t>(u16(loca_ + ug * 2 + 2)) * 2;
        }
        if (b <= a || b > glyf_len_) return {0, 0};
        return {glyf_ + a, glyf_ + b};
    }

    // Appends glyph `g`'s contours, flattened, transformed by [a b; c d] + (e, f), in font units.
    void outline(int g, float a, float b, float c, float d, float e, float f, std::vector<std::vector<Point>>& out,
                 int depth) const {
        const auto [start, end] = glyph_range(g);
        if (start == end || depth > 4) return;
        const int contours = s16(start);
        const auto tf = [&](float x, float y) { return Point{a * x + c * y + e, b * x + d * y + f}; };
        if (contours >= 0) {
            std::size_t p = start + 10;
            std::vector<unsigned> ends(static_cast<std::size_t>(contours));
            for (auto& v : ends) v = u16(p), p += 2;
            const unsigned points = contours ? ends.back() + 1 : 0;
            p += 2 + u16(p);  // instructions
            std::vector<std::uint8_t> flags;
            flags.reserve(points);
            while (flags.size() < points && p < end) {
                const std::uint8_t fl = d_[p++];
                flags.push_back(fl);
                if ((fl & 8) && p < end) {
                    for (int r = d_[p++]; r > 0 && flags.size() < points; --r) flags.push_back(fl);
                }
            }
            if (flags.size() < points) return;
            std::vector<float> xs(points), ys(points);
            int v = 0;
            for (unsigned i = 0; i < points; ++i) {
                const std::uint8_t fl = flags[i];
                if (fl & 2) {
                    const int dv = p < end ? d_[p++] : 0;
                    v += (fl & 16) ? dv : -dv;
                } else if (!(fl & 16)) {
                    v += s16(p), p += 2;
                }
                xs[i] = static_cast<float>(v);
            }
            v = 0;
            for (unsigned i = 0; i < points; ++i) {
                const std::uint8_t fl = flags[i];
                if (fl & 4) {
                    const int dv = p < end ? d_[p++] : 0;
                    v += (fl & 32) ? dv : -dv;
                } else if (!(fl & 32)) {
                    v += s16(p), p += 2;
                }
                ys[i] = static_cast<float>(v);
            }
            unsigned first = 0;
            for (unsigned ce : ends) {
                if (ce < first || ce >= points) break;
                const unsigned n = ce - first + 1;
                std::vector<Point> poly;
                const auto on = [&](unsigned k) { return (flags[first + k % n] & 1) != 0; };
                const auto pt = [&](unsigned k) { return Point{xs[first + k % n], ys[first + k % n]}; };
                // Start on an on-curve point (or the midpoint of two off-curve ones).
                unsigned s = 0;
                while (s < n && !on(s)) ++s;
                Point cur;
                if (s == n) {
                    cur = {(pt(0).x + pt(1).x) / 2, (pt(0).y + pt(1).y) / 2};
                    s = 0;
                } else {
                    cur = pt(s);
                }
                poly.push_back(tf(cur.x, cur.y));
                for (unsigned k = 1; k <= n; ++k) {
                    const unsigned i = s + k;
                    if (on(i)) {
                        cur = pt(i);
                        poly.push_back(tf(cur.x, cur.y));
                        continue;
                    }
                    const Point ctrl = pt(i);
                    Point to = on(i + 1) ? pt(i + 1) : Point{(ctrl.x + pt(i + 1).x) / 2, (ctrl.y + pt(i + 1).y) / 2};
                    for (int t = 1; t <= 6; ++t) {
                        const float u = static_cast<float>(t) / 6, w = 1 - u;
                        const float x = w * w * cur.x + 2 * w * u * ctrl.x + u * u * to.x;
                        const float y = w * w * cur.y + 2 * w * u * ctrl.y + u * u * to.y;
                        poly.push_back(tf(x, y));
                    }
                    cur = to;
                    if (on(i + 1)) ++k;
                }
                out.push_back(std::move(poly));
                first = ce + 1;
            }
            return;
        }
        std::size_t p = start + 10;
        for (;;) {
            if (p + 4 > end) return;
            const unsigned fl = u16(p), child = u16(p + 2);
            p += 4;
            float dx = 0, dy = 0;
            if (fl & 1) {
                if (fl & 2) dx = static_cast<float>(s16(p)), dy = static_cast<float>(s16(p + 2));
                p += 4;
            } else {
                if (fl & 2) dx = static_cast<float>(static_cast<std::int8_t>(d_[p])), dy = static_cast<float>(static_cast<std::int8_t>(d_[p + 1]));
                p += 2;
            }
            float ma = 1, mb = 0, mc = 0, md = 1;
            const auto f2d14 = [&](std::size_t at) { return static_cast<float>(s16(at)) / 16384.f; };
            if (fl & 8) {
                ma = md = f2d14(p);
                p += 2;
            } else if (fl & 0x40) {
                ma = f2d14(p), md = f2d14(p + 2);
                p += 4;
            } else if (fl & 0x80) {
                ma = f2d14(p), mb = f2d14(p + 2), mc = f2d14(p + 4), md = f2d14(p + 6);
                p += 8;
            }
            // child -> component transform -> our transform
            outline(static_cast<int>(child), a * ma + c * mb, b * ma + d * mb, a * mc + c * md, b * mc + d * md,
                    a * dx + c * dy + e, b * dx + d * dy + f, out, depth + 1);
            if (!(fl & 0x20)) return;
        }
    }

    std::span<const std::uint8_t> d_;
    std::size_t head_ = 0, hhea_ = 0, hmtx_ = 0, loca_ = 0, glyf_ = 0, glyf_len_ = 0, cmap_ = 0, maxp_ = 0;
    std::size_t cmap_sub_ = 0;
    unsigned cmap_format_ = 0;
    int units_ = 1000, glyphs_ = 0, hmetrics_ = 1;
    int ascent_ = 800, descent_ = -200, gap_ = 0;
    bool long_loca_ = false;
    mutable std::map<std::pair<int, int>, Bitmap> cache_;
};

}  // namespace anyui
