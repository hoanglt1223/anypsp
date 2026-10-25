#include "ge_surfaces.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace pspweb {
namespace {

constexpr std::uint32_t kVramSize = 0x200000u;
constexpr std::uint32_t kNotVram = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t kMaxSurfaceHeight = 512u;

// Guest address -> VRAM offset, or kNotVram.
std::uint32_t vram_offset(std::uint32_t address) {
    const std::uint32_t c = address & 0x1FFFFFFFu;
    if (c < 0x04000000u || c >= 0x04800000u) return kNotVram;
    return (c - 0x04000000u) & (kVramSize - 1u);
}

bool is_dst_alpha_factor(std::uint32_t f) { return f == 4u || f == 5u || f == 8u || f == 9u; }
bool is_src_alpha_factor(std::uint32_t f) { return f == 2u || f == 3u || f == 6u || f == 7u; }

std::uint32_t to_rgba(std::uint32_t v, std::uint32_t format) {
    auto e5 = [](std::uint32_t x) { return (x << 3u) | (x >> 2u); };
    auto e6 = [](std::uint32_t x) { return (x << 2u) | (x >> 4u); };
    switch (format) {
    case 0: return e5(v & 31u) | (e6((v >> 5u) & 63u) << 8u) | (e5((v >> 11u) & 31u) << 16u) | 0xFF000000u;
    case 1: return e5(v & 31u) | (e5((v >> 5u) & 31u) << 8u) | (e5((v >> 10u) & 31u) << 16u) | ((v >> 15u) ? 0xFF000000u : 0u);
    default: return ((v & 15u) * 17u) | ((((v >> 4u) & 15u) * 17u) << 8u) | ((((v >> 8u) & 15u) * 17u) << 16u) |
                    ((((v >> 12u) & 15u) * 17u) << 24u);
    }
}

std::uint32_t from_rgba(std::uint32_t rgba, std::uint32_t format) {
    const std::uint32_t r = rgba & 0xFFu, g = (rgba >> 8u) & 0xFFu, b = (rgba >> 16u) & 0xFFu, a = rgba >> 24u;
    switch (format) {
    case 0: return (r >> 3u) | ((g >> 2u) << 5u) | ((b >> 3u) << 11u);
    case 1: return (r >> 3u) | ((g >> 3u) << 5u) | ((b >> 3u) << 10u) | ((a >> 7u) << 15u);
    default: return (r >> 4u) | ((g >> 4u) << 4u) | ((b >> 4u) << 8u) | ((a >> 4u) << 12u);
    }
}

double clock_ms() {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

// PSPWEB_PASS_FRAME=n logs the extra GPU passes of frame n.
const std::uint32_t g_pass_frame = [] {
    const char *v = std::getenv("PSPWEB_PASS_FRAME");
    return v != nullptr ? static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
}();

} // namespace

GeSurfaces::GeSurfaces(psprecomp::GuestMemory &memory, std::unique_ptr<GeGpu> gpu) : memory_(memory), gpu_(std::move(gpu)) {
    batch_.reserve(65536u);
}

GeSurfaces::~GeSurfaces() {
    for (auto &s : surfaces_)
        if (s.alive) gpu_->destroy_target(s.gpu);
}

int GeSurfaces::set_render_scale(int scale) {
    scale = std::clamp(scale, 1, gpu_->max_scale());
    if (scale == scale_) return scale_;
    flush();
    for (auto &s : surfaces_) {
        if (!s.alive) continue;
        download(s);
        release(s);
    }
    surfaces_.clear();
    current_ = nullptr;
    scale_ = scale;
    gpu_->set_scale(scale);
    if (coherence_ == Coherence::Lazy) refresh_page_flags();
    return scale_;
}

void GeSurfaces::allocate(Surface &s, std::uint32_t height) {
    s.height = std::clamp(height, 1u, kMaxSurfaceHeight);
    s.rows.resize(s.height);
    if (coherence_ == Coherence::Lazy) mark_vram_pages(s.offset, surface_end(s), psprecomp::GuestMemory::kVramWatched);
    s.gpu = gpu_->create_target(static_cast<int>(s.stride), static_cast<int>(s.height));
    s.stencil_bits = 0u;
    s.alpha_bits = 0u;
}

void GeSurfaces::release(Surface &s) {
    if (s.gpu != 0u) gpu_->destroy_target(s.gpu);
    s.gpu = 0u;
}

// Reallocates `s` taller, keeping its pixels with a GPU copy.
void GeSurfaces::grow(Surface &s, std::uint32_t height) {
    height = std::min(height, kMaxSurfaceHeight);
    if (height <= s.height) return;
    Surface old = s;
    allocate(s, height);
    ++passes_.blits;
    gpu_->copy_target(old.gpu, 0, 0, s.gpu, 0, 0, static_cast<int>(s.stride), static_cast<int>(old.height));
    release(old);
    upload_rect(s, Rect{0, static_cast<int>(old.height), static_cast<int>(s.stride) - 1, static_cast<int>(s.height) - 1});
}

// Moves `from` (same layout, starting inside `into`) into `into` with a GPU copy.
void GeSurfaces::absorb(Surface &into, Surface &from) {
    int ox = 0, oy = 0;
    if (!locate(into, from.offset, ox, oy)) return;
    apply_pending_uploads(into);
    apply_pending_uploads(from);
    for (Surface *each : {&into, &from}) {
        resolve_alpha(*each, whole(*each));
        resolve_stencil(*each);
    }
    if (static_cast<std::uint32_t>(oy) + from.height > into.height) grow(into, static_cast<std::uint32_t>(oy) + from.height);
    const int w = std::min(static_cast<int>(from.stride), static_cast<int>(into.stride) - ox);
    const int h = std::min(static_cast<int>(from.height), static_cast<int>(into.height) - oy);
    if (w > 0 && h > 0) {
        ++passes_.blits;
        gpu_->copy_target(from.gpu, 0, 0, into.gpu, ox, oy, w, h);
    }
    // Carry the drawn and dirty extents over row by row.
    if (into.rows.size() < into.height) into.rows.resize(into.height);
    for (std::size_t y = 0; y < from.rows.size(); ++y) {
        const auto &src = from.rows[y];
        const std::size_t ty = y + static_cast<std::size_t>(oy);
        if (ty >= into.rows.size()) break;
        auto &dst = into.rows[ty];
        auto merge = [ox](int &d0, int &d1, int s0, int s1) {
            if (s1 < s0) return;
            if (d1 < d0) { d0 = s0 + ox; d1 = s1 + ox; }
            else { d0 = std::min(d0, s0 + ox); d1 = std::max(d1, s1 + ox); }
        };
        merge(dst.cx0, dst.cx1, src.cx0, src.cx1);
        merge(dst.dx0, dst.dx1, src.dx0, src.dx1);
    }
    recompute_bounds(into);
    into.stencil_bits |= from.stencil_bits;
    into.alpha_bits |= from.alpha_bits;
    from.alive = false;
    page_flags_stale_ = true;
    release(from);
    if (current_ == &from) current_ = nullptr;
}

void GeSurfaces::download(Surface &s) {
    apply_pending_uploads(s); // VRAM is newer there; keep it
    resolve_alpha(s, s.gpu_dirty);
    std::uint8_t *vram = memory_.raw_pointer(0x04000000u, kVramSize);
    const Rect r = s.gpu_dirty.clip(Rect{0, 0, static_cast<int>(s.stride) - 1, static_cast<int>(s.height) - 1});
    s.gpu_dirty = Rect{};
    page_flags_stale_ = true;
    std::vector<std::pair<int, int>> spans(s.rows.size(), {0, -1});
    for (std::size_t y = 0; y < s.rows.size(); ++y) {
        spans[y] = {s.rows[y].dx0, s.rows[y].dx1};
        s.rows[y].dx0 = 0;
        s.rows[y].dx1 = -1;
    }
    if (vram == nullptr || r.empty() || in_between_) return;
    ++downloads_;
    if (scale_ != 1) ++passes_.blits;
    const auto x0 = static_cast<std::uint32_t>(r.x0), y0 = static_cast<std::uint32_t>(r.y0);
    const auto w = static_cast<std::uint32_t>(r.width()), h = static_cast<std::uint32_t>(r.height());
    pixels_.resize(static_cast<std::size_t>(w) * h * 4u);
    gpu_->read_pixels(s.gpu, r, pixels_.data());
    const std::uint32_t bpp = bpp_of(s.format);
    for (std::uint32_t y = 0; y < h; ++y) {
        const auto &span = spans[y0 + y];
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto px = static_cast<int>(x0 + x);
            if (px < span.first || px > span.second) continue; // only pixels the GPU drew
            const std::uint32_t at = s.offset + ((y0 + y) * s.stride + x0 + x) * bpp;
            if (at + bpp > kVramSize) break;
            std::uint32_t rgba;
            std::memcpy(&rgba, &pixels_[(static_cast<std::size_t>(y) * w + x) * 4u], 4u);
            if (bpp == 4u) {
                std::memcpy(vram + at, &rgba, 4u);
            } else {
                const std::uint32_t packed = from_rgba(rgba, s.format);
                vram[at] = static_cast<std::uint8_t>(packed);
                vram[at + 1u] = static_cast<std::uint8_t>(packed >> 8u);
            }
        }
    }
    if (vram_write_hook_) vram_write_hook_(0x04000000u + s.offset + y0 * s.stride * bpp, h * s.stride * bpp);
}

