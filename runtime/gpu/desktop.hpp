#pragma once

// Pieces of the playable desktop host: SDL3 audio output that never builds up
// latency, a WAV recorder for headless runs, frame pacing at the PSP's
// 59.94 Hz, and the event pump that turns keyboard and gamepads into PSP input.

#include "input_map.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

struct SDL_AudioStream;
struct SDL_Gamepad;

namespace anypsp::gpu {
class GlHost;
}

namespace anypsp::desktop {

inline constexpr std::uint32_t kSampleRate = 44100u;
inline constexpr std::uint64_t kFrameNs = 16683000u; // 59.94 Hz, the kernel's vblank period

// Upper bound on queued output: past it, new audio is dropped so latency stays
// bounded when the host runs ahead of the sound card.
inline constexpr std::size_t kMaxQueuedBytes = kSampleRate * 4u / 10u; // 100 ms of stereo s16
[[nodiscard]] constexpr bool accept_audio(std::size_t queued_bytes) noexcept { return queued_bytes <= kMaxQueuedBytes; }

class AudioOut {
public:
    AudioOut() = default;
    ~AudioOut();
    AudioOut(const AudioOut &) = delete;
    AudioOut &operator=(const AudioOut &) = delete;

    // Opens the default playback device for 44.1 kHz stereo s16. Returns false
    // (with a message on stderr) when there is no usable device; the game then runs silent.
    bool open();
    // Scales what push() plays (0 = silent, 1 = as the game made it).
    void set_gain(float gain) noexcept { gain_ = gain; }
    void push(const std::int16_t *samples, std::uint32_t frames);
    [[nodiscard]] std::uint64_t dropped_frames() const noexcept { return dropped_; }
    // Playback speed (1 = 44.1 kHz as pushed): resamples to follow the display's clock.
    void set_ratio(double ratio);
    [[nodiscard]] std::size_t queued_bytes() const;

private:
    SDL_AudioStream *stream_{};
    bool started_{};
    std::uint64_t dropped_{};
    double ratio_{1.0};
    float gain_{1.f};
    std::vector<std::int16_t> scaled_;
};

// 44.1 kHz stereo s16 WAV file; the header is completed by close() or the destructor.
class WavWriter {
public:
    explicit WavWriter(const std::filesystem::path &path);
    ~WavWriter();
    WavWriter(const WavWriter &) = delete;
    WavWriter &operator=(const WavWriter &) = delete;
    void write(const std::int16_t *samples, std::uint32_t frames);
    void close();
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }

private:
    std::filesystem::path path_;
    std::ofstream out_;
    std::uint64_t frames_{};
};

// Sleeps so successive calls of wait() are one PSP frame apart. Falls back to
// the current time when the host is more than a few frames behind instead of
// rushing to catch up.
class FramePacer {
public:
    void wait();

private:
    std::uint64_t next_ns_{};
};

// A mouse or touch action in window pixels.
struct Pointer {
    enum class Kind : std::uint8_t { Move, Down, Up, Wheel };
    Kind kind{Kind::Move};
    float x{}, y{};
    float wheel{};  // notches, positive toward the user (scroll down)
    bool touch{};
    std::int64_t finger{};
};

// What the last poll() saw for the in-game menu.
struct UiEvents {
    std::vector<int> keys;     // SDL scancodes pressed (no auto-repeat)
    std::vector<int> buttons;  // gamepad buttons pressed, triggers as input::kLeftTrigger/kRightTrigger
    std::vector<Pointer> pointers;
    bool menu{};               // the Menu binding (Escape, F1) or Back+Start on a gamepad
};

// Starts this program again with the same command line, for Reset in the menu. Returns
// false (with a message on stderr) when it cannot.
bool relaunch(int argc, char **argv);

// Window events, hotkeys and input devices for the visible window.
class Controls {
public:
    // Initialises SDL's gamepad subsystem (video must already be up).
    explicit Controls(input::InputMap map);
    ~Controls();
    Controls(const Controls &) = delete;
    Controls &operator=(const Controls &) = delete;

    // Handles pending events. Returns false when the user closed the window.
    bool poll(gpu::GlHost &gl);
    [[nodiscard]] input::PadState pad() const;
    // True while a key or gamepad button bound to `a` is held (FastForward).
    [[nodiscard]] bool held(input::Action a) const;
    // Display hotkeys (Smoothing...) pressed since the last call; Fullscreen is handled by poll.
    [[nodiscard]] std::vector<input::Action> take_hotkeys();
    [[nodiscard]] bool menu_requested() const noexcept { return ui_.menu; }
    UiEvents take_ui_events() { return std::exchange(ui_, {}); }
    [[nodiscard]] bool shift_held() const;
    // The bindings and [player] settings in use; the menu changes them.
    [[nodiscard]] input::InputMap &map() noexcept { return map_; }

private:
    static bool key_held(int code);
    [[nodiscard]] bool button_held(int b) const;

    input::InputMap map_;
    std::vector<SDL_Gamepad *> pads_;
    std::vector<input::Action> hotkeys_;
    bool started_{};
    UiEvents ui_;
    bool triggers_[2]{};
};

} // namespace anypsp::desktop
