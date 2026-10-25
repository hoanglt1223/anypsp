// SPDX-License-Identifier: GPL-2.0-or-later
// Kernel objects, scePower, sceMp3 and the on-screen keyboard, called directly without a guest
// program (so without a current thread: nothing here waits).

#include "hle_harness.hpp"

#include <string>
#include <vector>

using anypsp_test::Hle;
using anypsp_test::kScratch;

namespace {

constexpr const char *kThreadMan = "ThreadManForUser";
constexpr std::uint32_t kName = kScratch + 0x3F00u;

std::uint32_t named(Hle &h, const std::string &name = "test") {
    h.store_string(kName, name);
    return kName;
}

std::u16string read_u16(Hle &h, std::uint32_t address) {
    std::u16string text;
    for (std::uint16_t c = h.rt.memory().load16(address); c != 0u; c = h.rt.memory().load16(address))
        text.push_back(static_cast<char16_t>(c)), address += 2u;
    return text;
}

void store_u16(Hle &h, std::uint32_t address, const std::u16string &text) {
    for (std::size_t i = 0; i <= text.size(); ++i)
        h.rt.memory().store16(address + static_cast<std::uint32_t>(i * 2u), i < text.size() ? text[i] : 0u);
}

} // namespace

TEST_CASE("threads are created dormant and report the PSP's status and exit codes") {
    Hle h;
    auto &m = h.rt.memory();
    CHECK(h.call(kThreadMan, 0x446D8DE6u, 0u, 0x08804000u, 0x20u, 0x1000u) == 0x80020001u);      // no name
    CHECK(h.call(kThreadMan, 0x446D8DE6u, named(h), 0x08804000u, 0x20u, 0x100u) == 0x80020194u); // stack too small
    CHECK(h.call(kThreadMan, 0x446D8DE6u, named(h), 0x08804000u, 0x78u, 0x1000u) == 0x80020193u); // priority
    const std::uint32_t thread = h.call(kThreadMan, 0x446D8DE6u, named(h, "worker"), 0x08804000u, 0x20u, 0x1000u);
    CHECK(static_cast<std::int32_t>(thread) > 0);
    CHECK(h.call(kThreadMan, 0x3B183E26u, thread) == 0x800201A2u); // exit status of a never-started thread
    m.store32(kScratch, 104u);
    CHECK(h.call(kThreadMan, 0x17C1684Eu, thread, kScratch) == 0u);
    CHECK(m.load32(kScratch) == 104u);
    CHECK(m.load32(kScratch + 0x24u) == 0x800000FFu); // attr: user, plus the low byte the kernel sets
    CHECK(m.load32(kScratch + 0x28u) == 0x10u);       // dormant
    CHECK(h.call(kThreadMan, 0x616403BAu, thread) == 0x800201A2u); // terminating a dormant thread
    CHECK(h.call(kThreadMan, 0x9FA03CD3u, thread) == 0u);
    CHECK(h.call(kThreadMan, 0x9FA03CD3u, thread) == 0x80020198u);
}

TEST_CASE("callbacks count notifications until cancelled") {
    Hle h;
    auto &m = h.rt.memory();
    CHECK(h.call(kThreadMan, 0xE81CAF8Fu, 0u, 0x08804000u) == 0x80020001u);
    CHECK(h.call(kThreadMan, 0xE81CAF8Fu, named(h), 0xDEADBEEFu) == 0x800200D3u);
    const std::uint32_t cb = h.call(kThreadMan, 0xE81CAF8Fu, named(h, "cb"), 0x08804000u, 0x1234u);
    CHECK(h.call(kThreadMan, 0xC11BA8C4u, cb, 7u) == 0u);
    CHECK(h.call(kThreadMan, 0xC11BA8C4u, cb, 9u) == 0u);
    CHECK(h.call(kThreadMan, 0x2A3D44FFu, cb) == 2u);
    m.store32(kScratch, 56u);
    CHECK(h.call(kThreadMan, 0x730ED8BCu, cb, kScratch) == 0u);
    CHECK(m.load32(kScratch + 0x2Cu) == 0x1234u); // common argument
    CHECK(m.load32(kScratch + 0x30u) == 2u);      // notify count
    CHECK(m.load32(kScratch + 0x34u) == 9u);      // last notify argument
    CHECK(h.call(kThreadMan, 0xBA4051D6u, cb) == 0u);
    CHECK(h.call(kThreadMan, 0x2A3D44FFu, cb) == 0u);
    CHECK(h.call(kThreadMan, 0xEDBA5844u, cb) == 0u);
    CHECK(h.call(kThreadMan, 0x2A3D44FFu, cb) == 0x800201A1u);
}

