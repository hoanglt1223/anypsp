#include "kernel.hpp"

#include "ge.hpp"
#include "ge_worker.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// Fonts compiled in from runtime/host/fonts (tools/gen-fallback-fonts.py, Noto under the SIL OFL 1.1),
// served for any of the PSP's fonts that is missing from flash0/font.
#define ANYPSP_FALLBACK_FONTS(X)                                                                                \
    X(sans_regular) X(sans_bold) X(sans_italic) X(sans_bold_italic) X(serif_regular) X(serif_bold)               \
    X(serif_italic) X(serif_bold_italic) X(sans_regular_small) X(sans_bold_small) X(sans_italic_small)           \
    X(sans_bold_italic_small) X(serif_regular_small) X(serif_bold_small) X(serif_italic_small)                   \
    X(serif_bold_italic_small) X(sans_jp) X(sans_kr)

#define ANYPSP_DECLARE_FONT(name)                                                                               \
    extern const unsigned char anypsp_font_##name[];                                                            \
    extern const std::size_t anypsp_font_##name##_size;
ANYPSP_FALLBACK_FONTS(ANYPSP_DECLARE_FONT)
#undef ANYPSP_DECLARE_FONT

namespace pspweb {
namespace {

constexpr std::uint32_t kErrorInvalidLibrary = 0x80460002u;
constexpr std::uint32_t kErrorInvalidParameter = 0x80460003u;
constexpr std::size_t kHeaderSize = 392u;

struct Fallback {
    const unsigned char *data;
    std::size_t size;
};

#define ANYPSP_FONT(name) Fallback{anypsp_font_##name, anypsp_font_##name##_size}

struct Registry {
    const char *file;
    std::uint16_t family, style, language, country;
    Fallback fallback;
};

// The PSP's flash0:/font set. Family 1 = sans-serif, 2 = serif; style 1 = regular, 2 = italic, 5 = bold,
// 6 = bold italic, 103 = DB; language 1 = Japanese, 2 = Latin, 3 = Korean. ltn8-15 are the 7 pt sizes.
const Registry kRegistry[] = {
    {"jpn0.pgf", 1, 103, 1, 1, ANYPSP_FONT(sans_jp)},
    {"ltn0.pgf", 1, 1, 2, 1, ANYPSP_FONT(sans_regular)},
    {"ltn1.pgf", 2, 1, 2, 1, ANYPSP_FONT(serif_regular)},
    {"ltn2.pgf", 1, 2, 2, 1, ANYPSP_FONT(sans_italic)},
    {"ltn3.pgf", 2, 2, 2, 1, ANYPSP_FONT(serif_italic)},
    {"ltn4.pgf", 1, 5, 2, 1, ANYPSP_FONT(sans_bold)},
    {"ltn5.pgf", 2, 5, 2, 1, ANYPSP_FONT(serif_bold)},
    {"ltn6.pgf", 1, 6, 2, 1, ANYPSP_FONT(sans_bold_italic)},
    {"ltn7.pgf", 2, 6, 2, 1, ANYPSP_FONT(serif_bold_italic)},
    {"ltn8.pgf", 1, 1, 2, 1, ANYPSP_FONT(sans_regular_small)},
    {"ltn9.pgf", 2, 1, 2, 1, ANYPSP_FONT(serif_regular_small)},
    {"ltn10.pgf", 1, 2, 2, 1, ANYPSP_FONT(sans_italic_small)},
    {"ltn11.pgf", 2, 2, 2, 1, ANYPSP_FONT(serif_italic_small)},
    {"ltn12.pgf", 1, 5, 2, 1, ANYPSP_FONT(sans_bold_small)},
    {"ltn13.pgf", 2, 5, 2, 1, ANYPSP_FONT(serif_bold_small)},
    {"ltn14.pgf", 1, 6, 2, 1, ANYPSP_FONT(sans_bold_italic_small)},
    {"ltn15.pgf", 2, 6, 2, 1, ANYPSP_FONT(serif_bold_italic_small)},
    {"kr0.pgf", 1, 1, 3, 3, ANYPSP_FONT(sans_kr)},
};

#undef ANYPSP_FONT

struct Glyph {
    bool valid{};
    std::int32_t width{}, height{}, left{}, top{};
    std::uint32_t flags{}, shadow_flags{}, shadow_id{};
    std::array<std::int32_t, 8> metrics{};
    std::size_t data_bit{};
};

struct Font {
    Registry registry{};
    std::vector<std::uint8_t> data;
    std::uint32_t first{}, map_count{}, pointer_count{}, map_bits{}, pointer_bits{};
    std::array<std::vector<std::array<std::int32_t, 2>>, 4> tables;
    std::size_t map_offset{}, pointer_offset{}, glyph_offset{};
    std::map<std::uint32_t, Glyph> glyphs;

