// SPDX-License-Identifier: MIT
// anyui (shared by AnyGBA and AnyPSP; keep the copies identical): the in-game menu's look.
// A runtime describes what to show in a Frame (tabs, the current page's rows or save slots,
// hints, a message) every host frame; MenuView draws it over a frosted copy of the game
// picture, animates focus and transitions, and maps pointer positions back to what was
// drawn there. It holds no menu logic: the runtime's controller owns navigation and acts
// on rows by their id.
#pragma once

#include "canvas.hpp"
#include "font.hpp"
#include "icons.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace anyui {

enum class RowKind : std::uint8_t {
    Action,   // runs something (A / click)
    Link,     // opens a sub-page; value shown before a chevron
    Toggle,   // on/off switch
    Choice,   // value stepped with left/right
    Slider,   // level 0-1 stepped with left/right
    Info,     // read-only text
    Header,   // section title, not selectable
    Binding,  // keyboard and gamepad bindings (value, value2)
    Check,    // a checkbox before the label (cheats); value at the right
    Field,    // typed text with a caret
};

struct Thumb {
    const std::uint32_t* pixels = nullptr;
    int width = 0, height = 0;
    bool empty() const { return !pixels || width <= 0 || height <= 0; }
};

struct Row {
    std::string id;
    RowKind kind = RowKind::Action;
    std::string label;
    std::string value;
    std::string value2;
    bool on = false;
    float level = -1;
    bool enabled = true;
    bool danger = false;
    bool more = false;  // Check rows: a value picker opens with right / the chevron
    Icon icon = Icon::None;
    Thumb thumb;
    std::string detail;  // a second, smaller line
};

struct Slot {
    int number = 0;
    bool used = false;
    std::string caption;  // when it was saved, or "Empty"
    Thumb thumb;
};

enum class Diagram : std::uint8_t { None, Gba, Psp };

// Where the user has put the Video tab's preview divider and loupe. The controller owns it
// (keys and the stick move them); MenuView::preview_pointer moves them with a mouse or a
// finger, and the view draws them from Preview::state.
struct PreviewState {
    float split = 0.5f;                    // the divider across the picture: before on its left, after on its right
    bool loupe = false;                    // the loupe is on (keys, the stick)
    bool pointer = false;                  // a mouse over the picture or a finger held on it shows the loupe
    float loupe_x = 0.5f, loupe_y = 0.5f;  // the loupe's centre over the picture, 0-1

    bool loupe_shown() const { return loupe || pointer; }
    void move_split(float d) { split = std::clamp(split + d, 0.f, 1.f); }
    void move_loupe(float dx, float dy) {
        loupe = true;
        loupe_x = std::clamp(loupe_x + dx, 0.f, 1.f);
        loupe_y = std::clamp(loupe_y + dy, 0.f, 1.f);
    }
};

// The Video tab's live preview: the paused game picture plain (before) and with the current
// settings (after) on either side of a divider, a loupe that magnifies both, and what the
// focused row does. The pictures may have any size and shape (each is fitted into the
// panel); the larger they are, the more detail the loupe shows.
struct Preview {
    bool shown = false;
    Thumb before, after;
    std::uint64_t version = 0;  // changes whenever the pictures do
    std::string before_label, after_label;
    std::string title;        // the focused row and its value
    std::string description;  // what that setting does
    std::string note;         // e.g. why the picture does not show it yet
    float zoom = 4;           // the loupe's magnification over the panel
    PreviewState state;
};

struct Page {
    std::string title;
    std::string crumb;  // where in a tree the page is ("Items / Potions")
    bool can_back = false;
    std::vector<Row> rows;
    int cursor = 0;
    std::vector<Slot> slots;  // non-empty: a grid of save slots instead of rows
    int columns = 3;
    float thumb_aspect = 1.5f;
    std::string popup_title;  // non-empty: actions for the selected slot
    std::vector<Row> popup;
    int popup_cursor = 0;
    bool search = false;
    bool search_active = false;
    std::string search_text;
    std::string search_placeholder;
    Diagram diagram = Diagram::None;
    std::string highlight;  // diagram button lit for the selected row
    std::string modal;      // e.g. "Press a key for A"; drawn over the page
    std::string modal_detail;
    std::string empty_text;
    Preview preview;  // shown: the rows share the page with a live preview (Video)
};

struct Tab {
    std::string id;
    std::string label;
    Icon icon = Icon::None;
};

struct Frame {
    std::string brand;  // "AnyGBA"
    std::string game;   // the game's title
    std::vector<Tab> tabs;
    int tab = 0;
    bool rail_focus = false;
    Page page;
    std::string hint;
    std::string message;
    bool message_error = false;
};

struct Theme {
    bool dark = true;
    Color tint, panel, rail, text, dim, accent, on_accent, select, hover, control, line, field, danger, ok, shadow;
    float frost = 0.45f;

    static Theme make(bool dark) {
        Theme t;
        t.dark = dark;
        if (dark) {
            t.tint = Color::rgb(0x0A0C11);
            t.frost = 0.5f;
            t.panel = Color::rgb(0x15181E, 0.96f);
            t.rail = Color::rgb(0x1C2028, 0.98f);
            t.text = Color::rgb(0xF2F4F8);
            t.dim = Color::rgb(0x98A1B0);
            t.accent = Color::rgb(0x4C8DFF);
            t.on_accent = Color::rgb(0xFFFFFF);
            t.select = Color::rgb(0x4C8DFF, 0.20f);
            t.hover = Color::rgb(0xFFFFFF, 0.05f);
            t.control = Color::rgb(0x3A404C);
            t.line = Color::rgb(0xFFFFFF, 0.08f);
            t.field = Color::rgb(0x0E1014);
            t.danger = Color::rgb(0xFF6B6B);
            t.ok = Color::rgb(0x43D17A);
            t.shadow = Color::rgb(0x000000, 0.45f);
        } else {
            t.tint = Color::rgb(0xF2F4F7);
            t.frost = 0.42f;
            t.panel = Color::rgb(0xFFFFFF, 0.97f);
            t.rail = Color::rgb(0xEEF1F5, 0.98f);
            t.text = Color::rgb(0x14171C);
            t.dim = Color::rgb(0x5D6573);
            t.accent = Color::rgb(0x2F6FEB);
            t.on_accent = Color::rgb(0xFFFFFF);
            t.select = Color::rgb(0x2F6FEB, 0.13f);
            t.hover = Color::rgb(0x000000, 0.04f);
            t.control = Color::rgb(0xC8CDD5);
            t.line = Color::rgb(0x000000, 0.08f);
            t.field = Color::rgb(0xF1F3F6);
            t.danger = Color::rgb(0xD93636);
            t.ok = Color::rgb(0x1E9E50);
            t.shadow = Color::rgb(0x1A2030, 0.22f);
        }
        return t;
    }
};

struct ViewOptions {
    float ui_scale = 1;  // on top of the size-based scale
    bool dark = true;
    bool reduce_motion = false;
    bool touch = false;  // bigger rows and targets
};

enum class HitKind : std::uint8_t { None, Tab, Row, Decrease, Increase, Slot, Popup, Close, Back, Search, Outside, Preview };

struct Hit {
    HitKind kind = HitKind::None;
    int index = -1;
    bool operator==(const Hit&) const = default;
};

class MenuView {
public:
    MenuView(const Font& regular, const Font& bold) : regular_(regular), bold_(bold) {}

    // The game picture at the output size; kept sharp and frosted for the backdrop.
    void set_background(const std::uint32_t* pixels, int w, int h, const ViewOptions& o) {
        sharp_.resize(w, h);
        if (pixels) std::copy(pixels, pixels + static_cast<std::size_t>(w) * static_cast<std::size_t>(h), sharp_.data());
        frosted_ = sharp_;
        const Theme t = Theme::make(o.dark);
        const int radius = std::max(8, std::min(w, h) / 40);
        frosted(frosted_, radius, std::max(2, std::min(w, h) / 270), t.tint, t.frost);
        bg_dark_ = o.dark;
    }
    bool background_fits(int w, int h, const ViewOptions& o) const {
        return sharp_.width() == w && sharp_.height() == h && bg_dark_ == o.dark;
    }

