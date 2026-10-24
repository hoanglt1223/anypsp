// SPDX-License-Identifier: GPL-2.0-or-later

#include "desktop.hpp"

#include "gl_host.hpp"

#include "psprecomp/common.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace anypsp::desktop {
namespace {

void put_le(std::ofstream &out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out.put(static_cast<char>((value >> (8 * i)) & 0xFFu));
}

} // namespace

AudioOut::~AudioOut() {
    if (stream_ != nullptr) SDL_DestroyAudioStream(stream_);
    if (started_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool AudioOut::open() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::cerr << "[anypsp] no audio: " << SDL_GetError() << "\n";
        return false;
    }
    started_ = true;
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(kSampleRate)};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream_ == nullptr) {
        std::cerr << "[anypsp] no audio device: " << SDL_GetError() << "\n";
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
        std::cerr << "[anypsp] cannot start audio: " << SDL_GetError() << "\n";
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
        return false;
    }
    return true;
}

void AudioOut::push(const std::int16_t *samples, std::uint32_t frames) {
    if (stream_ == nullptr || frames == 0u) return;
    if (gain_ < 0.999f) {
        scaled_.resize(static_cast<std::size_t>(frames) * 2u);
        for (std::size_t i = 0; i < scaled_.size(); ++i)
            scaled_[i] = static_cast<std::int16_t>(std::lround(static_cast<float>(samples[i]) * std::max(gain_, 0.f)));
        samples = scaled_.data();
    }
    const int queued = SDL_GetAudioStreamQueued(stream_);
    if (queued < 0 || !accept_audio(static_cast<std::size_t>(queued))) {
        dropped_ += frames;
        return;
    }
    if (!SDL_PutAudioStreamData(stream_, samples, static_cast<int>(frames * 4u))) dropped_ += frames;
}

WavWriter::WavWriter(const std::filesystem::path &path) : path_(path), out_(path, std::ios::binary | std::ios::trunc) {
    if (!out_) throw psprecomp::Error("Cannot write " + path.string());
    out_.write("RIFF\0\0\0\0WAVEfmt ", 16);
    put_le(out_, 16u, 4);
    put_le(out_, 1u, 2);                // PCM
    put_le(out_, 2u, 2);                // stereo
    put_le(out_, kSampleRate, 4);
    put_le(out_, kSampleRate * 4u, 4);  // bytes per second
    put_le(out_, 4u, 2);                // block align
    put_le(out_, 16u, 2);               // bits per sample
    out_.write("data\0\0\0\0", 8);
}

WavWriter::~WavWriter() {
    try {
        close();
    } catch (const std::exception &e) {
        std::cerr << "[anypsp] " << e.what() << "\n";
    }
}

void WavWriter::write(const std::int16_t *samples, std::uint32_t frames) {
    if (!out_.is_open()) return;
    for (std::uint32_t i = 0; i < frames * 2u; ++i) put_le(out_, static_cast<std::uint16_t>(samples[i]), 2);
    frames_ += frames;
}

void WavWriter::close() {
    if (!out_.is_open()) return;
    const std::uint64_t data = frames_ * 4u;
    if (data > 0xFFFFFFFFull - 36u) throw psprecomp::Error(path_.string() + ": WAV larger than 4 GiB");
    out_.seekp(4);
    put_le(out_, static_cast<std::uint32_t>(36u + data), 4);
    out_.seekp(40);
    put_le(out_, static_cast<std::uint32_t>(data), 4);
    out_.close();
    if (out_.fail()) throw psprecomp::Error("Failed writing " + path_.string());
}

void FramePacer::wait() {
    const std::uint64_t now = SDL_GetTicksNS();
    if (next_ns_ == 0u || now > next_ns_ + 4u * kFrameNs) next_ns_ = now;
    if (next_ns_ > now) SDL_DelayPrecise(next_ns_ - now);
    next_ns_ += kFrameNs;
}

Controls::Controls(input::InputMap map) : map_(std::move(map)) {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        std::cerr << "[anypsp] no gamepad support: " << SDL_GetError() << "\n";
        return;
    }
    started_ = true;
    // Gamepads already connected arrive as SDL_EVENT_GAMEPAD_ADDED on the first poll.
}

