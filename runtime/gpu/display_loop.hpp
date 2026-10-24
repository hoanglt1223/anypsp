#pragma once

// The visible window's side of each PSP vblank: frame pacing against the monitor, smoothing,
// pause, the display hotkeys and the window title (docs/user/desktop.md#display-settings).

#include "display.hpp"
#include "input_map.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pspweb {
struct DisplayState;
}

namespace anypsp::gpu {
class GlHost;
}

namespace anypsp::desktop {

class AudioOut;

class DisplayLoop {
public:
    // `audio` may be null (muted). Changes made with hotkeys are saved to `save_path`.
    DisplayLoop(gpu::GlHost &gl, AudioOut *audio, display::Settings settings, std::filesystem::path save_path,
                std::string game_name);

    void hotkeys(const std::vector<input::Action> &pressed);
    // The settings in use; the menu edits them, then calls settings_changed.
    [[nodiscard]] display::Settings &settings() noexcept { return settings_; }
    void settings_changed() { changed("display settings changed"); }
    // Rates over the last second, for the menu's badge: PSP vblanks and game frames per second.
    [[nodiscard]] double vblank_rate() const noexcept { return vblank_rate_; }
    [[nodiscard]] double game_fps() const noexcept { return game_fps_; }
    // After each emulated PSP vblank: shows it on the display's vblanks (or when ready with
    // VRR) and waits so the game keeps its rate. `game_frames` is the flip count (for the title).
    void after_vblank(const pspweb::DisplayState &d);

private:
    void update_pacing();
    double monitor_hz();
    void present_paced(bool blend);
    void update_title(std::uint64_t game_frames);
    void changed(const std::string &what);

    gpu::GlHost &gl_;
    AudioOut *audio_;
    display::Settings settings_;
    std::filesystem::path save_path_;
    std::string name_;
    double vblank_rate_ = 0, game_fps_ = 0;

    std::optional<display::Cadence> cadence_;
    std::uint64_t cad_frame_ = 0, cad_vblank_ = 0, cad_base_ns_ = 0;
    int swap_interval_ = 0;
    double detected_hz_ = 0;
    std::uint64_t refresh_checked_ns_ = 0;
    double sync_factor_ = 1.0;
    std::uint64_t free_next_ns_ = 0;  // VRR pacing at the game's rate
    std::uint64_t last_show_ns_ = 0;

    std::uint64_t shown_changes_ = 0;  // smoothing: flip count of the frame on screen
    unsigned frame_vblanks_ = 0;       // PSP vblanks it has been up
    bool blend_ = false;

    std::uint64_t title_ns_ = 0, title_vblanks_ = 0, title_shown_ = 0, title_changes_ = 0;
    std::uint64_t vblanks_ = 0, shown_ = 0;
    std::string notice_;
};

} // namespace anypsp::desktop
