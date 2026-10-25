#pragma once

// The GE's hardware renderer, independent of the graphics API. The Ge front end keeps vertex
// decoding, transform, lighting and clipping on the CPU and hands this class drawing-space
// triangles plus the render state of each primitive. Triangles sharing a state are batched
// into one draw, which a GeGpu device (ge_gpu.hpp: OpenGL or Vulkan) executes.
//
// Framebuffers live in GPU "surfaces": one render target per VRAM region, stride and pixel
// format. A framebuffer address that falls inside an existing surface of the same layout
// (games pack bloom chains side by side, or point the GE one row into a buffer) is drawn into
// that surface at an offset, and a texture read from such an address samples the surface
// directly. Guest VRAM is only synchronized when a block transfer, a CPU-side texture decode,
// a reinterpretation in another pixel format, or the display needs it.

#include "ge_gpu.hpp"
#include "ge_renderer.hpp"
#include "psprecomp/guest_memory.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace pspweb {

class GeSurfaces : public GeRenderer {
public:
    GeSurfaces(psprecomp::GuestMemory &memory, std::unique_ptr<GeGpu> gpu);
    ~GeSurfaces() override;
    GeSurfaces(const GeSurfaces &) = delete;
    GeSurfaces &operator=(const GeSurfaces &) = delete;

    // How guest VRAM follows the GPU surfaces outside block transfers, texture
    // decodes and reinterpretation (which always synchronize):
    //  - Off:  never; CPU reads of framebuffers see stale pixels.
    //  - Lazy: GuestMemory page tracking.  GPU-drawn pages are downloaded when
    //          the CPU touches them; pages the CPU wrote are reloaded into their
    //          surfaces when the next list starts or the frame is presented.
    //          Needs GuestMemory::set_vram_read_hook routed to prepare_vram_read.
    //  - Full: GPU pixels are written back after every list and changed VRAM is
    //          found by hashing; a readback per list, kept for debugging.
    enum class Coherence { Off, Lazy, Full };
    void set_coherence(Coherence mode) { coherence_ = mode; }
    [[nodiscard]] Coherence coherence() const noexcept { return coherence_; }
    void list_started() override;
    void list_finished() override;

    // An in-between renderer (ge_interp.hpp) draws a second picture next to the real one and must
    // never change guest memory: its downloads keep only the bookkeeping, it ignores block
    // transfer sources and CPU reads, and it reloads only VRAM pages the real renderer reports
    // as written by the CPU (set_cpu_write_observer on the real one, note_cpu_writes here).
    void set_in_between(bool on) { in_between_ = on; }
    using PageSet = std::array<bool, psprecomp::GuestMemory::kVramPages>;
    void set_cpu_write_observer(std::function<void(const PageSet &)> observer) { cpu_write_observer_ = std::move(observer); }
    void note_cpu_writes(const PageSet &pages) { queue_cpu_writes(pages); }

    // True when a texture at `address` (format 0..3, buffer width in pixels)
    // lies inside GPU-drawn surface content and can be sampled from it.
    [[nodiscard]] bool is_gpu_target(std::uint32_t address, std::uint32_t format, std::uint32_t buf_width) const override;
    // Makes VRAM current for a range the CPU is about to read (texture decode).
    // Returns true when GPU pixels were downloaded into the range.
    bool prepare_vram_read(std::uint32_t address, std::uint32_t bytes) override;

