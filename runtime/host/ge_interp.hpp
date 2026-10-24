// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Frame interpolation for games that draw at 30 fps: every primitive is drawn twice, once as
// the game sent it and once into a second ("in-between") renderer with its vertices moved half
// way back towards the same primitive in the previous game frame. The game itself, its timers
// and its picture stay exactly as they are; only the extra picture shown on the vblank between
// two game frames is new (docs/user/desktop.md#frame-interpolation).

#include "ge_renderer.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace pspweb {

// What identifies a primitive from one frame to the next.
struct PrimKey {
    std::uint32_t vertex_address{}, index_address{}, vertex_type{}, count{}, type{}, decoded{};

    bool operator==(const PrimKey &) const = default;
    // Same layout and vertex count, wherever the vertices are.
    [[nodiscard]] bool same_shape(const PrimKey &o) const {
        return vertex_type == o.vertex_type && count == o.count && type == o.type && decoded == o.decoded;
    }
};

// Pairs each primitive of the frame being drawn with one of the previous frame. Static models
// keep their vertex address, so a primitive drawn from the same address (and with the same
// shape) is the same object; the n-th draw from an address pairs with the n-th one before.
// Vertices the game builds each frame move around in memory, so a primitive with no such
// partner takes the next unused one of the same shape in drawing order, searching a few
// primitives past the last pairing.
class FrameMatcher {
public:
    static constexpr std::size_t kSearch = 24;

    // The current frame is complete: it becomes the previous one.
    void next_frame();
    // Records `xyzw` (4 floats per decoded vertex) for the current frame and returns the
    // partner's positions in the previous frame (the same number of floats), or nullptr.
    const float *match(const PrimKey &key, std::span<const float> xyzw);

    struct Totals {
        std::uint64_t frames{}, prims{}, matched{};
    };
    [[nodiscard]] const Totals &totals() const noexcept { return totals_; }

private:
    struct Record {
        PrimKey key;
        std::size_t at{}; // first float in the pool
        bool used{};
    };
    struct KeyHash {
        std::size_t operator()(const PrimKey &k) const noexcept {
            std::uint64_t h = (static_cast<std::uint64_t>(k.vertex_address) << 32u | k.index_address) * 0x9E3779B97F4A7C15ull;
            h ^= (static_cast<std::uint64_t>(k.vertex_type) << 8u ^ k.count ^ static_cast<std::uint64_t>(k.type) << 40u) *
                 0xC2B2AE3D27D4EB4Full;
            return static_cast<std::size_t>(h ^ (h >> 31u));
        }
    };
    struct Frame {
        std::vector<Record> records;
        std::vector<float> pool;
        std::unordered_map<PrimKey, std::vector<std::size_t>, KeyHash> by_key; // record indices in drawing order
        void clear() {
            records.clear();
            pool.clear();
            by_key.clear();
        }
    };

    Frame previous_, current_;
    std::unordered_map<PrimKey, std::size_t, KeyHash> seen_; // draws per key so far this frame
    std::size_t cursor_{};
    Totals totals_{};
};

// A renderer that feeds two GL renderers: the real one and the in-between one. By default each
// call goes to both; while the GE draws a moved primitive it sends the real vertices to one and
// the moved ones to the other.
class InterpolatingRenderer : public GeRenderer {
public:
    enum class Target { Both, Real, InBetween };
    virtual void set_target(Target target) = 0;
};

} // namespace pspweb