    // Starts the opening animation (instant with reduce_motion).
    void open() {
        target_open_ = 1;
        open_ = 0;
        scroll_ = scroll_target_ = 0;
        manual_ = false;
        first_ = true;
        split_drag_ = finger_loupe_ = false;
    }
    // Starts the closing animation; done once closed() is true.
    void close() { target_open_ = 0; }
    bool closed() const { return target_open_ == 0 && open_ <= 0.001f; }
    bool animating() const {
        return std::abs(open_ - target_open_) > 0.001f || std::abs(sel_y_ - sel_target_) > 0.5f ||
               std::abs(scroll_ - scroll_target_) > 0.5f || fade_ < 0.999f;
    }

    // Scrolls the page by mouse-wheel notches (positive: down).
    void wheel(float notches) {
        manual_ = true;
        scroll_target_ += notches * row_h_ * 1.5f;
    }
    void hover(float x, float y) { hover_ = hit(x, y); }
    void leave() { hover_ = {}; }

    Hit hit(float x, float y) const {
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            if (it->first.contains(x, y)) return it->second;
        }
        return {};
    }
    // Where the last render put a hit target (tests click through it).
    Rect rect_of(Hit h) const {
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            if (it->second == h) return it->first;
        }
        return {};
    }

    enum class PointerPhase : std::uint8_t { Down, Move, Up };
    // A mouse or finger over the Video preview, at window pixels (x, y). A mouse dragged on the
    // picture moves the divider and shows the loupe while over it; a finger moves the divider
    // when it starts near it, else holds the loupe under it. Returns true when the preview took
    // the event (no click, scroll or tap should follow from it).
    bool preview_pointer(PreviewState& s, PointerPhase phase, float x, float y, bool touch) {
        const Rect r = preview_pic_;
        if (r.w <= 0 || r.h <= 0) {
            split_drag_ = finger_loupe_ = false;
            s.pointer = false;
            return false;
        }
        const float nx = std::clamp((x - r.x) / r.w, 0.f, 1.f), ny = std::clamp((y - r.y) / r.h, 0.f, 1.f);
        const auto follow = [&] {
            s.pointer = true;
            s.loupe_x = nx;
            s.loupe_y = ny;
        };
        switch (phase) {
        case PointerPhase::Down:
            if (!r.contains(x, y)) return false;
            if (!touch || std::abs(x - (r.x + s.split * r.w)) <= 28 * u_) {
                split_drag_ = true;
                s.split = nx;
            } else {
                finger_loupe_ = true;
            }
            if (!touch || finger_loupe_) follow();
            touch_loupe_ = touch && finger_loupe_;
            return true;
        case PointerPhase::Move:
            if (split_drag_) {
                s.split = nx;
                if (!touch) follow();
                return true;
            }
            if (finger_loupe_) {
                follow();
                return true;
            }
            if (touch) return false;
            if (r.contains(x, y)) follow();
            else s.pointer = false;
            touch_loupe_ = false;
            return false;
        case PointerPhase::Up:
            if (!split_drag_ && !finger_loupe_) return false;
            if (touch) s.pointer = false;
            split_drag_ = finger_loupe_ = touch_loupe_ = false;
            return true;
        }
        return false;
    }

    // Draws the frame over the backdrop into `out` (resized to the backdrop when needed).
    // `dt` is the time since the last call, in seconds.
    void render(Canvas& out, const Frame& f, const ViewOptions& o, float dt) {
        const int W = sharp_.width(), H = sharp_.height();
        if (out.width() != W || out.height() != H) out.resize(W, H);
        hits_.clear();
        preview_pic_ = {};
        const Theme t = Theme::make(o.dark);
        const auto approach = [&](float& v, float target, float rate) {
            if (o.reduce_motion || first_) {
                v = target;
                return;
            }
            v += (target - v) * (1 - std::exp(-dt * rate));
            if (std::abs(target - v) < 0.0005f * std::max(1.f, std::abs(target))) v = target;
        };
        if (o.reduce_motion) {
            open_ = target_open_;
        } else {
            const float step = dt / 0.18f;
            open_ = target_open_ > open_ ? std::min(target_open_, open_ + step) : std::max(target_open_, open_ - step * 1.4f);
        }
        const float ga = ease(open_);

        // Backdrop: the picture frosts in as the menu opens.
        if (ga >= 0.999f) {
            std::copy(frosted_.pixels().begin(), frosted_.pixels().end(), out.pixels().begin());
        } else {
            const unsigned wgt = static_cast<unsigned>(ga * 256);
            const auto& a = sharp_.pixels();
            const auto& b = frosted_.pixels();
            auto& d = out.pixels();
            for (std::size_t i = 0; i < d.size(); ++i) d[i] = lerp_pixel(a[i], b[i], wgt);
        }
        if (ga <= 0.001f) return;

        // Size-based unit: 1 at 960x540, scaled by the UI scale setting.
        const float base = std::min(static_cast<float>(W) / 960.f, static_cast<float>(H) / 540.f);
        u_ = std::max(0.45f, base * o.ui_scale);
        const float u = u_;
        row_h_ = (o.touch ? 52.f : 40.f) * u;

        // Tab changes cross-fade the content.
        if (f.tab != last_tab_ || f.page.title != last_title_ || f.page.crumb != last_crumb_) {
            if (!first_ && !o.reduce_motion) fade_ = 0;
            last_tab_ = f.tab;
            last_title_ = f.page.title;
            last_crumb_ = f.page.crumb;
            manual_ = false;
            scroll_ = scroll_target_ = 0;
            sel_jump_ = true;
        }
        approach(fade_, 1, 14);
        const float margin = (o.touch ? 14.f : 24.f) * u;
        const float card_w = std::min(static_cast<float>(W) - 2 * margin, 940 * u);
        const float card_h = std::min(static_cast<float>(H) - 2 * margin, 500 * u);
        const float rise = (1 - ga) * 14 * u;
        const Rect card{(static_cast<float>(W) - card_w) / 2, (static_cast<float>(H) - card_h) / 2 + rise, card_w, card_h};
        const float radius = 18 * u;
        hits_.push_back({{0, 0, static_cast<float>(W), static_cast<float>(H)}, {HitKind::Outside, -1}});
        hits_.push_back({card, {HitKind::None, -1}});

        out.fill_round_rect({card.x - 4 * u, card.y + 2 * u, card.w + 8 * u, card.h + 12 * u}, radius + 8 * u,
                            t.shadow.alpha(ga), 28 * u);
        out.fill_round_rect(card, radius, t.panel.alpha(ga));

        // Rail.
        const bool compact = card_w < 640 * u;
        const float rail_w = compact ? 66 * u : 200 * u;
        const Rect rail{card.x, card.y, rail_w, card.h};
        out.push_clip(rail);
        out.fill_round_rect(card, radius, t.rail.alpha(ga));
        out.pop_clip();
        out.fill_rect({rail.right() - std::max(1.f, u), card.y, std::max(1.f, u), card.h}, t.line.alpha(ga));
        float y = card.y + 22 * u;
        if (!compact) {
            const float bpx = 17 * u;
            bold_.draw(out, rail.x + 22 * u, y + bold_.ascent(bpx), f.brand, bpx, t.text.alpha(ga));
            y += bold_.line_height(bpx);
            if (!f.game.empty()) {
                const float gpx = 12 * u;
                regular_.draw(out, rail.x + 22 * u, y + regular_.ascent(gpx),
                              regular_.fit(f.game, gpx, rail_w - 40 * u), gpx, t.dim.alpha(ga));
                y += regular_.line_height(gpx);
            }
            y += 14 * u;
        } else {
            y += 6 * u;
        }
        const float tab_h = (o.touch ? 50.f : 42.f) * u;
        const float tab_gap = 4 * u;
        const float tabs_room = card.bottom() - 14 * u - y;
        const float tab_step = std::min(tab_h + tab_gap, tabs_room / static_cast<float>(std::max<std::size_t>(f.tabs.size(), 1)));
        approach(tab_y_, y + static_cast<float>(f.tab) * tab_step, 22);
        for (std::size_t i = 0; i < f.tabs.size(); ++i) {
            const Rect r{rail.x + 10 * u, y + static_cast<float>(i) * tab_step, rail_w - 20 * u, std::min(tab_h, tab_step - 2 * u)};
            const bool active = static_cast<int>(i) == f.tab;
            const Hit h{HitKind::Tab, static_cast<int>(i)};
            if (hover_ == h && !active) out.fill_round_rect(r, 10 * u, t.hover.alpha(ga));
            hits_.push_back({r, h});
        }
        if (!f.tabs.empty()) {
            const Rect sel{rail.x + 10 * u, tab_y_, rail_w - 20 * u, std::min(tab_h, tab_step - 2 * u)};
            out.fill_round_rect(sel, 10 * u, (f.rail_focus ? t.accent : t.select).alpha(ga));
        }
        for (std::size_t i = 0; i < f.tabs.size(); ++i) {
            const Rect r{rail.x + 10 * u, y + static_cast<float>(i) * tab_step, rail_w - 20 * u, std::min(tab_h, tab_step - 2 * u)};
            const bool active = static_cast<int>(i) == f.tab;
            const Color col = active ? (f.rail_focus ? t.on_accent : t.accent) : t.dim;
            const float icon_x = compact ? r.x + r.w / 2 : r.x + 22 * u;
            draw_icon(out, f.tabs[i].icon, icon_x, r.y + r.h / 2, 20 * u, col.alpha(ga));
            if (!compact) {
                const float px = 15 * u;
                const Font& font = active ? bold_ : regular_;
                font.draw(out, r.x + 44 * u, center_baseline(font, r.y + r.h / 2, px),
                          font.fit(f.tabs[i].label, px, r.w - 52 * u),
                          px, (active ? (f.rail_focus ? t.on_accent : t.text) : t.dim).alpha(ga));
            }
        }

        // Content.
        const float fade = ease(fade_);
        const float ca = ga * fade;
        const float slide = (1 - fade) * 10 * u;
        const Rect content{rail.right() + 26 * u, card.y + 20 * u + slide, card.right() - rail.right() - 46 * u,
                           card.h - 40 * u};
        float title_x = content.x;
        const Page& p = f.page;
        if (p.can_back) {
            const Rect back{content.x - 6 * u, content.y + 2 * u, 34 * u, 34 * u};
            if (hover_ == Hit{HitKind::Back, -1}) out.fill_round_rect(back, 17 * u, t.hover.alpha(ca));
            draw_icon(out, Icon::Back, back.x + back.w / 2, back.y + back.h / 2, 20 * u, t.text.alpha(ca));
            hits_.push_back({back, {HitKind::Back, -1}});
            title_x += 34 * u;
        }
        const float tpx = 23 * u;
        const Rect close{card.right() - 50 * u, card.y + 16 * u, 34 * u, 34 * u};
        bold_.draw(out, title_x, content.y + 4 * u + bold_.ascent(tpx), bold_.fit(p.title, tpx, close.x - title_x - 8 * u), tpx,
                   t.text.alpha(ca));
        float list_top = content.y + 4 * u + bold_.line_height(tpx) + 2 * u;
        if (!p.crumb.empty()) {
            const float cpx = 13 * u;
            regular_.draw(out, title_x, list_top + regular_.ascent(cpx), regular_.fit_end(p.crumb, cpx, content.right() - title_x), cpx,
                          t.dim.alpha(ca));
            list_top += regular_.line_height(cpx);
        }
        list_top += 10 * u;
        if (hover_ == Hit{HitKind::Close, -1}) out.fill_round_rect(close, 17 * u, t.hover.alpha(ga));
        draw_icon(out, Icon::Close, close.x + close.w / 2, close.y + close.h / 2, 18 * u, t.dim.alpha(ga));
        hits_.push_back({close, {HitKind::Close, -1}});

        // Footer: hints, and the latest message.
        const float hpx = 12.5f * u;
        const float footer_y = content.bottom() - 4 * u;
        float msg_left = content.right();
        if (!f.message.empty()) {
            const float mpx = 13 * u;
            const std::string msg = regular_.fit(f.message, mpx, content.w * 0.6f);
            const float mw = regular_.measure(msg, mpx) + 34 * u;
            const Rect chip{content.right() - mw, footer_y - 24 * u, mw, 28 * u};
            out.fill_round_rect(chip, 14 * u, (f.message_error ? t.danger : t.ok).alpha(0.16f * ga));
            out.fill_circle(chip.x + 14 * u, chip.y + chip.h / 2, 4 * u, (f.message_error ? t.danger : t.ok).alpha(ga));
            regular_.draw(out, chip.x + 24 * u, center_baseline(regular_, chip.y + chip.h / 2, mpx), msg, mpx, t.text.alpha(ga));
            msg_left = chip.x - 12 * u;
        }
        if (!f.hint.empty()) {
            regular_.draw(out, content.x, center_baseline(regular_, footer_y - 10 * u, hpx),
                          regular_.fit(f.hint, hpx, msg_left - content.x), hpx, t.dim.alpha(ga));
        }
        const Rect list{content.x - 8 * u, list_top, content.w + 16 * u, footer_y - 34 * u - list_top};

        if (!p.slots.empty()) {
            draw_slots(out, t, p, list, ca, o);
        } else {
            Rect rows_area = list;
            if (p.diagram != Diagram::None) {
                if (list.w >= 560 * u) {
                    const float dw = list.w * 0.42f;
                    draw_diagram(out, t, p, {list.right() - dw, list.y, dw, std::min(list.h, dw * 0.62f)}, ca);
                    rows_area.w -= dw + 12 * u;
                } else {
                    const float dh = std::min(list.h * 0.36f, 150 * u);
                    draw_diagram(out, t, p, {list.x, list.y, list.w, dh}, ca);
                    rows_area.y += dh + 8 * u;
                    rows_area.h -= dh + 8 * u;
                }
            } else if (p.preview.shown) {
                if (list.w >= 560 * u) {
                    const float pw = std::round(list.w * 0.46f);
                    draw_preview(out, t, p.preview, {list.right() - pw, list.y, pw, list.h}, ca, o, true);
                    rows_area.w -= pw + 12 * u;
                } else {
                    const float ph = std::min(list.h * 0.42f, 170 * u);
                    draw_preview(out, t, p.preview, {list.x, list.y, list.w, ph}, ca, o, false);
                    rows_area.y += ph + 8 * u;
                    rows_area.h -= ph + 8 * u;
                }
            }
            if (p.search) {
                const Rect field{rows_area.x + 8 * u, rows_area.y, rows_area.w - 16 * u, 36 * u};
                out.fill_round_rect(field, 10 * u, t.field.alpha(ca));
                if (p.search_active) out.stroke_round_rect(field, 10 * u, 2 * u, t.accent.alpha(ca));
                draw_icon(out, Icon::Search, field.x + 18 * u, field.y + field.h / 2, 16 * u, t.dim.alpha(ca));
                const float spx = 14 * u;
                const bool empty = p.search_text.empty();
                const std::string shown = empty ? p.search_placeholder : regular_.fit_end(p.search_text, spx, field.w - 60 * u);
                const float end = regular_.draw(out, field.x + 34 * u, center_baseline(regular_, field.y + field.h / 2, spx),
                                                empty && p.search_active ? std::string() : shown, spx,
                                                (empty ? t.dim : t.text).alpha(ca));
                if (p.search_active) {
                    out.fill_rect({end + 1 * u, field.y + 9 * u, std::max(1.f, 1.5f * u), field.h - 18 * u}, t.accent.alpha(ca));
                }
                hits_.push_back({field, {HitKind::Search, -1}});
                rows_area.y += 44 * u;
                rows_area.h -= 44 * u;
            }
            draw_rows(out, t, p.rows, p.cursor, rows_area, ca, o, f.rail_focus || !p.modal.empty(), HitKind::Row, p.empty_text, dt,
                      true);
        }
        if (!p.modal.empty()) draw_modal(out, t, p, card, ca);
        first_ = false;
    }

