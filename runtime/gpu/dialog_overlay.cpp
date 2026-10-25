// SPDX-License-Identifier: GPL-2.0-or-later

#include "dialog_overlay.hpp"

#include "kernel.hpp"
#include "menu.hpp"
#include "menu_host.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace anypsp::dialog {
namespace {

using anyui::Color;
using anyui::Rect;
using Text = pspweb::SystemDialog::Text;

float ease(float x) {
    x = std::clamp(x, 0.f, 1.f);
    return 1 - (1 - x) * (1 - x) * (1 - x);
}

std::string hex_code(std::uint32_t code) {
    char text[12];
    std::snprintf(text, sizeof text, "%08X", code);
    return text;
}

// One line of text that may hold line breaks.
std::string one_line(std::string s) {
    std::replace(s.begin(), s.end(), '\n', ' ');
    std::replace(s.begin(), s.end(), '\r', ' ');
    return s;
}

const char *message_key(Text id) {
    switch (id) {
    case Text::NoData: return "dialog.nodata";
    case Text::ConfirmLoad: return "dialog.confirm.load";
    case Text::ConfirmSave: return "dialog.confirm.save";
    case Text::ConfirmDelete: return "dialog.confirm.delete";
    case Text::ConfirmOverwrite: return "dialog.confirm.overwrite";
    case Text::LoadDone: return "dialog.done.load";
    case Text::SaveDone: return "dialog.done.save";
    case Text::DeleteDone: return "dialog.done.delete";
    case Text::LoadFailed: return "dialog.failed.load";
    case Text::SaveFailed: return "dialog.failed.save";
    case Text::DeleteFailed: return "dialog.failed.delete";
    case Text::Error: return "dialog.error";
    case Text::Custom: break;
    }
    return "";
}

// The PSP's Cross (blue) or Circle (red) on a round chip, centred at (x, cy).
void glyph(anyui::Canvas &out, bool cross, float x, float cy, float size, const anyui::Theme &t, float a) {
    const float r = size / 2;
    out.fill_circle(x, cy, r, (t.dark ? Color::rgb(0x2A303B) : Color::rgb(0xE3E7ED)).alpha(a));
    const float w = std::max(1.f, size * 0.12f);
    if (cross) {
        const float k = r * 0.42f;
        const Color c = Color::rgb(t.dark ? 0x8DB4FF : 0x2F6FEB, a);
        out.line(x - k, cy - k, x + k, cy + k, w, c);
        out.line(x - k, cy + k, x + k, cy - k, w, c);
    } else {
        out.stroke_circle(x, cy, r * 0.5f, w, Color::rgb(t.dark ? 0xFF8A8A : 0xD93636, a));
    }
}

} // namespace

std::string format_time(std::int64_t unix_seconds) {
    if (unix_seconds <= 0) return {};
    const auto when = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &when);
#else
    localtime_r(&when, &tm);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &tm);
    return text;
}

