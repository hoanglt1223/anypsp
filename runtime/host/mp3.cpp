// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceMp3: MP3 streams the game feeds through a buffer, decoded a frame per call with minimp3
// (runtime/third_party/minimp3, CC0). The stream buffer starts with a 0x5C0-byte work area and
// is then filled in two halves; a half opens up for new data only once the decoder has read
// past it. Semantics and error codes follow PPSSPP (Core/HLE/sceMp3.cpp and the AuCtx part of
// Core/HW/SimpleAudioDec.cpp, GPL-2.0-or-later).

#include "kernel.hpp"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace pspweb {
namespace {

constexpr std::uint32_t kErrorInvalidHandle = 0x80671001u;
constexpr std::uint32_t kErrorBadAddr = 0x80671002u;
constexpr std::uint32_t kErrorBadSize = 0x80671003u;
constexpr std::uint32_t kErrorUnreservedHandle = 0x80671102u;
constexpr std::uint32_t kErrorNotYetInitHandle = 0x80671103u;
constexpr std::uint32_t kErrorNoResource = 0x80671201u;
constexpr std::uint32_t kErrorUnsupportedVersion = 0x80671301u;
constexpr std::uint32_t kErrorBadSampleRate = 0x80671302u;
constexpr std::uint32_t kErrorBadResetFrame = 0x80671501u;
constexpr std::uint32_t kErrorInvalidData = 0x807F00FDu;
constexpr std::uint32_t kErrorInvalidPointer = 0x80000103u;
constexpr std::uint32_t kErrorInvalidValue = 0x800001FEu;

constexpr std::uint32_t kMaxHandles = 2u;
constexpr std::uint32_t kStreamMinSize = 8192u;
constexpr std::uint32_t kPcmMinSize = 9216u;
constexpr std::int32_t kWorkareaSize = 0x5C0;
// Measured on hardware: decoding a frame, parsing the header, setting up resources.
constexpr std::uint64_t kDecodeDelayUs = 2400u, kInitDelayUs = 500u, kResourceInitDelayUs = 200u,
                        kResourceTermDelayUs = 100u, kLowLevelInitDelayUs = 600u;

struct Mp3State {
    bool resources{};
    std::map<std::uint32_t, std::unique_ptr<Mp3Stream>> streams;
};

Mp3State &mp3_state(std::shared_ptr<void> &slot) {
    if (!slot) slot = std::make_shared<Mp3State>();
    return *static_cast<Mp3State *>(slot.get());
}

std::int32_t mp3_bitrate(std::uint32_t index, std::uint32_t version, std::uint32_t layer) {
    static constexpr std::int32_t v1[3][16] = {
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1},     // layer III
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, -1},    // layer II
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, -1}, // layer I
    };
    static constexpr std::int32_t v2_low[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, -1};
    static constexpr std::int32_t v2_layer1[16] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, -1};
    if (layer == 0u) return -1;
    if (version == 3u) return v1[layer - 1u][index];
    if (version == 2u || version == 0u) return layer == 3u ? v2_layer1[index] : v2_low[index];
    return -1;
}

std::int32_t mp3_sample_rate(std::uint32_t index, std::uint32_t version) {
    static constexpr std::int32_t v1[4] = {44100, 48000, 32000, -1}, v2[4] = {22050, 24000, 16000, -1},
                                  v25[4] = {11025, 12000, 8000, -1};
    if (version == 3u) return v1[index];
    if (version == 2u) return v2[index];
    if (version == 0u) return v25[index];
    return -1;
}

std::uint32_t mp3_samples_per_frame(std::uint32_t version, std::uint32_t layer) {
    if (version == 1u || layer == 0u) return 0xFFFFFFFFu;
    if (layer == 3u) return 384u;
    if (layer == 2u || version == 3u) return 1152u;
    return 576u;
}

} // namespace