Controls::~Controls() {
    for (SDL_Gamepad *pad : pads_) SDL_CloseGamepad(pad);
    if (started_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

bool relaunch(int argc, char **argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    std::wstring command = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        std::cerr << "[anypsp] cannot start again: error " << GetLastError() << "\n";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    if (argc < 1) return false;
    const pid_t child = fork();
    if (child < 0) {
        std::cerr << "[anypsp] cannot start again\n";
        return false;
    }
    if (child == 0) {
        execv("/proc/self/exe", argv);
        execvp(argv[0], argv);
        _exit(127);
    }
    return true;
#endif
}

bool Controls::shift_held() const {
    const bool *keys = SDL_GetKeyboardState(nullptr);
    return keys != nullptr && (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]);
}

bool Controls::poll(gpu::GlHost &gl) {
    SDL_Event event;
    bool open = true;
    ui_ = {};
    int window_w = 0, window_h = 0, points_w = 0, points_h = 0;
    if (gl.window() != nullptr) {
        SDL_GetWindowSizeInPixels(gl.window(), &window_w, &window_h);
        SDL_GetWindowSize(gl.window(), &points_w, &points_h);
    }
    const float px = points_w > 0 ? static_cast<float>(window_w) / static_cast<float>(points_w) : 1.f;
    const float py = points_h > 0 ? static_cast<float>(window_h) / static_cast<float>(points_h) : 1.f;
    const auto pointer = [&](Pointer p) {
        if (ui_.pointers.size() < 256) ui_.pointers.push_back(p);
    };
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            open = false;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (event.motion.which != SDL_TOUCH_MOUSEID)
                pointer({Pointer::Kind::Move, event.motion.x * px, event.motion.y * py, 0, false, 0});
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.which != SDL_TOUCH_MOUSEID && event.button.button == SDL_BUTTON_LEFT)
                pointer({event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? Pointer::Kind::Down : Pointer::Kind::Up, event.button.x * px,
                         event.button.y * py, 0, false, 0});
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (event.wheel.which != SDL_TOUCH_MOUSEID)
                pointer({Pointer::Kind::Wheel, event.wheel.mouse_x * px, event.wheel.mouse_y * py,
                         event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? event.wheel.y : -event.wheel.y, false, 0});
            break;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {
            const bool cancelled = event.type == SDL_EVENT_FINGER_CANCELED;
            pointer({event.type == SDL_EVENT_FINGER_DOWN     ? Pointer::Kind::Down
                     : event.type == SDL_EVENT_FINGER_MOTION ? Pointer::Kind::Move
                                                             : Pointer::Kind::Up,
                     cancelled ? -1.f : event.tfinger.x * static_cast<float>(window_w),
                     cancelled ? -1.f : event.tfinger.y * static_cast<float>(window_h), 0, true,
                     static_cast<std::int64_t>(event.tfinger.fingerID)});
            break;
        }
        case SDL_EVENT_KEY_DOWN: {
            if (event.key.repeat) break;
            const auto &menu_keys = map_.keys(input::Action::Menu);
            if (std::find(menu_keys.begin(), menu_keys.end(), static_cast<int>(event.key.scancode)) != menu_keys.end())
                ui_.menu = true;
            if (ui_.keys.size() < 64) ui_.keys.push_back(static_cast<int>(event.key.scancode));
            if (std::find(map_.keys(input::Action::Fullscreen).begin(), map_.keys(input::Action::Fullscreen).end(),
                          static_cast<int>(event.key.scancode)) != map_.keys(input::Action::Fullscreen).end())
                gl.set_fullscreen(!gl.fullscreen());
            break;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            const int button = static_cast<int>(event.gbutton.button);
            const auto &bound = map_.buttons(input::Action::Fullscreen);
            if (std::find(bound.begin(), bound.end(), button) != bound.end())
                gl.set_fullscreen(!gl.fullscreen());
            const auto &menu_buttons = map_.buttons(input::Action::Menu);
            if (std::find(menu_buttons.begin(), menu_buttons.end(), button) != menu_buttons.end()) ui_.menu = true;
            // Back+Start: the other button of the chord is already held on this gamepad.
            if (SDL_Gamepad *pad = SDL_GetGamepadFromID(event.gbutton.which)) {
                if ((button == SDL_GAMEPAD_BUTTON_START && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK)) ||
                    (button == SDL_GAMEPAD_BUTTON_BACK && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START)))
                    ui_.menu = true;
            }
            if (ui_.buttons.size() < 64) ui_.buttons.push_back(button);
            break;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            const auto axis = event.gaxis.axis;
            if (axis != SDL_GAMEPAD_AXIS_LEFT_TRIGGER && axis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) break;
            bool &was = triggers_[axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 0 : 1];
            const bool now = event.gaxis.value > 16384;
            if (now && !was && ui_.buttons.size() < 64)
                ui_.buttons.push_back(axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? input::kLeftTrigger : input::kRightTrigger);
            was = now;
            break;
        }
        case SDL_EVENT_GAMEPAD_ADDED:
            if (SDL_Gamepad *pad = SDL_OpenGamepad(event.gdevice.which)) {
                pads_.push_back(pad);
                std::cout << "[anypsp] gamepad: " << (SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "?") << "\n";
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            for (auto it = pads_.begin(); it != pads_.end(); ++it)
                if (SDL_GetGamepadID(*it) == event.gdevice.which) {
                    SDL_CloseGamepad(*it);
                    pads_.erase(it);
                    break;
                }
            break;
        default:
            break;
        }
    }
    return open;
}

input::PadState Controls::pad() const {
    const bool *keys = SDL_GetKeyboardState(nullptr);
    const auto key = [keys](int code) { return keys != nullptr && keys[code]; };
    const auto button = [this](int b) {
        for (SDL_Gamepad *pad : pads_) {
            if (b == input::kLeftTrigger || b == input::kRightTrigger) {
                const auto axis = b == input::kLeftTrigger ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                if (SDL_GetGamepadAxis(pad, axis) > 16384) return true;
            } else if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b))) {
                return true;
            }
        }
        return false;
    };
    // The stick pushed furthest wins when several gamepads are connected.
    int x = 0, y = 0;
    for (SDL_Gamepad *pad : pads_) {
        const int px = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), py = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (std::abs(px) + std::abs(py) > std::abs(x) + std::abs(y)) {
            x = px;
            y = py;
        }
    }
    return map_.pad(key, button, x, y);
}

} // namespace anypsp::desktop