void GeSurfaces::upload_rect(Surface &s, const Rect &rect) {
    const std::uint8_t *vram = memory_.raw_pointer(0x04000000u, kVramSize);
    const Rect r = rect.clip(Rect{0, 0, static_cast<int>(s.stride) - 1, static_cast<int>(s.height) - 1});
    if (vram == nullptr || r.empty()) return;
    alpha_will_change(s, r);
    ++uploads_;
    upload_pixels_ += static_cast<std::uint64_t>(r.width()) * static_cast<std::uint64_t>(r.height());
    if (scale_ != 1) ++passes_.blits;
    const auto x0 = static_cast<std::uint32_t>(r.x0), y0 = static_cast<std::uint32_t>(r.y0);
    const auto w = static_cast<std::uint32_t>(r.width()), h = static_cast<std::uint32_t>(r.height());
    const std::uint32_t bpp = bpp_of(s.format);
    pixels_.assign(static_cast<std::size_t>(w) * h * 4u, 0u);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint32_t row_at = s.offset + ((y0 + y) * s.stride + x0) * bpp;
        if (bpp == 4u && row_at + w * 4u <= kVramSize) {
            std::memcpy(&pixels_[static_cast<std::size_t>(y) * w * 4u], vram + row_at, w * 4u);
            continue;
        }
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t at = s.offset + ((y0 + y) * s.stride + x0 + x) * bpp;
            if (at + bpp > kVramSize) break;
            std::uint32_t rgba;
            if (bpp == 4u) std::memcpy(&rgba, vram + at, 4u);
            else rgba = to_rgba(static_cast<std::uint32_t>(vram[at]) | (static_cast<std::uint32_t>(vram[at + 1u]) << 8u), s.format);
            std::memcpy(&pixels_[(static_cast<std::size_t>(y) * w + x) * 4u], &rgba, 4u);
        }
    }
    if (s.format != 0u) {
        std::uint32_t bits = 0u;
        for (std::size_t i = 3; i < pixels_.size(); i += 4u) bits |= pixels_[i];
        s.alpha_bits |= bits;
    }
    gpu_->write_pixels(s.gpu, r, pixels_.data());
}

bool GeSurfaces::reinterpret(Surface &into, const Surface &from, const Rect &hit) {
    const bool into_32 = into.format == 3u, from_32 = from.format == 3u;
    if (into_32 == from_32 || row_bytes(into) != row_bytes(from) || hit.empty()) return false;
    const auto delta = static_cast<int>(static_cast<std::int64_t>(into.offset) - static_cast<std::int64_t>(from.offset));
    if (delta % 2 != 0) return false;
    apply_pending_uploads(into);
    apply_pending_uploads(const_cast<Surface &>(from));
    resolve_alpha(const_cast<Surface &>(from), whole(from));
    alpha_will_change(into, hit);
    into.alpha_bits = 0xFFu;
    ++passes_.reinterprets;
    if (frame_ == g_pass_frame)
        std::cerr << "[pass] reinterpret into=0x" << std::hex << into.offset << "/f" << into.format << " from=0x" << from.offset << "/f" << from.format
                  << std::dec << " " << hit.x0 << "," << hit.y0 << ".." << hit.x1 << "," << hit.y1 << "\n";
    passes_.pixels += static_cast<std::uint64_t>(hit.width() * hit.height() * scale_ * scale_);
    GpuReinterpret params;
    params.mode = into_32 ? 1 : 0;
    params.format = static_cast<int>(into_32 ? from.format : into.format);
    params.delta = delta;
    params.row_bytes = static_cast<int>(row_bytes(into));
    params.src_width = static_cast<int>(from.stride);
    params.src_height = static_cast<int>(from.height);
    gpu_->reinterpret(into.gpu, from.gpu, hit, params);
    return true;
}

void GeSurfaces::resolve_alpha(Surface &s, const Rect &region, std::uint32_t bits) {
    if (s.format == 0u) {
        s.alpha_stale = Rect{};
        return;
    }
    const Rect r = s.alpha_stale.clip(region);
    if (r.empty()) return;
    const bool complete = (bits & 0xFFu) == 0xFFu;
    if (complete && r == s.alpha_stale) s.alpha_stale = Rect{};
    if (s.stale_value >= 0) {
        // One known value throughout: a single fill sets it.
        ++passes_.alpha_resolves;
        passes_.pixels += static_cast<std::uint64_t>(r.width() * r.height() * scale_ * scale_);
        s.alpha_bits |= static_cast<std::uint32_t>(s.stale_value);
        gpu_->fill_alpha(s.gpu, r, static_cast<std::uint8_t>(s.stale_value));
        if (r == s.alpha_stale) {
            s.alpha_stale = Rect{};
            s.stale_value = -1;
        }
        return;
    }
    s.alpha_bits |= s.stencil_bits;
    const std::uint32_t wanted = s.stencil_bits & bits;
    ++passes_.alpha_resolves;
    if (frame_ == g_pass_frame)
        std::cerr << "[pass] alpha bits=0x" << std::hex << wanted << " vram=0x" << s.offset << std::dec << " " << r.x0 << ","
                  << r.y0 << ".." << r.x1 << "," << r.y1 << "\n";
    passes_.pixels += static_cast<std::uint64_t>(1 + std::popcount(wanted)) *
                      static_cast<std::uint64_t>(r.width() * r.height() * scale_ * scale_);
    gpu_->stencil_to_alpha(s.gpu, r, wanted);
}