struct Mp3Stream {
    std::uint64_t start{}, end{};
    std::uint32_t stream{}, stream_size{}, pcm{}, pcm_size{};
    std::int32_t bitrate{}, sample_rate{-1}, channels{}, version{-1};
    std::uint32_t decoded{};
    std::int32_t loops{-1};
    std::uint32_t max_output{};
    std::int32_t frames{};
    std::int64_t read{};        // stream position the next added bytes come from
    std::int64_t base{};        // where the data in the buffer started: the start, or the last seek
    std::int32_t available{};   // added bytes not decoded yet
    std::int32_t next_half{};
    std::vector<std::uint8_t> source;
    mp3dec_t decoder{};

    Mp3Stream() { mp3dec_init(&decoder); }

    [[nodiscard]] std::int32_t half() const { return (static_cast<std::int32_t>(stream_size) - kWorkareaSize) / 2; }

    [[nodiscard]] std::int32_t write_offset() const {
        const std::int32_t size = half() * 2;
        if (size <= 0) return 0;
        std::int64_t position = (read - base) % size;
        if (position < 0) position += size;
        return static_cast<std::int32_t>(position);
    }

    [[nodiscard]] std::int32_t bytes_needed() const {
        if (read >= static_cast<std::int64_t>(end)) return 0;
        const std::int32_t h = half();
        if (h <= 0) return 0;
        const std::int64_t written = read - base;
        const std::int64_t consumed = written - available;
        const std::int64_t halves = consumed / h - ((consumed % h < 0) ? 1 : 0);
        return static_cast<std::int32_t>(std::max<std::int64_t>(0, (halves + 2) * h - written));
    }

    void reset(std::int64_t position, std::uint32_t decoded_samples) {
        read = position;
        base = position;
        decoded = decoded_samples;
        available = 0;
        source.clear();
        mp3dec_init(&decoder);
    }
};

// Ends the call with `result` after `usec` of guest time, as functions that keep the PSP busy do.
void Kernel::delay_result(Ctx &ctx, std::uint32_t result, std::uint64_t usec) {
    Thread *t = current();
    if (t == nullptr) return finish(ctx, result);
    t->wake_time_us = now_us() + usec;
    t->wait_uid = t->uid;
    t->wait_extra = result; // what the delay ends with
    block(ctx, WaitType::Delay, result);
}

void Kernel::install_mp3() {
    const char *lib = "sceMp3";
    hle(lib, 0x35750070u, "sceMp3InitResource", &Kernel::sceMp3InitResource);
    hle(lib, 0x3C2FA058u, "sceMp3TermResource", &Kernel::sceMp3TermResource);
    hle(lib, 0x07EC321Au, "sceMp3ReserveMp3Handle", &Kernel::sceMp3ReserveMp3Handle);
    hle(lib, 0xF5478233u, "sceMp3ReleaseMp3Handle", &Kernel::sceMp3ReleaseMp3Handle);
    hle(lib, 0x44E07129u, "sceMp3Init", &Kernel::sceMp3Init);
    hle(lib, 0xD021C0FBu, "sceMp3Decode", &Kernel::sceMp3Decode);
    hle(lib, 0xD0A56296u, "sceMp3CheckStreamDataNeeded", &Kernel::sceMp3CheckStreamDataNeeded);
    hle(lib, 0xA703FE0Fu, "sceMp3GetInfoToAddStreamData", &Kernel::sceMp3GetInfoToAddStreamData);
    hle(lib, 0x0DB149F4u, "sceMp3NotifyAddStreamData", &Kernel::sceMp3NotifyAddStreamData);
    hle(lib, 0x2A368661u, "sceMp3ResetPlayPosition", &Kernel::sceMp3ResetPlayPosition);
    hle(lib, 0x0840E808u, "sceMp3ResetPlayPositionByFrame", &Kernel::sceMp3ResetPlayPositionByFrame);
    hle(lib, 0x3CEF484Fu, "sceMp3SetLoopNum", &Kernel::sceMp3SetLoopNum);
    hle(lib, 0xD8F54A51u, "sceMp3GetLoopNum", &Kernel::sceMp3GetLoopNum);
    hle(lib, 0x354D27EAu, "sceMp3GetSumDecodedSample", &Kernel::sceMp3GetSumDecodedSample);
    hle(lib, 0x87C263D1u, "sceMp3GetMaxOutputSample", &Kernel::sceMp3GetMaxOutputSample);
    hle(lib, 0x7F696782u, "sceMp3GetMp3ChannelNum", &Kernel::sceMp3GetMp3ChannelNum);
    hle(lib, 0x87677E40u, "sceMp3GetBitRate", &Kernel::sceMp3GetBitRate);
    hle(lib, 0x8F450998u, "sceMp3GetSamplingRate", &Kernel::sceMp3GetSamplingRate);
    hle(lib, 0xAE6D2027u, "sceMp3GetMPEGVersion", &Kernel::sceMp3GetMPEGVersion);
    hle(lib, 0x3548AEC8u, "sceMp3GetFrameNum", &Kernel::sceMp3GetFrameNum);
    hle(lib, 0x1B839B83u, "sceMp3LowLevelInit", &Kernel::sceMp3LowLevelInit);
    hle(lib, 0xE3EE2C81u, "sceMp3LowLevelDecode", &Kernel::sceMp3LowLevelDecode);
    hle_value(lib, 0x8AB81558u, "sceMp3StartEntry", 0u);
    hle_value(lib, 0x732B042Au, "sceMp3EndEntry", 0u);
}

