#pragma once

// Display settings of the desktop host (docs/user/desktop.md): the anypsp-display.ini file,
// its command-line overrides, and the decisions of smoothing. Independent of SDL and GL so it
// can be unit tested.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace anypsp::display {

struct Settings {
    // A 50/50 blend on the vblank between two frames of a game drawing at 30 fps.
    bool smoothing = false;
    // Frame interpolation: the vblank between two frames of a game drawing at 30 fps shows the
    // game's next picture with everything moved half way back (host/ge_interp.hpp). Read at start.
    bool interpolate = false;
    int refresh = 0;               // monitor refresh in Hz pacing assumes; 0 = detect (Auto)
    bool vrr = false;              // variable refresh monitor: present each vblank when it is ready
    bool sync_to_monitor = false;  // run the PSP's vblanks at the monitor's rate when within 1%
    int menu_fps = 30;             // pictures per second while paused
    int fps_cap = 0;               // most pictures per second shown; 0 = no cap

    bool operator==(const Settings &) const = default;
};

inline constexpr const char *kFileName = "anypsp-display.ini";
inline constexpr int kRefreshRates[] = {0, 60, 75, 90, 120, 144, 165, 240};
inline constexpr int kPauseFps[] = {30, 60};
inline constexpr int kFpsCaps[] = {0, 30, 60, 120};
inline constexpr double kPspHz = 1e9 / 16683000.0; // the kernel's vblank rate, 59.94 Hz

// Steps a setting to the next allowed value (hotkeys).
[[nodiscard]] int next_value(std::span<const int> allowed, int value);

// Applies `text` (ini format, section [display]) over `settings`. Throws psprecomp::Error
// naming `source` and the line on any mistake.
void apply_ini(Settings &settings, const std::string &text, const std::string &source);
[[nodiscard]] std::string to_ini(const Settings &settings);
// One setting by its ini name (case-insensitive), as the command line passes it.
void apply_setting(Settings &settings, const std::string &name, const std::string &value, const std::string &where);

// The settings file next to the program, else the one in the per-user data folder (which is
// also where changes made while playing are saved).
[[nodiscard]] std::filesystem::path find_file(const std::filesystem::path &program_dir, const std::filesystem::path &user_dir);
[[nodiscard]] Settings load(const std::filesystem::path &path); // defaults when it does not exist
void save(const std::filesystem::path &path, const Settings &settings);

// Mean absolute difference of two equally sized RGBA pictures over R, G and B: 0 same, 1 opposite.
[[nodiscard]] double mean_difference(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b);
// Pictures differing by more than this are a scene cut: no blend across it.
inline constexpr double kSceneCut = 0.10;
// Whether the vblank where a new game frame appears shows a blend of it with the frame before:
// only when the frame before was on screen for exactly two vblanks (the game draws at 30 fps)
// and the two are not a scene cut.
[[nodiscard]] bool blend_on_new_frame(bool smoothing, unsigned previous_frame_vblanks, double difference);
// Whether that vblank shows the in-between picture instead: when the frame before was up for
// exactly two vblanks and the in-between picture is not a cut away from the new frame (which
// means the pairing went wrong, so the new frame is shown as it is).
[[nodiscard]] bool in_between_on_new_frame(bool interpolate, unsigned previous_frame_vblanks, double difference);

// Counts how long each game frame stays up, for the decisions above.
class FrameTracker {
public:
    // Call after each PSP vblank with the flip count; returns how many vblanks the frame before
    // was up when a new game frame appeared on this one, else 0.
    unsigned after_vblank(std::uint64_t buffer_changes);

private:
    std::uint64_t changes_ = 0;
    unsigned vblanks_ = 0;
};

// Frame pacing against the monitor: each PSP vblank is shown from the first display vblank at or
// after its time, so it stays up for an evenly spread number of display vblanks.
class Cadence {
public:
    Cadence(double game_hz, double display_hz); // both positive
    // Display vblank, counted from 0, on which game vblank `frame` first shows.
    [[nodiscard]] std::uint64_t first_vblank(std::uint64_t frame) const;
    [[nodiscard]] unsigned vblanks(std::uint64_t frame) const {
        return static_cast<unsigned>(first_vblank(frame + 1) - first_vblank(frame));
    }
    [[nodiscard]] double game_hz() const noexcept { return game_hz_; }
    [[nodiscard]] double display_hz() const noexcept { return display_hz_; }

private:
    double game_hz_, display_hz_;
    std::uint64_t game_uhz_, display_uhz_;
};

// The rate to run the PSP's vblanks at with "sync to monitor": the monitor's rate divided by 1-4
// when that is within `tolerance` of 59.94, else 59.94.
[[nodiscard]] double synced_game_hz(double game_hz, double display_hz, double tolerance = 0.01);
// Present every interval-th vblank so at most `cap` pictures a second are shown (0: no cap).
[[nodiscard]] int vsync_interval(double display_hz, int cap);
[[nodiscard]] bool same_rate(double a, double b); // within 1%
// Playback speed for the sound (1 = as emulated) while paced by the display: the sync speed-up,
// nudged by at most 0.5% to keep the queue near `target` bytes.
[[nodiscard]] double audio_ratio(double sync_factor, std::size_t queued, std::size_t target);

} // namespace anypsp::display
