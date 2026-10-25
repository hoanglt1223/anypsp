#include "kernel.hpp"

#include <bit>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace pspweb {
namespace {

#ifdef __EMSCRIPTEN__
// std::chrono goes through clock_gettime and a BigInt conversion in the
// browser; performance.now() directly is several times cheaper.
struct Clock {
    using duration = std::chrono::nanoseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<Clock>;
    static constexpr bool is_steady = true;
    static time_point now() { return time_point(duration(static_cast<rep>(emscripten_get_now() * 1e6))); }
};
#else
using Clock = std::chrono::steady_clock;
#endif

constexpr std::uint64_t kFrameUs = 16683u; // 59.94 Hz
constexpr std::uint32_t kHeartbeatInterval = 1024u;

constexpr std::uint32_t kErrorNoMemory = 0x80020190u;
constexpr std::uint32_t kErrorUnknownUid = 0x800200CBu;
constexpr std::uint32_t kErrorUnknownThread = 0x80020198u;
constexpr std::uint32_t kErrorUnknownSema = 0x80020199u;
constexpr std::uint32_t kErrorUnknownEventFlag = 0x8002019Au;
constexpr std::uint32_t kErrorNotDormant = 0x800201A4u;
constexpr std::uint32_t kErrorWaitTimeout = 0x800201A8u;
constexpr std::uint32_t kErrorSemaZero = 0x800201ADu;
constexpr std::uint32_t kErrorSemaOverflow = 0x800201AEu;
constexpr std::uint32_t kErrorEventFlagCondition = 0x800201AFu;
constexpr std::uint32_t kErrorWaitDelete = 0x800201B5u;
constexpr std::uint32_t kErrorIllegalCount = 0x800201BDu;
constexpr std::uint32_t kErrorUnknownMbx = 0x8002019Bu;
constexpr std::uint32_t kErrorUnknownAlarm = 0x8002019Fu;
constexpr std::uint32_t kErrorMbxNoMessage = 0x800201B2u;
constexpr std::uint32_t kErrorUnknownMutex = 0x800201C3u;
constexpr std::uint32_t kErrorMutexLocked = 0x800201C4u;
constexpr std::uint32_t kErrorMutexUnlockUnderflow = 0x800201C7u;
constexpr std::uint32_t kErrorMutexRecursive = 0x800201C8u;
constexpr std::uint32_t kVblankInterrupt = 30u;
constexpr std::uint64_t kTimeoutLatencyUs = 18u;
constexpr std::uint64_t kTimeoutDeadlineUs = 12u;

constexpr std::uint32_t kEventWaitOr = 0x01u;
constexpr std::uint32_t kEventWaitClearAll = 0x10u;
constexpr std::uint32_t kEventWaitClear = 0x20u;

Kernel *g_kernel = nullptr;
psprecomp::Runtime *g_runtime = nullptr;
std::int64_t g_slice_deadline_ns{};
bool g_deterministic = false;
std::int64_t g_virtual_ns = 0;
// Guest time one heartbeat (kHeartbeatInterval dispatches) stands for in deterministic mode.
constexpr std::int64_t kVirtualNsPerHeartbeat = 100'000;
// 2026-01-01 00:00:00 UTC.
constexpr std::uint64_t kDeterministicEpochUs = 1767225600ull * 1000000ull;

std::int64_t real_now_ns() {
    if (g_deterministic) return g_virtual_ns;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

void slice_heartbeat(std::uint64_t, std::uint32_t) {
    if (g_deterministic) g_virtual_ns += kVirtualNsPerHeartbeat;
    if (real_now_ns() >= g_slice_deadline_ns) g_runtime->stop("slice");
    else if (g_kernel->interrupt_pending()) g_runtime->stop("interrupt");
}

void thread_return_trampoline(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    g_kernel->thread_returned(ctx);
}

std::string hex(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// PSP paths are case-insensitive; the extracted disc keeps its original case.
std::filesystem::path resolve_case(const std::filesystem::path &root, const std::string &relative) {
    std::filesystem::path current = root;
    std::stringstream parts(relative);
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part.empty() || part == ".") continue;
        std::filesystem::path exact = current / part;
        std::error_code ec;
        if (std::filesystem::exists(exact, ec)) {
            current = exact;
            continue;
        }
        bool found = false;
        if (std::filesystem::is_directory(current, ec)) {
            const std::string wanted = lower(part);
            for (const auto &entry : std::filesystem::directory_iterator(current, ec)) {
                if (lower(entry.path().filename().string()) == wanted) {
                    current = entry.path();
                    found = true;
                    break;
                }
            }
        }
        if (!found) current = exact;
    }
    return current;
}

} // namespace

Kernel::Kernel(psprecomp::Runtime &runtime)
    : rt_(runtime), ge_(std::make_unique<Ge>(runtime.memory())), ge_worker_(std::make_unique<GeWorker>(*ge_)) {
    g_kernel = this;
    g_runtime = &runtime;
    frame_real_start_ns_ = real_now_ns();
    reserve(kUserPartitionStart, 0x4000u, "loader");
    ge_->set_signal_sink([this](std::int32_t callback, bool finish, std::uint32_t value) {
        const std::lock_guard<std::mutex> lock(ge_events_mutex_);
        ge_events_.push_back(GeEvent{callback, finish, value});
        ge_event_pending_.store(true, std::memory_order_relaxed);
    });
}

void Kernel::set_roots(std::filesystem::path disc_root, std::filesystem::path memstick_root) {
    disc_root_ = std::move(disc_root);
    memstick_root_ = std::move(memstick_root);
}

void Kernel::set_deterministic(bool on) {
    g_deterministic = on;
    g_virtual_ns = 0;
    frame_real_start_ns_ = real_now_ns();
}