void Kernel::sceMp3InitResource(Ctx &ctx) {
    Mp3State &s = mp3_state(mp3_);
    if (s.resources) return finish(ctx, 0u);
    s.resources = true;
    delay_result(ctx, 0u, kResourceInitDelayUs);
}

void Kernel::sceMp3TermResource(Ctx &ctx) {
    Mp3State &s = mp3_state(mp3_);
    if (!s.resources) return finish(ctx, 0u);
    s.streams.clear();
    s.resources = false;
    delay_result(ctx, 0u, kResourceTermDelayUs);
}

void Kernel::sceMp3ReserveMp3Handle(Ctx &ctx) {
    Mp3State &s = mp3_state(mp3_);
    auto &m = rt_.memory();
    const std::uint32_t args = ctx.gpr[4];
    if (!s.resources || s.streams.size() >= kMaxHandles) return finish(ctx, kErrorNoResource);
    if (args != 0u && !m.contains(args, 32u)) return finish(ctx, kErrorInvalidPointer);
    auto stream = std::make_unique<Mp3Stream>();
    if (args != 0u) {
        stream->start = m.load32(args) | static_cast<std::uint64_t>(m.load32(args + 4u)) << 32u;
        stream->end = m.load32(args + 8u) | static_cast<std::uint64_t>(m.load32(args + 12u)) << 32u;
        stream->stream = m.load32(args + 16u);
        stream->stream_size = m.load32(args + 20u);
        stream->pcm = m.load32(args + 24u);
        stream->pcm_size = m.load32(args + 28u);
        if (stream->start >= stream->end) return finish(ctx, kErrorBadSize);
        if (stream->stream == 0u || stream->pcm == 0u) return finish(ctx, kErrorBadAddr);
        if (static_cast<std::int32_t>(stream->stream_size) < static_cast<std::int32_t>(kStreamMinSize) ||
            static_cast<std::int32_t>(stream->pcm_size) < static_cast<std::int32_t>(kPcmMinSize))
            return finish(ctx, kErrorBadSize);
    }
    stream->read = static_cast<std::int64_t>(stream->start);
    stream->base = stream->read;
    // The lowest free handle.
    std::uint32_t handle = 0u;
    while (s.streams.contains(handle)) ++handle;
    s.streams[handle] = std::move(stream);
    finish(ctx, handle);
}

void Kernel::sceMp3ReleaseMp3Handle(Ctx &ctx) {
    Mp3State &s = mp3_state(mp3_);
    const std::uint32_t handle = ctx.gpr[4];
    if (s.streams.erase(handle) != 0u) return finish(ctx, 0u);
    finish(ctx, handle >= kMaxHandles ? kErrorInvalidHandle : 0u);
}