TEST_CASE("semaphores validate counts and cancel back to their initial count") {
    Hle h;
    auto &m = h.rt.memory();
    CHECK(h.call(kThreadMan, 0xD6DA4BA1u, named(h), 0x200u, 0u, 1u) == 0x80020191u);
    const std::uint32_t sema = h.call(kThreadMan, 0xD6DA4BA1u, named(h), 0u, 1u, 2u);
    CHECK(h.call(kThreadMan, 0x3F53E640u, sema, 2u) == 0x800201AEu); // 1 + 2 > max
    CHECK(h.call(kThreadMan, 0x58B1F937u, sema, 0u) == 0x800201BDu); // poll 0
    CHECK(h.call(kThreadMan, 0x58B1F937u, sema, 1u) == 0u);
    CHECK(h.call(kThreadMan, 0x58B1F937u, sema, 1u) == 0x800201ADu);
    CHECK(h.call(kThreadMan, 0x8FFDF9A2u, sema, 0xFFFFFFFFu, kScratch) == 0u); // back to the initial count
    CHECK(m.load32(kScratch) == 0u);
    m.store32(kScratch, 56u);
    CHECK(h.call(kThreadMan, 0xBC6FEBC5u, sema, kScratch) == 0u);
    CHECK(m.load32(kScratch + 0x2Cu) == 1u);
    CHECK(h.call(kThreadMan, 0x8FFDF9A2u, sema, 3u, 0u) == 0x800201BDu); // above max
}

TEST_CASE("event flags poll with OR, AND and clear modes") {
    Hle h;
    auto &m = h.rt.memory();
    CHECK(h.call(kThreadMan, 0x55C20A00u, named(h), 0x100u, 0u) == 0x80020191u);
    const std::uint32_t flag = h.call(kThreadMan, 0x55C20A00u, named(h), 0u, 0x0Fu);
    CHECK(h.call(kThreadMan, 0x30FD48F0u, flag, 0u, 0u, kScratch) == 0x800201B1u);    // no bits
    CHECK(h.call(kThreadMan, 0x30FD48F0u, flag, 1u, 0x30u, kScratch) == 0x80020195u); // clear and clear all
    CHECK(h.call(kThreadMan, 0x30FD48F0u, flag, 0x11u, 0u, kScratch) == 0x800201AFu); // AND not met
    CHECK(m.load32(kScratch) == 0x0Fu);
    CHECK(h.call(kThreadMan, 0x30FD48F0u, flag, 0x11u, 0x21u, kScratch) == 0u);       // OR, clear the bits asked
    m.store32(kScratch, 52u);
    CHECK(h.call(kThreadMan, 0xA66B0120u, flag, kScratch) == 0u);
    CHECK(m.load32(kScratch + 0x2Cu) == 0x0Eu);
    CHECK(h.call(kThreadMan, 0xCD203292u, flag, 0x80u, 0u) == 0u);
    CHECK(h.call(kThreadMan, 0x30FD48F0u, flag, 0x80u, 0x10u, 0u) == 0u);             // clear all
    CHECK(h.call(kThreadMan, 0x30FD48F0u, flag, 0xFFu, 1u, 0u) == 0x800201AFu);
}

TEST_CASE("message boxes keep their queue as a circular list through the guest's packets") {
    Hle h;
    auto &m = h.rt.memory();
    const std::uint32_t box = h.call(kThreadMan, 0x8125221Du, named(h), 0u);
    const std::uint32_t a = kScratch + 0x100u, b = kScratch + 0x200u;
    CHECK(h.call(kThreadMan, 0xE9B3061Eu, box, a) == 0u);
    CHECK(m.load32(a) == a); // a single packet points at itself
    CHECK(h.call(kThreadMan, 0xE9B3061Eu, box, b) == 0u);
    CHECK(m.load32(a) == b);
    CHECK(m.load32(b) == a);
    CHECK(h.call(kThreadMan, 0xE9B3061Eu, box, b) == 0x800201C9u); // already queued
    CHECK(h.call(kThreadMan, 0x0D81716Au, box, kScratch) == 0u);
    CHECK(m.load32(kScratch) == a);
    CHECK(h.call(kThreadMan, 0x0D81716Au, box, kScratch) == 0u);
    CHECK(m.load32(kScratch) == b);
    CHECK(h.call(kThreadMan, 0x0D81716Au, box, kScratch) == 0x800201B2u);
}