    void set_state(const GlDrawState &state) override;
    void triangle(const GlVertex &a, const GlVertex &b, const GlVertex &c) override;
    // Indexed form: add vertices once, then triangles by batch-relative index.
    std::uint32_t vertex(const GlVertex &v) override {
        batch_.push_back(v);
        return static_cast<std::uint32_t>(batch_.size() - 1u);
    }
    void indexed_triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) override {
        indices_.push_back(a);
        indices_.push_back(b);
        indices_.push_back(c);
    }
    void flush() override;
    void forget_texture(const void *identity) override;

    // Keep guest VRAM and surfaces coherent around a block transfer.
    void before_transfer(const TransferRect &src, const TransferRect &dst) override;
    void after_transfer(const TransferRect &dst) override;
    // Called whenever GPU pixels are written back into guest VRAM.
    void set_vram_write_hook(std::function<void(std::uint32_t, std::uint32_t)> hook) override { vram_write_hook_ = std::move(hook); }

    void next_frame() { ++frame_; }
    // Internal resolution as a multiple of the PSP's (1 = native 480x272).
    // Changing it writes GPU-only pixels back to VRAM and rebuilds every
    // surface at the new size.  Returns the scale actually used, which the
    // device's size limits may lower.
    int set_render_scale(int scale);
    [[nodiscard]] int render_scale() const noexcept { return scale_; }
    [[nodiscard]] int max_render_scale() const { return gpu_->max_scale(); }

    [[nodiscard]] std::uint64_t draw_calls() const noexcept { return draw_calls_; }
    [[nodiscard]] std::uint64_t downloads() const noexcept { return downloads_; }
    [[nodiscard]] std::uint64_t uploads() const noexcept { return uploads_; }
    // Milliseconds spent issuing draw batches (only measured with PSPWEB_FRAME_PROFILE).
    [[nodiscard]] double flush_ms() const noexcept { return flush_ms_; }
    [[nodiscard]] std::uint64_t upload_pixels() const noexcept { return upload_pixels_; }
    void reset_counters() { draw_calls_ = 0; downloads_ = 0; uploads_ = 0; upload_pixels_ = 0; flush_ms_ = 0.0; }
    void report_targets() const;
    // Extra GPU passes per kind, for profiling.
    struct PassCounters {
        std::uint64_t alpha_resolves{}, stencil_resolves{}, reinterprets{}, feedback_copies{}, blits{};
        std::uint64_t pixels{}; // output pixels those passes touched, at the internal resolution
    };
    [[nodiscard]] const PassCounters &passes() const noexcept { return passes_; }
    void reset_passes() { passes_ = PassCounters{}; }

    // What the display shows: part of a surface, or (cpu_pixels) a 480x272 RGBA picture of
    // a framebuffer only the CPU drew. Flushes and synchronizes first.
    struct PresentSource {
        GpuTarget target{};
        float rect_w{1.0f}, rect_h{1.0f}, origin_x{}, origin_y{}; // fractions of the surface
        std::uint32_t format{3}; // framebuffer pixel format: 16-bit ones are shown with their precision
        const std::uint8_t *cpu_pixels{};
    };
    static constexpr std::uint32_t kDisplayWidth = 480u, kDisplayHeight = 272u;
    PresentSource present_source(std::uint32_t fb_address, std::uint32_t stride, std::uint32_t format);

protected:
    [[nodiscard]] GeGpu &gpu() noexcept { return *gpu_; }

