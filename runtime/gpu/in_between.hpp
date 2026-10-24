// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The renderer the GE draws through when frame interpolation is on: it forwards every call to
// the real GL renderer and to a second one that only draws the in-between picture
// (host/ge_interp.hpp). Both share one GL context, so whenever the other one is about to issue
// GL calls, its cached GL state is dropped first.

#include "ge_gl.hpp"
#include "ge_interp.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace anypsp::gpu {

class InBetweenRenderer final : public pspweb::InterpolatingRenderer {
public:
    InBetweenRenderer(pspweb::GeGl &real, pspweb::GeGl &in_between);

    void set_target(Target target) override;
    // Someone else used the GL context: both renderers forget their cached GL state.
    void invalidate() { last_ = nullptr; }
    // Brings the in-between renderer up to date before it presents.
    void prepare_in_between();

    [[nodiscard]] bool is_gpu_target(std::uint32_t address, std::uint32_t format, std::uint32_t buf_width) const override {
        return real_.is_gpu_target(address, format, buf_width);
    }
    bool prepare_vram_read(std::uint32_t address, std::uint32_t bytes) override;
    void set_state(const pspweb::GlDrawState &state) override;
    void triangle(const pspweb::GlVertex &a, const pspweb::GlVertex &b, const pspweb::GlVertex &c) override;
    std::uint32_t vertex(const pspweb::GlVertex &v) override;
    void indexed_triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) override;
    void flush() override;
    void forget_texture(const void *identity) override;
    void before_transfer(const pspweb::TransferRect &src, const pspweb::TransferRect &dst) override;
    void after_transfer(const pspweb::TransferRect &dst) override;
    void set_vram_write_hook(std::function<void(std::uint32_t, std::uint32_t)> hook) override;
    void list_started() override;
    void list_finished() override;

private:
    pspweb::GeGl &use(pspweb::GeGl &renderer);

    pspweb::GeGl &real_, &in_between_;
    pspweb::GeGl *last_{};
    Target target_{Target::Both};
    // Target::Both: batch indices of a vertex in each renderer, by the index handed out.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs_;
    pspweb::GeGl::PageSet cpu_writes_{};
    bool cpu_writes_pending_{};
};

} // namespace anypsp::gpu