TEST_CASE("message pipes buffer bytes in order and report what moved") {
    Hle h;
    auto &m = h.rt.memory();
    CHECK(h.call(kThreadMan, 0x7C0DC2A0u, 0u, 2u, 0u, 0x100u) == 0x80020190u);
    CHECK(h.call(kThreadMan, 0x7C0DC2A0u, named(h), 1u, 0u, 0x100u) == 0x800200D1u);
    CHECK(h.call(kThreadMan, 0x7C0DC2A0u, named(h), 2u, 0x200u, 0x100u) == 0x80020191u);
    const std::uint32_t pipe = h.call(kThreadMan, 0x7C0DC2A0u, named(h), 2u, 0u, 0x100u);
    const std::uint32_t src = kScratch + 0x100u, dst = kScratch + 0x200u, count = kScratch + 0x300u;
    for (std::uint32_t i = 0; i < 16u; ++i) m.store8(src + i, static_cast<std::uint8_t>(i + 1u));
    CHECK(h.call(kThreadMan, 0x884C9F90u, pipe, src, 16u, 0u, count) == 0u);
    CHECK(m.load32(count) == 16u);
    CHECK(h.call(kThreadMan, 0x884C9F90u, pipe, src, 0x200u, 0u, count) == 0x800201BCu); // larger than the buffer
    m.store32(kScratch, 56u);
    CHECK(h.call(kThreadMan, 0x33BE4024u, pipe, kScratch) == 0u);
    CHECK(m.load32(kScratch + 0x2Cu) == 0xF0u); // free
    CHECK(h.call(kThreadMan, 0xDF52098Fu, pipe, dst, 10u, 0u, count) == 0u);
    CHECK(m.load8(dst) == 1u && m.load8(dst + 9u) == 10u);
    CHECK(h.call(kThreadMan, 0xDF52098Fu, pipe, dst, 3u, 1u, count) == 0u); // ASAP takes what there is
    CHECK(m.load32(count) == 3u);
    CHECK(m.load8(dst) == 11u);
    // FULL mode fails when the rest is not enough, but the PSP has copied what there was.
    CHECK(h.call(kThreadMan, 0xDF52098Fu, pipe, dst, 10u, 0u, count) == 0x800201B4u);
    CHECK(m.load8(dst) == 14u);
    CHECK(h.call(kThreadMan, 0xDF52098Fu, pipe, dst, 1u, 1u, count) == 0x800201B4u);
    CHECK(h.call(kThreadMan, 0x349B864Du, pipe, 0u, 0u) == 0u);
    CHECK(h.call(kThreadMan, 0xF0B7DA1Cu, pipe) == 0u);
    CHECK(h.call(kThreadMan, 0xF0B7DA1Cu, pipe) == 0x8002019Eu);
}

TEST_CASE("fixed pools hand out blocks round robin and variable pools account an 8-byte header") {
    Hle h;
    auto &m = h.rt.memory();
    const std::uint32_t fpl = h.call(kThreadMan, 0xC07BB470u, named(h), 2u, 0u, 0x10u, 3u);
    CHECK(h.call(kThreadMan, 0x623AE665u, fpl, kScratch) == 0u);
    const std::uint32_t first = m.load32(kScratch);
    CHECK(h.call(kThreadMan, 0xF6414A71u, fpl, first) == 0u);
    CHECK(h.call(kThreadMan, 0x623AE665u, fpl, kScratch) == 0u);
    CHECK(m.load32(kScratch) == first + 0x10u); // not the block just freed
    CHECK(h.call(kThreadMan, 0xF6414A71u, fpl, first) == 0x800201B6u);
    const std::uint32_t vpl = h.call(kThreadMan, 0x56C039B5u, named(h), 2u, 0u, 0x1000u);
    m.store32(kScratch, 52u);
    CHECK(h.call(kThreadMan, 0x39810265u, vpl, kScratch) == 0u);
    CHECK(m.load32(kScratch + 0x28u) == 0xFE0u);
    const std::uint32_t free_before = m.load32(kScratch + 0x2Cu);
    CHECK(h.call(kThreadMan, 0xAF36D708u, vpl, 0x10u, kScratch + 0x40u) == 0u);
    CHECK(h.call(kThreadMan, 0x39810265u, vpl, kScratch) == 0u);
    CHECK(m.load32(kScratch + 0x2Cu) == free_before - 0x18u);
    CHECK(h.call(kThreadMan, 0xB736E9FFu, vpl, m.load32(kScratch + 0x40u)) == 0u);
    CHECK(h.call(kThreadMan, 0xB736E9FFu, vpl, m.load32(kScratch + 0x40u)) == 0x800201B6u);
    CHECK(h.call(kThreadMan, 0x39810265u, vpl, kScratch) == 0u);
    CHECK(m.load32(kScratch + 0x2Cu) == free_before);
}