// The stream for a call, or nullptr with the call finished with the PSP's error. `missing` is
// the error for a handle that was never reserved; `need_init` refuses streams not yet initialised.
Mp3Stream *Kernel::mp3_stream(Ctx &ctx, std::uint32_t missing, bool need_init) {
    Mp3State &s = mp3_state(mp3_);
    const std::uint32_t handle = ctx.gpr[4];
    const auto it = s.streams.find(handle);
    if (it == s.streams.end()) {
        finish(ctx, handle >= kMaxHandles ? kErrorInvalidHandle : missing);
        return nullptr;
    }
    Mp3Stream &stream = *it->second;
    if (need_init ? (stream.version < 0 || stream.stream == 0u) : stream.stream == 0u) {
        finish(ctx, need_init ? kErrorNotYetInitHandle : kErrorUnreservedHandle);
        return nullptr;
    }
    return &stream;
}

void Kernel::sceMp3Init(Ctx &ctx) {
    Mp3Stream *stream = mp3_stream(ctx, kErrorUnreservedHandle, false);
    if (stream == nullptr) return;
    auto &m = rt_.memory();
    // The first frame header is at most 1439 bytes into the data, past any ID3 tag.
    const std::uint32_t at = stream->stream + kWorkareaSize;
    std::uint32_t header = 0u;
    bool found = false;
    for (std::uint32_t offset = 0; offset < 1440u && m.contains(at + offset, 4u); ++offset) {
        if (m.load8(at + offset) == 0xFFu && (m.load8(at + offset + 1u) & 0xC0u) == 0xC0u) {
            header = static_cast<std::uint32_t>(m.load8(at + offset)) << 24u | static_cast<std::uint32_t>(m.load8(at + offset + 1u)) << 16u |
                     static_cast<std::uint32_t>(m.load8(at + offset + 2u)) << 8u | m.load8(at + offset + 3u);
            found = true;
            break;
        }
    }
    if (!found) return delay_result(ctx, kErrorInvalidData, kInitDelayUs);
    const std::uint32_t layer = (header >> 17u) & 3u, version = (header >> 19u) & 3u;
    const std::int32_t bitrate = mp3_bitrate((header >> 12u) & 0xFu, version, layer);
    const std::int32_t rate = mp3_sample_rate((header >> 10u) & 3u, version);
    const std::uint32_t channel_bits = (header >> 6u) & 3u;
    // Only MPEG-1 layer III streams are accepted.
    if (layer != 1u) return delay_result(ctx, kErrorInvalidData, kInitDelayUs);
    if (version != 3u) return delay_result(ctx, kErrorUnsupportedVersion, kInitDelayUs);
    if (bitrate == 0 || bitrate == -1 || rate == -1) return delay_result(ctx, kErrorInvalidData, kInitDelayUs);
    if (sdk_version_ >= 0x06000000u && stream->read < 156) return delay_result(ctx, kErrorInvalidValue, kInitDelayUs);
    // Like PPSSPP, only a game without a compiled SDK version is held to 44.1 kHz.
    if (rate != 44100 && sdk_version_ == 0u) return delay_result(ctx, kErrorBadSampleRate, kInitDelayUs);
    stream->sample_rate = rate;
    stream->channels = channel_bits == 3u ? 1 : 2;
    stream->bitrate = bitrate;
    stream->max_output = mp3_samples_per_frame(version, layer);
    const std::uint32_t bytes_per_second = (stream->max_output / 8u) * static_cast<std::uint32_t>(bitrate) * 1000u;
    const std::uint64_t total = (stream->end & 0xFFFFFFFFu) - (stream->start & 0xFFFFFFFFu);
    stream->frames = bytes_per_second == 0u ? 0 : static_cast<std::int32_t>(total * static_cast<std::uint64_t>(rate) / bytes_per_second);
    stream->version = static_cast<std::int32_t>(version);
    delay_result(ctx, 0u, kInitDelayUs);
}