private:
    static float ease(float x) {
        x = std::clamp(x, 0.f, 1.f);
        return 1 - (1 - x) * (1 - x) * (1 - x);
    }

    static std::uint32_t lerp_pixel(std::uint32_t a, std::uint32_t b, unsigned w) {
        const unsigned v = 256 - w;
        const std::uint32_t rb = (((a & 0xFF00FFu) * v + (b & 0xFF00FFu) * w) >> 8) & 0xFF00FFu;
        const std::uint32_t g = (((a & 0x00FF00u) * v + (b & 0x00FF00u) * w) >> 8) & 0x00FF00u;
        return rb | g | 0xFF000000u;
    }

    float center_baseline(const Font& font, float cy, float px) const {
        return cy + (font.ascent(px) - font.descent(px)) / 2;
    }

    float row_height(const Row& r) const {
        if (r.kind == RowKind::Header) return 30 * u_;
        if (!r.thumb.empty()) return row_h_ * 1.5f;
        if (!r.detail.empty()) return row_h_ * 1.3f;
        return row_h_;
    }

    void draw_rows(Canvas& out, const Theme& t, const std::vector<Row>& rows, int cursor, Rect area, float a,
                   const ViewOptions& o, bool unfocused, HitKind kind, const std::string& empty_text, float dt, bool main) {
        const float u = u_;
        if (rows.empty()) {
            if (!empty_text.empty()) {
                const float px = 14 * u;
                regular_.draw(out, area.x + 16 * u, area.y + 24 * u, regular_.fit(empty_text, px, area.w - 32 * u), px, t.dim.alpha(a));
            }
            return;
        }
        std::vector<float> tops(rows.size());
        float total = 0;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            tops[i] = total;
            total += row_height(rows[i]);
        }
        float scroll = 0;
        const int cur = std::clamp(cursor, 0, static_cast<int>(rows.size()) - 1);
        if (main) {
            const float max_scroll = std::max(0.f, total - area.h);
            if (!manual_ || cursor != last_cursor_) {
                manual_ = manual_ && cursor == last_cursor_;
                const float top = tops[static_cast<std::size_t>(cur)];
                const float bottom = top + row_height(rows[static_cast<std::size_t>(cur)]);
                if (top - scroll_target_ < row_h_ * 0.5f) scroll_target_ = top - row_h_ * 0.5f;
                if (bottom - scroll_target_ > area.h - row_h_ * 0.5f) scroll_target_ = bottom - area.h + row_h_ * 0.5f;
            }
            scroll_target_ = std::clamp(scroll_target_, 0.f, max_scroll);
            if (o.reduce_motion || sel_jump_) scroll_ = scroll_target_;
            else scroll_ += (scroll_target_ - scroll_) * (1 - std::exp(-dt * 18));
            scroll = scroll_;
            const float target = tops[static_cast<std::size_t>(cur)];
            if (o.reduce_motion || sel_jump_ || (cursor != last_cursor_ && std::abs(target - sel_target_) > area.h)) sel_y_ = target;
            sel_target_ = target;
            if (!o.reduce_motion) sel_y_ += (sel_target_ - sel_y_) * (1 - std::exp(-dt * 24));
            else sel_y_ = sel_target_;
            last_cursor_ = cursor;
            sel_jump_ = false;
        }
        out.push_clip(area);
        const float sel_top = main ? sel_y_ : tops[static_cast<std::size_t>(cur)];
        if (rows[static_cast<std::size_t>(cur)].kind != RowKind::Header) {
            const Rect sel{area.x, area.y + sel_top - scroll, area.w, row_height(rows[static_cast<std::size_t>(cur)])};
            out.fill_round_rect(sel, 10 * u, (unfocused ? t.hover : t.select).alpha(a));
            if (!unfocused) out.fill_round_rect({sel.x + 2 * u, sel.y + sel.h * 0.25f, 3 * u, sel.h * 0.5f}, 1.5f * u, t.accent.alpha(a));
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            const Rect rr{area.x, area.y + tops[i] - scroll, area.w, row_height(r)};
            if (rr.bottom() < area.y || rr.y > area.bottom()) continue;
            const Hit h{kind, static_cast<int>(i)};
            const Rect visible{rr.x, std::max(rr.y, area.y), rr.w, std::min(rr.bottom(), area.bottom()) - std::max(rr.y, area.y)};
            if (r.kind != RowKind::Header) {
                if (hover_ == h && static_cast<int>(i) != cur) out.fill_round_rect(rr, 10 * u, t.hover.alpha(a));
                hits_.push_back({visible, h});
            }
            draw_row(out, t, r, rr, a, static_cast<int>(i) == cur && !unfocused, kind, static_cast<int>(i), visible);
        }
        out.pop_clip();
        if (main && total > area.h) {
            const float bar_h = std::max(24 * u, area.h * area.h / total);
            const float bar_y = area.y + (area.h - bar_h) * (scroll / std::max(1.f, total - area.h));
            out.fill_round_rect({area.right() + 2 * u, bar_y, 3 * u, bar_h}, 1.5f * u, t.dim.alpha(0.5f * a));
        }
    }

    void draw_row(Canvas& out, const Theme& t, const Row& r, Rect rr, float a, bool selected, HitKind kind, int index,
                  Rect visible) {
        const float u = u_;
        const float px = 15 * u;
        const float cy = rr.y + rr.h / 2;
        const Color text = (r.enabled ? (r.danger ? t.danger : t.text) : t.dim).alpha(a);
        if (r.kind == RowKind::Header) {
            const float hp = 12 * u;
            bold_.draw(out, rr.x + 16 * u, rr.bottom() - 9 * u, r.label, hp, t.dim.alpha(a));
            return;
        }
        float x = rr.x + 16 * u;
        if (!r.thumb.empty()) {
            const float th = rr.h - 12 * u;
            const float tw = th * static_cast<float>(r.thumb.width) / static_cast<float>(r.thumb.height);
            out.image(r.thumb.pixels, r.thumb.width, r.thumb.height, {x, rr.y + 6 * u, tw, th}, 6 * u, a);
            x += tw + 14 * u;
        } else if (r.icon != Icon::None) {
            draw_icon(out, r.icon, x + 10 * u, cy, 18 * u, (r.danger ? t.danger : selected ? t.accent : t.dim).alpha(a));
            x += 30 * u;
        }
        if (r.kind == RowKind::Check) {
            const Rect box{x, cy - 9 * u, 18 * u, 18 * u};
            if (r.on) {
                out.fill_round_rect(box, 5 * u, t.accent.alpha(a));
                draw_icon(out, Icon::Check, box.x + box.w / 2, box.y + box.h / 2, 14 * u, t.on_accent.alpha(a));
            } else {
                out.stroke_round_rect(box, 5 * u, std::max(1.f, 1.6f * u), t.dim.alpha(a));
            }
            x += 30 * u;
        }

        // The right side first: it decides how much room the label gets.
        float right = rr.right() - 16 * u;
        const float vpx = 14 * u;
        switch (r.kind) {
        case RowKind::Toggle: {
            const Rect sw{right - 40 * u, cy - 11 * u, 40 * u, 22 * u};
            out.fill_round_rect(sw, 11 * u, (r.on ? t.accent : t.control).alpha(a));
            out.fill_circle(r.on ? sw.right() - 11 * u : sw.x + 11 * u, cy, 8 * u, Color::rgb(0xFFFFFF, a));
            right = sw.x - 10 * u;
            break;
        }
        case RowKind::Choice: {
            const float vw = std::min(regular_.measure(r.value, vpx), rr.w * 0.45f);
            const std::string v = regular_.fit(r.value, vpx, rr.w * 0.45f);
            const Rect inc{right - 24 * u, cy - 14 * u, 24 * u, 28 * u};
            draw_icon(out, Icon::ChevronRight, inc.x + inc.w / 2, cy, 16 * u, (selected ? t.accent : t.dim).alpha(a));
            const float vx = inc.x - 6 * u - vw;
            regular_.draw(out, vx, center_baseline(regular_, cy, vpx), v, vpx, (selected ? t.text : t.dim).alpha(a));
            const Rect dec{vx - 30 * u, cy - 14 * u, 24 * u, 28 * u};
            draw_icon(out, Icon::Back, dec.x + dec.w / 2, cy, 16 * u, (selected ? t.accent : t.dim).alpha(a));
            add_step_hits(kind, index, dec, inc, visible);
            right = dec.x - 8 * u;
            break;
        }
        case RowKind::Slider: {
            const float track_w = std::min(150 * u, rr.w * 0.3f);
            const Rect track{right - track_w, cy - 3 * u, track_w, 6 * u};
            const float lv = std::clamp(r.level, 0.f, 1.f);
            out.fill_round_rect(track, 3 * u, t.control.alpha(a));
            out.fill_round_rect({track.x, track.y, track.w * lv, track.h}, 3 * u, t.accent.alpha(a));
            out.fill_circle(track.x + track.w * lv, cy, 8 * u, (selected ? t.accent : Color::rgb(0xFFFFFF)).alpha(a));
            if (!selected) out.stroke_circle(track.x + track.w * lv, cy, 8 * u, std::max(1.f, u), t.control.alpha(a));
            const float vw = regular_.measure(r.value, vpx);
            regular_.draw(out, track.x - 14 * u - vw, center_baseline(regular_, cy, vpx), r.value, vpx, t.dim.alpha(a));
            add_step_hits(kind, index, {track.x - 4 * u, cy - 14 * u, track.w * lv + 4 * u, 28 * u},
                          {track.x + track.w * lv, cy - 14 * u, track.w * (1 - lv) + 4 * u, 28 * u}, visible);
            right = track.x - 24 * u - vw;
            break;
        }
        case RowKind::Link:
        case RowKind::Info:
        case RowKind::Action:
        case RowKind::Check: {
            if (r.kind == RowKind::Link || r.more) {
                draw_icon(out, Icon::ChevronRight, right - 8 * u, cy, 16 * u, t.dim.alpha(a));
                if (r.more) add_step_hits(kind, index, {}, {right - 24 * u, cy - 14 * u, 28 * u, 28 * u}, visible);
                right -= 26 * u;
            }
            if (!r.value.empty()) {
                const std::string v = regular_.fit_end(r.value, vpx, rr.w * 0.45f);
                const float vw = regular_.measure(v, vpx);
                regular_.draw(out, right - vw, center_baseline(regular_, cy, vpx), v, vpx, t.dim.alpha(a));
                right -= vw + 12 * u;
            }
            break;
        }
        case RowKind::Binding: {
            const auto chip = [&](const std::string& s, float& at) {
                const float cpx = 13 * u;
                const std::string shown = s.empty() ? std::string("\xE2\x80\x93") : regular_.fit(s, cpx, rr.w * 0.24f);
                const float w = std::max(regular_.measure(shown, cpx) + 18 * u, 34 * u);
                const Rect c{at - w, cy - 13 * u, w, 26 * u};
                out.fill_round_rect(c, 7 * u, t.field.alpha(a));
                out.stroke_round_rect(c, 7 * u, std::max(1.f, u), t.line.alpha(a * 2));
                regular_.draw(out, c.x + (w - regular_.measure(shown, cpx)) / 2, center_baseline(regular_, cy, cpx), shown, cpx,
                              (s.empty() ? t.dim : t.text).alpha(a));
                at = c.x - 8 * u;
            };
            float at = right;
            const float col = std::min(rr.w * 0.27f, 190 * u);
            float pad_at = at;
            chip(r.value2, pad_at);
            float key_at = right - col;
            chip(r.value, key_at);
            right = std::min(key_at, pad_at);
            break;
        }
        case RowKind::Field: {
            const Rect field{x, cy - 15 * u, rr.right() - 16 * u - x, 30 * u};
            out.fill_round_rect(field, 8 * u, t.field.alpha(a));
            if (selected) out.stroke_round_rect(field, 8 * u, 2 * u, t.accent.alpha(a));
            const float fpx = 15 * u;
            const float end = regular_.draw(out, field.x + 12 * u, center_baseline(regular_, cy, fpx), r.label, fpx, t.text.alpha(a));
            if (selected) out.fill_rect({end + u, field.y + 7 * u, std::max(1.f, 1.5f * u), field.h - 14 * u}, t.accent.alpha(a));
            return;
        }
        case RowKind::Header: break;
        }
        const float room = right - x;
        if (r.detail.empty()) {
            regular_.draw(out, x, center_baseline(regular_, cy, px), regular_.fit(r.label, px, room), px, text);
        } else {
            const float dpx = 12 * u;
            regular_.draw(out, x, cy - 2 * u, regular_.fit(r.label, px, room), px, text);
            regular_.draw(out, x, cy + 4 * u + regular_.ascent(dpx), regular_.fit(r.detail, dpx, room), dpx, t.dim.alpha(a));
        }
    }

    void add_step_hits(HitKind kind, int index, Rect dec, Rect inc, Rect visible) {
        if (kind != HitKind::Row) return;
        const auto clipped = [&](Rect r) {
            const float y0 = std::max(r.y, visible.y), y1 = std::min(r.bottom(), visible.bottom());
            return Rect{r.x, y0, r.w, std::max(0.f, y1 - y0)};
        };
        if (dec.w > 0) hits_.push_back({clipped(dec), {HitKind::Decrease, index}});
        if (inc.w > 0) hits_.push_back({clipped(inc), {HitKind::Increase, index}});
    }

    void draw_slots(Canvas& out, const Theme& t, const Page& p, Rect area, float a, const ViewOptions& o) {
        const float u = u_;
        const int cols = std::max(1, p.columns);
        const int count = static_cast<int>(p.slots.size());
        const int rows = (count + cols - 1) / cols;
        const float gap = 14 * u;
        const float caption_h = 24 * u;
        float cw = (area.w - 16 * u - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        const float by_height = ((area.h - gap * static_cast<float>(rows - 1)) / static_cast<float>(rows) - caption_h) * p.thumb_aspect;
        cw = std::max(40 * u, std::min(cw, by_height));
        const float th = cw / p.thumb_aspect;
        const float ch = th + caption_h;
        const float gw = cw * static_cast<float>(cols) + gap * static_cast<float>(cols - 1);
        const float x0 = area.x + (area.w - gw) / 2;
        const float y0 = area.y;
        const bool popup = !p.popup.empty();
        Rect selected_cell{};
        for (int i = 0; i < count; ++i) {
            const Slot& s = p.slots[static_cast<std::size_t>(i)];
            const Rect cell{x0 + static_cast<float>(i % cols) * (cw + gap), y0 + static_cast<float>(i / cols) * (ch + gap), cw, th};
            const bool sel = i == p.cursor;
            if (sel) selected_cell = cell;
            const Hit h{HitKind::Slot, i};
            if (!s.thumb.empty()) {
                out.image(s.thumb.pixels, s.thumb.width, s.thumb.height, cell, 10 * u, a);
            } else {
                out.fill_round_rect(cell, 10 * u, t.field.alpha(a));
                draw_icon(out, s.used ? Icon::Save : Icon::Plus, cell.x + cell.w / 2, cell.y + cell.h / 2, 22 * u, t.dim.alpha(a));
            }
            if (hover_ == h && !sel) out.stroke_round_rect(cell, 10 * u, 2 * u, t.dim.alpha(a * 0.6f));
            if (sel) out.stroke_round_rect(cell.inset(-3 * u), 13 * u, 3 * u, (popup ? t.dim : t.accent).alpha(a));
            const Rect badge{cell.x + 8 * u, cell.y + 8 * u, 24 * u, 24 * u};
            out.fill_round_rect(badge, 12 * u, Color::rgb(0x000000, 0.55f * a));
            const std::string n = std::to_string(s.number);
            const float npx = 13 * u;
            bold_.draw(out, badge.x + (badge.w - bold_.measure(n, npx)) / 2, center_baseline(bold_, badge.y + badge.h / 2, npx), n, npx,
                       Color::rgb(0xFFFFFF, a));
            const float cpx = 12.5f * u;
            regular_.draw(out, cell.x + 2 * u, cell.bottom() + 6 * u + regular_.ascent(cpx), regular_.fit(s.caption, cpx, cw - 4 * u), cpx,
                          (s.used ? (sel ? t.text : t.dim) : t.dim).alpha(a));
            hits_.push_back({{cell.x, cell.y, cell.w, ch}, h});
        }
        if (!popup) return;
        // Actions for the selected slot, in a small card beside it.
        float total = 0;
        for (const Row& r : p.popup) total += row_height(r);
        const float pw = std::min(250 * u, area.w * 0.6f);
        const float title_h = p.popup_title.empty() ? 0 : 32 * u;
        const float ph = total + title_h + 16 * u;
        float px = selected_cell.right() + 12 * u;
        if (px + pw > area.right()) px = selected_cell.x - 12 * u - pw;
        if (px < area.x) px = area.x + (area.w - pw) / 2;
        float py = std::clamp(selected_cell.y, area.y, std::max(area.y, area.bottom() - ph));
        const Rect card{px, py, pw, ph};
        out.fill_round_rect({card.x, card.y + 4 * u, card.w, card.h + 6 * u}, 14 * u, t.shadow.alpha(a), 18 * u);
        out.fill_round_rect(card, 14 * u, (t.dark ? Color::rgb(0x232833) : Color::rgb(0xFFFFFF)).alpha(a));
        out.stroke_round_rect(card, 14 * u, std::max(1.f, u), t.line.alpha(a));
        hits_.push_back({card, {HitKind::None, -1}});
        if (title_h > 0) {
            const float tp = 13 * u;
            bold_.draw(out, card.x + 16 * u, card.y + 10 * u + bold_.ascent(tp), bold_.fit(p.popup_title, tp, pw - 32 * u), tp,
                       t.dim.alpha(a));
        }
        draw_rows(out, t, p.popup, p.popup_cursor, {card.x + 8 * u, card.y + 8 * u + title_h, pw - 16 * u, total}, a, o, false,
                  HitKind::Popup, {}, 0, false);
    }

    // `text` broken at spaces into at most `max_lines` lines of `width`; the last one ends
    // with "…" when the text does not fit.
    std::vector<std::string> wrap(const Font& font, const std::string& text, float px, float width, int max_lines) const {
        std::vector<std::string> lines;
        std::string line;
        std::size_t i = 0;
        while (i < text.size() && static_cast<int>(lines.size()) < max_lines) {
            std::size_t end = text.find(' ', i);
            if (end == std::string::npos) end = text.size();
            const std::string word = text.substr(i, end - i);
            const std::string joined = line.empty() ? word : line + " " + word;
            if (line.empty() || font.measure(joined, px) <= width) {
                line = joined;
                i = end + 1;
                continue;
            }
            lines.push_back(line);
            line.clear();
        }
        if (!line.empty() && static_cast<int>(lines.size()) < max_lines) lines.push_back(line);
        if (!lines.empty()) {
            const bool cut = i < text.size();
            lines.back() = font.fit(cut ? lines.back() + " \xE2\x80\xA6" : lines.back(), px, width);
        }
        return lines;
    }

    // A picture resized once into a cache for the panel (shrunk by area, so scanlines and
    // grids do not turn into moire), redone when the picture or the size changes.
    struct Scaled {
        const std::uint32_t* source = nullptr;
        int sw = 0, sh = 0;
        std::uint64_t version = 0;
        Canvas picture;
    };
    static const Canvas& scaled(Scaled& cache, const Thumb& src, std::uint64_t version, int w, int h) {
        if (cache.source != src.pixels || cache.sw != src.width || cache.sh != src.height || cache.version != version ||
            cache.picture.width() != w || cache.picture.height() != h) {
            cache.source = src.pixels;
            cache.sw = src.width;
            cache.sh = src.height;
            cache.version = version;
            cache.picture.resize(w, h);
            resample(src.pixels, src.width, src.height, cache.picture.data(), w, h, Resample::Area);
        }
        return cache.picture;
    }

    // `src` fitted (centred, its own shape) into `box`, on whole pixels.
    static Rect fitted(const Thumb& src, Rect box) {
        float w = box.w, h = box.w * static_cast<float>(src.height) / static_cast<float>(src.width);
        if (h > box.h) {
            h = box.h;
            w = h * static_cast<float>(src.width) / static_cast<float>(src.height);
        }
        return {std::round(box.x + (box.w - w) / 2), std::round(box.y + (box.h - h) / 2), std::max(1.f, std::round(w)),
                std::max(1.f, std::round(h))};
    }

    // The Video tab's preview panel: the picture (before | after), its labels, the divider and
    // the loupe; then the focused setting and what it does. `wide`: text under the picture,
    // else beside it.
    void draw_preview(Canvas& out, const Theme& t, const Preview& pv, Rect area, float a, const ViewOptions& o, bool wide) {
        const float u = u_;
        out.fill_round_rect(area, 12 * u, t.field.alpha(a));
        const float pad = 10 * u;
        const float tpx = 14.5f * u, dpx = 12.5f * u;
        const Thumb& shape = !pv.after.empty() ? pv.after : pv.before;
        const float aspect = shape.empty() ? 1.5f : static_cast<float>(shape.width) / static_cast<float>(shape.height);
        Rect pic, text;
        if (wide) {
            const float text_h = bold_.line_height(tpx) + 3 * regular_.line_height(dpx) + 4 * u;
            float pw = area.w - 2 * pad, ph = pw / aspect;
            const float room = std::max(40 * u, area.h - 3 * pad - text_h);
            if (ph > room) {
                ph = room;
                pw = ph * aspect;
            }
            pic = {area.x + (area.w - pw) / 2, area.y + pad, pw, ph};
            text = {area.x + pad + 4 * u, pic.bottom() + pad, area.w - 2 * pad - 8 * u, area.bottom() - pic.bottom() - 2 * pad};
        } else {
            float ph = area.h - 2 * pad, pw = ph * aspect;
            if (pw > area.w * 0.6f) {
                pw = area.w * 0.6f;
                ph = pw / aspect;
            }
            pic = {area.x + pad, area.y + (area.h - ph) / 2, pw, ph};
            text = {pic.right() + pad + 4 * u, area.y + pad, area.right() - pic.right() - 2 * pad - 4 * u, area.h - 2 * pad};
        }
        pic = {std::round(pic.x), std::round(pic.y), std::round(pic.w), std::round(pic.h)};
        if (pic.w < 8 || pic.h < 8) return;
        preview_pic_ = pic;
        hits_.push_back({pic, {HitKind::Preview, -1}});

        // The two sides, each fitted into the picture's box and clipped at the divider.
        const PreviewState& st = pv.state;
        const float dx = pic.x + std::round(std::clamp(st.split, 0.f, 1.f) * pic.w);
        out.fill_rect(pic, Color::rgb(0x000000, a));
        const auto side = [&](const Thumb& src, Scaled& cache, Rect clip) {
            if (src.empty() || clip.w <= 0) return;
            const Rect place = fitted(src, pic);
            const Canvas& c = scaled(cache, src, pv.version, static_cast<int>(place.w), static_cast<int>(place.h));
            out.push_clip(clip);
            out.image(c.data(), c.width(), c.height(), place, 0, a);
            out.pop_clip();
        };
        side(pv.before, before_cache_, {pic.x, pic.y, dx - pic.x, pic.h});
        side(pv.after, after_cache_, {dx, pic.y, pic.right() - dx, pic.h});

        const float lpx = 12 * u;
        const auto chip = [&](const std::string& label, bool left) {
            if (label.empty()) return;
            const std::string shown = regular_.fit(label, lpx, pic.w * 0.45f);
            const float w = regular_.measure(shown, lpx) + 16 * u, h = 22 * u;
            const Rect c{left ? pic.x + 8 * u : pic.right() - 8 * u - w, pic.y + 8 * u, w, h};
            if (left ? c.right() > dx - 4 * u : c.x < dx + 4 * u) return;  // no room on that side
            out.fill_round_rect(c, h / 2, Color::rgb(0x000000, 0.6f * a));
            regular_.draw(out, c.x + 8 * u, center_baseline(regular_, c.y + h / 2, lpx), shown, lpx, Color::rgb(0xFFFFFF, a));
        };
        chip(pv.before_label, true);
        chip(pv.after_label, false);

        // The divider and its handle.
        const float lw = std::max(1.f, std::round(2 * u));
        out.fill_rect({dx - lw / 2, pic.y, lw, pic.h}, Color::rgb(0xFFFFFF, 0.9f * a));
        const float hy = pic.y + pic.h / 2, hr = 11 * u;
        out.fill_circle(dx, hy + u, hr + 2 * u, Color::rgb(0x000000, 0.3f * a));
        out.fill_circle(dx, hy, hr, Color::rgb(0xFFFFFF, a));
        const float k = 3.5f * u, sw = std::max(1.f, 1.6f * u);
        const Color arrow = t.accent.alpha(a);
        out.line(dx - 2 * u, hy - k, dx - 2 * u - k, hy, sw, arrow);
        out.line(dx - 2 * u - k, hy, dx - 2 * u, hy + k, sw, arrow);
        out.line(dx + 2 * u, hy - k, dx + 2 * u + k, hy, sw, arrow);
        out.line(dx + 2 * u + k, hy, dx + 2 * u, hy + k, sw, arrow);

        if (st.loupe_shown()) draw_loupe(out, pv, pic, dx, area, a, o);

        // The focused setting and what it does.
        if (text.w < 60 * u || text.h < bold_.line_height(tpx)) return;
        out.push_clip(text);
        float y = text.y + bold_.ascent(tpx);
        bold_.draw(out, text.x, y, bold_.fit(pv.title, tpx, text.w), tpx, t.text.alpha(a));
        y += bold_.line_height(tpx) * 0.25f;
        const float lh = regular_.line_height(dpx);
        int room = static_cast<int>((text.bottom() - y) / lh);
        if (!pv.note.empty()) --room;
        for (const std::string& line : wrap(regular_, pv.description, dpx, text.w, std::max(0, room))) {
            y += lh;
            regular_.draw(out, text.x, y, line, dpx, t.dim.alpha(a));
        }
        if (!pv.note.empty() && room >= 0) {
            y += lh;
            regular_.draw(out, text.x, y, regular_.fit(pv.note, dpx, text.w), dpx, t.accent.alpha(a));
        }
        out.pop_clip();
    }

    // A round loupe over the picture, `zoom` times the panel, sampling the full-size pictures
    // (nearest) so single pixels show. A finger would hide it, so with touch it sits above.
    void draw_loupe(Canvas& out, const Preview& pv, Rect pic, float dx, Rect area, float a, const ViewOptions& o) {
        const float u = u_;
        const PreviewState& st = pv.state;
        const float zoom = std::max(1.f, pv.zoom);
        const float px = pic.x + st.loupe_x * pic.w, py = pic.y + st.loupe_y * pic.h;
        const int r = static_cast<int>(std::round(std::min(62 * u, std::min(pic.w, pic.h) * 0.36f)));
        if (r < 6) return;
        float cx = px, cy = py;
        if (touch_loupe_ && o.touch) cy = py - static_cast<float>(r) - 26 * u >= area.y ? py - static_cast<float>(r) - 26 * u : py + static_cast<float>(r) + 26 * u;
        cx = std::round(cx), cy = std::round(cy);
        const int d = 2 * r;
        loupe_.resize(d, d);
        const std::uint32_t black = 0xFF000000u;
        const Rect before_at = pv.before.empty() ? Rect{} : fitted(pv.before, pic);
        const Rect after_at = pv.after.empty() ? Rect{} : fitted(pv.after, pic);
        const auto sample = [&](const Thumb& src, Rect at, float qx, float qy) {
            if (src.empty() || !at.contains(qx, qy)) return black;
            const int sx = std::clamp(static_cast<int>((qx - at.x) / at.w * static_cast<float>(src.width)), 0, src.width - 1);
            const int sy = std::clamp(static_cast<int>((qy - at.y) / at.h * static_cast<float>(src.height)), 0, src.height - 1);
            return src.pixels[static_cast<std::size_t>(sy) * static_cast<std::size_t>(src.width) + static_cast<std::size_t>(sx)] | black;
        };
        for (int j = 0; j < d; ++j) {
            std::uint32_t* line = loupe_.row(j);
            const float qy = py + (static_cast<float>(j) + 0.5f - static_cast<float>(r)) / zoom;
            for (int i = 0; i < d; ++i) {
                const float qx = px + (static_cast<float>(i) + 0.5f - static_cast<float>(r)) / zoom;
                line[i] = !pic.contains(qx, qy) ? black : qx < dx ? sample(pv.before, before_at, qx, qy) : sample(pv.after, after_at, qx, qy);
            }
        }
        // The divider, magnified too.
        const float ldx = static_cast<float>(r) + (dx - px) * zoom;
        if (ldx > -2 && ldx < static_cast<float>(d) + 2) loupe_.fill_rect({ldx - std::max(1.f, u), 0, 2 * std::max(1.f, u), static_cast<float>(d)}, Color::rgb(0xFFFFFF, 0.9f));
        out.push_clip(area);
        const float fr = static_cast<float>(r);
        out.fill_circle(cx, cy + 2 * u, fr + 4 * u, Color::rgb(0x000000, 0.35f * a));
        out.image(loupe_.data(), d, d, {cx - fr, cy - fr, static_cast<float>(d), static_cast<float>(d)}, fr, a);
        out.stroke_circle(cx, cy, fr, std::max(1.5f, 2.5f * u), Color::rgb(0xFFFFFF, a));
        const float zpx = 11 * u;
        char label[16];
        std::snprintf(label, sizeof label, "%g\xC3\x97", static_cast<double>(zoom));
        const float zw = regular_.measure(label, zpx) + 12 * u;
        const Rect z{cx - zw / 2, cy + fr - 10 * u, zw, 18 * u};
        out.fill_round_rect(z, 9 * u, Color::rgb(0x000000, 0.65f * a));
        regular_.draw(out, z.x + 6 * u, center_baseline(regular_, z.y + z.h / 2, zpx), label, zpx, Color::rgb(0xFFFFFF, a));
        out.pop_clip();
    }

    void draw_modal(Canvas& out, const Theme& t, const Page& p, Rect card, float a) {
        const float u = u_;
        out.fill_round_rect(card, 18 * u, (t.dark ? Color::rgb(0x000000, 0.45f) : Color::rgb(0xFFFFFF, 0.55f)).alpha(a));
        const float w = std::min(card.w * 0.7f, 420 * u), h = 150 * u;
        const Rect m{card.x + (card.w - w) / 2, card.y + (card.h - h) / 2, w, h};
        hits_.push_back({card, {HitKind::Outside, -1}});
        hits_.push_back({m, {HitKind::None, -1}});
        out.fill_round_rect({m.x, m.y + 4 * u, m.w, m.h + 6 * u}, 16 * u, t.shadow.alpha(a), 20 * u);
        out.fill_round_rect(m, 16 * u, (t.dark ? Color::rgb(0x232833) : Color::rgb(0xFFFFFF)).alpha(a));
        draw_icon(out, Icon::Keyboard, m.x + m.w / 2, m.y + 38 * u, 34 * u, t.accent.alpha(a));
        const float px = 17 * u;
        const std::string title = bold_.fit(p.modal, px, w - 32 * u);
        bold_.draw(out, m.x + (w - bold_.measure(title, px)) / 2, m.y + 86 * u, title, px, t.text.alpha(a));
        const float dpx = 13 * u;
        const std::string detail = regular_.fit(p.modal_detail, dpx, w - 32 * u);
        regular_.draw(out, m.x + (w - regular_.measure(detail, dpx)) / 2, m.y + 116 * u, detail, dpx, t.dim.alpha(a));
    }

    void draw_diagram(Canvas& out, const Theme& t, const Page& p, Rect area, float a) {
        const float u = u_;
        const bool psp = p.diagram == Diagram::Psp;
        const float aspect = psp ? 2.5f : 2.1f;
        float w = std::min(area.w - 16 * u, (area.h - 30 * u) * aspect);
        if (w <= 40 * u) return;
        const float h = w / aspect;
        const Rect body{area.x + (area.w - w) / 2, area.y + 22 * u, w, h};
        const Color edge = t.dim.alpha(a * 0.9f);
        const Color fill = (t.dark ? Color::rgb(0x252A34) : Color::rgb(0xE6E9EE)).alpha(a);
        const auto lit = [&](const char* name) { return p.highlight == name; };
        const auto col = [&](const char* name) { return lit(name) ? t.accent.alpha(a) : edge; };
        const float sw = std::max(1.2f, 2 * u * w / (380 * u));
        // Shoulders.
        const float sh = h * 0.12f;
        const Rect l{body.x + w * 0.06f, body.y - sh * 0.9f, w * 0.22f, sh * 1.6f};
        const Rect r{body.right() - w * 0.28f, body.y - sh * 0.9f, w * 0.22f, sh * 1.6f};
        out.fill_round_rect(l, sh * 0.6f, lit("L") ? t.accent.alpha(a) : fill);
        out.fill_round_rect(r, sh * 0.6f, lit("R") ? t.accent.alpha(a) : fill);
        out.stroke_round_rect(l, sh * 0.6f, sw, col("L"));
        out.stroke_round_rect(r, sh * 0.6f, sw, col("R"));
        out.fill_round_rect(body, h * 0.36f, fill);
        out.stroke_round_rect(body, h * 0.36f, sw, edge);
        const float sx = psp ? w * 0.27f : w * 0.29f;
        const Rect screen{body.x + sx, body.y + h * 0.12f, w - 2 * sx, h * 0.76f};
        out.fill_round_rect(screen, 6 * u, (t.dark ? Color::rgb(0x11141A) : Color::rgb(0xFAFBFC)).alpha(a));
        out.stroke_round_rect(screen, 6 * u, sw, edge);
        // D-pad.
        const float dc = body.x + w * (psp ? 0.15f : 0.135f), dy = body.y + h * (psp ? 0.4f : 0.45f);
        const float arm = h * (psp ? 0.085f : 0.11f), len = h * (psp ? 0.24f : 0.3f);
        // A plus-shaped pad; the lit arm is filled.
        const std::vector<Point> cross{{dc - arm, dy - len}, {dc + arm, dy - len}, {dc + arm, dy - arm}, {dc + len, dy - arm},
                                       {dc + len, dy + arm}, {dc + arm, dy + arm}, {dc + arm, dy + len}, {dc - arm, dy + len},
                                       {dc - arm, dy + arm}, {dc - len, dy + arm}, {dc - len, dy - arm}, {dc - arm, dy - arm}};
        out.fill_polygons({cross}, edge);
        const float in = sw;
        std::vector<Point> inner;
        for (std::size_t i = 0; i < cross.size(); ++i) {
            const Point q = cross[i];
            inner.push_back({q.x + (q.x < dc ? in : -in), q.y + (q.y < dy ? in : -in)});
        }
        out.fill_polygons({inner}, fill);
        const auto arm_lit = [&](const char* name, Rect r) {
            if (lit(name)) out.fill_round_rect(r, arm * 0.25f, t.accent.alpha(a));
        };
        arm_lit("Up", {dc - arm + in, dy - len + in, 2 * (arm - in), len - arm});
        arm_lit("Down", {dc - arm + in, dy + arm - in, 2 * (arm - in), len - arm});
        arm_lit("Left", {dc - len + in, dy - arm + in, len - arm, 2 * (arm - in)});
        arm_lit("Right", {dc + arm - in, dy - arm + in, len - arm, 2 * (arm - in)});
        const float br = h * 0.085f;
        const auto button = [&](const char* name, const char* label, float x, float y) {
            out.fill_circle(x, y, br, lit(name) ? t.accent.alpha(a) : fill);
            out.stroke_circle(x, y, br, sw, col(name));
            const float lp = br * 1.0f;
            const Color lc = lit(name) ? t.on_accent.alpha(a) : t.dim.alpha(a);
            bold_.draw(out, x - bold_.measure(label, lp) / 2, center_baseline(bold_, y, lp), label, lp, lc);
        };
        const float fx = body.right() - w * (psp ? 0.12f : 0.12f), fy = body.y + h * (psp ? 0.38f : 0.45f);
        if (psp) {
            // PlayStation symbols, drawn: the font has none of them.
            const auto symbol_colour = [&](const char* name) { return lit(name) ? t.on_accent.alpha(a) : t.dim.alpha(a); };
            const float k = br * 0.45f, lw = std::max(1.f, br * 0.16f);
            button("Triangle", "", fx, fy - br * 2.1f);
            const float ty = fy - br * 2.1f;
            out.line(fx, ty - k, fx + k, ty + k * 0.75f, lw, symbol_colour("Triangle"));
            out.line(fx + k, ty + k * 0.75f, fx - k, ty + k * 0.75f, lw, symbol_colour("Triangle"));
            out.line(fx - k, ty + k * 0.75f, fx, ty - k, lw, symbol_colour("Triangle"));
            button("Cross", "", fx, fy + br * 2.1f);
            out.line(fx - k * 0.8f, fy + br * 2.1f - k * 0.8f, fx + k * 0.8f, fy + br * 2.1f + k * 0.8f, lw, symbol_colour("Cross"));
            out.line(fx - k * 0.8f, fy + br * 2.1f + k * 0.8f, fx + k * 0.8f, fy + br * 2.1f - k * 0.8f, lw, symbol_colour("Cross"));
            button("Square", "", fx - br * 2.1f, fy);
            out.stroke_round_rect({fx - br * 2.1f - k * 0.8f, fy - k * 0.8f, k * 1.6f, k * 1.6f}, 0, lw, symbol_colour("Square"));
            button("Circle", "", fx + br * 2.1f, fy);
            out.stroke_circle(fx + br * 2.1f, fy, k * 0.9f, lw, symbol_colour("Circle"));
            const float nx = body.x + w * 0.15f, ny = body.y + h * 0.78f;
            const bool analog = p.highlight.starts_with("Analog");
            out.fill_circle(nx, ny, br * 1.1f, analog ? t.accent.alpha(a) : fill);
            out.stroke_circle(nx, ny, br * 1.1f, sw, analog ? t.accent.alpha(a) : edge);
        } else {
            button("B", "B", fx - br * 1.5f, fy + br * 0.9f);
            button("A", "A", fx + br * 1.5f, fy - br * 0.9f);
        }
        const auto pill = [&](const char* name, const char* label, float x, float y) {
            const Rect b{x - h * 0.09f, y - h * 0.035f, h * 0.18f, h * 0.07f};
            out.fill_round_rect(b, b.h / 2, lit(name) ? t.accent.alpha(a) : fill);
            out.stroke_round_rect(b, b.h / 2, sw, col(name));
            const float lp = std::max(7 * u, h * 0.055f);
            regular_.draw(out, x - regular_.measure(label, lp) / 2, b.bottom() + regular_.ascent(lp) + 2 * u, label, lp,
                          (lit(name) ? t.accent : t.dim).alpha(a));
        };
        if (psp) {
            pill("Select", "SELECT", body.right() - w * 0.21f, body.y + h * 0.72f);
            pill("Start", "START", body.right() - w * 0.1f, body.y + h * 0.72f);
        } else {
            pill("Select", "SELECT", body.x + w * 0.08f, body.y + h * 0.8f);
            pill("Start", "START", body.x + w * 0.2f, body.y + h * 0.8f);
        }
    }

    const Font& regular_;
    const Font& bold_;
    Canvas sharp_, frosted_;
    bool bg_dark_ = true;
    std::vector<std::pair<Rect, Hit>> hits_;
    Hit hover_;
    Rect preview_pic_;  // where the last render put the preview picture (empty: none)
    Scaled before_cache_, after_cache_;
    Canvas loupe_;
    bool split_drag_ = false, finger_loupe_ = false, touch_loupe_ = false;
    float u_ = 1, row_h_ = 40;
    float open_ = 0, target_open_ = 0;
    float fade_ = 1;
    float tab_y_ = 0;
    float sel_y_ = 0, sel_target_ = 0;
    float scroll_ = 0, scroll_target_ = 0;
    bool manual_ = false, first_ = true, sel_jump_ = true;
    int last_cursor_ = -1;
    int last_tab_ = -1;
    std::string last_title_, last_crumb_;
};

}  // namespace anyui