Frame make_frame(const pspweb::SystemDialog &d, const anyui::Strings &strings,
                 const std::function<anyui::Thumb(const std::filesystem::path &)> &icon) {
    using Heading = pspweb::SystemDialog::Heading;
    Frame f;
    f.cross_confirms = d.cross_confirms;
    switch (d.heading) {
    case Heading::Load:
        f.icon = anyui::Icon::Load;
        f.heading = strings.tr("dialog.load");
        break;
    case Heading::Save:
        f.icon = anyui::Icon::Save;
        f.heading = strings.tr("dialog.save");
        break;
    case Heading::Delete:
        f.icon = anyui::Icon::Delete;
        f.heading = strings.tr("dialog.delete");
        break;
    case Heading::None: break;
    }
    for (const pspweb::SystemDialog::Item &item : d.items) {
        Row r;
        r.new_data = !item.exists;
        if (item.exists) {
            r.title = item.title.empty() ? item.name : item.title;
            r.subtitle = item.savedata_title;
            r.detail = item.detail;
            r.date = format_time(item.time);
            if (!item.icon.empty() && icon) r.thumb = icon(item.icon);
        } else {
            r.title = strings.tr("dialog.newdata");
        }
        f.rows.push_back(std::move(r));
    }
    f.focus = d.focus;
    f.back_hint = strings.tr("dialog.back");
    if (d.kind == pspweb::SystemDialog::Kind::Keyboard) {
        f.message = d.text;
        f.input = d.input;
        f.key_columns = pspweb::kOskColumns;
        for (int i = 0; i < pspweb::kOskKeyCount; ++i) {
            if (i == pspweb::kOskSpace) f.keys.push_back(strings.tr("dialog.key.space"));
            else if (i == pspweb::kOskDelete) f.keys.push_back(strings.tr("dialog.key.delete"));
            else if (i == pspweb::kOskDone) f.keys.push_back(strings.tr("dialog.key.done"));
            else f.keys.emplace_back(pspweb::kOskKeys[i]);
        }
        f.confirm_hint = strings.tr("dialog.type");
        f.back_hint = strings.tr("dialog.cancel");
        return f;
    }
    if (d.kind == pspweb::SystemDialog::Kind::SaveList) {
        f.list = true;
        f.confirm_hint = strings.tr("dialog.choose");
        return f;
    }
    f.confirm_hint = strings.tr("dialog.ok");
    switch (d.text_id) {
    case Text::Custom: f.message = d.text; break;
    case Text::Error:
    case Text::LoadFailed:
    case Text::SaveFailed:
    case Text::DeleteFailed: f.message = strings.fmt(message_key(d.text_id), {hex_code(d.error)}); break;
    default: f.message = strings.tr(message_key(d.text_id)); break;
    }
    if (d.buttons == pspweb::SystemDialog::Buttons::YesNo) f.buttons = {strings.tr("dialog.yes"), strings.tr("dialog.no")};
    else f.buttons = {strings.tr("dialog.ok")};
    return f;
}

std::vector<std::string> wrap(const anyui::Font &font, const std::string &text, float px, float width) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string paragraph = text.substr(start, end - start);
        if (!paragraph.empty() && paragraph.back() == '\r') paragraph.pop_back();
        std::string line;
        std::size_t at = 0;
        while (at <= paragraph.size()) {
            std::size_t space = paragraph.find(' ', at);
            if (space == std::string::npos) space = paragraph.size();
            const std::string word = paragraph.substr(at, space - at);
            const std::string joined = line.empty() ? word : line + " " + word;
            if (line.empty() || font.measure(joined, px) <= width) {
                line = joined;
            } else {
                lines.push_back(font.fit(line, px, width));
                line = word;
            }
            at = space + 1;
        }
        lines.push_back(font.fit(line, px, width));
        start = end + 1;
    }
    while (lines.size() > 1 && lines.back().empty()) lines.pop_back();
    return lines;
}