// Decodes one frame into the next half of the PCM buffer; returns its size in bytes.
std::uint32_t Kernel::mp3_decode_frame(Mp3Stream &stream, std::uint32_t pcm_out) {
    auto &m = rt_.memory();
    const std::uint32_t half_size = stream.pcm_size / 2u;
    const std::uint32_t out = stream.pcm + static_cast<std::uint32_t>(stream.next_half) * half_size;
    if (pcm_out != 0u && m.contains(pcm_out, 4u)) m.store32(pcm_out, out);
    bool end = stream.read - stream.available >= static_cast<std::int64_t>(stream.end);
    std::uint32_t bytes = 0u;
    if (!end && !stream.source.empty()) {
        mp3dec_frame_info_t info{};
        std::vector<mp3d_sample_t> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
        int samples = 0;
        // Skip what is not audio (an ID3 tag, a partial frame) up to the next frame that decodes.
        while (!stream.source.empty()) {
            samples = mp3dec_decode_frame(&stream.decoder, stream.source.data(), static_cast<int>(stream.source.size()), pcm.data(), &info);
            if (samples > 0 || info.frame_bytes <= 0) break;
            const auto skip = static_cast<std::int32_t>(std::min<std::size_t>(static_cast<std::size_t>(info.frame_bytes), stream.source.size()));
            stream.source.erase(stream.source.begin(), stream.source.begin() + skip);
            stream.available -= skip;
        }
        if (samples <= 0) {
            stream.available = 0;
            stream.source.clear();
        } else {
            // Always stereo out.
            const auto frames = static_cast<std::uint32_t>(samples);
            bytes = std::min(frames * 4u, half_size);
            for (std::uint32_t i = 0; i < bytes / 4u; ++i) {
                const std::int16_t left = pcm[info.channels == 1 ? i : i * 2u];
                const std::int16_t right = pcm[info.channels == 1 ? i : i * 2u + 1u];
                m.store16(out + i * 4u, static_cast<std::uint16_t>(left));
                m.store16(out + i * 4u + 2u, static_cast<std::uint16_t>(right));
            }
            stream.decoded += frames;
            const auto used = static_cast<std::int32_t>(std::min<std::size_t>(static_cast<std::size_t>(info.frame_bytes), stream.source.size()));
            stream.source.erase(stream.source.begin(), stream.source.begin() + used);
            stream.available -= used;
        }
    }
    // The hardware rewinds a looping stream in the call that decodes its last frame.
    end = stream.read - stream.available >= static_cast<std::int64_t>(stream.end);
    if (end && stream.loops != 0) {
        stream.decoded = 0u;
        stream.read = static_cast<std::int64_t>(stream.start);
        stream.base = stream.read;
        if (stream.loops > 0) --stream.loops;
    }
    if (bytes == 0u && !end) {
        m.zero(out, half_size);
        bytes = half_size;
    } else if (bytes < half_size) {
        m.zero(out + bytes, half_size - bytes);
    }
    stream.next_half ^= 1;
    return bytes;
}

void Kernel::sceMp3Decode(Ctx &ctx) {
    Mp3Stream *stream = mp3_stream(ctx, kErrorNotYetInitHandle, true);
    if (stream == nullptr) return;
    const std::uint32_t bytes = mp3_decode_frame(*stream, ctx.gpr[5]);
    if (bytes == 0u) return finish(ctx, 0u);
    delay_result(ctx, bytes, kDecodeDelayUs);
}

void Kernel::sceMp3CheckStreamDataNeeded(Ctx &ctx) {
    Mp3Stream *stream = mp3_stream(ctx, kErrorUnreservedHandle, false);
    if (stream == nullptr) return;
    finish(ctx, stream->bytes_needed() > 0 ? 1u : 0u);
}

void Kernel::sceMp3GetInfoToAddStreamData(Ctx &ctx) {
    Mp3Stream *stream = mp3_stream(ctx, kErrorUnreservedHandle, false);
    if (stream == nullptr) return;
    auto &m = rt_.memory();
    const std::int32_t needed = stream->bytes_needed();
    const auto put = [&m](std::uint32_t address, std::uint32_t value) {
        if (m.contains(address, 4u)) m.store32(address, value);
    };
    put(ctx.gpr[5], needed != 0 ? stream->stream + kWorkareaSize + static_cast<std::uint32_t>(stream->write_offset()) : 0u);
    put(ctx.gpr[6], static_cast<std::uint32_t>(needed));
    put(ctx.gpr[7], needed != 0 ? static_cast<std::uint32_t>(stream->read) : 0u);
    finish(ctx, 0u);
}

