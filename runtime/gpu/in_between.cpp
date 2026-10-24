// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_between.hpp"

namespace anypsp::gpu {

InBetweenRenderer::InBetweenRenderer(pspweb::GeGl &real, pspweb::GeGl &in_between) : real_(real), in_between_(in_between) {
    in_between_.set_in_between(true);
    // Recorded here and applied the next time the in-between renderer works, so it never issues
    // GL calls in the middle of one of the real renderer's.
    real_.set_cpu_write_observer([this](const pspweb::GeGl::PageSet &pages) {
        for (std::size_t i = 0; i < pages.size(); ++i) cpu_writes_[i] = cpu_writes_[i] || pages[i];
        cpu_writes_pending_ = true;
    });
}

pspweb::GeGl &InBetweenRenderer::use(pspweb::GeGl &renderer) {
    if (last_ != &renderer) {
        renderer.reset_gl_cache();
        last_ = &renderer;
    }
    if (&renderer == &in_between_ && cpu_writes_pending_) {
        cpu_writes_pending_ = false;
        in_between_.note_cpu_writes(cpu_writes_);
        cpu_writes_.fill(false);
    }
    return renderer;
}

void InBetweenRenderer::prepare_in_between() {
    invalidate();
    use(in_between_);
}

void InBetweenRenderer::set_target(Target target) {
    target_ = target;
    pairs_.clear();
}

bool InBetweenRenderer::prepare_vram_read(std::uint32_t address, std::uint32_t bytes) {
    return use(real_).prepare_vram_read(address, bytes);
}

void InBetweenRenderer::set_state(const pspweb::GlDrawState &state) {
    pairs_.clear();
    use(real_).set_state(state);
    use(in_between_).set_state(state);
}

void InBetweenRenderer::triangle(const pspweb::GlVertex &a, const pspweb::GlVertex &b, const pspweb::GlVertex &c) {
    if (target_ != Target::InBetween) real_.triangle(a, b, c);
    if (target_ != Target::Real) in_between_.triangle(a, b, c);
}

std::uint32_t InBetweenRenderer::vertex(const pspweb::GlVertex &v) {
    if (target_ == Target::Real) return real_.vertex(v);
    if (target_ == Target::InBetween) return in_between_.vertex(v);
    pairs_.emplace_back(real_.vertex(v), in_between_.vertex(v));
    return static_cast<std::uint32_t>(pairs_.size() - 1u);
}

void InBetweenRenderer::indexed_triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    if (target_ == Target::Real) {
        real_.indexed_triangle(a, b, c);
    } else if (target_ == Target::InBetween) {
        in_between_.indexed_triangle(a, b, c);
    } else if (a < pairs_.size() && b < pairs_.size() && c < pairs_.size()) {
        real_.indexed_triangle(pairs_[a].first, pairs_[b].first, pairs_[c].first);
        in_between_.indexed_triangle(pairs_[a].second, pairs_[b].second, pairs_[c].second);
    }
}

void InBetweenRenderer::flush() {
    pairs_.clear();
    use(real_).flush();
    use(in_between_).flush();
}

void InBetweenRenderer::forget_texture(const void *identity) {
    use(real_).forget_texture(identity);
    use(in_between_).forget_texture(identity);
}

void InBetweenRenderer::before_transfer(const pspweb::TransferRect &src, const pspweb::TransferRect &dst) {
    use(real_).before_transfer(src, dst);
    use(in_between_).before_transfer(src, dst);
}

void InBetweenRenderer::after_transfer(const pspweb::TransferRect &dst) {
    use(real_).after_transfer(dst);
    use(in_between_).after_transfer(dst);
}

void InBetweenRenderer::set_vram_write_hook(std::function<void(std::uint32_t, std::uint32_t)> hook) {
    real_.set_vram_write_hook(std::move(hook)); // only the real renderer writes guest VRAM
}

void InBetweenRenderer::list_started() {
    use(real_).list_started();
    use(in_between_).list_started();
}

void InBetweenRenderer::list_finished() {
    use(real_).list_finished();
    use(in_between_).list_finished();
}

} // namespace anypsp::gpu