void View::render(anyui::Canvas &out, const Frame &f, const anyui::ViewOptions &o, float dt) {
    const float W = static_cast<float>(out.width()), H = static_cast<float>(out.height());
    hits_.clear();
    if (W <= 0 || H <= 0) return;
    const anyui::Theme t = anyui::Theme::make(o.dark);
    open_ = o.reduce_motion ? 1.f : std::min(1.f, open_ + dt / 0.16f);
    const float a = ease(open_);
    out.fill_rect({0, 0, W, H}, (o.dark ? Color::rgb(0x05070B, 0.55f) : Color::rgb(0x1A2030, 0.38f)).alpha(a));

    u_ = std::max(0.45f, std::min(W / 960.f, H / 540.f) * o.ui_scale);
    const float u = u_;
    const float margin = 16 * u, pad = 30 * u;
    const float mpx = 17 * u, line_h = regular_.line_height(mpx) * 1.12f;
    const float row_h = 78 * u, head_h = f.heading.empty() ? 0.f : 44 * u, foot_h = 40 * u, button_h = 42 * u;

    float card_w = 0, card_h = 0;
    std::vector<std::string> lines;
    if (f.list) {
        card_w = std::min(W - 2 * margin, 720 * u);
        const float rows_h = static_cast<float>(std::max<std::size_t>(f.rows.size(), 1)) * row_h;
        card_h = std::min(H - 2 * margin, 22 * u + head_h + rows_h + 8 * u + foot_h + 10 * u);
    } else if (!f.keys.empty()) {
        card_w = std::min(W - 2 * margin, 640 * u);
        lines = wrap(regular_, f.message, mpx, card_w - 2 * pad);
        const float key_rows = static_cast<float>((f.keys.size() + static_cast<std::size_t>(f.key_columns) - 1u) /
                                                  static_cast<std::size_t>(f.key_columns));
        card_h = 24 * u + static_cast<float>(lines.size()) * line_h + 14 * u + 40 * u + 14 * u + key_rows * 34 * u + foot_h + 10 * u;
        card_h = std::min(card_h, H - 2 * margin);
    } else {
        card_w = std::min(W - 2 * margin, 560 * u);
        lines = wrap(regular_, f.message, mpx, card_w - 2 * pad);
        card_h = 24 * u + head_h + (f.rows.empty() ? 0.f : row_h + 14 * u) + static_cast<float>(lines.size()) * line_h + 22 * u +
                 (f.buttons.empty() ? 0.f : button_h + 16 * u) + foot_h + 6 * u;
        card_h = std::min(card_h, H - 2 * margin);
    }
    const float rise = (1 - a) * 12 * u;
    const Rect card{(W - card_w) / 2, (H - card_h) / 2 + rise, card_w, card_h};
    const float radius = 18 * u;
    out.fill_round_rect({card.x - 4 * u, card.y + 2 * u, card.w + 8 * u, card.h + 12 * u}, radius + 8 * u, t.shadow.alpha(a), 28 * u);
    out.fill_round_rect(card, radius, (o.dark ? Color::rgb(0x1B1F27) : Color::rgb(0xFFFFFF)).alpha(a));
    out.push_clip(card);

    float y = card.y + 22 * u;
    if (!f.heading.empty()) {
        const float hpx = 20 * u;
        float x = card.x + pad;
        if (f.icon != anyui::Icon::None) {
            anyui::draw_icon(out, f.icon, x + 11 * u, y + 14 * u, 22 * u, t.accent.alpha(a));
            x += 32 * u;
        }
        bold_.draw(out, x, center_baseline(bold_, y + 14 * u, hpx), bold_.fit(f.heading, hpx, card.right() - pad - x), hpx, t.text.alpha(a));
        y += head_h;
        out.fill_rect({card.x + pad, y - 10 * u, card.w - 2 * pad, std::max(1.f, u)}, t.line.alpha(a));
    }

    if (!f.keys.empty()) {
        for (const std::string &line : lines) {
            const float lw = regular_.measure(line, mpx);
            regular_.draw(out, card.x + (card.w - lw) / 2, y + regular_.ascent(mpx), line, mpx, t.text.alpha(a));
            y += line_h;
        }
        y += 14 * u;
        // The text field, with a caret after the text.
        const Rect field{card.x + pad, y, card.w - 2 * pad, 40 * u};
        out.fill_round_rect(field, 8 * u, t.field.alpha(a));
        out.stroke_round_rect(field, 8 * u, std::max(1.f, u), t.control.alpha(a));
        const float ipx = 17 * u;
        const std::string shown = regular_.fit(f.input + "|", ipx, field.w - 24 * u);
        regular_.draw(out, field.x + 12 * u, center_baseline(regular_, field.y + field.h / 2, ipx), shown, ipx, t.text.alpha(a));
        y += field.h + 14 * u;
        // The keys: a grid of key_columns, the last row spread over the whole width.
        const int columns = std::max(1, f.key_columns);
        const float gap = 4 * u, key_h = 30 * u, kpx = 14 * u;
        const float key_w = (card.w - 2 * pad - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
        const int count = static_cast<int>(f.keys.size());
        const int full_rows = count / columns;
        for (int i = 0; i < count; ++i) {
            const int row = i / columns, column = i % columns;
            Rect k{card.x + pad + static_cast<float>(column) * (key_w + gap), y + static_cast<float>(row) * (key_h + gap), key_w, key_h};
            if (row == full_rows) {
                // The action row: its keys share the width.
                const int in_row = count - full_rows * columns;
                const float w = (card.w - 2 * pad - gap * static_cast<float>(in_row - 1)) / static_cast<float>(in_row);
                k.x = card.x + pad + static_cast<float>(column) * (w + gap);
                k.w = w;
            }
            const bool focused = i == f.focus;
            out.fill_round_rect(k, 6 * u, (focused ? t.accent : t.field).alpha(a));
            if (!focused) out.stroke_round_rect(k, 6 * u, std::max(1.f, u), t.control.alpha(a));
            const std::string label = bold_.fit(f.keys[static_cast<std::size_t>(i)], kpx, k.w - 6 * u);
            bold_.draw(out, k.x + (k.w - bold_.measure(label, kpx)) / 2, center_baseline(bold_, k.y + k.h / 2, kpx), label, kpx,
                       (focused ? t.on_accent : t.text).alpha(a));
            hits_.push_back({k, {Hit::Kind::Button, i}});
        }
    } else if (f.list) {
        const Rect area{card.x + 14 * u, y, card.w - 28 * u, card.bottom() - foot_h - 6 * u - y};
        const float total = static_cast<float>(f.rows.size()) * row_h;
        const int focus = std::clamp(f.focus, 0, std::max(0, static_cast<int>(f.rows.size()) - 1));
        const float top = static_cast<float>(focus) * row_h;
        if (top < scroll_) scroll_ = top;
        if (top + row_h > scroll_ + area.h) scroll_ = top + row_h - area.h;
        scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, total - area.h));
        out.push_clip(area);
        for (std::size_t i = 0; i < f.rows.size(); ++i) {
            const Rect rr{area.x, area.y + static_cast<float>(i) * row_h - scroll_, area.w, row_h};
            if (rr.bottom() < area.y || rr.y > area.bottom()) continue;
            const bool selected = static_cast<int>(i) == focus;
            if (selected) {
                out.fill_round_rect(rr, 10 * u, t.select.alpha(a));
                out.fill_round_rect({rr.x + 2 * u, rr.y + rr.h * 0.25f, 3 * u, rr.h * 0.5f}, 1.5f * u, t.accent.alpha(a));
            }
            const float y0 = std::max(rr.y, area.y), y1 = std::min(rr.bottom(), area.bottom());
            hits_.push_back({{rr.x, y0, rr.w, y1 - y0}, {Hit::Kind::Row, static_cast<int>(i)}});
            draw_row(out, t, f.rows[i], rr, a, selected);
        }
        out.pop_clip();
        if (total > area.h) {
            const float bar_h = std::max(24 * u, area.h * area.h / total);
            const float bar_y = area.y + (area.h - bar_h) * (scroll_ / std::max(1.f, total - area.h));
            out.fill_round_rect({area.right() + 4 * u, bar_y, 3 * u, bar_h}, 1.5f * u, t.dim.alpha(0.5f * a));
        }
    } else {
        if (!f.rows.empty()) {
            const Rect rr{card.x + 14 * u, y, card.w - 28 * u, row_h};
            out.fill_round_rect(rr, 10 * u, t.hover.alpha(a));
            draw_row(out, t, f.rows.front(), rr, a, false);
            y += row_h + 14 * u;
        }
        y += 6 * u;
        for (const std::string &line : lines) {
            const float lw = regular_.measure(line, mpx);
            regular_.draw(out, card.x + (card.w - lw) / 2, y + regular_.ascent(mpx), line, mpx, t.text.alpha(a));
            y += line_h;
        }
        y += 16 * u;
        if (!f.buttons.empty()) {
            const float bpx = 15 * u, gap = 14 * u;
            std::vector<float> widths;
            float row_w = 0;
            for (const std::string &b : f.buttons) {
                widths.push_back(std::max(120 * u, bold_.measure(b, bpx) + 44 * u));
                row_w += widths.back();
            }
            row_w += gap * static_cast<float>(f.buttons.size() - 1);
            float x = card.x + (card.w - row_w) / 2;
            for (std::size_t i = 0; i < f.buttons.size(); ++i) {
                const Rect b{x, y, widths[i], button_h};
                const bool focused = static_cast<int>(i) == f.focus;
                if (focused) {
                    out.fill_round_rect(b, button_h / 2, t.accent.alpha(a));
                } else {
                    out.fill_round_rect(b, button_h / 2, t.field.alpha(a));
                    out.stroke_round_rect(b, button_h / 2, std::max(1.f, u), t.control.alpha(a));
                }
                const std::string label = bold_.fit(f.buttons[i], bpx, b.w - 16 * u);
                bold_.draw(out, b.x + (b.w - bold_.measure(label, bpx)) / 2, center_baseline(bold_, b.y + b.h / 2, bpx), label, bpx,
                           (focused ? t.on_accent : t.text).alpha(a));
                hits_.push_back({b, {Hit::Kind::Button, static_cast<int>(i)}});
                x += widths[i] + gap;
            }
        }
    }

    // Footer: what the PSP's confirm and back buttons do, at the bottom right as on the PSP.
    const float hpx = 13 * u, gs = 18 * u;
    const float fy = card.bottom() - foot_h / 2 - 4 * u;
    float x = card.right() - pad;
    const auto hint = [&](bool cross, const std::string &text) {
        const float w = regular_.measure(text, hpx);
        x -= w;
        regular_.draw(out, x, center_baseline(regular_, fy, hpx), text, hpx, t.dim.alpha(a));
        x -= 7 * u + gs / 2;
        glyph(out, cross, x, fy, gs, t, a);
        x -= gs / 2 + 18 * u;
    };
    hint(!f.cross_confirms, f.back_hint);
    hint(f.cross_confirms, f.confirm_hint);
    out.pop_clip();
}