TEST_CASE("thread-local storage pools give each thread one cleared block") {
    Hle h;
    auto &m = h.rt.memory();
    CHECK(h.call(kThreadMan, 0x8DAFF657u, named(h), 2u, 0u, 0u, 4u) == 0x800201B7u); // zero block size
    const std::uint32_t pool = h.call(kThreadMan, 0x8DAFF657u, named(h), 2u, 0u, 0x20u, 2u);
    CHECK(h.call(kThreadMan, 0x65F54FFBu, pool, kScratch, 0u) == 0u);
    const std::uint32_t block = m.load32(kScratch);
    CHECK(block != 0u && m.load32(block) == 0u);
    CHECK(h.call(kThreadMan, 0x65F54FFBu, pool, kScratch, 0u) == 0u);
    CHECK(m.load32(kScratch) == block); // the same thread gets its own block again
    m.store32(kScratch, 60u);
    CHECK(h.call(kThreadMan, 0x721067F3u, pool, kScratch) == 0u);
    CHECK(m.load32(kScratch + 0x34u) == 1u); // one block left
    CHECK(h.call(kThreadMan, 0x4A719FB2u, pool) == 0u);
    CHECK(h.call(kThreadMan, 0x32BF938Eu, pool) == 0u);
    CHECK(h.call(kThreadMan, 0x32BF938Eu, pool) == 0x800201D0u);
}

TEST_CASE("virtual timers start, stop and keep their accumulated time") {
    Hle h;
    auto &m = h.rt.memory();
    CHECK(h.call(kThreadMan, 0x20FFF560u, 0u) == 0x80020001u);
    const std::uint32_t timer = h.call(kThreadMan, 0x20FFF560u, named(h, "vt"));
    CHECK(h.call(kThreadMan, 0xC68D9437u, timer) == 0u);
    CHECK(h.call(kThreadMan, 0xC68D9437u, timer) == 1u); // already running
    CHECK(h.call(kThreadMan, 0xD0AEEE87u, timer) == 1u);
    CHECK(h.call(kThreadMan, 0xD0AEEE87u, timer) == 0u); // already stopped
    m.store32(kScratch, 0x12345u);
    m.store32(kScratch + 4u, 0u);
    CHECK(h.call(kThreadMan, 0x542AD630u, timer, kScratch) == 0u);
    CHECK(h.call(kThreadMan, 0xC0B3FFD2u, timer) == 0x12345u);
    m.store32(kScratch, 0x48u);
    CHECK(h.call(kThreadMan, 0x5F32BEAAu, timer, kScratch) == 0u);
    CHECK(m.load32(kScratch + 0x24u) == 0u);       // not active
    CHECK(m.load32(kScratch + 0x30u) == 0x12345u); // current
    CHECK(h.call(kThreadMan, 0x328F9E52u, timer) == 0u);
    CHECK(h.call(kThreadMan, 0x328F9E52u, timer) == 0x800201BEu);
}