void GeSurfaces::resolve_stencil(Surface &s) {
    const Rect r = s.stencil_stale.clip(whole(s));
    s.stencil_stale = Rect{};
    if (r.empty() || s.format == 0u) return;
    s.stencil_bits |= s.alpha_bits;
    ++passes_.stencil_resolves;
    if (frame_ == g_pass_frame)
        std::cerr << "[pass] stencil bits=0x" << std::hex << s.alpha_bits << " vram=0x" << s.offset << std::dec << " " << r.x0 << ","
                  << r.y0 << ".." << r.x1 << "," << r.y1 << "\n";
    passes_.pixels += static_cast<std::uint64_t>(1 + std::popcount(s.alpha_bits)) *
                      static_cast<std::uint64_t>(r.width() * r.height() * scale_ * scale_);
    gpu_->alpha_to_stencil(s.gpu, r, s.alpha_bits);
}

// `source` is the surface a render-to-texture draw samples, already brought up
// to date by apply_state.
void GeSurfaces::texture_for_state(GpuDraw &d, Surface *source, int ox, int oy) {
    const GlDrawState &s = state_;
    GpuShaderParams &p = d.params;
    if (s.tex_from_target && source != nullptr) {
        p.set(2, static_cast<float>(source->stride), static_cast<float>(source->height), static_cast<float>(ox),
              static_cast<float>(oy));
        p.set(3, static_cast<float>(s.tex_width), static_cast<float>(s.tex_height), s.clamp_u ? 1.0f : 0.0f,
              s.clamp_v ? 1.0f : 0.0f);
        p.v[23] = static_cast<float>(scale_);
        if (s.tex_format == 0u) p.v[7] = static_cast<float>(static_cast<std::uint32_t>(p.v[7]) | GpuShaderParams::kTexOpaque);
        // At native size the shader samples texels like the GE; upscaled targets are filtered by the GPU.
        if (scale_ == 1) p.v[7] = static_cast<float>(static_cast<std::uint32_t>(p.v[7]) | GpuShaderParams::kExactSample);
        d.texture_target = source->gpu;
        if (source != current_) {
            d.source = GpuDraw::Source::Target;
            return;
        }
        ++passes_.feedback_copies;
        // Sampling the surface being drawn: read from a snapshot of the sampled part.
        const Rect window = Rect{ox, oy, ox + static_cast<int>(s.tex_width) - 1, oy + static_cast<int>(s.tex_height) - 1}
                                .clip(whole(*source));
        if (!window.empty()) {
            passes_.pixels += static_cast<std::uint64_t>(window.width() * window.height() * scale_ * scale_);
            if (frame_ == g_pass_frame)
                std::cerr << "[pass] feedback vram=0x" << std::hex << source->offset << std::dec << " " << window.x0 << "," << window.y0
                          << ".." << window.x1 << "," << window.y1 << "\n";
        }
        d.source = GpuDraw::Source::Snapshot;
        d.snapshot = window;
        return;
    }
    if (s.texels == nullptr) return;
    d.source = GpuDraw::Source::Texels;
    d.texels = s.texels;
    d.texture_identity = s.texture_identity;
    d.texture_hash = s.texture_hash;
    d.tex_width = s.tex_width;
    d.tex_height = s.tex_height;
    p.set(2, static_cast<float>(s.tex_width), static_cast<float>(s.tex_height), 0.0f, 0.0f);
    p.set(3, 0.0f, 0.0f, s.clamp_u ? 1.0f : 0.0f, s.clamp_v ? 1.0f : 0.0f);
    p.v[23] = 1.0f;
    p.v[7] = static_cast<float>(static_cast<std::uint32_t>(p.v[7]) | GpuShaderParams::kExactSample);
}

void GeSurfaces::forget_texture(const void *identity) { gpu_->forget_texture(identity); }

namespace {

GpuBlendFactor blend_factor(std::uint32_t f, bool source, std::uint32_t fixed, bool &uses_constant) {
    switch (f) {
    case 0: return source ? GpuBlendFactor::DstColor : GpuBlendFactor::SrcColor;
    case 1: return source ? GpuBlendFactor::OneMinusDstColor : GpuBlendFactor::OneMinusSrcColor;
    case 2: case 6: return GpuBlendFactor::SrcAlpha;
    case 3: case 7: return GpuBlendFactor::OneMinusSrcAlpha;
    case 4: case 8: return GpuBlendFactor::DstAlpha;
    case 5: case 9: return GpuBlendFactor::OneMinusDstAlpha;
    default:
        if ((fixed & 0xFFFFFFu) == 0u) return GpuBlendFactor::Zero;
        if ((fixed & 0xFFFFFFu) == 0xFFFFFFu) return GpuBlendFactor::One;
        if (source) return GpuBlendFactor::One; // the shader pre-multiplies by FIXA
        uses_constant = true;
        return GpuBlendFactor::ConstantColor;
    }
}

GpuStencilOp stencil_op(std::uint32_t op) { return op < 6u ? static_cast<GpuStencilOp>(op) : GpuStencilOp::Keep; }

float channel(std::uint32_t color, unsigned shift) { return static_cast<float>((color >> shift) & 0xFFu) / 255.0f; }

} // namespace

