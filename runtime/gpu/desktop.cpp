// SPDX-License-Identifier: GPL-2.0-or-later

#include "desktop.hpp"

#include "gl_host.hpp"

#include "psprecomp/common.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>

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

bool Controls::poll(gpu::GlHost &gl) {
    SDL_Event event;
    bool open = true;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            open = false;
            break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.repeat) break;
            if (std::find(map_.keys(input::Action::Fullscreen).begin(), map_.keys(input::Action::Fullscreen).end(),
                          static_cast<int>(event.key.scancode)) != map_.keys(input::Action::Fullscreen).end())
                gl.set_fullscreen(!gl.fullscreen());
            for (const input::Action a : {input::Action::Smoothing}) {
                const auto &keys = map_.keys(a);
                if (std::find(keys.begin(), keys.end(), static_cast<int>(event.key.scancode)) != keys.end()) hotkeys_.push_back(a);
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            const auto &bound = map_.buttons(input::Action::Fullscreen);
            if (std::find(bound.begin(), bound.end(), static_cast<int>(event.gbutton.button)) != bound.end())
                gl.set_fullscreen(!gl.fullscreen());
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

std::vector<input::Action> Controls::take_hotkeys() { return std::exchange(hotkeys_, {}); }

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