void View::draw_row(anyui::Canvas &out, const anyui::Theme &t, const Row &r, Rect rr, float a, bool selected) const {
    const float u = u_;
    const float th = rr.h - 14 * u, tw = th * 144.f / 80.f;
    const Rect thumb{rr.x + 14 * u, rr.y + 7 * u, tw, th};
    if (!r.thumb.empty()) {
        out.image(r.thumb.pixels, r.thumb.width, r.thumb.height, thumb, 6 * u, a);
    } else {
        out.fill_round_rect(thumb, 6 * u, t.field.alpha(a));
        out.stroke_round_rect(thumb, 6 * u, std::max(1.f, u), t.line.alpha(a * 2));
        anyui::draw_icon(out, r.new_data ? anyui::Icon::Plus : anyui::Icon::Card, thumb.x + tw / 2, thumb.y + th / 2, 22 * u,
                         (selected ? t.accent : t.dim).alpha(a));
    }
    const float x = thumb.right() + 16 * u, right = rr.right() - 14 * u;
    const float tpx = 15 * u, spx = 13 * u, dpx = 12 * u;
    float date_w = 0;
    if (!r.date.empty()) {
        date_w = regular_.measure(r.date, dpx);
        regular_.draw(out, right - date_w, rr.y + 12 * u + regular_.ascent(dpx), r.date, dpx, t.dim.alpha(a));
        date_w += 12 * u;
    }
    if (r.subtitle.empty() && r.detail.empty()) {
        bold_.draw(out, x, center_baseline(bold_, rr.y + rr.h / 2, tpx), bold_.fit(one_line(r.title), tpx, right - date_w - x), tpx,
                   t.text.alpha(a));
        return;
    }
    float y = rr.y + 10 * u + bold_.ascent(tpx);
    bold_.draw(out, x, y, bold_.fit(one_line(r.title), tpx, right - date_w - x), tpx, t.text.alpha(a));
    y += bold_.line_height(tpx) * 0.95f;
    if (!r.subtitle.empty()) {
        regular_.draw(out, x, y, regular_.fit(one_line(r.subtitle), spx, right - x), spx, t.text.alpha(a * 0.9f));
        y += regular_.line_height(spx);
    }
    if (!r.detail.empty()) regular_.draw(out, x, y, regular_.fit(one_line(r.detail), dpx, right - x), dpx, t.dim.alpha(a));
}