void Kernel::sceMp3NotifyAddStreamData(Ctx &ctx) {
    Mp3Stream *stream = mp3_stream(ctx, kErrorUnreservedHandle, false);
    if (stream == nullptr) return;
    auto &m = rt_.memory();
    const auto size = static_cast<std::int32_t>(ctx.gpr[5]);
    const std::int32_t offset = stream->write_offset();
    stream->read += size;
    stream->available += size;
    const std::uint32_t from = stream->stream + kWorkareaSize + static_cast<std::uint32_t>(offset);
    if (size > 0 && static_cast<std::int64_t>(kWorkareaSize) + offset + size <= stream->stream_size &&
        m.contains(from, static_cast<std::uint32_t>(size))) {
        const std::size_t old = stream->source.size();
        stream->source.resize(old + static_cast<std::size_t>(size));
        for (std::int32_t i = 0; i < size; ++i) stream->source[old + static_cast<std::size_t>(i)] = m.load8(from + static_cast<std::uint32_t>(i));
    }
    finish(ctx, 0u);
}

void Kernel::sceMp3ResetPlayPosition(Ctx &ctx) {
    Mp3Stream *stream = mp3_stream(ctx, kErrorNotYetInitHandle, true);
    if (stream == nullptr) return;
    stream->reset(static_cast<std::int64_t>(stream->start), 0u);
    finish(ctx, 0u);
}

void Kernel::sceMp3ResetPlayPositionByFrame(Ctx &ctx) {
    Mp3Stream *stream = mp3_stream(ctx, kErrorNotYetInitHandle, true);
    if (stream == nullptr) return;
    const std::uint32_t frame = ctx.gpr[5];
    if (frame >= static_cast<std::uint32_t>(stream->frames)) return finish(ctx, kErrorBadResetFrame);
    const std::uint64_t bytes_per_second = (stream->max_output / 8u) * static_cast<std::uint64_t>(stream->bitrate) * 1000u;
    std::int64_t position = static_cast<std::int64_t>(stream->start + frame * bytes_per_second / static_cast<std::uint64_t>(stream->sample_rate));
    // The PSP seeks one byte early.
    if (frame != 0u) --position;
    stream->reset(position, frame * stream->max_output);
    finish(ctx, 0u);
}

void Kernel::sceMp3SetLoopNum(Ctx &ctx) {
    Mp3Stream *stream = mp3_stream(ctx, kErrorUnreservedHandle, false);
    if (stream == nullptr) return;
    const auto loops = static_cast<std::int32_t>(ctx.gpr[5]);
    stream->loops = loops < 0 ? -1 : loops;
    finish(ctx, 0u);
}

void Kernel::sceMp3GetLoopNum(Ctx &ctx) {
    if (Mp3Stream *stream = mp3_stream(ctx, kErrorUnreservedHandle, false); stream != nullptr)
        finish(ctx, static_cast<std::uint32_t>(stream->loops));
}

void Kernel::sceMp3GetSumDecodedSample(Ctx &ctx) {
    if (Mp3Stream *stream = mp3_stream(ctx, kErrorUnreservedHandle, false); stream != nullptr) finish(ctx, stream->decoded);
}

// The format queries: a low-level handle has no stream and reports 0.
void Kernel::mp3_query(Ctx &ctx, std::uint32_t not_init_error, std::uint32_t (*field)(const Mp3Stream &)) {
    Mp3State &s = mp3_state(mp3_);
    const std::uint32_t handle = ctx.gpr[4];
    const auto it = s.streams.find(handle);
    if (it == s.streams.end()) return finish(ctx, handle >= kMaxHandles ? kErrorInvalidHandle : kErrorNotYetInitHandle);
    const Mp3Stream &stream = *it->second;
    if (stream.version < 0) return finish(ctx, not_init_error);
    if (stream.stream == 0u) return finish(ctx, 0u);
    finish(ctx, field(stream));
}