void GeSurfaces::apply_state() {
    const GlDrawState &s = state_;
    int ox = 0, oy = 0;
    Surface &surface = surface_for_draw(s.fb_offset, s.fb_stride, s.fb_format,
                                        static_cast<std::uint32_t>(std::max(s.sy2, 0) + 1), ox, oy);
    apply_pending_uploads(surface);
    sync_overlaps(surface);
    // Everything that may run its own GPU passes (syncing a sampled surface,
    // moving stencil to alpha or back) happens before this draw is described.
    Surface *source = nullptr;
    int source_ox = 0, source_oy = 0;
    if (!s.clear && s.texture && s.tex_from_target) {
        source = find_surface(vram_offset(s.tex_address), s.tex_buf_width, s.tex_format, source_ox, source_oy);
        if (source != nullptr) {
            apply_pending_uploads(*source);
            if (source != &surface) sync_overlaps(*source);
            // The texture's alpha only matters if something downstream looks at it.
            const bool src_alpha_blend = s.blend && (is_src_alpha_factor(s.src_factor) || is_src_alpha_factor(s.dst_factor));
            if (s.tex_alpha && s.tex_format != 0u && (s.alpha_test || src_alpha_blend || s.tex_func == 1u)) {
                // Read only by a masked alpha test (and vertex alpha is 1, so texel alpha
                // reaches the test unchanged): only the tested bits need to be right.
                std::uint32_t bits = 0xFFu;
                if (!src_alpha_blend && s.tex_func != 1u &&
                    std::all_of(batch_.begin(), batch_.end(), [](const GlVertex &v) { return v.a == 255u; }))
                    bits = s.alpha_mask & 0xFFu;
                const Rect window{source_ox, source_oy, source_ox + static_cast<int>(s.tex_width) - 1,
                                  source_oy + static_cast<int>(s.tex_height) - 1};
                resolve_alpha(*source, window, bits);
            }
        }
    }
    const bool stencil_on = s.clear ? (s.clear_flags & 0x200u) != 0u : s.stencil;
    const Rect scissored = Rect{std::max(s.sx1, 0) + ox, std::max(s.sy1, 0) + oy, s.sx2 + ox, s.sy2 + oy}.clip(whole(surface));
    if (!s.clear && s.blend && (is_dst_alpha_factor(s.src_factor) || is_dst_alpha_factor(s.dst_factor)))
        resolve_alpha(surface, scissored);
    batch_stencil_value_ = -1;
    if (stencil_on) {
        // A rectangle that sets every pixel it covers regardless of the old
        // stencil (test always passes, ops zero/replace, all bits written)
        // makes resolving the old stencil underneath it pointless.
        bool rectangle = false;
        const Rect covered = batch_bounds(rectangle).shifted(ox, oy).clip(whole(surface));
        auto overwrites = [](std::uint32_t op) { return op == 1u || op == 2u; };
        const bool independent = !s.clear && (s.stencil_func & 7u) == 1u && overwrites(s.stencil_ops & 0xFFu) &&
                                  overwrites((s.stencil_ops >> 8u) & 0xFFu) && overwrites((s.stencil_ops >> 16u) & 0xFFu) &&
                                  (s.mask_alpha & 0xFFu) == 0u;
        if (independent && rectangle) {
            const std::uint32_t ref = (s.stencil_func >> 8u) & 0xFFu;
            auto value = [ref](std::uint32_t op) { return op == 1u ? 0 : static_cast<int>(ref); };
            const int pass_value = value((s.stencil_ops >> 16u) & 0xFFu);
            if (value(s.stencil_ops & 0xFFu) == pass_value && value((s.stencil_ops >> 8u) & 0xFFu) == pass_value)
                batch_stencil_value_ = pass_value;
        }
        Rect &stale = surface.stencil_stale;
        if (independent && rectangle && !stale.empty() && covered.x0 <= stale.x0 && covered.x1 >= stale.x1) {
            // Keep only the rows the rectangle leaves uncovered (above or below it).
            if (covered.y0 <= stale.y0 && covered.y1 >= stale.y1) stale = Rect{};
            else if (covered.y0 <= stale.y0 && covered.y1 >= stale.y0) stale.y0 = covered.y1 + 1;
            else if (covered.y1 >= stale.y1 && covered.y0 <= stale.y1) stale.y1 = covered.y0 - 1;
        }
        resolve_stencil(surface);
    }
    batch_writes_stencil_ = false;
    current_ = &surface;
    current_ox_ = ox;
    current_oy_ = oy;

    GpuDraw &d = draw_;
    d = GpuDraw{};
    d.target = surface.gpu;
    // An empty scissor (x1 < x0) draws nothing.
    const int x1 = std::max(s.sx1, 0) + ox, y1 = std::max(s.sy1, 0) + oy;
    d.scissor = Rect{x1, y1, x1 + std::max(s.sx2 + ox - x1 + 1, 0) - 1, y1 + std::max(s.sy2 + oy - y1 + 1, 0) - 1};
    GpuShaderParams &p = d.params;
    p.set(0, static_cast<float>(surface.stride), static_cast<float>(surface.height), static_cast<float>(ox), static_cast<float>(oy));
    p.set(6, 1.0f, 1.0f, 1.0f, static_cast<float>(scale_));
    std::uint32_t flags = 0u;
    if (s.dither) {
        // Clears are dithered too.
        flags |= GpuShaderParams::kDither;
        p.set(7, static_cast<float>(s.dither_rows[0]), static_cast<float>(s.dither_rows[1]), static_cast<float>(s.dither_rows[2]),
              static_cast<float>(s.dither_rows[3]));
    }
    GpuPipeline &pipe = d.pipeline;

    // PSP stencil -> GPU stencil.  Clears with the alpha flag set it to the clear alpha.
    pipe.stencil_test = stencil_on;
    if (stencil_on) {
        if (!s.clear) {
            pipe.stencil_func = static_cast<GpuCompare>(s.stencil_func & 7u);
            d.stencil_ref = static_cast<std::uint8_t>((s.stencil_func >> 8u) & 0xFFu);
            d.stencil_compare_mask = static_cast<std::uint8_t>((s.stencil_func >> 16u) & 0xFFu);
            pipe.stencil_fail = stencil_op(s.stencil_ops & 0xFFu);
            pipe.stencil_zfail = stencil_op((s.stencil_ops >> 8u) & 0xFFu);
            pipe.stencil_zpass = stencil_op((s.stencil_ops >> 16u) & 0xFFu);
            const std::uint32_t write = ~s.mask_alpha & 0xFFu;
            d.stencil_write_mask = static_cast<std::uint8_t>(write);
            auto keeps = [](GpuStencilOp op) { return op == GpuStencilOp::Keep; };
            batch_writes_stencil_ = write != 0u && !(keeps(pipe.stencil_fail) && keeps(pipe.stencil_zfail) && keeps(pipe.stencil_zpass));
            if (batch_writes_stencil_) {
                auto arbitrary = [](GpuStencilOp op) {
                    return op == GpuStencilOp::Invert || op == GpuStencilOp::IncrementClamp || op == GpuStencilOp::DecrementClamp;
                };
                const bool any = arbitrary(pipe.stencil_fail) || arbitrary(pipe.stencil_zfail) || arbitrary(pipe.stencil_zpass);
                surface.stencil_bits |= (any ? 0xFFu : static_cast<std::uint32_t>(d.stencil_ref)) & write;
            }
        } else {
            pipe.stencil_func = GpuCompare::Always;
            pipe.stencil_fail = pipe.stencil_zfail = pipe.stencil_zpass = GpuStencilOp::Replace;
            d.stencil_ref = static_cast<std::uint8_t>(clear_alpha_ & 0xFF);
            surface.stencil_bits |= static_cast<std::uint32_t>(clear_alpha_) & 0xFFu;
            surface.alpha_bits |= static_cast<std::uint32_t>(clear_alpha_) & 0xFFu;
        }
    }

    if (s.clear) {
        const bool color = (s.clear_flags & 0x100u) != 0u, alpha = (s.clear_flags & 0x200u) != 0u;
        pipe.color_mask = static_cast<std::uint8_t>((color ? 7u : 0u) | (alpha ? 8u : 0u));
        pipe.depth_test = (s.clear_flags & 0x400u) != 0u;
        pipe.depth_write = pipe.depth_test;
        p.set(1, 0.0f, 0.0f, 0.0f, static_cast<float>(flags | GpuShaderParams::kClear));
        return;
    }

    // Alpha is the stencil: written through the stencil buffer, never directly.
    pipe.color_mask = static_cast<std::uint8_t>(((s.mask_rgb & 0x0000FFu) != 0x0000FFu ? 1u : 0u) |
                                                ((s.mask_rgb & 0x00FF00u) != 0x00FF00u ? 2u : 0u) |
                                                ((s.mask_rgb & 0xFF0000u) != 0xFF0000u ? 4u : 0u));
    pipe.depth_test = s.z_test;
    if (s.z_test) {
        pipe.depth_func = static_cast<GpuCompare>(s.z_func & 7u);
        pipe.depth_write = s.z_write;
    }
    pipe.blend = s.blend;
    if (s.blend) {
        bool uses_constant = false;
        pipe.blend_src = blend_factor(s.src_factor, true, s.fix_a, uses_constant);
        pipe.blend_dst = blend_factor(s.dst_factor, false, s.fix_b, uses_constant);
        if (s.src_factor >= 10u && pipe.blend_src == GpuBlendFactor::One && (s.fix_a & 0xFFFFFFu) != 0xFFFFFFu)
            p.set(6, channel(s.fix_a, 0u), channel(s.fix_a, 8u), channel(s.fix_a, 16u), static_cast<float>(scale_));
        if (uses_constant) d.blend_constant = {channel(s.fix_b, 0u), channel(s.fix_b, 8u), channel(s.fix_b, 16u), 1.0f};
        static constexpr GpuBlendOp equations[6] = {GpuBlendOp::Add, GpuBlendOp::Subtract, GpuBlendOp::ReverseSubtract,
                                                    GpuBlendOp::Min, GpuBlendOp::Max, GpuBlendOp::Add};
        pipe.blend_op = equations[std::min(s.equation, 5u)];
    }
    auto bytes = [](std::uint32_t color, unsigned shift) { return static_cast<float>((color >> shift) & 0xFFu); };
    if (s.fog) {
        flags |= GpuShaderParams::kFog;
        p.set(5, bytes(s.fog_color, 0u), bytes(s.fog_color, 8u), bytes(s.fog_color, 16u), 1.0f);
    }
    if (s.alpha_test) {
        flags |= GpuShaderParams::kAlphaTest;
        p.v[19] = static_cast<float>((s.alpha_ref & 0xFFu) | ((s.alpha_mask & 0xFFu) << 8u) | ((s.alpha_func & 7u) << 16u));
    }
    if (!s.blend && s.fb_format != 3u) flags |= GpuShaderParams::kQuantize;
    p.set(1, static_cast<float>(s.fb_format & 3u), 0.0f, static_cast<float>(s.tex_func), static_cast<float>(flags));
    if (!s.texture) return;
    texture_for_state(d, source, source_ox, source_oy);
    if (d.source == GpuDraw::Source::None) return;
    flags = static_cast<std::uint32_t>(p.v[7]) | GpuShaderParams::kTexEnable | (s.tex_alpha ? GpuShaderParams::kTexAlpha : 0u) |
            (s.tex_double ? GpuShaderParams::kTexDouble : 0u) | (s.linear ? GpuShaderParams::kLinear : 0u);
    p.v[7] = static_cast<float>(flags);
    d.linear = s.linear;
    d.clamp_u = s.clamp_u || s.tex_from_target;
    d.clamp_v = s.clamp_v || s.tex_from_target;
    p.v[16] = bytes(s.env, 0u);
    p.v[17] = bytes(s.env, 8u);
    p.v[18] = bytes(s.env, 16u);
}