    [[nodiscard]] std::int32_t s32(std::size_t at) const {
        std::int32_t value;
        std::memcpy(&value, data.data() + at, 4u);
        return value;
    }
    [[nodiscard]] std::uint16_t u16(std::size_t at) const { return static_cast<std::uint16_t>(data[at] | (data[at + 1u] << 8u)); }
    [[nodiscard]] std::uint32_t bits(std::size_t base, std::size_t position, std::uint32_t count) const {
        const std::size_t first_byte = base + position / 8u;
        const std::uint32_t shift = static_cast<std::uint32_t>(position % 8u);
        std::uint64_t value = 0u;
        for (std::uint32_t i = 0; i < (shift + count + 7u) / 8u; ++i)
            if (first_byte + i < data.size()) value |= static_cast<std::uint64_t>(data[first_byte + i]) << (8u * i);
        return static_cast<std::uint32_t>((value >> shift) & ((1ull << count) - 1ull));
    }
};

struct Library {
    std::uint32_t alt_char{0x5Fu};
};

struct FontState {
    bool loaded{};
    std::vector<Font> fonts;
    std::map<std::uint32_t, Library> libraries;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::size_t>> open;
};

std::int32_t sign_extend(std::uint32_t value, std::uint32_t count) {
    return value & (1u << (count - 1u)) ? static_cast<std::int32_t>(value) - static_cast<std::int32_t>(1u << count)
                                        : static_cast<std::int32_t>(value);
}

std::size_t packed_bytes(std::int64_t count, std::int64_t bits) {
    return static_cast<std::size_t>((count * bits + 31) / 32 * 4);
}

bool parse(Font &font) {
    const auto &d = font.data;
    if (d.size() < kHeaderSize || std::memcmp(d.data() + 4, "PGF0", 4) != 0 || font.s32(8) != 2) return false;
    font.map_count = static_cast<std::uint32_t>(font.s32(16));
    font.pointer_count = static_cast<std::uint32_t>(font.s32(20));
    font.map_bits = static_cast<std::uint32_t>(font.s32(24));
    font.pointer_bits = static_cast<std::uint32_t>(font.s32(28));
    font.first = font.u16(182);
    if (font.map_bits == 0u || font.map_bits > 32u || font.pointer_bits == 0u || font.pointer_bits > 32u) return false;
    std::size_t cursor = kHeaderSize;
    for (std::size_t t = 0; t < 4u; ++t) {
        for (std::uint32_t i = 0; i < d[258u + t]; ++i, cursor += 8u) {
            if (cursor + 8u > d.size()) return false;
            font.tables[t].push_back({font.s32(cursor), font.s32(cursor + 4u)});
        }
    }
    cursor += packed_bytes(font.s32(364), font.s32(368));
    font.map_offset = cursor;
    cursor += packed_bytes(font.map_count, font.map_bits);
    font.pointer_offset = cursor;
    cursor += packed_bytes(font.pointer_count, font.pointer_bits);
    font.glyph_offset = cursor;
    return cursor <= d.size();
}

const Glyph *find_glyph(Font &font, std::uint32_t code) {
    if (code < font.first || code >= font.first + font.map_count) return nullptr;
    auto [it, inserted] = font.glyphs.try_emplace(code);
    Glyph &g = it->second;
    if (!inserted) return g.valid ? &g : nullptr;
    const std::uint32_t index = font.bits(font.map_offset, std::size_t{code - font.first} * font.map_bits, font.map_bits);
    if (index >= font.pointer_count) return nullptr;
    std::size_t position = std::size_t{font.bits(font.pointer_offset, std::size_t{index} * font.pointer_bits, font.pointer_bits)} * 32u + 14u;
    auto take = [&](std::uint32_t count) {
        const std::uint32_t value = font.bits(font.glyph_offset, position, count);
        position += count;
        return value;
    };
    g.width = static_cast<std::int32_t>(take(7));
    g.height = static_cast<std::int32_t>(take(7));
    g.left = sign_extend(take(7), 7);
    g.top = sign_extend(take(7), 7);
    g.flags = take(6);
    g.shadow_flags = take(2) << 5u;
    g.shadow_flags |= take(2) << 3u;
    g.shadow_flags |= take(3);
    g.shadow_id = take(9);
    for (std::size_t t = 0; t < 4u; ++t) {
        if (g.flags & (4u << t)) {
            const std::uint32_t entry = take(8);
            if (entry < font.tables[t].size()) {
                g.metrics[t * 2u] = font.tables[t][entry][0];
                g.metrics[t * 2u + 1u] = font.tables[t][entry][1];
            }
        } else {
            g.metrics[t * 2u] = static_cast<std::int32_t>(take(32));
            g.metrics[t * 2u + 1u] = static_cast<std::int32_t>(take(32));
        }
    }
    g.data_bit = position / 8u * 8u;
    g.valid = true;
    return &g;
}

std::vector<std::uint8_t> decode_pixels(const Font &font, const Glyph &g) {
    const std::size_t count = static_cast<std::size_t>(g.width) * static_cast<std::size_t>(g.height);
    std::vector<std::uint8_t> run;
    run.reserve(count);
    std::size_t position = g.data_bit;
    const std::size_t limit = (font.data.size() - font.glyph_offset) * 8u;
    auto take = [&] {
        const std::uint32_t value = font.bits(font.glyph_offset, position, 4);
        position += 4u;
        return static_cast<std::uint8_t>(value * 17u);
    };
    while (run.size() < count && position + 8u < limit) {
        const std::uint32_t control = font.bits(font.glyph_offset, position, 4);
        position += 4u;
        if (control < 8u) {
            const std::uint8_t value = take();
            run.insert(run.end(), std::min<std::size_t>(control + 1u, count - run.size()), value);
        } else {
            for (std::uint32_t i = 0; i < 16u - control && run.size() < count; ++i) run.push_back(take());
        }
    }
    run.resize(count, 0u);
    if ((g.flags & 3u) != 2u) return run;
    std::vector<std::uint8_t> rows(count);
    for (std::int32_t y = 0; y < g.height; ++y)
        for (std::int32_t x = 0; x < g.width; ++x) rows[y * g.width + x] = run[x * g.height + y];
    return rows;
}

void set_pixel(psprecomp::GuestMemory &m, std::uint32_t image_buffer, std::int32_t stride, std::int32_t width,
               std::int32_t height, std::int32_t x, std::int32_t y, std::uint8_t color, std::uint32_t format) {
    constexpr std::int32_t kBytes[] = {0, 0, 1, 3, 4};
    if (x < 0 || y < 0 || x >= width || y >= height || format > 4u) return;
    const std::int32_t bytes = kBytes[format];
    if (x >= (bytes == 0 ? stride * 2 : stride / bytes)) return;
    const std::uint32_t address = image_buffer + static_cast<std::uint32_t>(y * stride + (bytes == 0 ? x / 2 : x * bytes));
    switch (format) {
    case 0:
    case 1: {
        const std::uint8_t nibble = color >> 4u;
        const std::uint8_t old = m.load8(address);
        const bool high = static_cast<std::uint32_t>(x & 1) != format;
        m.store8(address, high ? static_cast<std::uint8_t>((nibble << 4u) | (old & 0x0Fu)) : static_cast<std::uint8_t>((old & 0xF0u) | nibble));
        break;
    }
    case 2: m.store8(address, color); break;
    case 3:
        for (std::uint32_t i = 0; i < 3u; ++i) m.store8(address + i, color);
        break;
    default: m.store32(address, color * 0x01010101u); break;
    }
}

void write_style(psprecomp::GuestMemory &m, std::uint32_t at, const Font &font) {
    const std::array<float, 5> sizes{font.s32(36) / 64.0f, font.s32(40) / 64.0f, font.s32(44) / 64.0f, font.s32(48) / 64.0f, 0.0f};
    for (std::uint32_t i = 0; i < 5u; ++i) m.store32(at + i * 4u, std::bit_cast<std::uint32_t>(sizes[i]));
    const std::array<std::uint16_t, 6> fields{font.registry.family, font.registry.style, 0u, font.registry.language, 0u, font.registry.country};
    for (std::uint32_t i = 0; i < 6u; ++i) m.store16(at + 20u + i * 2u, fields[i]);
    for (std::uint32_t i = 0; i < 64u; ++i) m.store8(at + 32u + i, font.data[53u + i]);
    const std::size_t name_length = std::strlen(font.registry.file);
    for (std::uint32_t i = 0; i < 64u; ++i)
        m.store8(at + 96u + i, i < name_length ? static_cast<std::uint8_t>(font.registry.file[i]) : 0u);
    m.store32(at + 160u, 0u);
    m.store32(at + 164u, 0u);
}

void write_info(psprecomp::GuestMemory &m, std::uint32_t at, const Font &font) {
    constexpr std::array<std::size_t, 10> kMetrics{244, 248, 212, 216, 220, 224, 228, 232, 236, 240};
    for (std::uint32_t i = 0; i < 10u; ++i) {
        const std::int32_t value = font.s32(kMetrics[i]);
        m.store32(at + i * 4u, static_cast<std::uint32_t>(value));
        m.store32(at + 40u + i * 4u, std::bit_cast<std::uint32_t>(value / 64.0f));
    }
    m.store16(at + 80u, font.u16(252));
    m.store16(at + 82u, font.u16(254));
    m.store32(at + 84u, font.pointer_count);
    m.store32(at + 88u, static_cast<std::uint32_t>(font.s32(364)));
    write_style(m, at + 92u, font);
    m.store32(at + 260u, font.data[34]);
}

void write_char_info(psprecomp::GuestMemory &m, std::uint32_t at, const Glyph *g) {
    const Glyph empty{};
    const Glyph &glyph = g != nullptr ? *g : empty;
    const auto &metrics = glyph.metrics;
    const std::array<std::int32_t, 14> values{glyph.width, glyph.height, glyph.left, glyph.top,
                                              metrics[0], metrics[1], metrics[4], metrics[4] - metrics[1],
                                              metrics[2], metrics[4], metrics[3], metrics[5], metrics[6], metrics[7]};
    for (std::uint32_t i = 0; i < 14u; ++i) m.store32(at + i * 4u, static_cast<std::uint32_t>(values[i]));
    m.store16(at + 56u, static_cast<std::uint16_t>(glyph.shadow_flags));
    m.store16(at + 58u, static_cast<std::uint16_t>(glyph.shadow_id));
}

std::size_t optimum_font(const FontState &state, psprecomp::GuestMemory &m, std::uint32_t style) {
    const float wanted_h = std::bit_cast<float>(m.load32(style));
    const float wanted_res = std::bit_cast<float>(m.load32(style + 8u));
    const std::array<std::uint16_t, 3> fields{m.load16(style + 20u), m.load16(style + 22u), m.load16(style + 26u)};
    std::size_t best = 0u;
    float best_distance = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < state.fonts.size(); ++i) {
        const Font &font = state.fonts[i];
        const std::array<std::uint16_t, 3> have{font.registry.family, font.registry.style, font.registry.language};
        bool matches = true;
        for (std::size_t f = 0; f < fields.size(); ++f) matches = matches && (fields[f] == 0u || fields[f] == have[f]);
        if (!matches) continue;
        const float resolution = wanted_res > 0.0f ? wanted_res : 128.0f;
        const float distance = wanted_h > 0.0f ? std::fabs(font.s32(36) / 64.0f * font.s32(44) / 64.0f - wanted_h * resolution) : 0.0f;
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

} // namespace

void Kernel::install_fonts() {
    auto &m = rt_.memory();
    auto state = std::make_shared<FontState>();
    fonts_ = state;
    auto add = [this](std::uint32_t nid, const char *name, std::function<std::uint32_t(Ctx &)> body) {
        static const bool trace = std::getenv("PSPWEB_TRACE_HLE") != nullptr;
        rt_.register_hle("sceLibFont", nid, [body, name](psprecomp::Runtime &, Ctx &ctx) {
            const std::uint32_t a0 = ctx.gpr[4], a1 = ctx.gpr[5], a2 = ctx.gpr[6];
            const std::uint32_t result = body(ctx);
            if (trace) std::fprintf(stderr, "[font] %s(0x%X, 0x%X, 0x%X) -> 0x%X\n", name, a0, a1, a2, result);
            finish(ctx, result);
        });
        rt_.nids().add("sceLibFont", nid, name);
        char key[48];
        std::snprintf(key, sizeof key, "sceLibFont:0x%08X", nid);
        registered_.insert(key);
    };
    auto load = [this, state] {
        if (state->loaded) return;
        state->loaded = true;
        std::size_t bundled = 0u;
        for (const Registry &entry : kRegistry) {
            Font font;
            font.registry = entry;
            if (std::ifstream file(flash_root_ / "font" / entry.file, std::ios::binary); file) {
                font.data.assign(std::istreambuf_iterator<char>(file), {});
                if (parse(font)) {
                    state->fonts.push_back(std::move(font));
                    continue;
                }
                std::fprintf(stderr, "[font] cannot read %s, using the bundled font\n", entry.file);
                font = Font{};
                font.registry = entry;
            }
            font.data.assign(entry.fallback.data, entry.fallback.data + entry.fallback.size);
            if (!parse(font)) throw std::runtime_error(std::string("[font] bundled replacement for ") + entry.file + " is corrupt");
            state->fonts.push_back(std::move(font));
            ++bundled;
        }
        if (bundled != 0u)
            std::fprintf(stderr, "[font] %zu of %zu system fonts not usable in %s: using AnyPSP's bundled Noto replacements\n", bundled,
                         std::size(kRegistry), (flash_root_ / "font").string().c_str());
    };
    auto opened = [state](std::uint32_t handle) -> Font * {
        const auto it = state->open.find(handle);
        return it == state->open.end() ? nullptr : &state->fonts[it->second.second];
    };
    auto put = [&m](std::uint32_t address, std::uint32_t value) {
        if (address != 0u) m.store32(address, value);
    };
    auto draw = [this, state, opened, &m](Ctx &ctx, std::int32_t clip_x, std::int32_t clip_y, std::int32_t clip_w, std::int32_t clip_h) {
        Font *font = opened(ctx.gpr[4]);
        const std::uint32_t image = ctx.gpr[6];
        if (font == nullptr || image == 0u) return kErrorInvalidParameter;
        const Glyph *glyph = find_glyph(*font, ctx.gpr[5] & 0xFFFFu);
        if (glyph == nullptr) glyph = find_glyph(*font, state->libraries[state->open[ctx.gpr[4]].first].alt_char);
        if (glyph == nullptr) return 0u;
        const std::uint32_t format = m.load32(image);
        const auto x64 = static_cast<std::int32_t>(m.load32(image + 4u));
        const auto y64 = static_cast<std::int32_t>(m.load32(image + 8u));
        const std::int32_t width = m.load16(image + 12u), height = m.load16(image + 14u), stride = m.load16(image + 16u);
        const std::uint32_t buffer = m.load32(image + 20u);
        const std::int32_t x = x64 >> 6, y = y64 >> 6, x_fraction = x64 & 63, y_fraction = y64 & 63;
        const std::vector<std::uint8_t> pixels = decode_pixels(*font, *glyph);
        auto sample = [&](std::int32_t px, std::int32_t py) -> std::int32_t {
            return px < 0 || py < 0 || px >= glyph->width || py >= glyph->height ? 0 : pixels[py * glyph->width + px];
        };
        clip_x = std::max(clip_x, 0);
        clip_y = std::max(clip_y, 0);
        if (clip_w < 0) clip_w = 8192;
        if (clip_h < 0) clip_h = 8192;
        const std::int32_t x1 = std::max(clip_x, x) - x, y1 = std::max(clip_y, y) - y;
        const std::int32_t x2 = std::min(clip_x + clip_w - x, glyph->width + (x_fraction > 0 ? 1 : 0));
        const std::int32_t y2 = std::min(clip_y + clip_h - y, glyph->height + (y_fraction > 0 ? 1 : 0));
        for (std::int32_t py = y1; py < y2; ++py) {
            for (std::int32_t px = x1; px < x2; ++px) {
                std::int32_t color = sample(px, py);
                if (x_fraction != 0 || y_fraction != 0) {
                    const std::int32_t above = sample(px - 1, py - 1) * x_fraction + sample(px, py - 1) * (64 - x_fraction);
                    const std::int32_t row = sample(px - 1, py) * x_fraction + sample(px, py) * (64 - x_fraction);
                    color = (above * y_fraction + row * (64 - y_fraction) + 0x800) >> 12;
                }
                set_pixel(m, buffer, stride, width, height, x + px, y + py, static_cast<std::uint8_t>(std::min(color, 255)), format);
            }
        }
        const std::uint32_t bytes = static_cast<std::uint32_t>(stride * height);
        ge_worker_->post([ge = ge_.get(), buffer, bytes] { ge->cpu_wrote(buffer, bytes); });
        return 0u;
    };

    add(0x67F17ED7u, "sceFontNewLib", [this, state, load, put](Ctx &ctx) {
        load();
        const auto id = static_cast<std::uint32_t>(new_uid());
        state->libraries[id] = Library{};
        put(ctx.gpr[5], 0u);
        return id;
    });
    add(0x574B6FBCu, "sceFontDoneLib", [state](Ctx &ctx) {
        state->libraries.erase(ctx.gpr[4]);
        std::erase_if(state->open, [&](const auto &entry) { return entry.second.first == ctx.gpr[4]; });
        return 0u;
    });
    add(0x27F6E642u, "sceFontGetNumFontList", [state, put](Ctx &ctx) {
        const bool valid = state->libraries.contains(ctx.gpr[4]);
        put(ctx.gpr[5], valid ? 0u : kErrorInvalidLibrary);
        return valid ? static_cast<std::uint32_t>(state->fonts.size()) : 0u;
    });
    add(0xBC75D85Bu, "sceFontGetFontList", [state, &m](Ctx &ctx) {
        if (!state->libraries.contains(ctx.gpr[4])) return kErrorInvalidLibrary;
        const std::size_t count = std::min<std::size_t>(ctx.gpr[6], state->fonts.size());
        for (std::size_t i = 0; i < count; ++i) write_style(m, ctx.gpr[5] + static_cast<std::uint32_t>(i * 168u), state->fonts[i]);
        return 0u;
    });
    add(0x099EF33Cu, "sceFontFindOptimumFont", [state, put, &m](Ctx &ctx) {
        const bool valid = state->libraries.contains(ctx.gpr[4]) && ctx.gpr[5] != 0u && !state->fonts.empty();
        put(ctx.gpr[6], valid ? 0u : kErrorInvalidParameter);
        return valid ? static_cast<std::uint32_t>(optimum_font(*state, m, ctx.gpr[5])) : 0u;
    });
    add(0xA834319Du, "sceFontOpen", [this, state, put](Ctx &ctx) {
        const bool valid = state->libraries.contains(ctx.gpr[4]) && ctx.gpr[5] < state->fonts.size();
        put(ctx.gpr[7], valid ? 0u : kErrorInvalidParameter);
        if (!valid) return 0u;
        const auto id = static_cast<std::uint32_t>(new_uid());
        state->open[id] = {ctx.gpr[4], ctx.gpr[5]};
        return id;
    });
    add(0x3AEA8CB6u, "sceFontClose", [state](Ctx &ctx) {
        state->open.erase(ctx.gpr[4]);
        return 0u;
    });
    add(0x0DA7535Eu, "sceFontGetFontInfo", [opened, &m](Ctx &ctx) {
        const Font *font = opened(ctx.gpr[4]);
        if (font == nullptr || ctx.gpr[5] == 0u) return kErrorInvalidParameter;
        write_info(m, ctx.gpr[5], *font);
        return 0u;
    });
    add(0xDCC80C2Fu, "sceFontGetCharInfo", [state, opened, &m](Ctx &ctx) {
        Font *font = opened(ctx.gpr[4]);
        if (font == nullptr || ctx.gpr[6] == 0u) return kErrorInvalidParameter;
        const Glyph *glyph = find_glyph(*font, ctx.gpr[5] & 0xFFFFu);
        if (glyph == nullptr) glyph = find_glyph(*font, state->libraries[state->open[ctx.gpr[4]].first].alt_char);
        write_char_info(m, ctx.gpr[6], glyph);
        return 0u;
    });
    add(0x980F4895u, "sceFontGetCharGlyphImage", [draw](Ctx &ctx) { return draw(ctx, 0, 0, -1, -1); });
    add(0xCA1E6945u, "sceFontGetCharGlyphImage_Clip", [draw](Ctx &ctx) {
        return draw(ctx, static_cast<std::int32_t>(ctx.gpr[7]), static_cast<std::int32_t>(ctx.gpr[8]),
                    static_cast<std::int32_t>(ctx.gpr[9]), static_cast<std::int32_t>(ctx.gpr[10]));
    });
    add(0xEE232411u, "sceFontSetAltCharacterCode", [state](Ctx &ctx) {
        const auto it = state->libraries.find(ctx.gpr[4]);
        if (it == state->libraries.end()) return kErrorInvalidLibrary;
        it->second.alt_char = ctx.gpr[5] & 0xFFFFu;
        return 0u;
    });
}

} // namespace pspweb