private:
    using Rect = GeRect;
    struct Surface {
        std::uint32_t offset{}, stride{}, format{}, height{};
        GpuTarget gpu{};
        Rect gpu_dirty;  // drawn by the GPU since the last download to VRAM
        Rect content;    // everything the GPU has drawn
        bool alive{true};
        std::uint64_t last_draw{};  // draw sequence of the newest batch drawn into it
        std::vector<Rect> pending_uploads; // VRAM rectangles a block transfer rewrote
        // The PSP keeps the stencil in the framebuffer's alpha.  Here it lives
        // in the GPU stencil buffer while drawing and is copied into the colour
        // alpha (or back) only when the other side is about to be used.
        // Never overlapping: inside alpha_stale the stencil is newer than the
        // colour alpha, inside stencil_stale the colour alpha is newer.
        Rect alpha_stale;
        Rect stencil_stale;
        // Bits that may be set anywhere in the stencil / colour alpha; resolves
        // skip the others (games tend to use one or two stencil bits).
        std::uint32_t stencil_bits{}, alpha_bits{0xFFu};
        int stale_value{-1};  // the stencil value throughout alpha_stale, when one rectangle set it
        // Exact per-row extents behind the bounding rectangles above, so a block
        // transfer that overwrites whole rows of stale GPU output (games reuse
        // post-processing buffers for texture uploads) just retires those rows.
        struct RowSpan {
            int cx0{0}, cx1{-1};  // content
            int dx0{0}, dx1{-1};  // GPU dirty
        };
        std::vector<RowSpan> rows;
        // Newest draw of each overlapping surface already copied into this one.
        std::vector<std::pair<const Surface *, std::uint64_t>> synced;
        std::uint64_t vram_hash{}; // coherent mode: VRAM contents when the last list ended
    };

    [[nodiscard]] static std::uint32_t bpp_of(std::uint32_t format) { return format == 3u ? 4u : 2u; }
    [[nodiscard]] std::uint32_t row_bytes(const Surface &s) const { return s.stride * bpp_of(s.format); }
    [[nodiscard]] std::uint32_t surface_end(const Surface &s) const { return s.offset + row_bytes(s) * s.height; }
    // Position of VRAM `offset` inside `s` in pixels; false when outside.
    [[nodiscard]] bool locate(const Surface &s, std::uint32_t offset, int &ox, int &oy) const;
    // Surface and origin for drawing at `offset`; creates, grows or merges surfaces.
    Surface &surface_for_draw(std::uint32_t offset, std::uint32_t stride, std::uint32_t format,
                              std::uint32_t rows, int &ox, int &oy);
    Surface *find_surface(std::uint32_t offset, std::uint32_t stride, std::uint32_t format, int &ox, int &oy);
    void allocate(Surface &s, std::uint32_t height);
    void release(Surface &s);
    void grow(Surface &s, std::uint32_t height);
    void absorb(Surface &into, Surface &from);
    // Brings `s` up to date with GPU pixels of overlapping surfaces in another layout.
    void sync_overlaps(Surface &s);
    // Copies `from` into `into` on the GPU, reinterpreting 32-bit pixels as
    // pairs of 16-bit ones (or back); false when the layouts are incompatible.
    bool reinterpret(Surface &into, const Surface &from, const Rect &hit);
    void download(Surface &s);
    void mark_drawn(Surface &s, const Rect &r);
    void recompute_bounds(Surface &s);
    // Calls f(row, x0, x1) for each piece of `rect` that lands in surface `s`.
    template <typename F>
    void for_each_segment(const Surface &s, const TransferRect &rect, F &&f) const;
    // Uploads rectangles block transfers wrote, before the surface is used.
    void apply_pending_uploads(Surface &s);
    void upload_rect(Surface &s, const Rect &rect);
    // Part of a VRAM rectangle that falls inside `s`, in surface pixels.
    [[nodiscard]] Rect overlap(const Surface &s, const TransferRect &rect) const;
    [[nodiscard]] bool batch_uses(const Surface &s) const;
    void texture_for_state(GpuDraw &draw, Surface *source, int ox, int oy);
    [[nodiscard]] static Rect whole(const Surface &s) {
        return Rect{0, 0, static_cast<int>(s.stride) - 1, static_cast<int>(s.height) - 1};
    }
    // stencil -> colour alpha inside `region`.  With fewer than all `bits`, only
    // those bits are made correct and the area stays marked stale.
    void resolve_alpha(Surface &s, const Rect &region, std::uint32_t bits = 0xFFu);
    // Bounds of the pending batch in the PSP pixels of the drawing-space
    // (unshifted) frame, and whether it is a single axis-aligned rectangle.
    [[nodiscard]] Rect batch_bounds(bool &rectangle) const;
    void resolve_stencil(Surface &s);                    // colour alpha -> stencil where it is stale
    // Call before colour alpha in `r` is overwritten from VRAM or another surface.
    void alpha_will_change(Surface &s, const Rect &r);
    // Brings the surfaces a draw uses up to date and describes the draw in draw_.
    void apply_state();
    bool batch_writes_stencil_{};
    int batch_stencil_value_{-1}; // value a rectangular stencil batch sets everywhere it covers
    int clear_alpha_{};

    psprecomp::GuestMemory &memory_;
    std::unique_ptr<GeGpu> gpu_;
    std::uint64_t draw_sequence_{};
    // Surfaces are scale_ times larger than the PSP pixels they hold.
    int scale_{1};
    std::deque<Surface> surfaces_;
    std::vector<GlVertex> batch_;
    std::vector<std::uint32_t> indices_;
    GlDrawState state_{};
    GpuDraw draw_{};
    bool state_valid_{};
    Surface *current_{};
    int current_ox_{}, current_oy_{};
    std::uint32_t frame_{1};
    std::uint64_t draw_calls_{};
    PassCounters passes_{};
    std::uint64_t downloads_{};
    std::uint64_t uploads_{}, upload_pixels_{};
    double flush_ms_{};
    std::vector<std::uint8_t> pixels_;
    std::function<void(std::uint32_t, std::uint32_t)> vram_write_hook_;
    Coherence coherence_{Coherence::Off};
    [[nodiscard]] std::uint64_t hash_vram(const Surface &s) const;
    void reload_changed_vram();
    // Lazy coherence: GuestMemory page bits for VRAM bytes [begin, end).
    void mark_vram_pages(std::uint32_t begin, std::uint32_t end, std::uint8_t bits);
    // Rebuilds every page's GPU-dirty and watched bits from the surfaces.
    void refresh_page_flags();
    // Queues reloads of surface rows on pages the CPU wrote.
    void apply_cpu_writes();
    void queue_cpu_writes(const PageSet &pages);
    bool in_between_{};
    std::function<void(const PageSet &)> cpu_write_observer_;
    bool page_flags_stale_{};
    std::array<bool, psprecomp::GuestMemory::kVramPages> cpu_written_{};
};

} // namespace pspweb