void GeSurfaces::flush() {
    if (batch_.empty() || indices_.empty() || !state_valid_) {
        batch_.clear();
        indices_.clear();
        return;
    }
    static const bool timed = std::getenv("PSPWEB_FRAME_PROFILE") != nullptr;
    const double started = timed ? clock_ms() : 0.0;
    struct FlushTimer {
        double &total;
        double start;
        bool on;
        ~FlushTimer() { if (on) total += clock_ms() - start; }
    } flush_timer{flush_ms_, started, timed};
    clear_alpha_ = batch_.front().a;
    apply_state();
    if (current_ != nullptr) {
        bool rectangle = false;
        const Rect drawn = batch_bounds(rectangle).shifted(current_ox_, current_oy_).clip(
            Rect{0, 0, static_cast<int>(current_->stride) - 1, static_cast<int>(current_->height) - 1});
        mark_drawn(*current_, drawn);
        if (batch_writes_stencil_) {
            Rect &stale = current_->alpha_stale;
            const bool replaces_all = stale.empty() || (drawn.x0 <= stale.x0 && drawn.y0 <= stale.y0 &&
                                                         drawn.x1 >= stale.x1 && drawn.y1 >= stale.y1);
            if (batch_stencil_value_ >= 0 && replaces_all) {
                stale = drawn;
                current_->stale_value = batch_stencil_value_;
            } else {
                stale.add(drawn);
                current_->stale_value = -1;
            }
        }
        current_->last_draw = ++draw_sequence_;
    }
    draw_.vertices = batch_;
    draw_.indices = indices_;
    gpu_->draw(draw_);
    draw_.vertices = {};
    draw_.indices = {};
    indices_.clear();
    ++draw_calls_;
    batch_.clear();
}

GeSurfaces::PresentSource GeSurfaces::present_source(std::uint32_t fb_address, std::uint32_t stride, std::uint32_t format) {
    flush();
    // The CPU may have drawn since the last list (debug text).
    if (coherence_ == Coherence::Full) reload_changed_vram();
    else if (coherence_ == Coherence::Lazy) apply_cpu_writes();
    PresentSource out;
    const std::uint32_t offset = vram_offset(fb_address);
    int ox = 0, oy = 0;
    Surface *s = offset != kNotVram ? find_surface(offset, stride, format, ox, oy) : nullptr;
    if (s != nullptr && !s->content.empty()) {
        // Rows below what was drawn are shown from VRAM, not stretched from the surface edge.
        if (static_cast<std::uint32_t>(oy) + kDisplayHeight > s->height) grow(*s, static_cast<std::uint32_t>(oy) + kDisplayHeight);
        apply_pending_uploads(*s);
        sync_overlaps(*s);
        out.target = s->gpu;
        out.format = format;
        out.rect_w = static_cast<float>(kDisplayWidth) / static_cast<float>(s->stride);
        out.rect_h = static_cast<float>(kDisplayHeight) / static_cast<float>(s->height);
        out.origin_x = static_cast<float>(ox) / static_cast<float>(s->stride);
        out.origin_y = static_cast<float>(oy) / static_cast<float>(s->height);
    } else if (fb_address != 0u) {
        // CPU-drawn framebuffer: show it as it is in guest memory.
        const std::uint32_t bpp = bpp_of(format);
        prepare_vram_read(fb_address, stride * kDisplayHeight * bpp);
        pixels_.assign(kDisplayWidth * kDisplayHeight * 4u, 0u);
        for (std::uint32_t y = 0; y < kDisplayHeight; ++y) {
            const std::uint8_t *row = memory_.raw_pointer(fb_address + y * stride * bpp, kDisplayWidth * bpp);
            if (row == nullptr) continue;
            for (std::uint32_t x = 0; x < kDisplayWidth; ++x) {
                std::uint32_t rgba;
                if (bpp == 4u) std::memcpy(&rgba, row + x * 4u, 4u);
                else rgba = to_rgba(static_cast<std::uint32_t>(row[x * 2u]) | (static_cast<std::uint32_t>(row[x * 2u + 1u]) << 8u), format);
                std::memcpy(&pixels_[(y * kDisplayWidth + x) * 4u], &rgba, 4u);
            }
        }
        out.cpu_pixels = pixels_.data();
    }
    return out;
}

