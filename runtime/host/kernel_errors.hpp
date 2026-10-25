// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// PSP kernel error codes (SceKernelErrors), as the HLE functions return them.

#include <cstdint>

namespace pspweb::err {

inline constexpr std::uint32_t kError = 0x80020001u;
inline constexpr std::uint32_t kIllegalContext = 0x80020064u;
inline constexpr std::uint32_t kCpuDi = 0x80020066u;
inline constexpr std::uint32_t kUnknownUid = 0x800200CBu;
inline constexpr std::uint32_t kIllegalPerm = 0x800200D1u;
inline constexpr std::uint32_t kIllegalArgument = 0x800200D2u;
inline constexpr std::uint32_t kIllegalAddr = 0x800200D3u;
inline constexpr std::uint32_t kIllegalPartition = 0x800200D6u;
inline constexpr std::uint32_t kIllegalMemblockType = 0x800200D8u;
inline constexpr std::uint32_t kMemblockAllocFailed = 0x800200D9u;
inline constexpr std::uint32_t kNoMemory = 0x80020190u;
inline constexpr std::uint32_t kIllegalAttr = 0x80020191u;
inline constexpr std::uint32_t kIllegalEntry = 0x80020192u;
inline constexpr std::uint32_t kIllegalPriority = 0x80020193u;
inline constexpr std::uint32_t kIllegalStackSize = 0x80020194u;
inline constexpr std::uint32_t kIllegalMode = 0x80020195u;
inline constexpr std::uint32_t kIllegalMask = 0x80020196u;
inline constexpr std::uint32_t kIllegalThid = 0x80020197u;
inline constexpr std::uint32_t kUnknownThid = 0x80020198u;
inline constexpr std::uint32_t kUnknownSemid = 0x80020199u;
inline constexpr std::uint32_t kUnknownEvfid = 0x8002019Au;
inline constexpr std::uint32_t kUnknownMbxid = 0x8002019Bu;
inline constexpr std::uint32_t kUnknownVplid = 0x8002019Cu;
inline constexpr std::uint32_t kUnknownFplid = 0x8002019Du;
inline constexpr std::uint32_t kUnknownMppid = 0x8002019Eu;
inline constexpr std::uint32_t kUnknownAlmid = 0x8002019Fu;
inline constexpr std::uint32_t kUnknownTeid = 0x800201A0u;
inline constexpr std::uint32_t kUnknownCbid = 0x800201A1u;
inline constexpr std::uint32_t kDormant = 0x800201A2u;
inline constexpr std::uint32_t kSuspend = 0x800201A3u;
inline constexpr std::uint32_t kNotDormant = 0x800201A4u;
inline constexpr std::uint32_t kNotSuspend = 0x800201A5u;
inline constexpr std::uint32_t kNotWait = 0x800201A6u;
inline constexpr std::uint32_t kCanNotWait = 0x800201A7u;
inline constexpr std::uint32_t kWaitTimeout = 0x800201A8u;
inline constexpr std::uint32_t kWaitCancel = 0x800201A9u;
inline constexpr std::uint32_t kReleaseWait = 0x800201AAu;
inline constexpr std::uint32_t kThreadTerminated = 0x800201ACu;
inline constexpr std::uint32_t kSemaZero = 0x800201ADu;
inline constexpr std::uint32_t kSemaOverflow = 0x800201AEu;
inline constexpr std::uint32_t kEvfCond = 0x800201AFu;
inline constexpr std::uint32_t kEvfMulti = 0x800201B0u;
inline constexpr std::uint32_t kEvfIllegalPattern = 0x800201B1u;
inline constexpr std::uint32_t kMboxNoMessage = 0x800201B2u;
inline constexpr std::uint32_t kMppFull = 0x800201B3u;
inline constexpr std::uint32_t kMppEmpty = 0x800201B4u;
inline constexpr std::uint32_t kWaitDelete = 0x800201B5u;
inline constexpr std::uint32_t kIllegalMemblock = 0x800201B6u;
inline constexpr std::uint32_t kIllegalMemsize = 0x800201B7u;
inline constexpr std::uint32_t kIllegalType = 0x800201BBu;
inline constexpr std::uint32_t kIllegalSize = 0x800201BCu;
inline constexpr std::uint32_t kIllegalCount = 0x800201BDu;
inline constexpr std::uint32_t kUnknownVtid = 0x800201BEu;
inline constexpr std::uint32_t kIllegalVtid = 0x800201BFu;
inline constexpr std::uint32_t kMutexNotFound = 0x800201C3u;
inline constexpr std::uint32_t kMutexLocked = 0x800201C4u;
inline constexpr std::uint32_t kMutexUnlocked = 0x800201C5u;
inline constexpr std::uint32_t kMutexLockOverflow = 0x800201C6u;
inline constexpr std::uint32_t kMutexUnlockUnderflow = 0x800201C7u;
inline constexpr std::uint32_t kMutexRecursiveNotAllowed = 0x800201C8u;
inline constexpr std::uint32_t kMessageboxDuplicateMessage = 0x800201C9u;
inline constexpr std::uint32_t kUnknownTlsplId = 0x800201D0u;
inline constexpr std::uint32_t kTooManyTlspl = 0x800201D1u;
inline constexpr std::uint32_t kTlsplInUse = 0x800201D2u;

} // namespace pspweb::err