std::uint64_t Kernel::unix_time_us() const {
    if (g_deterministic) return kDeterministicEpochUs + now_us();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::uint64_t Kernel::now_us() const {
    const std::int64_t elapsed_ns = real_now_ns() - frame_real_start_ns_;
    const std::uint64_t elapsed_us = elapsed_ns > 0 ? static_cast<std::uint64_t>(elapsed_ns / 1000) : 0u;
    const std::uint64_t now = frame_start_guest_us_ + elapsed_us + warp_us_;
    return std::min(now, frame_start_guest_us_ + kFrameUs - 1u);
}

void Kernel::finish(Ctx &ctx, std::uint32_t result) {
    ctx.gpr[2] = result;
    ctx.pc = ctx.gpr[31];
}

void Kernel::finish64(Ctx &ctx, std::uint64_t result) {
    ctx.gpr[2] = static_cast<std::uint32_t>(result);
    ctx.gpr[3] = static_cast<std::uint32_t>(result >> 32u);
    ctx.pc = ctx.gpr[31];
}

std::string Kernel::read_string(std::uint32_t address, std::size_t limit) const {
    std::string text;
    if (address == 0u) return text;
    for (std::size_t i = 0; i < limit; ++i) {
        const char c = static_cast<char>(rt_.memory().load8(address + static_cast<std::uint32_t>(i)));
        if (c == '\0') break;
        text.push_back(c);
    }
    return text;
}

std::filesystem::path Kernel::host_path(const std::string &psp_path) const {
    std::string path = psp_path;
    const auto colon = path.find(':');
    if (colon == std::string::npos) {
        std::string base = cwd_;
        if (!base.empty() && base.back() != '/') base.push_back('/');
        path = base + path;
    }
    const auto device_end = path.find(':');
    const std::string device = lower(path.substr(0, device_end));
    std::string rest = path.substr(device_end + 1u);
    const std::filesystem::path &root =
        (device.rfind("ms", 0) == 0 || device.rfind("fatms", 0) == 0) ? memstick_root_ : disc_root_;
    return resolve_case(root, rest);
}

// ---------------------------------------------------------------------------
// Registration

void Kernel::hle(const char *library, std::uint32_t nid, const char *name,
                 void (Kernel::*handler)(Ctx &)) {
    static const bool trace = std::getenv("PSPWEB_TRACE_HLE") != nullptr;
    if (trace) {
        rt_.register_hle(library, nid, [this, handler, name](psprecomp::Runtime &, Ctx &ctx) {
            const std::int32_t caller = current_uid_;
            std::cerr << "[hle] " << caller << " " << name << "(" << hex(ctx.gpr[4]) << ", " << hex(ctx.gpr[5]) << ", "
                      << hex(ctx.gpr[6]) << ", " << hex(ctx.gpr[7]) << ", " << hex(ctx.gpr[8]) << ")";
            (this->*handler)(ctx);
            std::cerr << (current_uid_ == caller ? " -> " + hex(ctx.gpr[2]) : std::string(" -> (switched)")) << "\n";
        });
    } else {
        rt_.register_hle(library, nid, [this, handler](psprecomp::Runtime &, Ctx &ctx) { (this->*handler)(ctx); });
    }
    rt_.nids().add(library, nid, name);
    registered_.insert(std::string(library) + ":" + hex(nid));
}

void Kernel::hle_value(const char *library, std::uint32_t nid, const char *name, std::uint32_t result) {
    rt_.register_hle(library, nid, [result](psprecomp::Runtime &, Ctx &ctx) { finish(ctx, result); });
    rt_.nids().add(library, nid, name);
    registered_.insert(std::string(library) + ":" + hex(nid));
}

void Kernel::install_fallback(const psprecomp::PspImport &import) {
    const std::string key = import.library + ":" + hex(import.nid);
    if (registered_.contains(key)) return;
    registered_.insert(key);
    const std::string name = import.library + "::" +
        rt_.nids().resolve(import.library, import.nid).value_or(hex(import.nid));
    unimplemented_imports_.push_back(name);
    auto stub = hle_stubs_.find(name);
    if (stub == hle_stubs_.end()) stub = hle_stubs_.find(import.library + "::" + hex(import.nid));
    if (stub == hle_stubs_.end()) {
        // Strict by default: an unimplemented function stops the guest when it is called.
        const std::string message = "unimplemented HLE function " + name + " (" + import.library + " NID " +
                                    hex(import.nid) + "); implement it or stub it with --hle-stubs";
        rt_.register_hle(import.library, import.nid, [this, message](psprecomp::Runtime &, Ctx &) {
            std::cerr << "[hle] " << message << "\n";
            rt_.stop(message);
        });
        return;
    }
    const std::uint32_t result = stub->second;
    rt_.register_hle(import.library, import.nid, [this, name, result](psprecomp::Runtime &, Ctx &ctx) {
        if (fallback_logged_.insert(name).second) {
            std::cerr << "[hle] stubbed " << name << "(" << hex(ctx.gpr[4]) << ", " << hex(ctx.gpr[5])
                      << ", " << hex(ctx.gpr[6]) << ", " << hex(ctx.gpr[7]) << ") -> " << hex(result) << "\n";
        }
        finish(ctx, result);
    });
}

void Kernel::install(const std::vector<psprecomp::PspImport> &imports) {
    rt_.register_function(kThreadReturnAddress, &thread_return_trampoline, "pspweb_thread_return");
    psprecomp::set_runtime_heartbeat_hook(&slice_heartbeat, kHeartbeatInterval);

    install_threads();
    const char *tm = "ThreadManForUser";
    hle(tm, 0x369ED59Du, "sceKernelGetSystemTimeLow", &Kernel::sceKernelGetSystemTimeLow);
    hle(tm, 0x82BC5777u, "sceKernelGetSystemTimeWide", &Kernel::sceKernelGetSystemTimeWide);
    hle(tm, 0xDB738F35u, "sceKernelGetSystemTime", &Kernel::sceKernelGetSystemTime);
    const char *im = "InterruptManager";
    hle(im, 0xCA04A2B9u, "sceKernelRegisterSubIntrHandler", &Kernel::sceKernelRegisterSubIntrHandler);
    hle(im, 0xD61E6961u, "sceKernelReleaseSubIntrHandler", &Kernel::sceKernelReleaseSubIntrHandler);
    hle(im, 0xFB8E22ECu, "sceKernelEnableSubIntr", &Kernel::sceKernelEnableSubIntr);
    hle(im, 0x8A389411u, "sceKernelDisableSubIntr", &Kernel::sceKernelDisableSubIntr);
    hle(tm, 0x6652B8CAu, "sceKernelSetAlarm", &Kernel::sceKernelSetAlarm);
    hle(tm, 0xB2C25152u, "sceKernelSetSysClockAlarm", &Kernel::sceKernelSetSysClockAlarm);
    hle(tm, 0x7E65B999u, "sceKernelCancelAlarm", &Kernel::sceKernelCancelAlarm);
    hle(tm, 0xDAA3F564u, "sceKernelReferAlarmStatus", &Kernel::sceKernelReferAlarmStatus);

    const char *sm = "SysMemUserForUser";
    hle(sm, 0xA291F107u, "sceKernelMaxFreeMemSize", &Kernel::sceKernelMaxFreeMemSize);
    hle(sm, 0xF919F628u, "sceKernelTotalFreeMemSize", &Kernel::sceKernelTotalFreeMemSize);
    hle(sm, 0x237DBD4Fu, "sceKernelAllocPartitionMemory", &Kernel::sceKernelAllocPartitionMemory);
    hle(sm, 0xB6D61D02u, "sceKernelFreePartitionMemory", &Kernel::sceKernelFreePartitionMemory);
    hle(sm, 0x9D9A5BA1u, "sceKernelGetBlockHeadAddr", &Kernel::sceKernelGetBlockHeadAddr);
    hle(sm, 0x3FC9AE6Au, "sceKernelDevkitVersion", &Kernel::sceKernelDevkitVersion);
    hle(sm, 0x13A5ABEFu, "sceKernelPrintf", &Kernel::sceKernelPrintf);
    for (const auto &[nid, name] : std::initializer_list<std::pair<std::uint32_t, const char *>>{
             {0x7591C7DBu, "sceKernelSetCompiledSdkVersion"}, {0x342061E5u, "sceKernelSetCompiledSdkVersion370"},
             {0x315AD3A0u, "sceKernelSetCompiledSdkVersion380_390"}, {0xEBD5C3E6u, "sceKernelSetCompiledSdkVersion395"},
             {0x057E7380u, "sceKernelSetCompiledSdkVersion401_402"}, {0x91DE343Cu, "sceKernelSetCompiledSdkVersion500_505"},
             {0x7893F79Au, "sceKernelSetCompiledSdkVersion507"}, {0x35669D4Cu, "sceKernelSetCompiledSdkVersion600_602"},
             {0x1B4217BCu, "sceKernelSetCompiledSdkVersion603_605"}, {0x358CA1BBu, "sceKernelSetCompiledSdkVersion606"}})
        hle(sm, nid, name, &Kernel::sceKernelSetCompiledSdkVersion);
    hle(sm, 0xFC114573u, "sceKernelGetCompiledSdkVersion", &Kernel::sceKernelGetCompiledSdkVersion);
    hle(sm, 0xF77D77CBu, "sceKernelSetCompilerVersion", &Kernel::return_zero);

    hle("LoadExecForUser", 0x05572A5Fu, "sceKernelExitGame", &Kernel::sceKernelExitGame);
    hle("LoadExecForUser", 0x2AC9954Bu, "sceKernelExitGameWithStatus", &Kernel::sceKernelExitGame);

    const char *mm = "ModuleMgrForUser";
    hle(mm, 0xD675EBB8u, "sceKernelSelfStopUnloadModule", &Kernel::sceKernelSelfStopUnloadModule);
    hle(mm, 0x977DE386u, "sceKernelLoadModule", &Kernel::sceKernelLoadModule);
    hle(mm, 0x50F0C1ECu, "sceKernelStartModule", &Kernel::sceKernelStartModule);
    hle(mm, 0xD1FF982Au, "sceKernelStopModule", &Kernel::sceKernelStopModule);
    hle(mm, 0x2E0911AAu, "sceKernelUnloadModule", &Kernel::sceKernelUnloadModule);
    hle(mm, 0x8F2DF740u, "sceKernelStopUnloadSelfModuleWithStatus", &Kernel::sceKernelStopUnloadSelfModuleWithStatus);
    hle(mm, 0xD8B73127u, "sceKernelGetModuleIdByAddress", &Kernel::sceKernelGetModuleIdByAddress);
    hle(mm, 0xF0A26395u, "sceKernelGetModuleId", &Kernel::sceKernelGetModuleId);
    hle(mm, 0x748CBED9u, "sceKernelQueryModuleInfo", &Kernel::sceKernelQueryModuleInfo);
    hle(mm, 0x644395E2u, "sceKernelGetModuleIdList", &Kernel::sceKernelGetModuleIdList);

    hle("StdioForUser", 0x172D316Eu, "sceKernelStdin", &Kernel::sceKernelStdin);
    hle("StdioForUser", 0xA6BAB2E9u, "sceKernelStdout", &Kernel::sceKernelStdout);
    hle("StdioForUser", 0xF78BA90Au, "sceKernelStderr", &Kernel::sceKernelStderr);

    const char *io = "IoFileMgrForUser";
    hle(io, 0x109F50BCu, "sceIoOpen", &Kernel::sceIoOpen);
    hle(io, 0x810C4BC3u, "sceIoClose", &Kernel::sceIoClose);
    hle(io, 0x6A638D83u, "sceIoRead", &Kernel::sceIoRead);
    hle(io, 0x63632449u, "sceIoIoctl", &Kernel::sceIoIoctl);
    hle(io, 0xB293727Fu, "sceIoChangeAsyncPriority", &Kernel::return_zero);
    hle(io, 0x42EC03ACu, "sceIoWrite", &Kernel::sceIoWrite);
    hle(io, 0x27EB27B8u, "sceIoLseek", &Kernel::sceIoLseek);
    hle(io, 0x68963324u, "sceIoLseek32", &Kernel::sceIoLseek32);
    hle(io, 0xACE946E8u, "sceIoGetstat", &Kernel::sceIoGetstat);
    hle(io, 0x55F4717Du, "sceIoChdir", &Kernel::sceIoChdir);
    hle(io, 0xB29DDF9Cu, "sceIoDopen", &Kernel::sceIoDopen);
    hle(io, 0xE3EB004Cu, "sceIoDread", &Kernel::sceIoDread);
    hle(io, 0xEB092469u, "sceIoDclose", &Kernel::sceIoDclose);
    hle(io, 0x89AA9906u, "sceIoOpenAsync", &Kernel::sceIoOpenAsync);
    hle(io, 0xA0B5A7C2u, "sceIoReadAsync", &Kernel::sceIoReadAsync);
    hle(io, 0x71B19E77u, "sceIoLseekAsync", &Kernel::sceIoLseekAsync);
    hle(io, 0xFF5940B6u, "sceIoCloseAsync", &Kernel::sceIoCloseAsync);
    hle(io, 0xE23EEC33u, "sceIoWaitAsync", &Kernel::sceIoWaitAsync);
    hle_cb(io, 0x35DBD746u, "sceIoWaitAsyncCB", &Kernel::sceIoWaitAsync);
    hle(io, 0x3251EA56u, "sceIoPollAsync", &Kernel::sceIoPollAsync);

    const char *dp = "sceDisplay";
    hle(dp, 0x0E20F177u, "sceDisplaySetMode", &Kernel::sceDisplaySetMode);
    hle(dp, 0x289D82FEu, "sceDisplaySetFrameBuf", &Kernel::sceDisplaySetFrameBuf);
    hle(dp, 0xEEDA2E54u, "sceDisplayGetFrameBuf", &Kernel::sceDisplayGetFrameBuf);
    hle(dp, 0x984C27E7u, "sceDisplayWaitVblankStart", &Kernel::sceDisplayWaitVblankStart);
    hle_cb(dp, 0x46F186C3u, "sceDisplayWaitVblankStartCB", &Kernel::sceDisplayWaitVblankStart);
    hle(dp, 0x36CDFADEu, "sceDisplayWaitVblank", &Kernel::sceDisplayWaitVblankStart);
    hle_cb(dp, 0x8EB9EC49u, "sceDisplayWaitVblankCB", &Kernel::sceDisplayWaitVblankStart);
    hle(dp, 0x9C6EAAD7u, "sceDisplayGetVcount", &Kernel::sceDisplayGetVcount);
    {
        // Small functions registered in place; behaviour follows PPSSPP (Core/HLE/sceDmac.cpp,
        // sceDisplay.cpp, sceUmd.cpp; GPL-2.0-or-later).
        const auto add = [this](const char *library, std::uint32_t nid, const char *name,
                                std::function<std::uint32_t(Ctx &)> body) {
            rt_.register_hle(library, nid, [this, body](psprecomp::Runtime &, Ctx &ctx) { finish(ctx, body(ctx)); });
            rt_.nids().add(library, nid, name);
            registered_.insert(std::string(library) + ":" + hex(nid));
        };
        const auto dmac_copy = [this](Ctx &ctx) -> std::uint32_t {
            constexpr std::uint32_t kInvalidSize = 0x80000104u, kInvalidPointer = 0x80000103u, kPrivRequired = 0x80000023u;
            const std::uint32_t dst = ctx.gpr[4], src = ctx.gpr[5], size = ctx.gpr[6];
            auto &memory = rt_.memory();
            if (size == 0u) return kInvalidSize;
            if (!memory.contains(dst, 1u) || !memory.contains(src, 1u)) return kInvalidPointer;
            if (dst + size >= 0x80000000u || src + size >= 0x80000000u || size >= 0x80000000u) return kPrivRequired;
            memory.note_raw_access(src, size, false);
            memory.note_raw_access(dst, size, true);
            std::uint8_t *to = memory.raw_pointer(dst, size);
            const std::uint8_t *from = memory.raw_pointer(src, size);
            if (to != nullptr && from != nullptr) std::memmove(to, from, size);
            return 0u;
        };
        add("sceDmac", 0x617F3FE6u, "sceDmacMemcpy", dmac_copy);
        add("sceDmac", 0xD97F94D8u, "sceDmacTryMemcpy", dmac_copy);
        // 286 horizontal lines per frame.
        add(dp, 0x210EAB3Au, "sceDisplayGetAccumulatedHcount",
            [this](Ctx &) { return static_cast<std::uint32_t>(display_.vcount * 286u); });
        add("sceUmdUser", 0x87533940u, "sceUmdReplaceProhibit", [](Ctx &) { return 0u; });
        add("sceUmdUser", 0xCBE9F02Au, "sceUmdReplacePermit", [](Ctx &) { return 0u; });
        // Translated and interpreted code is found by address and the bytes in memory,
        // so there is no instruction cache to invalidate.
        add("UtilsForUser", 0xC2DF770Eu, "sceKernelIcacheInvalidateRange", [](Ctx &) { return 0u; });
        add("UtilsForKernel", 0xC2DF770Eu, "sceKernelIcacheInvalidateRange", [](Ctx &) { return 0u; });
        add("UtilsForUser", 0x920F104Au, "sceKernelIcacheInvalidateAll", [](Ctx &) { return 0u; });
    }
    hle("IoFileMgrForKernel", 0x54F5FB11u, "sceIoDevctl", &Kernel::sceIoDevctl);
    {
    }

    hle("sceGe_user", 0xE47E40E4u, "sceGeEdramGetAddr", &Kernel::sceGeEdramGetAddr);
    hle("sceGe_user", 0x1F6752ADu, "sceGeEdramGetSize", &Kernel::sceGeEdramGetSize);
    hle("sceGe_user", 0xAB49E76Au, "sceGeListEnQueue", &Kernel::sceGeListEnQueue);
    hle("sceGe_user", 0x1C0D95A6u, "sceGeListEnQueueHead", &Kernel::sceGeListEnQueue);
    hle("sceGe_user", 0xE0D68148u, "sceGeListUpdateStallAddr", &Kernel::sceGeListUpdateStallAddr);
    hle("sceGe_user", 0x03444EB4u, "sceGeListSync", &Kernel::sceGeListSync);
    hle("sceGe_user", 0xB287BD61u, "sceGeDrawSync", &Kernel::sceGeDrawSync);
    hle("sceGe_user", 0x4C06E472u, "sceGeContinue", &Kernel::return_zero);
    hle("sceGe_user", 0xB448EC0Du, "sceGeBreak", &Kernel::return_zero);
    hle("sceGe_user", 0xA4FC06A4u, "sceGeSetCallback", &Kernel::sceGeSetCallback);
    hle("sceGe_user", 0x05DB22CEu, "sceGeUnsetCallback", &Kernel::sceGeUnsetCallback);
    hle("sceGe_user", 0xB77905EAu, "sceGeEdramSetAddrTranslation", &Kernel::return_zero);
    hle("sceGe_user", 0xDC93CFEFu, "sceGeGetCmd", &Kernel::sceGeGetCmd);
    hle("sceGe_user", 0x438A385Au, "sceGeSaveContext", &Kernel::sceGeSaveContext);
    hle("sceGe_user", 0x0BF608FBu, "sceGeRestoreContext", &Kernel::sceGeRestoreContext);

    const char *ut = "UtilsForUser";
    hle(ut, 0x27CC57F0u, "sceKernelLibcTime", &Kernel::sceKernelLibcTime);
    hle(ut, 0x71EC4271u, "sceKernelLibcGettimeofday", &Kernel::sceKernelLibcGettimeofday);
    hle(ut, 0x79D1C3FAu, "sceKernelDcacheWritebackAll", &Kernel::return_zero);
    hle(ut, 0xB435DEC5u, "sceKernelDcacheWritebackInvalidateAll", &Kernel::return_zero);
    hle(ut, 0x3EE30821u, "sceKernelDcacheWritebackRange", &Kernel::return_zero);
    hle(ut, 0x34B9FA9Eu, "sceKernelDcacheWritebackInvalidateRange", &Kernel::return_zero);
    hle(ut, 0xBFA98062u, "sceKernelDcacheInvalidateRange", &Kernel::return_zero);
    hle(ut, 0x920F104Au, "sceKernelIcacheInvalidateAll", &Kernel::return_zero);
    hle(ut, 0x6AD345D7u, "sceKernelSetGPO", &Kernel::return_zero);

    hle("Kernel_Library", 0x092968F4u, "sceKernelCpuSuspendIntr", &Kernel::sceKernelCpuSuspendIntr);
    hle("Kernel_Library", 0x5F10D406u, "sceKernelCpuResumeIntr", &Kernel::sceKernelCpuResumeIntr);
    hle("Kernel_Library", 0x3B84732Du, "sceKernelCpuResumeIntrWithSync", &Kernel::sceKernelCpuResumeIntr);
    hle("Kernel_Library", 0x47A0B729u, "sceKernelIsCpuIntrSuspended", &Kernel::sceKernelIsCpuIntrSuspended);
    hle("Kernel_Library", 0x1839852Au, "sceKernelMemcpy", &Kernel::sceKernelMemcpy);
    hle("Kernel_Library", 0xA089ECA4u, "sceKernelMemset", &Kernel::sceKernelMemset);

    install_devices();
    install_callbacks();
    install_sync();
    install_power();
    install_mp3();
    install_msgpipe();
    install_tlspl();
    install_vtimer();
    install_lwmutex();
    install_mpeg();
    install_sas();
    install_atrac();
    install_network();
    install_kernel_objects();
    install_fonts();
    install_savedata();
    for (const auto &import : imports) install_fallback(import);
}

// ---------------------------------------------------------------------------
// Scheduling

Thread *Kernel::current() {
    const auto it = threads_.find(current_uid_);
    return it == threads_.end() ? nullptr : &it->second;
}

Thread *Kernel::thread(std::int32_t uid) {
    if (uid == 0) return current();
    const auto it = threads_.find(uid);
    return it == threads_.end() ? nullptr : &it->second;
}

void Kernel::make_ready(Thread &t, bool front) {
    t.state = ThreadState::Ready;
    t.wait = WaitType::None;
    t.cb_wait = false;
    t.wait_paused = false;
    t.ready_sequence = front ? --front_sequence_ : static_cast<std::int64_t>(++ready_sequence_);
}

Thread *Kernel::pick_ready() {
    Thread *best = nullptr;
    for (auto &[uid, t] : threads_) {
        (void)uid;
        if (t.state != ThreadState::Ready || t.suspended) continue;
        if (best == nullptr || t.priority < best->priority ||
            (t.priority == best->priority && t.ready_sequence < best->ready_sequence))
            best = &t;
    }
    return best;
}

void Kernel::load_thread(Thread &t) {
    rt_.cpu() = t.ctx;
    t.state = ThreadState::Running;
    current_uid_ = t.uid;
    psprecomp::set_runtime_thread_identity(t.uid, t.name);
}

// A new thread's registers, as PPSSPP sets them (PSPThreadContext::reset): FPU and
// VFPU registers hold a NaN, the VFPU control registers their power-on values.
void Kernel::reset_context(Ctx &ctx) const {
    ctx = Ctx{};
    ctx.gpr.fill(0xDEADBEEFu);
    ctx.gpr[0] = 0u;
    ctx.hi = ctx.lo = 0xDEADBEEFu;
    ctx.fpr.fill(std::bit_cast<float>(0x7F800001u));
    ctx.vfpu.fill(std::bit_cast<float>(0x7F800001u));
    ctx.fcr31 = 0x00000E00u;
    static constexpr std::uint32_t kVfpuControl[16] = {
        0xE4u, 0xE4u, 0u, 0x3Fu, 0u, 0u, 0u, 0x7772CEABu,
        0x3F800001u, 0x3F800002u, 0x3F800004u, 0x3F800008u, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u,
    };
    for (std::size_t i = 0; i < 16u; ++i) ctx.vfpu_ctrl[i] = kVfpuControl[i];
}

void Kernel::block(Ctx &ctx, WaitType wait, std::uint32_t result) {
    Thread *t = current();
    finish(ctx, result);
    if (t == nullptr) {
        rt_.stop("idle");
        return;
    }
    t->state = ThreadState::Waiting;
    t->wait = wait;
    t->has_waited = true;
    ++t->releases;
    t->wait_sequence = ++wait_sequence_;
    t->cb_wait = cb_call_;
    t->wait_paused = false;
    // In a CB wait, callbacks already notified run first, on this thread.
    if (t->cb_wait && start_pending_callback(*t, ctx, true, true)) return;
    t->ctx = ctx;
    reschedule(ctx);
}

void Kernel::reschedule(Ctx &ctx) {
    poll_waits();
    dispatch_callbacks();
    Thread *next = pick_ready();
    if (next == nullptr) {
        current_uid_ = -1;
        psprecomp::set_runtime_thread_identity(-1, "idle");
        rt_.stop("idle");
        return;
    }
    ctx = next->ctx;
    next->state = ThreadState::Running;
    current_uid_ = next->uid;
    psprecomp::set_runtime_thread_identity(next->uid, next->name);
}

void Kernel::maybe_preempt(Ctx &ctx) {
    Thread *cur = current();
    Thread *best = pick_ready();
    if (cur == nullptr || best == nullptr || best->priority >= cur->priority) return;
    cur->ctx = ctx;
    make_ready(*cur, true);
    reschedule(ctx);
}

void Kernel::wake(Thread &t, std::uint32_t result) {
    t.ctx.gpr[2] = result;
    if (t.timeout_address != 0u) {
        // The timer goes off a little after the deadline the time left counts to.
        const std::uint64_t now = now_us();
        rt_.memory().store32(t.timeout_address, t.wake_time_us > now + kTimeoutLatencyUs
                                                    ? static_cast<std::uint32_t>(t.wake_time_us - kTimeoutLatencyUs - now) : 0u);
    }
    t.wake_time_us = 0u;
    t.timeout_address = 0u;
    make_ready(t);
}

// A wait's timeout ends max(timeout, 205us) plus about 30us after the call, as measured on hardware.
void Kernel::begin_timeout(Thread &t, std::uint32_t timeout_address) {
    t.timeout_address = timeout_address;
    t.wake_time_us = timeout_address != 0u
        ? now_us() + std::max<std::uint64_t>(rt_.memory().load32(timeout_address), 205u) + kTimeoutDeadlineUs + kTimeoutLatencyUs
        : 0u;
}

void Kernel::poll_waits() {
    const std::uint64_t now = now_us();
    for (auto &[uid, t] : threads_) {
        (void)uid;
        if (t.state == ThreadState::Waiting && t.wait == WaitType::Ge && ge_worker_->done(t.wait_count)) {
            wake(t, 0u);
            continue;
        }
        if (t.state != ThreadState::Waiting || t.wait_paused || t.wake_time_us == 0u || now < t.wake_time_us) continue;
        if (t.wait == WaitType::Delay) {
            t.wake_time_us = 0u;
            wake(t, t.wait_extra);
        } else {
            // Every other wait with a deadline is an object wait with a timeout.
            if (t.wait == WaitType::EventFlag && event_flags_.contains(t.wait_uid) && valid_word(t.wait_out_address))
                rt_.memory().store32(t.wait_out_address, event_flags_.at(t.wait_uid).pattern);
            wake(t, kErrorWaitTimeout);
        }
    }
}

bool Kernel::waiting_for_ge() const {
    for (const auto &[uid, t] : threads_) {
        (void)uid;
        if (t.state == ThreadState::Waiting && t.wait == WaitType::Ge) return true;
    }
    return false;
}

std::uint64_t Kernel::earliest_timed_wake() const {
    std::uint64_t earliest = 0u;
    for (const auto &[uid, t] : threads_) {
        (void)uid;
        if (t.state == ThreadState::Waiting && t.wake_time_us != 0u &&
            (earliest == 0u || t.wake_time_us < earliest))
            earliest = t.wake_time_us;
    }
    for (const auto &[uid, alarm] : alarms_) {
        (void)uid;
        if (alarm.running == 0 && (earliest == 0u || alarm.at < earliest)) earliest = alarm.at;
    }
    if (const std::uint64_t vtimer = earliest_vtimer(); vtimer != 0u && (earliest == 0u || vtimer < earliest)) earliest = vtimer;
    return earliest;
}

void Kernel::call_guest(Ctx &ctx, std::uint32_t entry, std::initializer_list<std::uint32_t> args,
                        std::function<std::uint32_t(std::uint32_t)> done, std::uint32_t gp,
                        std::uint32_t stack_size) {
    Thread *caller = current();
    if (caller == nullptr || entry == 0u) {
        finish(ctx, done ? done(0u) : 0u);
        return;
    }
    Thread t;
    t.uid = new_uid();
    t.name = "pspweb_callback";
    t.entry = entry;
    t.priority = caller->priority;
    t.stack_size = stack_size;
    t.stack_block = allocate("stack:callback", 1u, t.stack_size, 0u);
    if (t.stack_block < 0) {
        finish(ctx, done ? done(0u) : 0u);
        return;
    }
    t.stack_top = blocks_.at(t.stack_block).address + t.stack_size;
    t.gp = gp != 0u ? gp : ctx.gpr[28];
    const std::int32_t uid = t.uid;
    auto &stored = threads_[uid] = std::move(t);
    start_thread(stored, 0u, {});
    std::uint32_t reg = 4u;
    for (const std::uint32_t value : args) stored.ctx.gpr[reg++] = value;
    make_ready(stored, true);
    guest_calls_[uid] = GuestCall{caller->uid, std::move(done)};
    block(ctx, WaitType::Callback, 0u);
}

void Kernel::dump_threads() const {
    static const char *states[] = {"dormant", "ready", "running", "waiting", "dead"};
    static const char *waits[] = {"-", "sleep", "delay", "vblank", "thread-end", "sema", "event-flag", "fpl", "io", "callback", "ge", "mutex", "mbx", "ctrl"};
    for (const auto &[uid, t] : threads_) {
        const auto &c = uid == current_uid_ ? rt_.cpu() : t.ctx;
        std::cerr << "[thread] " << uid << " " << t.name << " prio=" << t.priority << " "
                  << states[static_cast<int>(t.state)] << " wait=" << waits[static_cast<int>(t.wait)]
                  << " on=" << t.wait_uid << " pc=" << hex(c.pc) << " ra=" << hex(c.gpr[31]) << "\n";
    }
    for (const auto &[uid, s] : semaphores_)
        std::cerr << "[sema] " << uid << " " << s.name << " count=" << s.count << "\n";
    for (const auto &[uid, e] : event_flags_)
        std::cerr << "[event] " << uid << " " << e.name << " pattern=" << hex(e.pattern) << "\n";
}

void Kernel::boot(std::uint32_t entry, std::uint32_t gp, const std::string &module_path) {
    Thread t;
    t.uid = new_uid();
    t.name = "pspweb_loader";
    t.entry = entry;
    t.priority = 0x20;
    t.stack_size = 0x40000u;
    t.stack_block = allocate("stack:" + t.name, 1u, t.stack_size, 0u);
    t.stack_top = blocks_.at(t.stack_block).address + t.stack_size;
    t.gp = gp;
    std::vector<std::uint8_t> args(module_path.begin(), module_path.end());
    args.push_back(0u);
    auto &stored = threads_[t.uid] = std::move(t);
    start_thread(stored, static_cast<std::uint32_t>(args.size()), args);
}

bool Kernel::frame(double budget_ms) {
    if (halted_) return false;
    poll_io();
    ++display_.vcount;
    ge_worker_->post([ge = ge_.get()] { ge->next_frame(); });
    sample_pad();
    frame_start_guest_us_ = static_cast<std::uint64_t>(display_.vcount) * kFrameUs;
    frame_real_start_ns_ = real_now_ns();
    warp_us_ = 0u;
    for (auto &[uid, t] : threads_) {
        (void)uid;
        if (t.state == ThreadState::Waiting && t.wait == WaitType::Vblank) wake(t, 0u);
        if (t.state == ThreadState::Waiting && t.wait == WaitType::CtrlRead)
            if (const std::uint32_t copied = copy_pad(t.wait_out_address, t.wait_count, t.wait_mode != 0u, false); copied != 0u)
                wake(t, copied);
    }
    run_interrupts(kVblankInterrupt);

    const std::int64_t deadline = real_now_ns() + static_cast<std::int64_t>(budget_ms * 1e6);
    while (!halted_ && real_now_ns() < deadline) {
        if (g_deterministic) g_virtual_ns += 1000; // scheduler passes that run no guest code still end the frame
        poll_waits();
        run_alarms();
        run_vtimers();
        run_ge_callbacks();
        dispatch_callbacks();
        if (Thread *running = current()) {
            const Thread *next = pick_ready();
            if (next != nullptr && next->priority < running->priority) {
                running->ctx = rt_.cpu();
                make_ready(*running, true);
                current_uid_ = -1;
            }
        }
        if (current() == nullptr) {
            Thread *next = pick_ready();
            if (next == nullptr && waiting_for_ge()) {
                // The GE is drawing on its own thread: let real time pass
                // (no warping) until it finishes something or a timer is due.
                const auto waited_from = Clock::now();
                ge_worker_->wait_for_progress(0.25);
                ge_wait_ms_ += std::chrono::duration<double, std::milli>(Clock::now() - waited_from).count();
                continue;
            }
            if (next == nullptr) {
                const std::uint64_t wake_at = earliest_timed_wake();
                const std::uint64_t now = now_us();
                if (wake_at != 0u && wake_at < frame_start_guest_us_ + kFrameUs - 1u) {
                    if (wake_at > now) warp_us_ += wake_at - now;
                    continue;
                }
                break;
            }
            load_thread(*next);
        }

        g_slice_deadline_ns = deadline;
        try {
            rt_.run(rt_.cpu().pc, std::numeric_limits<std::uint64_t>::max());
        } catch (const std::exception &e) {
            halted_ = true;
            halt_reason_ = e.what();
            break;
        }
        const std::string &reason = rt_.stop_reason();
        if (reason == "idle" || reason == "interrupt") continue;
        if (reason == "slice") break;
        halted_ = true;
        halt_reason_ = reason;
    }

    drain_audio(frame_start_guest_us_ + kFrameUs);
    if (halted_) {
        if (halt_reason_ != "exit") std::cerr << "[kernel] halted: " << halt_reason_ << "\n";
        return false;
    }
    for (const auto &[uid, t] : threads_) {
        (void)uid;
        if (t.state == ThreadState::Ready || t.state == ThreadState::Running || t.state == ThreadState::Waiting)
            return true;
    }
    halt_reason_ = "no threads left";
    return false;
}

// ---------------------------------------------------------------------------
// Memory

void Kernel::reserve(std::uint32_t address, std::uint32_t size, std::string name) {
    blocks_[new_uid()] = MemoryBlock{std::move(name), address, align_up(size, 0x100u)};
}

std::int32_t Kernel::allocate(std::string name, std::uint32_t type, std::uint32_t size, std::uint32_t address) {
    return allocate_range(kUserPartitionStart, kUserPartitionEnd, std::move(name), type, size, address);
}

std::int32_t Kernel::allocate_in(std::int32_t partition, std::string name, std::uint32_t type, std::uint32_t size,
                                 std::uint32_t address) {
    if (partition == 5) return allocate_range(kVolatilePartitionStart, kVolatilePartitionEnd, std::move(name), type, size, address);
    return allocate(std::move(name), type, size, address);
}

std::int32_t Kernel::allocate_range(std::uint32_t low, std::uint32_t high, std::string name, std::uint32_t type,
                                    std::uint32_t size, std::uint32_t address) {
    size = align_up(std::max(size, 1u), 0x100u);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> used;
    for (const auto &[uid, block] : blocks_) {
        (void)uid;
        used.emplace_back(block.address, block.address + block.size);
    }
    std::sort(used.begin(), used.end());
    // Free gaps between used ranges inside the partition.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> gaps;
    std::uint32_t cursor = low;
    for (const auto &[begin, end] : used) {
        if (begin >= high) break;
        if (begin > cursor) gaps.emplace_back(cursor, begin);
        cursor = std::max(cursor, end);
    }
    if (cursor < high) gaps.emplace_back(cursor, high);

    std::uint32_t chosen = 0u;
    if (type == 2u) { // fixed address
        const std::uint32_t wanted = align_up(address, 0x100u);
        for (const auto &[begin, end] : gaps)
            if (wanted >= begin && wanted + size <= end) chosen = wanted;
    } else if (type == 1u) { // high
        for (auto it = gaps.rbegin(); it != gaps.rend() && chosen == 0u; ++it)
            if (it->second - it->first >= size) chosen = (it->second - size) & ~0xFFu;
    } else { // low
        for (const auto &[begin, end] : gaps)
            if (chosen == 0u && end - begin >= size) chosen = begin;
    }
    if (chosen == 0u) return -1;
    const std::int32_t uid = new_uid();
    blocks_[uid] = MemoryBlock{std::move(name), chosen, size};
    return uid;
}

bool Kernel::in_interrupt() const {
    const auto call = guest_calls_.find(current_uid_);
    return call != guest_calls_.end() && call->second.caller == 0;
}

std::uint32_t Kernel::largest_free() const {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> used;
    for (const auto &[uid, block] : blocks_) {
        (void)uid;
        used.emplace_back(block.address, block.address + block.size);
    }
    std::sort(used.begin(), used.end());
    std::uint32_t cursor = kUserPartitionStart, largest = 0u;
    for (const auto &[begin, end] : used) {
        if (begin > cursor) largest = std::max(largest, begin - cursor);
        cursor = std::max(cursor, end);
    }
    if (cursor < kUserPartitionEnd) largest = std::max(largest, kUserPartitionEnd - cursor);
    return largest;
}

std::uint32_t Kernel::total_free() const {
    std::uint32_t used = 0u;
    for (const auto &[uid, block] : blocks_) {
        (void)uid;
        used += block.size;
    }
    const std::uint32_t total = kUserPartitionEnd - kUserPartitionStart;
    return used < total ? total - used : 0u;
}

// ---------------------------------------------------------------------------
// ThreadManForUser

void Kernel::sceKernelGetSystemTimeLow(Ctx &ctx) { finish(ctx, static_cast<std::uint32_t>(now_us())); }
void Kernel::sceKernelGetSystemTimeWide(Ctx &ctx) { finish64(ctx, now_us()); }

void Kernel::sceKernelGetSystemTime(Ctx &ctx) {
    const std::uint64_t now = now_us();
    if (ctx.gpr[4] != 0u) {
        rt_.memory().store32(ctx.gpr[4], static_cast<std::uint32_t>(now));
        rt_.memory().store32(ctx.gpr[4] + 4u, static_cast<std::uint32_t>(now >> 32u));
    }
    finish(ctx, 0u);
}

// ---------------------------------------------------------------------------
// Mutexes: owned by one thread at a time, optionally recursive (attr 0x200).


// ---------------------------------------------------------------------------
// Message boxes

// ---------------------------------------------------------------------------
// Sub-interrupt handlers.  Vblank handlers run once per frame, each on a
// short-lived thread that outranks every game thread.

void Kernel::sceKernelRegisterSubIntrHandler(Ctx &ctx) {
    sub_interrupts_[{ctx.gpr[4], ctx.gpr[5]}] = SubInterrupt{ctx.gpr[6], ctx.gpr[7], ctx.gpr[28], false, 0};
    finish(ctx, 0u);
}

void Kernel::sceKernelReleaseSubIntrHandler(Ctx &ctx) {
    sub_interrupts_.erase({ctx.gpr[4], ctx.gpr[5]});
    finish(ctx, 0u);
}

void Kernel::sceKernelEnableSubIntr(Ctx &ctx) {
    auto it = sub_interrupts_.find({ctx.gpr[4], ctx.gpr[5]});
    if (it != sub_interrupts_.end()) it->second.enabled = true;
    finish(ctx, 0u);
}

void Kernel::sceKernelDisableSubIntr(Ctx &ctx) {
    auto it = sub_interrupts_.find({ctx.gpr[4], ctx.gpr[5]});
    if (it != sub_interrupts_.end()) it->second.enabled = false;
    finish(ctx, 0u);
}

void Kernel::run_interrupts(std::uint32_t interrupt) {
    if (!interrupts_enabled_) return;
    for (auto &[key, handler] : sub_interrupts_) {
        if (key.first != interrupt || !handler.enabled || handler.handler == 0u) continue;
        if (handler.running != 0 && threads_.contains(handler.running)) continue; // still busy with the last one
        const std::int32_t uid = start_handler(handler.handler, handler.gp, key.second, handler.arg, nullptr);
        if (uid == 0) return;
        handler.running = uid;
    }
}

std::int32_t Kernel::start_handler(std::uint32_t entry, std::uint32_t gp, std::uint32_t a0, std::uint32_t a1,
                                   std::function<std::uint32_t(std::uint32_t)> done) {
    Thread t;
    t.uid = new_uid();
    t.name = "pspweb_interrupt";
    t.entry = entry;
    t.priority = 1;
    t.stack_size = 0x4000u;
    t.stack_block = allocate("stack:interrupt", 1u, t.stack_size, 0u);
    if (t.stack_block < 0) return 0;
    t.stack_top = blocks_.at(t.stack_block).address + t.stack_size;
    t.gp = gp;
    const std::int32_t uid = t.uid;
    auto &stored = threads_[uid] = std::move(t);
    start_thread(stored, 0u, {});
    stored.ctx.gpr[4] = a0;
    stored.ctx.gpr[5] = a1;
    make_ready(stored, true);
    guest_calls_[uid] = GuestCall{0, std::move(done)}; // removed again when it returns
    return uid;
}

// Alarms follow PPSSPP (Core/HLE/sceKernelAlarm.cpp): the handler runs no sooner than 215us
// after the call; a positive result reschedules it that much after its last schedule.
void Kernel::set_alarm(Ctx &ctx, std::uint64_t delay_us) {
    if (!rt_.memory().contains(ctx.gpr[5], 4u)) return finish(ctx, 0x800200D3u);
    const std::int32_t uid = new_uid();
    const std::uint64_t now = now_us();
    alarms_[uid] = Alarm{now + std::clamp<std::uint64_t>(delay_us, 215u, 1ull << 52u), now + delay_us, ctx.gpr[5],
                         ctx.gpr[6], ctx.gpr[28], 0};
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelSetAlarm(Ctx &ctx) { set_alarm(ctx, ctx.gpr[4]); }

void Kernel::sceKernelSetSysClockAlarm(Ctx &ctx) {
    auto &m = rt_.memory();
    if ((ctx.gpr[4] & 3u) != 0u || !m.contains(ctx.gpr[4], 8u)) return finish(ctx, 0x800200D3u);
    set_alarm(ctx, m.load32(ctx.gpr[4]) | static_cast<std::uint64_t>(m.load32(ctx.gpr[4] + 4u)) << 32u);
}

void Kernel::sceKernelCancelAlarm(Ctx &ctx) {
    finish(ctx, alarms_.erase(static_cast<std::int32_t>(ctx.gpr[4])) != 0u ? 0u : kErrorUnknownAlarm);
}

// SceKernelAlarmInfo: size, (pad), schedule (8), handler, common; written as far as size allows.
void Kernel::sceKernelReferAlarmStatus(Ctx &ctx) {
    const auto it = alarms_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == alarms_.end()) return finish(ctx, kErrorUnknownAlarm);
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    if (!m.contains(info, 20u)) return finish(ctx, 0xFFFFFFFFu);
    const std::uint32_t size = m.load32(info);
    if (size > 0u) m.store32(info, 20u);
    if (size > 4u) {
        m.store32(info + 4u, static_cast<std::uint32_t>(it->second.schedule));
        m.store32(info + 8u, static_cast<std::uint32_t>(it->second.schedule >> 32u));
    }
    if (size > 12u) m.store32(info + 12u, it->second.handler);
    if (size > 16u) m.store32(info + 16u, it->second.common);
    finish(ctx, 0u);
}

void Kernel::run_alarms() {
    if (!interrupts_enabled_) return;
    const std::uint64_t now = now_us();
    for (auto &[uid, alarm] : alarms_) {
        if (alarm.running != 0 || now < alarm.at) continue;
        const std::int32_t id = uid;
        alarm.running = start_handler(alarm.handler, alarm.gp, alarm.common, 0u, [this, id](std::uint32_t next) {
            const auto it = alarms_.find(id);
            if (it == alarms_.end()) return 0u;
            if (static_cast<std::int32_t>(next) <= 0) {
                alarms_.erase(it);
            } else {
                const std::uint64_t at = now_us();
                Alarm &a = it->second;
                a.schedule = a.schedule + next > at ? a.schedule + next : at + next;
                a.at = a.schedule;
                a.running = 0;
            }
            return 0u;
        });
        if (alarm.running == 0) return;
    }
}

// ---------------------------------------------------------------------------
// SysMemUserForUser

void Kernel::sceKernelMaxFreeMemSize(Ctx &ctx) { finish(ctx, largest_free()); }
void Kernel::sceKernelTotalFreeMemSize(Ctx &ctx) { finish(ctx, total_free()); }

void Kernel::sceKernelAllocPartitionMemory(Ctx &ctx) {
    const std::int32_t uid = allocate(read_string(ctx.gpr[5], 32), ctx.gpr[6], ctx.gpr[7], ctx.gpr[8]);
    finish(ctx, uid < 0 ? kErrorNoMemory : static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelFreePartitionMemory(Ctx &ctx) {
    finish(ctx, blocks_.erase(static_cast<std::int32_t>(ctx.gpr[4])) != 0u ? 0u : kErrorUnknownUid);
}

void Kernel::sceKernelGetBlockHeadAddr(Ctx &ctx) {
    const auto it = blocks_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    finish(ctx, it != blocks_.end() ? it->second.address : kErrorUnknownUid);
}

void Kernel::sceKernelDevkitVersion(Ctx &ctx) { finish(ctx, 0x05000010u); }

// Every variant just records the version; several HLE functions behave differently by it.
void Kernel::sceKernelSetCompiledSdkVersion(Ctx &ctx) {
    sdk_version_ = ctx.gpr[4];
    finish(ctx, 0u);
}

void Kernel::sceKernelGetCompiledSdkVersion(Ctx &ctx) { finish(ctx, sdk_version_); }

void Kernel::sceKernelPrintf(Ctx &ctx) {
    std::cout << "[guest] " << read_string(ctx.gpr[4], 512) << "\n";
    finish(ctx, 0u);
}

// ---------------------------------------------------------------------------
// LoadExecForUser / ModuleMgrForUser / StdioForUser

void Kernel::sceKernelExitGame(Ctx &ctx) {
    finish(ctx, 0u);
    halted_ = true;
    halt_reason_ = "exit";
    rt_.stop("exit");
}

void Kernel::sceKernelStdin(Ctx &ctx) { finish(ctx, 0u); }
void Kernel::sceKernelStdout(Ctx &ctx) { finish(ctx, 1u); }
void Kernel::sceKernelStderr(Ctx &ctx) { finish(ctx, 2u); }

// ---------------------------------------------------------------------------
// sceDisplay / sceGe_user

void Kernel::sceDisplaySetMode(Ctx &ctx) { finish(ctx, 0u); }

// Some games (God of War among them) flip without waiting for vblank and would
// render several frames per displayed one.  Like PPSSPP, a second buffer change
// within one vblank holds the calling thread until the next vblank.
void Kernel::sceDisplaySetFrameBuf(Ctx &ctx) {
    ++display_.flips;
    const bool new_buffer = ctx.gpr[4] != 0u && display_.framebuffer != 0u && ctx.gpr[4] != display_.framebuffer;
    if (new_buffer) {
        ++display_.buffer_changes;
        ge_worker_->post([ge = ge_.get()] { ge->frame_flipped(); }); // after the lists queued so far
    }
    display_.framebuffer = ctx.gpr[4];
    if (ctx.gpr[5] != 0u) display_.stride = ctx.gpr[5];
    display_.format = ctx.gpr[6];
    if (new_buffer && throttle_flips_) {
        if (display_.flip_vcount == display_.vcount) {
            display_.flip_vcount = display_.vcount + 1u;
            block(ctx, WaitType::Vblank, 0u);
            return;
        }
        display_.flip_vcount = display_.vcount;
    }
    finish(ctx, 0u);
}

void Kernel::sceDisplayGetFrameBuf(Ctx &ctx) {
    if (ctx.gpr[4] != 0u) rt_.memory().store32(ctx.gpr[4], display_.framebuffer);
    if (ctx.gpr[5] != 0u) rt_.memory().store32(ctx.gpr[5], display_.stride);
    if (ctx.gpr[6] != 0u) rt_.memory().store32(ctx.gpr[6], display_.format);
    finish(ctx, 0u);
}

void Kernel::sceDisplayWaitVblankStart(Ctx &ctx) { block(ctx, WaitType::Vblank, 0u); }
void Kernel::sceDisplayGetVcount(Ctx &ctx) { finish(ctx, display_.vcount); }
void Kernel::sceGeEdramGetAddr(Ctx &ctx) { finish(ctx, kVramAddress); }
void Kernel::sceGeEdramGetSize(Ctx &ctx) { finish(ctx, kVramSize); }

// Display lists go to the GE worker and run while the guest carries on, as on
// the PSP; a stalled list resumes from where it stopped when the stall address
// moves.  The sync calls wait (mode 0) or report the state (mode 1).
void Kernel::sceGeListEnQueue(Ctx &ctx) {
    const auto callback = static_cast<std::int32_t>(ctx.gpr[6]);
    finish(ctx, ge_worker_->enqueue(ctx.gpr[4], ctx.gpr[5], ge_callbacks_.contains(callback) ? callback : -1));
}

void Kernel::sceGeSetCallback(Ctx &ctx) {
    auto &m = rt_.memory();
    const std::uint32_t data = ctx.gpr[4];
    for (std::int32_t id = 0; id < 16; ++id) {
        if (ge_callbacks_.contains(id)) continue;
        ge_callbacks_[id] = GeCallback{m.load32(data), m.load32(data + 4u), m.load32(data + 8u), m.load32(data + 12u), ctx.gpr[28]};
        finish(ctx, static_cast<std::uint32_t>(id));
        return;
    }
    finish(ctx, kErrorNoMemory);
}

void Kernel::sceGeUnsetCallback(Ctx &ctx) {
    ge_callbacks_.erase(static_cast<std::int32_t>(ctx.gpr[4]));
    finish(ctx, 0u);
}

void Kernel::run_ge_callbacks() {
    std::vector<GeEvent> events;
    {
        const std::lock_guard<std::mutex> lock(ge_events_mutex_);
        ge_event_pending_.store(false, std::memory_order_relaxed);
        if (ge_events_.empty()) return;
        events.swap(ge_events_);
    }
    for (const GeEvent &event : events) {
        const auto it = ge_callbacks_.find(event.callback);
        if (it == ge_callbacks_.end()) continue;
        const GeCallback &callback = it->second;
        const std::uint32_t entry = event.finish ? callback.finish : callback.signal;
        if (entry != 0u)
            start_handler(entry, callback.gp, event.value, event.finish ? callback.finish_arg : callback.signal_arg, nullptr);
    }
}

void Kernel::sceGeListUpdateStallAddr(Ctx &ctx) {
    ge_worker_->update_stall(ctx.gpr[4], ctx.gpr[5]);
    finish(ctx, 0u);
}

namespace {
constexpr std::uint32_t kGeListCompleted = 0u, kGeListDrawing = 2u;
}

void Kernel::sceGeListSync(Ctx &ctx) {
    const std::uint32_t id = ctx.gpr[4];
    if (ge_worker_->done(id == 0u ? 1u : id)) {
        finish(ctx, kGeListCompleted);
    } else if (ctx.gpr[5] == 1u) {
        finish(ctx, kGeListDrawing);
    } else {
        current()->wait_count = id == 0u ? 1u : id;
        block(ctx, WaitType::Ge, kGeListCompleted);
    }
}

void Kernel::sceGeDrawSync(Ctx &ctx) {
    if (ge_worker_->done(0u)) {
        finish(ctx, kGeListCompleted);
    } else if (ctx.gpr[4] == 1u) {
        finish(ctx, kGeListDrawing);
    } else {
        current()->wait_count = 0u;
        block(ctx, WaitType::Ge, kGeListCompleted);
    }
}

void Kernel::sceGeGetCmd(Ctx &ctx) { finish(ctx, 0u); }

// The context is saved and restored on the GE thread, in order with the queued lists.
void Kernel::sceGeSaveContext(Ctx &ctx) {
    std::vector<std::uint32_t> words;
    ge_worker_->call([&] { words = ge_->save_context(); });
    const std::uint32_t address = ctx.gpr[4];
    for (std::uint32_t i = 0; i < 512u; ++i)
        rt_.memory().store32(address + i * 4u, i < words.size() ? words[i] : 0u);
    finish(ctx, 0u);
}

void Kernel::sceGeRestoreContext(Ctx &ctx) {
    std::vector<std::uint32_t> words;
    for (std::uint32_t i = 0; i < 512u; ++i) {
        const std::uint32_t op = rt_.memory().load32(ctx.gpr[4] + i * 4u);
        if (op == 0u) break;
        words.push_back(op);
    }
    ge_worker_->post([ge = ge_.get(), words = std::move(words)] { ge->restore_context(words); });
    finish(ctx, 0u);
}

void Kernel::read_framebuffer(std::uint8_t *rgba) const {
    constexpr std::uint32_t width = 480u, height = 272u;
    if (display_.framebuffer == 0u) {
        for (std::uint32_t i = 0; i < width * height; ++i) {
            rgba[i * 4u + 0u] = rgba[i * 4u + 1u] = rgba[i * 4u + 2u] = 0u;
            rgba[i * 4u + 3u] = 255u;
        }
        return;
    }
    const std::uint32_t bpp = display_.format == 3u ? 4u : 2u;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t *row = rt_.memory().raw_pointer(
            display_.framebuffer + y * display_.stride * bpp, width * bpp);
        std::uint8_t *out = rgba + y * width * 4u;
        for (std::uint32_t x = 0; x < width; ++x, out += 4u) {
            if (row == nullptr) {
                out[0] = out[1] = out[2] = 0u;
            } else if (bpp == 4u) {
                out[0] = row[x * 4u + 0u];
                out[1] = row[x * 4u + 1u];
                out[2] = row[x * 4u + 2u];
            } else {
                const std::uint32_t v = static_cast<std::uint32_t>(row[x * 2u]) |
                                        (static_cast<std::uint32_t>(row[x * 2u + 1u]) << 8u);
                if (display_.format == 0u) { // 5650
                    out[0] = static_cast<std::uint8_t>(((v >> 0u) & 31u) * 255u / 31u);
                    out[1] = static_cast<std::uint8_t>(((v >> 5u) & 63u) * 255u / 63u);
                    out[2] = static_cast<std::uint8_t>(((v >> 11u) & 31u) * 255u / 31u);
                } else if (display_.format == 1u) { // 5551
                    out[0] = static_cast<std::uint8_t>(((v >> 0u) & 31u) * 255u / 31u);
                    out[1] = static_cast<std::uint8_t>(((v >> 5u) & 31u) * 255u / 31u);
                    out[2] = static_cast<std::uint8_t>(((v >> 10u) & 31u) * 255u / 31u);
                } else { // 4444
                    out[0] = static_cast<std::uint8_t>(((v >> 0u) & 15u) * 17u);
                    out[1] = static_cast<std::uint8_t>(((v >> 4u) & 15u) * 17u);
                    out[2] = static_cast<std::uint8_t>(((v >> 8u) & 15u) * 17u);
                }
            }
            out[3] = 255u;
        }
    }
}

// ---------------------------------------------------------------------------
// UtilsForUser / Kernel_Library

void Kernel::sceKernelLibcTime(Ctx &ctx) {
    const auto now = static_cast<std::uint32_t>(unix_time_us() / 1000000u);
    if (ctx.gpr[4] != 0u) rt_.memory().store32(ctx.gpr[4], now);
    finish(ctx, now);
}

void Kernel::track_gpu_vram() {
    rt_.memory().set_vram_read_hook(
        [](void *self, std::uint32_t offset, std::uint32_t length) {
            auto &kernel = *static_cast<Kernel *>(self);
            if (!kernel.ge_worker_->threaded() || kernel.ge_worker_->on_ge_thread()) {
                kernel.ge_->sync_vram_for_cpu(offset, length);
                return;
            }
            // Behind every list queued so far, as the guest synchronizes before reading what the GE drew.
            kernel.ge_worker_->call([&] { kernel.ge_->sync_vram_for_cpu(offset, length); });
        },
        this);
}

void Kernel::sceKernelLibcGettimeofday(Ctx &ctx) {
    const std::uint64_t now = unix_time_us() / 1000000u * 1000000u + now_us() % 1000000u;
    if (ctx.gpr[4] != 0u) {
        rt_.memory().store32(ctx.gpr[4], static_cast<std::uint32_t>(now / 1000000u));
        rt_.memory().store32(ctx.gpr[4] + 4u, static_cast<std::uint32_t>(now % 1000000u));
    }
    finish(ctx, 0u);
}

void Kernel::sceKernelMemcpy(Ctx &ctx) {
    const std::uint32_t dst = ctx.gpr[4], src = ctx.gpr[5], size = ctx.gpr[6];
    rt_.memory().note_raw_access(src, size, false);
    rt_.memory().note_raw_access(dst, size, true);
    std::uint8_t *to = rt_.memory().raw_pointer(dst, size);
    const std::uint8_t *from = rt_.memory().raw_pointer(src, size);
    if (to != nullptr && from != nullptr) {
        std::memmove(to, from, size);
    } else {
        std::vector<std::uint8_t> bytes(size);
        for (std::uint32_t i = 0; i < size; ++i) bytes[i] = rt_.memory().load8(src + i);
        for (std::uint32_t i = 0; i < size; ++i) rt_.memory().store8(dst + i, bytes[i]);
    }
    finish(ctx, dst);
}

void Kernel::sceKernelMemset(Ctx &ctx) {
    const std::uint32_t dst = ctx.gpr[4], size = ctx.gpr[6];
    const auto value = static_cast<std::uint8_t>(ctx.gpr[5]);
    rt_.memory().note_raw_access(dst, size, true);
    if (std::uint8_t *to = rt_.memory().raw_pointer(dst, size); to != nullptr) {
        std::memset(to, value, size);
    } else {
        for (std::uint32_t i = 0; i < size; ++i) rt_.memory().store8(dst + i, value);
    }
    finish(ctx, dst);
}

// Interrupts (vblank handlers, alarms, virtual timers) stay pending while suspended.
void Kernel::sceKernelCpuSuspendIntr(Ctx &ctx) {
    finish(ctx, interrupts_enabled_ ? 1u : 0u);
    interrupts_enabled_ = false;
}

void Kernel::sceKernelCpuResumeIntr(Ctx &ctx) {
    interrupts_enabled_ = ctx.gpr[4] != 0u;
    finish(ctx, 0u);
}

void Kernel::sceKernelIsCpuIntrEnable(Ctx &ctx) { finish(ctx, interrupts_enabled_ ? 1u : 0u); }
void Kernel::sceKernelIsCpuIntrSuspended(Ctx &ctx) { finish(ctx, ctx.gpr[4] == 0u ? 1u : 0u); }
void Kernel::return_zero(Ctx &ctx) { finish(ctx, 0u); }

} // namespace pspweb