void GeSurfaces::mark_drawn(Surface &s, const Rect &r) {
    const Rect c = r.clip(Rect{0, 0, static_cast<int>(s.stride) - 1, static_cast<int>(s.height) - 1});
    if (c.empty()) return;
    s.content.add(c);
    s.gpu_dirty.add(c);
    if (s.rows.size() < s.height) s.rows.resize(s.height);
    bool dirtier = false; // some pixel was not GPU-dirty yet, so its page may not be marked
    for (int y = c.y0; y <= c.y1; ++y) {
        auto &row = s.rows[static_cast<std::size_t>(y)];
        if (row.cx1 < row.cx0) { row.cx0 = c.x0; row.cx1 = c.x1; }
        else { row.cx0 = std::min(row.cx0, c.x0); row.cx1 = std::max(row.cx1, c.x1); }
        if (row.dx1 < row.dx0) { row.dx0 = c.x0; row.dx1 = c.x1; dirtier = true; }
        else {
            dirtier = dirtier || c.x0 < row.dx0 || c.x1 > row.dx1;
            row.dx0 = std::min(row.dx0, c.x0);
            row.dx1 = std::max(row.dx1, c.x1);
        }
    }
    if (coherence_ == Coherence::Lazy && dirtier) {
        const std::uint32_t bpp = bpp_of(s.format), rb = row_bytes(s);
        mark_vram_pages(s.offset + static_cast<std::uint32_t>(c.y0) * rb + static_cast<std::uint32_t>(c.x0) * bpp,
                        s.offset + static_cast<std::uint32_t>(c.y1) * rb + static_cast<std::uint32_t>(c.x1 + 1) * bpp,
                        psprecomp::GuestMemory::kVramGpuDirty | psprecomp::GuestMemory::kVramWatched);
    }
}

void GeSurfaces::recompute_bounds(Surface &s) {
    s.content = Rect{};
    s.gpu_dirty = Rect{};
    for (std::size_t y = 0; y < s.rows.size(); ++y) {
        const auto &row = s.rows[y];
        if (row.cx0 <= row.cx1) s.content.add(row.cx0, static_cast<int>(y), row.cx1, static_cast<int>(y));
        if (row.dx0 <= row.dx1) s.gpu_dirty.add(row.dx0, static_cast<int>(y), row.dx1, static_cast<int>(y));
    }
}

template <typename F>
void GeSurfaces::for_each_segment(const Surface &s, const TransferRect &rect, F &&f) const {
    const std::uint32_t base = vram_offset(rect.base);
    if (!s.alive || base == kNotVram || rect.width == 0u || rect.height == 0u) return;
    const std::uint32_t bpp = bpp_of(s.format), rb = row_bytes(s), end = surface_end(s);
    const std::uint32_t span_first = base + (rect.y * rect.stride + rect.x) * rect.bpp;
    const std::uint32_t span_last = base + ((rect.y + rect.height - 1u) * rect.stride + rect.x + rect.width) * rect.bpp;
    if (span_last <= s.offset || span_first >= end) return;
    // Byte runs of the copy, with touching rows coalesced (texture uploads are
    // usually one linear run), split into surface rows.
    auto emit = [&](std::uint32_t start, std::uint32_t stop) {
        if (stop <= s.offset || start >= end) return;
        std::uint32_t from = std::max(start, s.offset) - s.offset;
        const std::uint32_t to = std::min(stop, end) - s.offset; // exclusive
        while (from < to) {
            const std::uint32_t row = from / rb, row_end = std::min(to, (row + 1u) * rb);
            f(static_cast<int>(row), static_cast<int>((from % rb) / bpp), static_cast<int>(((row_end - 1u) % rb) / bpp));
            from = row_end;
        }
    };
    std::uint32_t run_start = 0, run_stop = 0;
    for (std::uint32_t r = 0; r < rect.height; ++r) {
        const std::uint32_t start = base + ((rect.y + r) * rect.stride + rect.x) * rect.bpp;
        const std::uint32_t stop = start + rect.width * rect.bpp;
        if (r != 0u && start == run_stop) {
            run_stop = stop;
            continue;
        }
        if (r != 0u) emit(run_start, run_stop);
        run_start = start;
        run_stop = stop;
    }
    emit(run_start, run_stop);
}

bool GeSurfaces::locate(const Surface &s, std::uint32_t offset, int &ox, int &oy) const {
    if (!s.alive || offset < s.offset) return false;
    const std::uint32_t diff = offset - s.offset, bpp = bpp_of(s.format);
    if (diff % bpp != 0u) return false;
    const std::uint32_t row = diff / row_bytes(s);
    if (row >= s.height) return false;
    oy = static_cast<int>(row);
    ox = static_cast<int>((diff % row_bytes(s)) / bpp);
    return true;
}

GeSurfaces::Surface *GeSurfaces::find_surface(std::uint32_t offset, std::uint32_t stride, std::uint32_t format, int &ox, int &oy) {
    for (auto &s : surfaces_)
        if (s.alive && s.stride == stride && s.format == format && locate(s, offset, ox, oy)) return &s;
    return nullptr;
}

bool GeSurfaces::is_gpu_target(std::uint32_t address, std::uint32_t format, std::uint32_t buf_width) const {
    const std::uint32_t offset = vram_offset(address);
    if (offset == kNotVram) return false;
    for (const auto &s : surfaces_) {
        int ox = 0, oy = 0;
        if (!s.alive || s.stride != buf_width || s.format != format || !locate(s, offset, ox, oy)) continue;
        if (static_cast<std::size_t>(oy) < s.rows.size()) {
            const auto &row = s.rows[static_cast<std::size_t>(oy)];
            if (row.cx0 <= ox && ox <= row.cx1) return true;
        }
    }
    return false;
}

GeSurfaces::Surface &GeSurfaces::surface_for_draw(std::uint32_t offset, std::uint32_t stride, std::uint32_t format,
                                      std::uint32_t rows, int &ox, int &oy) {
    rows = std::clamp(rows, 1u, kMaxSurfaceHeight);
    stride = std::max(stride, 1u);
    const std::uint32_t bpp = bpp_of(format);
    for (auto &s : surfaces_) {
        if (!s.alive || s.stride != stride || s.format != format || offset < s.offset) continue;
        const std::uint32_t diff = offset - s.offset;
        if (diff % bpp != 0u || diff / row_bytes(s) >= s.height) continue;
        oy = static_cast<int>(diff / row_bytes(s));
        ox = static_cast<int>((diff % row_bytes(s)) / bpp);
        if (static_cast<std::uint32_t>(oy) + rows > s.height) grow(s, static_cast<std::uint32_t>(oy) + rows);
        return s;
    }
    surfaces_.emplace_back();
    Surface &n = surfaces_.back();
    n.offset = offset;
    n.stride = stride;
    n.format = format;
    allocate(n, rows);
    upload_rect(n, Rect{0, 0, static_cast<int>(n.stride) - 1, static_cast<int>(n.height) - 1});
    // Surfaces of the same layout that start inside the new one become part of it.
    for (auto &s : surfaces_) {
        if (&s == &n || !s.alive || s.stride != stride || s.format != format || s.offset <= offset) continue;
        const std::uint32_t diff = s.offset - offset;
        if (diff % bpp != 0u || diff / row_bytes(n) >= kMaxSurfaceHeight) continue;
        if (diff / row_bytes(n) >= n.height && diff / row_bytes(n) >= rows) continue;
        absorb(n, s);
    }
    ox = 0;
    oy = 0;
    return n;
}

void GeSurfaces::apply_pending_uploads(Surface &s) {
    if (s.pending_uploads.empty()) return;
    std::vector<Rect> rects;
    rects.swap(s.pending_uploads);
    for (const Rect &r : rects) upload_rect(s, r);
}