TEST_CASE("scePower reports AC power and a full battery, and tells power callbacks so") {
    Hle h;
    CHECK(h.call("scePower", 0x87440F5Eu) == 1u);   // on AC power
    CHECK(h.call("scePower", 0x0AFD0D8Bu) == 1u);   // battery present
    CHECK(h.call("scePower", 0x1E490401u) == 0u);   // not charging
    CHECK(h.call("scePower", 0x2085D15Du) == 100u); // percent
    CHECK(h.call("scePower", 0xD3075926u) == 0u);   // not low
    const std::uint32_t cb = h.call(kThreadMan, 0xE81CAF8Fu, named(h, "power"), 0x08804000u);
    CHECK(h.call("scePower", 0x04B7766Eu, 0xFFFFFFFFu, cb) == 0u); // first free slot
    CHECK(h.call(kThreadMan, 0x2A3D44FFu, cb) == 1u);
    CHECK(h.call("scePower", 0x04B7766Eu, 0u, cb) == 0x80000020u); // slot taken
    CHECK(h.call("scePower", 0xDFA8BAF8u, 0u) == 0u);
    CHECK(h.call("scePower", 0xDFA8BAF8u, 0u) == 0x80000025u);
    CHECK(h.call("scePower", 0x737486F2u, 333u, 333u, 166u) == 0u);
    CHECK(h.call("scePower", 0xFDB5BFE9u) == 333u);
    CHECK(h.call("scePower", 0xBD681969u) == 166u);
    CHECK(h.call("scePower", 0x737486F2u, 400u, 333u, 166u) == 0x800001FEu);
}

TEST_CASE("sceMp3 decodes a stream fed through the work area and the two halves") {
    Hle h;
    auto &m = h.rt.memory();
    constexpr std::uint32_t args = kScratch, stream = kScratch + 0x1000u, pcm = kScratch + 0x4000u, info = kScratch + 0x100u;
    // Silent MPEG-1 layer III frames at 128 kbit/s, 44.1 kHz, stereo: 417 bytes each.
    constexpr std::uint32_t frame = 417u, stream_size = 8192u, end = 6720u;
    CHECK(h.call("sceMp3", 0x07EC321Au, args) == 0x80671201u); // resources not initialised
    CHECK(h.call("sceMp3", 0x35750070u) == 0u);
    for (std::uint32_t i = 0; i < 32u; i += 4u) m.store32(args + i, 0u);
    m.store32(args + 8u, end);
    m.store32(args + 16u, stream);
    m.store32(args + 20u, stream_size);
    m.store32(args + 24u, pcm);
    m.store32(args + 28u, 9216u);
    const std::uint32_t handle = h.call("sceMp3", 0x07EC321Au, args);
    CHECK(handle == 0u);
    CHECK(h.call("sceMp3", 0xD0A56296u, handle) == 1u); // needs data
    CHECK(h.call("sceMp3", 0xA703FE0Fu, handle, info, info + 4u, info + 8u) == 0u);
    CHECK(m.load32(info) == stream + 0x5C0u);
    CHECK(m.load32(info + 4u) == end);
    for (std::uint32_t i = 0; i < end; ++i) m.store8(stream + 0x5C0u + i, 0u);
    for (std::uint32_t at = 0; at + frame <= end; at += frame) {
        m.store8(stream + 0x5C0u + at, 0xFFu);
        m.store8(stream + 0x5C0u + at + 1u, 0xFBu);
        m.store8(stream + 0x5C0u + at + 2u, 0x90u);
    }
    CHECK(h.call("sceMp3", 0x0DB149F4u, handle, end) == 0u);
    CHECK(h.call("sceMp3", 0x87677E40u, handle) == 0x80671103u); // before sceMp3Init
    CHECK(h.call("sceMp3", 0x44E07129u, handle) == 0u);
    CHECK(h.call("sceMp3", 0x87677E40u, handle) == 128u);
    CHECK(h.call("sceMp3", 0x8F450998u, handle) == 44100u);
    CHECK(h.call("sceMp3", 0x7F696782u, handle) == 2u);
    CHECK(h.call("sceMp3", 0x87C263D1u, handle) == 1152u);
    for (std::uint32_t i = 0; i < 9216u; i += 4u) m.store32(pcm + i, 0x5A5A5A5Au);
    CHECK(h.call("sceMp3", 0xD021C0FBu, handle, info) == 4608u); // 1152 stereo samples
    CHECK(m.load32(info) == pcm);
    CHECK(m.load32(pcm) == 0u); // silence
    CHECK(h.call("sceMp3", 0x354D27EAu, handle) == 1152u);
    CHECK(h.call("sceMp3", 0xD021C0FBu, handle, info) == 4608u);
    CHECK(m.load32(info) == pcm + 4608u); // the other half
    CHECK(h.call("sceMp3", 0xF5478233u, handle) == 0u);
    CHECK(h.call("sceMp3", 0x87677E40u, 5u) == 0x80671001u);
    CHECK(h.call("sceMp3", 0x3C2FA058u) == 0u);
}