void Kernel::sceMp3GetMaxOutputSample(Ctx &ctx) {
    mp3_query(ctx, kErrorNotYetInitHandle, [](const Mp3Stream &s) { return s.max_output; });
}
void Kernel::sceMp3GetMp3ChannelNum(Ctx &ctx) {
    mp3_query(ctx, kErrorNotYetInitHandle, [](const Mp3Stream &s) { return static_cast<std::uint32_t>(s.channels); });
}
void Kernel::sceMp3GetBitRate(Ctx &ctx) {
    mp3_query(ctx, kErrorNotYetInitHandle, [](const Mp3Stream &s) { return static_cast<std::uint32_t>(s.bitrate); });
}
void Kernel::sceMp3GetSamplingRate(Ctx &ctx) {
    mp3_query(ctx, kErrorNotYetInitHandle, [](const Mp3Stream &s) { return static_cast<std::uint32_t>(s.sample_rate); });
}
void Kernel::sceMp3GetMPEGVersion(Ctx &ctx) {
    // The PSP reports an unreserved handle for a stream not initialised yet.
    Mp3State &s = mp3_state(mp3_);
    const auto it = s.streams.find(ctx.gpr[4]);
    if (it == s.streams.end()) return finish(ctx, ctx.gpr[4] >= kMaxHandles ? kErrorInvalidHandle : kErrorUnreservedHandle);
    mp3_query(ctx, kErrorUnreservedHandle, [](const Mp3Stream &st) { return static_cast<std::uint32_t>(st.version); });
}

void Kernel::sceMp3GetFrameNum(Ctx &ctx) {
    if (Mp3Stream *stream = mp3_stream(ctx, kErrorNotYetInitHandle, true); stream != nullptr)
        finish(ctx, static_cast<std::uint32_t>(stream->frames));
}

void Kernel::sceMp3LowLevelInit(Ctx &ctx) {
    Mp3State &s = mp3_state(mp3_);
    auto stream = std::make_unique<Mp3Stream>();
    // A low-level handle is marked initialised with version 1.
    stream->version = 1;
    s.streams[ctx.gpr[4]] = std::move(stream);
    delay_result(ctx, 0u, kLowLevelInitDelayUs);
}

// (handle, source, &bytes consumed, pcm out, &pcm bytes): one frame from up to 4096 source bytes.
void Kernel::sceMp3LowLevelDecode(Ctx &ctx) {
    Mp3State &s = mp3_state(mp3_);
    const auto it = s.streams.find(ctx.gpr[4]);
    if (it == s.streams.end()) return finish(ctx, kErrorInvalidHandle);
    auto &m = rt_.memory();
    const std::uint32_t source = ctx.gpr[5], consumed_out = ctx.gpr[6], pcm_out = ctx.gpr[7], bytes_out = ctx.gpr[8];
    if (!m.contains(source, 1u) || !m.contains(consumed_out, 4u) || !m.contains(pcm_out, 1u) || !m.contains(bytes_out, 4u))
        return finish(ctx, 0xFFFFFFFFu);
    std::vector<std::uint8_t> input(4096u);
    for (std::uint32_t i = 0; i < input.size(); ++i) input[i] = m.load8(source + i);
    mp3dec_frame_info_t info{};
    std::vector<mp3d_sample_t> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
    const int samples = mp3dec_decode_frame(&it->second->decoder, input.data(), static_cast<int>(input.size()), pcm.data(), &info);
    const std::uint32_t frames = samples > 0 ? static_cast<std::uint32_t>(samples) : 0u;
    for (std::uint32_t i = 0; i < frames; ++i) {
        m.store16(pcm_out + i * 4u, static_cast<std::uint16_t>(pcm[info.channels == 1 ? i : i * 2u]));
        m.store16(pcm_out + i * 4u + 2u, static_cast<std::uint16_t>(pcm[info.channels == 1 ? i : i * 2u + 1u]));
    }
    m.store32(consumed_out, static_cast<std::uint32_t>(info.frame_bytes));
    m.store32(bytes_out, frames * 4u);
    finish(ctx, 0u);
}

} // namespace pspweb