GeSurfaces::Rect GeSurfaces::overlap(const Surface &s, const TransferRect &rect) const {
    Rect out;
    const std::uint32_t base = vram_offset(rect.base);
    if (!s.alive || base == kNotVram || rect.width == 0u || rect.height == 0u) return out;
    const std::uint32_t bpp = bpp_of(s.format), rb = row_bytes(s), end = surface_end(s);
    const std::uint32_t span_first = base + (rect.y * rect.stride + rect.x) * rect.bpp;
    const std::uint32_t span_last = base + ((rect.y + rect.height - 1u) * rect.stride + rect.x + rect.width) * rect.bpp;
    if (span_last <= s.offset || span_first >= end) return out; // quick reject
    for (std::uint32_t row = 0; row < rect.height; ++row) {
        const std::uint32_t start = base + ((rect.y + row) * rect.stride + rect.x) * rect.bpp;
        const std::uint32_t stop = start + rect.width * rect.bpp; // exclusive
        if (stop <= s.offset || start >= end) continue;
        const std::uint32_t from = std::max(start, s.offset) - s.offset, to = std::min(stop, end) - s.offset;
        const auto y0 = static_cast<int>(from / rb), y1 = static_cast<int>((to - 1u) / rb);
        if (y0 == y1) out.add(static_cast<int>((from % rb) / bpp), y0, static_cast<int>(((to - 1u) % rb) / bpp), y1);
        else out.add(0, y0, static_cast<int>(s.stride) - 1, y1);
    }
    return out;
}

void GeSurfaces::alpha_will_change(Surface &s, const Rect &r) {
    if (s.format == 0u) return; // RGB565 has neither alpha nor stencil
    // Keep the two stale areas apart: settle the alpha first if they would touch.
    Rect grown = s.stencil_stale;
    grown.add(r);
    if (grown.intersects(s.alpha_stale)) resolve_alpha(s, whole(s));
    s.stencil_stale.add(r);
}

void GeSurfaces::sync_overlaps(Surface &s) {
    for (auto &other : surfaces_) {
        if (&other == &s || !other.alive || other.content.empty() || other.last_draw == 0u) continue;
        auto synced = std::find_if(s.synced.begin(), s.synced.end(), [&](const auto &e) { return e.first == &other; });
        if (synced != s.synced.end() && synced->second >= other.last_draw) continue;
        const std::uint32_t bpp = bpp_of(other.format), rb = row_bytes(other);
        const std::uint32_t begin = other.offset + static_cast<std::uint32_t>(other.content.y0) * rb;
        const std::uint32_t end = other.offset + static_cast<std::uint32_t>(other.content.y1 + 1) * rb;
        if (!(begin < surface_end(s) && s.offset < end)) continue;
        const TransferRect drawn{0x04000000u + other.offset, other.stride, static_cast<std::uint32_t>(other.content.x0),
                                 static_cast<std::uint32_t>(other.content.y0),
                                 static_cast<std::uint32_t>(other.content.x1 - other.content.x0 + 1),
                                 static_cast<std::uint32_t>(other.content.y1 - other.content.y0 + 1), bpp};
        const Rect hit = overlap(s, drawn);
        if (synced == s.synced.end()) {
            s.synced.emplace_back(&other, 0u);
            synced = s.synced.end() - 1;
        }
        synced->second = other.last_draw;
        if (hit.empty()) continue;
        // Earlier batches are already drawn: surfaces are synced while applying state.
        if (!reinterpret(s, other, hit)) {
            download(other);
            upload_rect(s, hit);
        }
        mark_drawn(s, hit);
    }
}

bool GeSurfaces::batch_uses(const Surface &s) const {
    if (batch_.empty() || !state_valid_) return false;
    int ox = 0, oy = 0;
    if (s.stride == state_.fb_stride && s.format == state_.fb_format && locate(s, state_.fb_offset, ox, oy)) return true;
    return state_.tex_from_target && locate(s, vram_offset(state_.tex_address), ox, oy);
}

bool GeSurfaces::prepare_vram_read(std::uint32_t address, std::uint32_t bytes) {
    if (in_between_) return false;
    const std::uint32_t offset = vram_offset(address);
    if (offset == kNotVram) return false;
    bool downloaded = false;
    for (auto &s : surfaces_) {
        if (!s.alive || s.gpu_dirty.empty()) continue;
        const std::uint32_t rb = row_bytes(s);
        const std::uint32_t begin = s.offset + static_cast<std::uint32_t>(s.gpu_dirty.y0) * rb;
        const std::uint32_t end = s.offset + static_cast<std::uint32_t>(s.gpu_dirty.y1 + 1) * rb;
        if (offset < end && begin < offset + bytes) {
            if (batch_uses(s)) flush();
            download(s);
            downloaded = true;
        }
    }
    // Also clears pages left marked by surfaces that were retired or absorbed,
    // so the CPU stops calling in for them.
    if (coherence_ == Coherence::Lazy && page_flags_stale_) refresh_page_flags();
    return downloaded;
}

void GeSurfaces::set_state(const GlDrawState &state) {
    if (state_valid_ && state == state_) return;
    flush();
    state_ = state;
    state_valid_ = true;
}

void GeSurfaces::triangle(const GlVertex &a, const GlVertex &b, const GlVertex &c) {
    const auto base = static_cast<std::uint32_t>(batch_.size());
    batch_.push_back(a);
    batch_.push_back(b);
    batch_.push_back(c);
    indices_.push_back(base);
    indices_.push_back(base + 1u);
    indices_.push_back(base + 2u);
}

GeSurfaces::Rect GeSurfaces::batch_bounds(bool &rectangle) const {
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    for (const auto &v : batch_) {
        min_x = std::min(min_x, v.x);
        max_x = std::max(max_x, v.x);
        min_y = std::min(min_y, v.y);
        max_y = std::max(max_y, v.y);
    }
    rectangle = batch_.size() == 6u && std::all_of(batch_.begin(), batch_.end(), [&](const GlVertex &v) {
        return (v.x == min_x || v.x == max_x) && (v.y == min_y || v.y == max_y);
    });
    // Pixels whose centres a primitive can cover: floor(min) .. ceil(max) - 1.
    const int x0 = std::max(static_cast<int>(std::floor(std::max(min_x, -4096.0f))), state_.sx1);
    const int x1 = std::min(static_cast<int>(std::ceil(std::min(max_x, 4096.0f))) - 1, state_.sx2);
    const int y0 = std::max(static_cast<int>(std::floor(std::max(min_y, -4096.0f))), state_.sy1);
    const int y1 = std::min(static_cast<int>(std::ceil(std::min(max_y, 4096.0f))) - 1, state_.sy2);
    return Rect{x0, y0, x1, y1};
}

void GeSurfaces::before_transfer(const TransferRect &src, const TransferRect &) {
    // Draws still batched may cover the source; they only count as GPU-dirty once issued.
    flush();
    if (in_between_) return;
    // Pixels only the GPU has must reach VRAM before the copy reads them.
    // Destinations need no download: the copy is newer than whatever is there.
    for (auto &s : surfaces_) {
        if (!s.alive || s.gpu_dirty.empty()) continue;
        bool hit = false;
        for_each_segment(s, src, [&](int row, int x0, int x1) {
            if (hit || static_cast<std::size_t>(row) >= s.rows.size()) return;
            const auto &span = s.rows[static_cast<std::size_t>(row)];
            hit = span.dx0 <= span.dx1 && x0 <= span.dx1 && span.dx0 <= x1;
        });
        if (hit) {
            flush();
            download(s);
        }
    }
}