Hit View::hit(float x, float y) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
        if (it->first.contains(x, y)) return it->second;
    return {};
}

Rect View::rect_of(Hit h) const {
    for (const auto &[r, hit] : hits_)
        if (hit == h) return r;
    return {};
}

Keys::Out Keys::update(const pspweb::SystemDialog *d, std::uint32_t held) {
    namespace pad = pspweb::pad;
    Out out;
    if (d == nullptr) {
        hidden_ &= held;  // released buttons reach the game again
        out.game = held & ~hidden_;
        last_ = held;
        repeat_frames_ = 0;
        return out;
    }
    constexpr std::uint32_t kDirections = pad::kUp | pad::kDown | pad::kLeft | pad::kRight;
    const std::uint32_t pressed = held & ~last_;
    std::uint32_t moves = pressed & kDirections;
    if ((held & kDirections) == 0u || moves != 0u) repeat_frames_ = 0;
    else if (++repeat_frames_ >= 24 && (repeat_frames_ - 24) % 5 == 0) moves = held & kDirections;
    // A list moves up and down; Yes and No sit side by side, so left and right move too. On the
    // keyboard up and down go a row of keys at a time.
    const int row = d->kind == pspweb::SystemDialog::Kind::Keyboard ? pspweb::kOskColumns : 1;
    if ((moves & pad::kUp) != 0u) out.move -= row;
    if ((moves & pad::kDown) != 0u) out.move += row;
    if (d->kind != pspweb::SystemDialog::Kind::SaveList) {
        if ((moves & pad::kLeft) != 0u) out.move -= 1;
        if ((moves & pad::kRight) != 0u) out.move += 1;
    }
    const std::uint32_t confirm = d->cross_confirms ? pad::kCross : pad::kCircle;
    const std::uint32_t back = d->cross_confirms ? pad::kCircle : pad::kCross;
    out.confirm = (pressed & confirm) != 0u;
    out.back = !out.confirm && (pressed & back) != 0u;
    hidden_ |= held;
    last_ = held;
    return out;
}

