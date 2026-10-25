// SPDX-License-Identifier: GPL-2.0-or-later

#include "display_loop.hpp"

#include "desktop.hpp"
#include "gl_host.hpp"
#include "speed.hpp"

#include "kernel.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <iostream>

namespace anypsp::desktop {

DisplayLoop::DisplayLoop(gpu::GlHost &gl, AudioOut *audio, display::Settings settings, std::filesystem::path save_path,
                         std::string game_name)
    : gl_(gl), audio_(audio), settings_(settings), save_path_(std::move(save_path)), name_(std::move(game_name)) {}

void DisplayLoop::changed(const std::string &what) {
    notice_ = what;
    std::cout << "[anypsp] " << what << "\n";
    try {
        display::save(save_path_, settings_);
    } catch (const std::exception &e) {
        std::cerr << "[anypsp] " << e.what() << "\n";
    }
    cadence_.reset();
}

void DisplayLoop::hotkeys(const std::vector<input::Action> &pressed) {
    using input::Action;
    const auto on_off = [](bool b) { return std::string(b ? "on" : "off"); };
    const auto number = [](int v, const char *zero) { return v == 0 ? std::string(zero) : std::to_string(v); };
    for (const Action a : pressed) {
        switch (a) {
        case Action::Smoothing:
            settings_.smoothing = !settings_.smoothing;
            changed("smoothing " + on_off(settings_.smoothing));
            break;
        case Action::Interpolation:
            settings_.interpolate = !settings_.interpolate;
            settings_.interpolate_auto = false;
            // The second renderer only exists when the game started with interpolation on.
            changed("frame interpolation " + on_off(settings_.interpolate) +
                    (settings_.interpolate && !gl_.has_in_between() ? " from the next start" : ""));
            break;
        case Action::Refresh:
            settings_.refresh = display::next_value(display::kRefreshRates, settings_.refresh);
            changed("refresh " + number(settings_.refresh, "auto"));
            break;
        case Action::Vrr:
            settings_.vrr = !settings_.vrr;
            changed("VRR " + on_off(settings_.vrr));
            break;
        case Action::SyncToMonitor:
            settings_.sync_to_monitor = !settings_.sync_to_monitor;
            changed("sync to monitor " + on_off(settings_.sync_to_monitor));
            break;
        case Action::FpsCap:
            settings_.fps_cap = display::next_value(display::kFpsCaps, settings_.fps_cap);
            changed("fps cap " + number(settings_.fps_cap, "off"));
            break;
        case Action::PauseFps:
            settings_.menu_fps = display::next_value(display::kPauseFps, settings_.menu_fps);
            changed("menu fps " + std::to_string(settings_.menu_fps));
            break;
        default:
            break;
        }
    }
}

double DisplayLoop::monitor_hz() {
    const std::uint64_t now = SDL_GetTicksNS();
    if (refresh_checked_ns_ == 0u || now - refresh_checked_ns_ > 1'000'000'000u) {
        refresh_checked_ns_ = now;
        detected_hz_ = gl_.refresh_rate();
    }
    if (settings_.refresh != 0) return settings_.refresh;
    return detected_hz_ > 0.0 ? detected_hz_ : 60.0;
}

void DisplayLoop::update_pacing() {
    const double hz = monitor_hz();
    const bool paced = !settings_.vrr;
    const int interval = display::vsync_interval(hz, settings_.fps_cap);
    // Vsync only keeps time when the rate pacing assumes is the monitor's real one.
    int swap = paced && display::same_rate(hz, detected_hz_) ? interval : 0;
    if (swap != swap_interval_) {
        if (!gl_.set_swap_interval(swap)) {
            swap = 0;
            gl_.set_swap_interval(0);
        }
        swap_interval_ = swap;
    }
    if (!paced) {
        cadence_.reset();
        sync_factor_ = 1.0;
        return;
    }
    const double shown = hz / interval;
    const double game = settings_.sync_to_monitor ? display::synced_game_hz(display::kPspHz, shown) : display::kPspHz;
    if (!cadence_ || cadence_->game_hz() != game || cadence_->display_hz() != shown) {
        cadence_.emplace(game, shown);
        cad_frame_ = cad_vblank_ = cad_base_ns_ = 0;
    }
    sync_factor_ = game / display::kPspHz;
}

void DisplayLoop::present_paced(bool blend) {
    ++cad_frame_;
    const std::uint64_t until = cadence_->first_vblank(cad_frame_);
    const double period = 1e9 / cadence_->display_hz();
    const auto due = [&](std::uint64_t vblank) { return cad_base_ns_ + static_cast<std::uint64_t>(period * static_cast<double>(vblank)); };
    while (cad_vblank_ < until) {
        std::uint64_t now = SDL_GetTicksNS();
        if (cad_base_ns_ == 0u || now > due(cad_vblank_) + 250'000'000u)
            cad_base_ns_ = now - static_cast<std::uint64_t>(period * static_cast<double>(cad_vblank_)); // start, or a long stall
        if (cad_vblank_ + 1u < until && now > due(cad_vblank_ + 1u)) {
            ++cad_vblank_; // this vblank has passed: skip its picture, keep the game's speed
            continue;
        }
        gl_.show(blend);
        ++shown_;
        ++cad_vblank_;
        now = SDL_GetTicksNS();
        const std::uint64_t next = due(cad_vblank_);
        if (now < next && (swap_interval_ == 0 || static_cast<double>(next - now) > period / 2)) {
            SDL_DelayPrecise(next - now); // no vsync, or the swap did not wait for it
        } else if (swap_interval_ != 0 && static_cast<double>(now) < static_cast<double>(next) + period / 2) {
            // Vsync keeps the time: follow the display's clock rather than the timer's.
            cad_base_ns_ = now - static_cast<std::uint64_t>(period * static_cast<double>(cad_vblank_));
        }
    }
}

void DisplayLoop::after_vblank(const pspweb::DisplayState &d, int vblanks) {
    vblanks_ += static_cast<std::uint64_t>(vblanks);
    update_pacing();
    bool blend = false;
    if (d.buffer_changes != shown_changes_) {
        // A new game frame: with smoothing, the vblank it appears on shows it blended with the
        // frame before when that one was up for two vblanks (the game draws at 30 fps).
        const unsigned previous = frame_vblanks_;
        shown_changes_ = d.buffer_changes;
        frame_vblanks_ = 1;
        if (settings_.interpolate && speed_ == 100 && previous == 2u && gl_.has_in_between()) {
            // The in-between picture, unless it is a cut away from the new frame.
            gl_.render_output(d.framebuffer, d.stride, d.format);
            gl_.keep_previous();
            gl_.render_in_between(d.framebuffer, d.stride, d.format);
            if (!display::in_between_on_new_frame(true, previous, gl_.difference()))
                gl_.render_output(d.framebuffer, d.stride, d.format);
        } else if (settings_.smoothing && speed_ == 100 && previous == 2u) {
            gl_.keep_previous();
            gl_.render_output(d.framebuffer, d.stride, d.format);
            blend = display::blend_on_new_frame(true, previous, gl_.difference());
        } else {
            gl_.render_output(d.framebuffer, d.stride, d.format);
        }
    } else {
        ++frame_vblanks_;
        gl_.render_output(d.framebuffer, d.stride, d.format);
    }
    if (cadence_) {
        present_paced(blend);
        if (audio_ != nullptr) {
            // Paced by the display: steer the sound queue by playback speed instead of waiting.
            constexpr std::size_t target = kSampleRate * 4u * 5u / 100u; // 50 ms
            audio_->set_ratio(display::audio_ratio(sync_factor_, audio_->queued_bytes(), target) * speed::audio_rate(speed_));
        }
    } else {
        // Variable refresh: present each vblank as soon as it is ready, timed at 59.94 Hz.
        if (audio_ != nullptr) audio_->set_ratio(speed::audio_rate(speed_));
        const std::uint64_t now = SDL_GetTicksNS();
        if (free_next_ns_ == 0u || now > free_next_ns_ + 4u * kFrameNs) free_next_ns_ = now;
        if (free_next_ns_ > now) SDL_DelayPrecise(free_next_ns_ - now);
        free_next_ns_ += kFrameNs;
        const std::uint64_t shown_at = SDL_GetTicksNS();
        if (settings_.fps_cap == 0 || shown_at - last_show_ns_ >= 950'000'000u / static_cast<unsigned>(settings_.fps_cap)) {
            gl_.show(blend);
            ++shown_;
            last_show_ns_ = shown_at;
        }
    }
    update_title(d.buffer_changes);
}

void DisplayLoop::present_now(const pspweb::DisplayState &d, int vblanks) {
    vblanks_ += static_cast<std::uint64_t>(vblanks);
    if (audio_ != nullptr) audio_->set_ratio(1.0);
    gl_.render_output(d.framebuffer, d.stride, d.format);
    shown_changes_ = d.buffer_changes;
    frame_vblanks_ = 1;
    gl_.show(false);
    ++shown_;
    update_title(d.buffer_changes);
}

void DisplayLoop::update_title(std::uint64_t game_frames) {
    const std::uint64_t now = SDL_GetTicksNS();
    if (title_ns_ != 0u && now - title_ns_ < 1'000'000'000u) return;
    const double seconds = title_ns_ == 0u ? 0.0 : static_cast<double>(now - title_ns_) / 1e9;
    const auto rate = [seconds](std::uint64_t count) { return seconds > 0.0 ? static_cast<double>(count) / seconds : 0.0; };
    const double hz = monitor_hz();
    const char *pacing = settings_.vrr ? "VRR" : swap_interval_ != 0 ? "vsync" : "timer";
    std::string extras;
    if (cadence_ && sync_factor_ != 1.0) extras += " / synced";
    if (settings_.smoothing) extras += " / smoothing";
    if (speed_ != 100) extras += " / speed " + speed::format(speed_);
    if (settings_.interpolate && gl_.has_in_between()) extras += " / interpolated";
    if (!notice_.empty()) extras += " - " + notice_;
    char title[256];
    std::snprintf(title, sizeof title, "AnyPSP - %s - %.0f Hz %s %s / game %.0f fps / shown %.0f fps / %.0f vblank/s%s",
                  name_.c_str(), hz, settings_.refresh != 0 ? "set" : "auto", pacing, rate(game_frames - title_changes_),
                  rate(shown_ - title_shown_), rate(vblanks_ - title_vblanks_), extras.c_str());
    SDL_SetWindowTitle(gl_.window(), title);
    notice_.clear();
    if (seconds > 0.0) {
        vblank_rate_ = rate(vblanks_ - title_vblanks_);
        game_fps_ = rate(game_frames - title_changes_);
    }
    title_ns_ = now;
    title_vblanks_ = vblanks_;
    title_shown_ = shown_;
    title_changes_ = game_frames;
}

} // namespace anypsp::desktop