void GeSurfaces::after_transfer(const TransferRect &dst) {
    // Only pixels inside what the GPU has drawn matter: copies elsewhere (for
    // example textures packed into the unused part of a framebuffer stride) are
    // read from VRAM directly.  A copy covering a row's whole drawn extent
    // retires that row: VRAM is the truth there again and nothing is uploaded.
    for (auto &s : surfaces_) {
        if (!s.alive || s.content.empty()) continue;
        bool flushed = false, retired = false;
        Rect partial;
        for_each_segment(s, dst, [&](int row, int x0, int x1) {
            if (static_cast<std::size_t>(row) >= s.rows.size()) return;
            auto &span = s.rows[static_cast<std::size_t>(row)];
            if (span.cx1 < span.cx0 || x1 < span.cx0 || span.cx1 < x0) return;
            if (!flushed && batch_uses(s)) flush(); // draws recorded before the copy must not see it
            flushed = true;
            if (x0 <= span.cx0 && span.cx1 <= x1) {
                span = Surface::RowSpan{};
                retired = true;
                return;
            }
            partial.add(std::max(x0, span.cx0), row, std::min(x1, span.cx1), row);
        });
        if (retired) {
            recompute_bounds(s);
            page_flags_stale_ = true;
        }
        if (!partial.empty()) {
            // One rectangle per copy keeps the number of GL uploads low.
            if (!s.pending_uploads.empty()) {
                Rect &last = s.pending_uploads.back();
                if (last.x0 == partial.x0 && last.x1 == partial.x1 && last.y1 + 1 >= partial.y0 && partial.y1 + 1 >= last.y0) {
                    last.y0 = std::min(last.y0, partial.y0);
                    last.y1 = std::max(last.y1, partial.y1);
                    partial = Rect{};
                }
            }
            if (!partial.empty()) s.pending_uploads.push_back(partial);
        }
        if (s.pending_uploads.size() > 64u) apply_pending_uploads(s);
    }
}

std::uint64_t GeSurfaces::hash_vram(const Surface &s) const {
    const std::uint32_t end = std::min(surface_end(s), kVramSize);
    const std::uint8_t *vram = memory_.raw_pointer(0x04000000u, kVramSize);
    if (vram == nullptr || end <= s.offset) return 0u;
    std::uint64_t h = 0xCBF29CE484222325ull;
    std::uint32_t at = s.offset;
    for (; at + 8u <= end; at += 8u) {
        std::uint64_t word;
        std::memcpy(&word, vram + at, 8u);
        h = (h ^ word) * 0x100000001B3ull;
        h ^= h >> 29u;
    }
    for (; at < end; ++at) h = (h ^ vram[at]) * 0x100000001B3ull;
    return h;
}

void GeSurfaces::list_started() {
    if (coherence_ == Coherence::Full) reload_changed_vram();
    else if (coherence_ == Coherence::Lazy) apply_cpu_writes();
}

void GeSurfaces::mark_vram_pages(std::uint32_t begin, std::uint32_t end, std::uint8_t bits) {
    using M = psprecomp::GuestMemory;
    end = std::min(end, kVramSize);
    if (begin >= end) return;
    for (std::uint32_t page = begin >> M::kVramPageShift; page <= (end - 1u) >> M::kVramPageShift; ++page) {
        const auto now = static_cast<std::uint8_t>(memory_.vram_page_state(page) & (M::kVramGpuDirty | M::kVramWatched));
        if ((now | bits) != now) memory_.set_vram_page_state(page, static_cast<std::uint8_t>(now | bits));
    }
}

void GeSurfaces::refresh_page_flags() {
    using M = psprecomp::GuestMemory;
    std::array<std::uint8_t, M::kVramPages> want{};
    auto mark = [&](std::uint32_t begin, std::uint32_t end, std::uint8_t bits) {
        end = std::min(end, kVramSize);
        if (begin >= end) return;
        for (std::uint32_t page = begin >> M::kVramPageShift; page <= (end - 1u) >> M::kVramPageShift; ++page)
            want[page] |= bits;
    };
    for (const auto &s : surfaces_) {
        if (!s.alive) continue;
        mark(s.offset, surface_end(s), M::kVramWatched);
        if (s.gpu_dirty.empty()) continue;
        const std::uint32_t bpp = bpp_of(s.format), rb = row_bytes(s);
        for (std::size_t y = 0; y < s.rows.size(); ++y) {
            const auto &row = s.rows[y];
            if (row.dx1 < row.dx0) continue;
            const std::uint32_t at = s.offset + static_cast<std::uint32_t>(y) * rb;
            mark(at + static_cast<std::uint32_t>(row.dx0) * bpp, at + static_cast<std::uint32_t>(row.dx1 + 1) * bpp,
                 M::kVramGpuDirty);
        }
    }
    for (std::uint32_t page = 0; page < M::kVramPages; ++page)
        if ((memory_.vram_page_state(page) & (M::kVramGpuDirty | M::kVramWatched)) != want[page])
            memory_.set_vram_page_state(page, want[page]);
    page_flags_stale_ = false;
}

void GeSurfaces::apply_cpu_writes() {
    if (!memory_.take_vram_cpu_writes(cpu_written_)) return;
    if (cpu_write_observer_) cpu_write_observer_(cpu_written_);
    queue_cpu_writes(cpu_written_);
}

void GeSurfaces::queue_cpu_writes(const PageSet &pages) {
    using M = psprecomp::GuestMemory;
    for (auto &s : surfaces_) {
        if (!s.alive) continue;
        const std::uint32_t rb = row_bytes(s), end = std::min(surface_end(s), kVramSize);
        Rect rows;
        auto queue = [&] {
            if (rows.empty()) return;
            if (batch_uses(s)) flush(); // draws recorded before the write must not see it
            s.pending_uploads.push_back(rows);
            rows = Rect{};
        };
        for (std::uint32_t page = s.offset >> M::kVramPageShift; s.offset < end && page <= (end - 1u) >> M::kVramPageShift; ++page) {
            if (!pages[page]) {
                queue();
                continue;
            }
            const std::uint32_t from = std::max(page << M::kVramPageShift, s.offset) - s.offset;
            const std::uint32_t to = std::min((page + 1u) << M::kVramPageShift, end) - s.offset; // exclusive
            rows.add(0, static_cast<int>(from / rb), static_cast<int>(s.stride) - 1, static_cast<int>((to - 1u) / rb));
        }
        queue();
        if (s.pending_uploads.size() > 64u) apply_pending_uploads(s);
    }
}

void GeSurfaces::reload_changed_vram() {
    for (auto &s : surfaces_) {
        if (!s.alive || hash_vram(s) == s.vram_hash) continue;
        s.pending_uploads.clear();
        upload_rect(s, whole(s));
    }
}

void GeSurfaces::list_finished() {
    if (coherence_ == Coherence::Off) return;
    flush(); // draws mark their pages GPU-dirty only once issued
    if (coherence_ == Coherence::Lazy) {
        if (page_flags_stale_) refresh_page_flags();
        return;
    }
    for (auto &s : surfaces_) {
        if (!s.alive) continue;
        apply_pending_uploads(s);
        if (!s.gpu_dirty.empty()) download(s);
        s.vram_hash = hash_vram(s);
    }
}

void GeSurfaces::report_targets() const {
    for (const auto &s : surfaces_) {
        if (!s.alive) continue;
        std::cerr << "[ge-surface] vram=0x" << std::hex << s.offset << std::dec << " " << s.stride << "x" << s.height
                  << " fmt=" << s.format << " content=" << s.content.x0 << "," << s.content.y0 << ".." << s.content.x1
                  << "," << s.content.y1 << "\n";
    }
}

} // namespace pspweb