anyui::Thumb Icons::get(const std::filesystem::path &png) {
    auto it = cache_.find(png);
    if (it == cache_.end()) {
        Picture p;
        if (!menu::load_icon_pixels(png, p.pixels, p.width, p.height)) p = {};
        it = cache_.emplace(png, std::move(p)).first;
    }
    return {it->second.pixels.empty() ? nullptr : it->second.pixels.data(), it->second.width, it->second.height};
}

Overlay::Overlay(const input::InputMap &map) : map_(map), view_(menu::regular_font(), menu::bold_font()) {}

anyui::ViewOptions Overlay::look() const {
    const input::PlayerSettings &p = map_.player();
    anyui::ViewOptions o;
    o.dark = !p.light_theme;
    o.reduce_motion = p.reduce_motion;
    o.ui_scale = static_cast<float>(p.ui_scale) / 100.f;
    return o;
}

void Overlay::draw(const pspweb::SystemDialog &d, std::uint32_t *pixels, int w, int h, float dt) {
    if (pixels == nullptr || w <= 0 || h <= 0) return;
    if (d.serial != serial_) {
        // A new dialog: its saves may have new icons.
        if (d.kind == pspweb::SystemDialog::Kind::SaveList) icons_.clear();
        serial_ = d.serial;
    }
    anyui::Strings &strings = menu::strings();
    strings.set_language(map_.player().language);
    const Frame frame = make_frame(d, strings, [this](const std::filesystem::path &png) { return icons_.get(png); });
    if (canvas_.width() != w || canvas_.height() != h) canvas_.resize(w, h);
    std::copy(pixels, pixels + static_cast<std::size_t>(w) * static_cast<std::size_t>(h), canvas_.data());
    view_.render(canvas_, frame, look(), dt);
    std::copy(canvas_.pixels().begin(), canvas_.pixels().end(), pixels);
}

void Overlay::hide() { view_.reset(); }

} // namespace anypsp::dialog