namespace {

constexpr std::uint32_t kOskParams = kScratch, kOskField = kScratch + 0x100u, kOskDesc = kScratch + 0x200u,
                        kOskIn = kScratch + 0x300u, kOskOut = kScratch + 0x400u;

void osk_request(Hle &h, const std::u16string &initial, std::uint32_t out_length = 16u) {
    auto &m = h.rt.memory();
    for (std::uint32_t i = 0; i < 0x500u; i += 4u) m.store32(kScratch + i, 0u);
    m.store32(kOskParams, 0x44u);
    m.store32(kOskParams + 8u, 1u);
    m.store32(kOskParams + 0x30u, 1u);
    m.store32(kOskParams + 0x34u, kOskField);
    store_u16(h, kOskDesc, u"Name");
    store_u16(h, kOskIn, initial);
    m.store32(kOskField + 0x1Cu, kOskDesc);
    m.store32(kOskField + 0x20u, kOskIn);
    m.store32(kOskField + 0x24u, out_length);
    m.store32(kOskField + 0x28u, kOskOut);
}

constexpr std::uint32_t kOskInit = 0xF6269B82u, kOskStatus = 0xF3F76017u, kOskShutdown = 0x3DFAEBA9u;

} // namespace

TEST_CASE("headless keyboard answers with the scripted text, then the initial text") {
    Hle h;
    auto &m = h.rt.memory();
    h.kernel->set_osk_texts({"Naruto Uzumaki"});
    osk_request(h, u"Player", 7u); // room for 6 characters
    CHECK(h.call("sceUtility", kOskInit, kOskParams) == 0u);
    CHECK(h.kernel->system_dialog() == nullptr);
    CHECK(read_u16(h, kOskOut) == u"Naruto");
    CHECK(m.load32(kOskField + 0x2Cu) == 2u); // changed
    CHECK(h.call("sceUtility", kOskStatus) == 1u);
    CHECK(h.call("sceUtility", kOskStatus) == 2u);
    CHECK(h.call("sceUtility", kOskStatus) == 3u);
    CHECK(h.call("sceUtility", kOskShutdown) == 0u);
    CHECK(h.call("sceUtility", kOskStatus) == 4u);
    CHECK(h.call("sceUtility", kOskStatus) == 0u);
    osk_request(h, u"Player");
    CHECK(h.call("sceUtility", kOskInit, kOskParams) == 0u);
    CHECK(read_u16(h, kOskOut) == u"Player");
}

TEST_CASE("interactive keyboard types the focused keys and real-keyboard text, and can be cancelled") {
    Hle h;
    auto &m = h.rt.memory();
    h.kernel->set_interactive_dialogs(true);
    osk_request(h, u"");
    CHECK(h.call("sceUtility", kOskInit, kOskParams) == 0u);
    const pspweb::SystemDialog *d = h.kernel->system_dialog();
    CHECK(d != nullptr && d->kind == pspweb::SystemDialog::Kind::Keyboard && d->text == "Name");
    h.kernel->dialog_focus(2); // "C"
    h.kernel->dialog_confirm();
    h.kernel->dialog_move(pspweb::kOskColumns); // a row down: "M"
    h.kernel->dialog_confirm();
    h.kernel->dialog_type("ày");
    CHECK(h.kernel->system_dialog()->input == "CMày");
    h.kernel->dialog_focus(pspweb::kOskDelete);
    h.kernel->dialog_confirm();
    h.kernel->dialog_focus(pspweb::kOskDone);
    h.kernel->dialog_confirm();
    CHECK(h.kernel->system_dialog() == nullptr);
    CHECK(read_u16(h, kOskOut) == u"CMà");
    CHECK(m.load32(kOskField + 0x2Cu) == 2u);
    CHECK(h.call("sceUtility", kOskStatus) == 1u);
    CHECK(h.call("sceUtility", kOskStatus) == 2u);
    CHECK(h.call("sceUtility", kOskStatus) == 3u);
    CHECK(h.call("sceUtility", kOskShutdown) == 0u);
    CHECK(h.call("sceUtility", kOskStatus) == 4u);
    CHECK(h.call("sceUtility", kOskStatus) == 0u);
    osk_request(h, u"Keep");
    CHECK(h.call("sceUtility", kOskInit, kOskParams) == 0u);
    h.kernel->dialog_type("x");
    h.kernel->dialog_back();
    CHECK(read_u16(h, kOskOut) == u"Keep");
    CHECK(m.load32(kOskField + 0x2Cu) == 1u); // cancelled
}
