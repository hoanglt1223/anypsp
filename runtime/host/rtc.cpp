// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceRtc: ticks are microseconds since 0001-01-01, ScePspDateTime is year, month, day, hour,
// minute, second (16 bits each) and microseconds. Conversions, validation and the date
// arithmetic follow PPSSPP (Core/HLE/sceRtc.cpp, GPL-2.0-or-later). The local time zone is the
// host's.

#include "kernel.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace pspweb {
namespace {

constexpr std::uint64_t kUnixEpochTick = 62135596800000000ull;  // 1970-01-01 in ticks
constexpr std::uint64_t kFiletimeEpochTick = 50491123200000000ull; // 1601-01-01 in ticks
constexpr std::uint64_t kLastAdjustedTick = kUnixEpochTick + 41ull * 365u * 24u * 3600u * 1000000u;
constexpr std::uint64_t kLastReincarnatedTick = kUnixEpochTick + 40ull * 365u * 24u * 3600u * 1000000u;
constexpr std::uint64_t kDayUs = 24ull * 60u * 60u * 1000000u;
constexpr std::uint32_t kErrorInvalidValue = 0x800001FEu;
constexpr std::uint32_t kErrorInvalidArgument = 0x800001FFu;

struct DateTime {
    std::int16_t year{}, month{}, day{}, hour{}, minute{}, second{};
    std::uint32_t microsecond{};
};

std::int64_t days_from_civil(std::int64_t y, std::uint32_t m, std::uint32_t d) {
    y -= m <= 2u;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<std::uint32_t>(y - era * 400);
    const std::uint32_t doy = (153u * (m > 2u ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;
    const std::uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, std::int64_t &y, std::uint32_t &m, std::uint32_t &d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<std::uint32_t>(z - era * 146097);
    const std::uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const std::uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const std::uint32_t mp = (5u * doy + 2u) / 153u;
    d = doy - (153u * mp + 2u) / 5u + 1u;
    m = mp < 10u ? mp + 3u : mp - 9u;
    y = static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2u);
}

DateTime from_ticks(std::uint64_t ticks) {
    DateTime t;
    t.microsecond = static_cast<std::uint32_t>(ticks % 1000000u);
    t.second = static_cast<std::int16_t>(ticks / 1000000u % 60u);
    t.minute = static_cast<std::int16_t>(ticks / 60000000u % 60u);
    t.hour = static_cast<std::int16_t>(ticks / 3600000000ull % 24u);
    std::int64_t y = 0;
    std::uint32_t m = 0, d = 0;
    civil_from_days(static_cast<std::int64_t>(ticks / kDayUs) - static_cast<std::int64_t>(kUnixEpochTick / kDayUs), y, m, d);
    t.year = static_cast<std::int16_t>(y);
    t.month = static_cast<std::int16_t>(m);
    t.day = static_cast<std::int16_t>(d);
    return t;
}

std::uint64_t to_ticks(const DateTime &t) {
    const std::int64_t z = days_from_civil(t.year, static_cast<std::uint32_t>(static_cast<std::uint16_t>(t.month)),
                                           static_cast<std::uint32_t>(static_cast<std::uint16_t>(t.day)));
    return kUnixEpochTick + t.microsecond +
           1000000ull * static_cast<std::uint64_t>(t.second + 60ll * (t.minute + 60ll * (t.hour + 24ll * z)));
}

bool valid_year(const DateTime &t) { return t.year > 0 && t.year <= 9999; }
bool leap(std::uint32_t year) { return year % 4u == 0u && (year % 100u != 0u || year % 400u == 0u); }

int days_in_month(std::uint32_t year, std::uint32_t month) {
    switch (month) {
    case 4: case 6: case 9: case 11: return 30;
    case 2: return leap(year) ? 29 : 28;
    default: return 31;
    }
}

std::int64_t host_utc_offset_seconds() {
    const std::time_t zero = 0;
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &zero);
    std::tm utc{};
    gmtime_s(&utc, &zero);
    return static_cast<std::int64_t>(std::difftime(std::mktime(&local), std::mktime(&utc)));
#else
    localtime_r(&zero, &local);
    return local.tm_gmtoff;
#endif
}

bool parse_digits(const char *&p, int n, int &value) {
    value = 0;
    for (int i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
        value = value * 10 + (p[i] - '0');
    }
    p += n;
    return true;
}

// "2010-09-20T07:12:15.00Z" or with a "+hh:mm" offset.
bool parse_rfc3339(const char *s, DateTime &date, int &offset_minutes) {
    int year, month, day, hour, minute, second, micro = 0;
    if (!parse_digits(s, 4, year) || *s++ != '-' || !parse_digits(s, 2, month) || *s++ != '-' || !parse_digits(s, 2, day)) return false;
    if (*s++ != 'T' || !parse_digits(s, 2, hour) || *s++ != ':' || !parse_digits(s, 2, minute) || *s++ != ':' || !parse_digits(s, 2, second))
        return false;
    if (*s == '.') {
        ++s;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 6) {
            micro = micro * 10 + (*s++ - '0');
            ++digits;
        }
        for (; digits < 6; ++digits) micro *= 10;
    }
    if (*s == 'Z') {
        ++s;
        offset_minutes = 0;
    } else if (*s == '+' || *s == '-') {
        const int sign = *s++ == '+' ? 1 : -1;
        int tz_hour, tz_minute;
        if (!parse_digits(s, 2, tz_hour) || *s++ != ':' || !parse_digits(s, 2, tz_minute)) return false;
        offset_minutes = sign * (tz_hour * 60 + tz_minute);
    } else {
        return false;
    }
    if (*s != '\0') return false;
    date = DateTime{static_cast<std::int16_t>(year), static_cast<std::int16_t>(month), static_cast<std::int16_t>(day),
                    static_cast<std::int16_t>(hour), static_cast<std::int16_t>(minute), static_cast<std::int16_t>(second),
                    static_cast<std::uint32_t>(micro)};
    return true;
}

// "Mon, 20 Sep 2010 07:12:15 +0000".
bool parse_rfc2822(const char *s, DateTime &date, int &offset_minutes) {
    while (*s != '\0' && *s != ' ' && *s != ',') ++s;
    if (*s++ != ',' || *s++ != ' ') return false;
    static constexpr const char *names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int day, month = 0, year, hour, minute, second;
    if (!parse_digits(s, 2, day) || *s++ != ' ') return false;
    for (int i = 0; i < 12 && month == 0; ++i)
        if (std::strncmp(s, names[i], 3) == 0) month = i + 1;
    if (month == 0) return false;
    s += 3;
    if (*s++ != ' ' || !parse_digits(s, 4, year) || *s++ != ' ') return false;
    if (!parse_digits(s, 2, hour) || *s++ != ':' || !parse_digits(s, 2, minute) || *s++ != ':' || !parse_digits(s, 2, second)) return false;
    if (*s++ != ' ' || (*s != '+' && *s != '-')) return false;
    const int sign = *s++ == '+' ? 1 : -1;
    int tz_hour, tz_minute;
    if (!parse_digits(s, 2, tz_hour) || !parse_digits(s, 2, tz_minute) || *s != '\0') return false;
    offset_minutes = sign * (tz_hour * 60 + tz_minute);
    date = DateTime{static_cast<std::int16_t>(year), static_cast<std::int16_t>(month), static_cast<std::int16_t>(day),
                    static_cast<std::int16_t>(hour), static_cast<std::int16_t>(minute), static_cast<std::int16_t>(second), 0u};
    return true;
}

} // namespace

void Kernel::install_rtc() {
    psprecomp::GuestMemory *const mem = &rt_.memory();
    const auto read_date = [mem](std::uint32_t at) {
        DateTime t;
        t.year = static_cast<std::int16_t>(mem->load16(at));
        t.month = static_cast<std::int16_t>(mem->load16(at + 2u));
        t.day = static_cast<std::int16_t>(mem->load16(at + 4u));
        t.hour = static_cast<std::int16_t>(mem->load16(at + 6u));
        t.minute = static_cast<std::int16_t>(mem->load16(at + 8u));
        t.second = static_cast<std::int16_t>(mem->load16(at + 10u));
        t.microsecond = mem->load32(at + 12u);
        return t;
    };
    const auto write_date = [mem](std::uint32_t at, const DateTime &t) {
        mem->store16(at, static_cast<std::uint16_t>(t.year));
        mem->store16(at + 2u, static_cast<std::uint16_t>(t.month));
        mem->store16(at + 4u, static_cast<std::uint16_t>(t.day));
        mem->store16(at + 6u, static_cast<std::uint16_t>(t.hour));
        mem->store16(at + 8u, static_cast<std::uint16_t>(t.minute));
        mem->store16(at + 10u, static_cast<std::uint16_t>(t.second));
        mem->store32(at + 12u, t.microsecond);
    };
    const auto date_ok = [mem](std::uint32_t at) { return at != 0u && mem->contains(at, 16u); };
    const auto tick_ok = [mem](std::uint32_t at) { return at != 0u && (at & 3u) == 0u && mem->contains(at, 8u); };
    const auto load64 = [mem](std::uint32_t at) { return mem->load32(at) | static_cast<std::uint64_t>(mem->load32(at + 4u)) << 32u; };
    const auto store64 = [mem](std::uint32_t at, std::uint64_t v) {
        mem->store32(at, static_cast<std::uint32_t>(v));
        mem->store32(at + 4u, static_cast<std::uint32_t>(v >> 32u));
    };
    // The guest's clock in ticks: the host's calendar time when the guest started plus guest time.
    const auto current_tick = [this] {
        if (rtc_base_us_ == 0u) rtc_base_us_ = unix_time_us() / 1000000u * 1000000u - now_us();
        return kUnixEpochTick + rtc_base_us_ + now_us();
    };
    const auto arg64 = [](const Ctx &ctx) { return ctx.gpr[6] | static_cast<std::uint64_t>(ctx.gpr[7]) << 32u; };
    // (dest tick, src tick, amount): adds amount * unit; amount is 64-bit (a2:a3) or 32-bit (a2).
    const auto add = [=, this](std::uint32_t nid, const char *name, std::uint64_t unit, bool wide) {
        hle_lambda("sceRtc", nid, name, [=](Ctx &ctx) {
            if (tick_ok(ctx.gpr[4]) && tick_ok(ctx.gpr[5])) {
                const std::int64_t amount = wide ? static_cast<std::int64_t>(arg64(ctx)) : static_cast<std::int32_t>(ctx.gpr[6]);
                store64(ctx.gpr[4], load64(ctx.gpr[5]) + static_cast<std::uint64_t>(amount) * unit);
            }
            finish(ctx, 0u);
        });
    };
    const char *rtc = "sceRtc";
    hle_lambda(rtc, 0xC41C2853u, "sceRtcGetTickResolution", [](Ctx &ctx) { finish(ctx, 1000000u); });
    hle_lambda(rtc, 0x3F7AD767u, "sceRtcGetCurrentTick", [=](Ctx &ctx) {
        if (tick_ok(ctx.gpr[4])) store64(ctx.gpr[4], current_tick());
        finish(ctx, 0u);
    });
    const auto accumulative = [=](Ctx &ctx) { finish64(ctx, current_tick()); };
    hle_lambda(rtc, 0x011F03C1u, "sceRtcGetAccumulativeTime", accumulative);
    hle_lambda(rtc, 0x029CA3B3u, "sceRtcGetAccumlativeTime", accumulative);
    hle_lambda(rtc, 0x4CFA57B0u, "sceRtcGetCurrentClock", [=](Ctx &ctx) {
        // UTC shifted by the time zone the caller gives, in minutes.
        const std::uint64_t tick = current_tick() + static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(ctx.gpr[5])) * 60000000);
        if (date_ok(ctx.gpr[4])) write_date(ctx.gpr[4], from_ticks(tick));
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0xE7C27D1Bu, "sceRtcGetCurrentClockLocalTime", [=](Ctx &ctx) {
        const std::uint64_t tick = current_tick() + static_cast<std::uint64_t>(host_utc_offset_seconds() * 1000000);
        if (date_ok(ctx.gpr[4])) write_date(ctx.gpr[4], from_ticks(tick));
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0x34885E0Du, "sceRtcConvertUtcToLocalTime", [=](Ctx &ctx) {
        if (!tick_ok(ctx.gpr[4]) || !tick_ok(ctx.gpr[5])) return finish(ctx, 1u);
        store64(ctx.gpr[5], load64(ctx.gpr[4]) + static_cast<std::uint64_t>(host_utc_offset_seconds() * 1000000));
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0x779242A2u, "sceRtcConvertLocalTimeToUTC", [=](Ctx &ctx) {
        if (!tick_ok(ctx.gpr[4]) || !tick_ok(ctx.gpr[5])) return finish(ctx, 1u);
        store64(ctx.gpr[5], load64(ctx.gpr[4]) - static_cast<std::uint64_t>(host_utc_offset_seconds() * 1000000));
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0x42307A17u, "sceRtcIsLeapYear", [](Ctx &ctx) { finish(ctx, leap(ctx.gpr[4]) ? 1u : 0u); });
    hle_lambda(rtc, 0x05EF322Cu, "sceRtcGetDaysInMonth", [](Ctx &ctx) {
        if (ctx.gpr[4] == 0u || ctx.gpr[5] == 0u || ctx.gpr[5] > 12u) return finish(ctx, kErrorInvalidArgument);
        finish(ctx, static_cast<std::uint32_t>(days_in_month(ctx.gpr[4], ctx.gpr[5])));
    });
    hle_lambda(rtc, 0x57726BC1u, "sceRtcGetDayOfWeek", [](Ctx &ctx) {
        std::uint32_t year = ctx.gpr[4], month = ctx.gpr[5], day = ctx.gpr[6];
        // Month 0 counts as August, and months past 12 go on 31, 31, 30, 31, 30 days.
        if (month == 0u) month = 8u;
        if (month > 12u) {
            std::uint32_t rest = month - 12u;
            day += rest / 5u * (31u * 3u + 30u * 2u);
            rest %= 5u;
            static constexpr std::uint32_t extra[] = {31u, 31u * 2u, 31u * 2u + 30u, 31u * 3u + 30u, 31u * 3u + 30u * 2u};
            if (rest > 0u) day += extra[rest - 1u];
            month = 12u;
        }
        finish(ctx, static_cast<std::uint32_t>(((days_from_civil(year, month, day) % 7) + 11) % 7));
    });
    hle_lambda(rtc, 0x4B1B5E82u, "sceRtcCheckValid", [=](Ctx &ctx) {
        if (!date_ok(ctx.gpr[4])) return finish(ctx, 0xFFFFFFFFu);
        const DateTime t = read_date(ctx.gpr[4]);
        std::int32_t result = 0;
        if (t.year < 1 || t.year > 9999) result = -1;
        else if (t.month < 1 || t.month > 12) result = -2;
        else if (t.day < 1 || t.day > 31 || t.day > days_in_month(static_cast<std::uint32_t>(t.year), static_cast<std::uint32_t>(t.month))) result = -3;
        else if (t.hour < 0 || t.hour > 23) result = -4;
        else if (t.minute < 0 || t.minute > 59) result = -5;
        else if (t.second < 0 || t.second > 59) result = -6;
        else if (t.microsecond >= 1000000u) result = -7;
        finish(ctx, static_cast<std::uint32_t>(result));
    });
    hle_lambda(rtc, 0x3A807CC8u, "sceRtcSetTime_t", [=](Ctx &ctx) {
        if (!date_ok(ctx.gpr[4])) return finish(ctx, 1u);
        write_date(ctx.gpr[4], from_ticks(ctx.gpr[5] * 1000000ull + kUnixEpochTick));
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0x1909C99Bu, "sceRtcSetTime64_t", [=](Ctx &ctx) {
        if (!date_ok(ctx.gpr[4])) return finish(ctx, 1u);
        write_date(ctx.gpr[4], from_ticks(arg64(ctx) * 1000000ull + kUnixEpochTick));
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0x27C4594Cu, "sceRtcGetTime_t", [=](Ctx &ctx) {
        if (!date_ok(ctx.gpr[4]) || !mem->contains(ctx.gpr[5], 4u)) return finish(ctx, 1u);
        mem->store32(ctx.gpr[5], static_cast<std::uint32_t>((to_ticks(read_date(ctx.gpr[4])) - kUnixEpochTick) / 1000000u));
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0xE1C93E47u, "sceRtcGetTime64_t", [=](Ctx &ctx) {
        if (!date_ok(ctx.gpr[4]) || !tick_ok(ctx.gpr[5])) return finish(ctx, 1u);
        store64(ctx.gpr[5], (to_ticks(read_date(ctx.gpr[4])) - kUnixEpochTick) / 1000000u);
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0xF006F264u, "sceRtcSetDosTime", [=](Ctx &ctx) {
        if (!date_ok(ctx.gpr[4])) return finish(ctx, 1u);
        const std::uint32_t hms = ctx.gpr[5] & 0xFFFFu, ymd = ctx.gpr[5] >> 16u;
        DateTime t;
        t.year = static_cast<std::int16_t>(1980u + (ymd >> 9u));
        t.month = static_cast<std::int16_t>((ymd >> 5u) & 0xFu);
        t.day = static_cast<std::int16_t>(ymd & 0x1Fu);
        t.hour = static_cast<std::int16_t>(hms >> 11u);
        t.minute = static_cast<std::int16_t>((hms >> 5u) & 0x3Fu);
        t.second = static_cast<std::int16_t>((hms << 1u) & 0x3Eu);
        write_date(ctx.gpr[4], t);
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0x36075567u, "sceRtcGetDosTime", [=](Ctx &ctx) {
        if (!date_ok(ctx.gpr[4]) || !mem->contains(ctx.gpr[5], 4u)) return finish(ctx, 0xFFFFFFFFu);
        const DateTime t = read_date(ctx.gpr[4]);
        if (t.year < 1980) {
            mem->store32(ctx.gpr[5], 0u);
            return finish(ctx, 0xFFFFFFFFu);
        }
        if (t.year >= 2108) {
            mem->store32(ctx.gpr[5], 0xFF9FBF7Du);
            return finish(ctx, 0xFFFFFFFFu);
        }
        const std::uint32_t ymd = ((static_cast<std::uint32_t>(t.year - 1980) & 0x7Fu) << 9u) | ((static_cast<std::uint32_t>(t.month) & 0xFu) << 5u) |
                                  (static_cast<std::uint32_t>(t.day) & 0x1Fu);
        const std::uint32_t hms = ((static_cast<std::uint32_t>(t.hour) & 0x1Fu) << 11u) | ((static_cast<std::uint32_t>(t.minute) & 0x3Fu) << 5u) |
                                  ((static_cast<std::uint32_t>(t.second) >> 1u) & 0x1Fu);
        mem->store32(ctx.gpr[5], (ymd << 16u) | hms);
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0x7ACE4C04u, "sceRtcSetWin32FileTime", [=](Ctx &ctx) {
        if (!date_ok(ctx.gpr[4])) return finish(ctx, 0xFFFFFFFFu);
        write_date(ctx.gpr[4], from_ticks(arg64(ctx) / 10u + kFiletimeEpochTick));
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0xCF561893u, "sceRtcGetWin32FileTime", [=](Ctx &ctx) {
        if (!date_ok(ctx.gpr[4])) return finish(ctx, 0xFFFFFFFFu);
        if (!tick_ok(ctx.gpr[5])) return finish(ctx, kErrorInvalidValue);
        const DateTime t = read_date(ctx.gpr[4]);
        const std::uint64_t ticks = to_ticks(t);
        if (!valid_year(t) || ticks < kFiletimeEpochTick) {
            store64(ctx.gpr[5], 0u);
            return finish(ctx, kErrorInvalidValue);
        }
        store64(ctx.gpr[5], (ticks - kFiletimeEpochTick) * 10u);
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0x7ED29E40u, "sceRtcSetTick", [=](Ctx &ctx) {
        if (date_ok(ctx.gpr[4]) && tick_ok(ctx.gpr[5])) write_date(ctx.gpr[4], from_ticks(load64(ctx.gpr[5])));
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0x6FF40ACCu, "sceRtcGetTick", [=](Ctx &ctx) {
        if (!date_ok(ctx.gpr[4]) || !tick_ok(ctx.gpr[5])) return finish(ctx, 0u);
        const DateTime t = read_date(ctx.gpr[4]);
        if (!valid_year(t)) return finish(ctx, kErrorInvalidValue);
        store64(ctx.gpr[5], to_ticks(t));
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0x9ED0AE87u, "sceRtcCompareTick", [=](Ctx &ctx) {
        std::uint32_t result = 0u;
        if (tick_ok(ctx.gpr[4]) && tick_ok(ctx.gpr[5])) {
            const std::uint64_t a = load64(ctx.gpr[4]), b = load64(ctx.gpr[5]);
            result = a > b ? 1u : a < b ? 0xFFFFFFFFu : 0u;
        }
        finish(ctx, result);
    });
    add(0x44F45E05u, "sceRtcTickAddTicks", 1u, true);
    add(0x26D25A5Du, "sceRtcTickAddMicroseconds", 1u, true);
    add(0xF2A4AFE5u, "sceRtcTickAddSeconds", 1000000u, true);
    add(0xE6605BCAu, "sceRtcTickAddMinutes", 60000000u, true);
    add(0x26D7A24Au, "sceRtcTickAddHours", 3600000000ull, false);
    add(0xE51B4B7Au, "sceRtcTickAddDays", kDayUs, false);
    add(0xCF3A2CA8u, "sceRtcTickAddWeeks", 7u * kDayUs, false);
    // Months and years keep the day, clamping 29 February to the 28th in other years.
    const auto add_calendar = [=, this](std::uint32_t nid, const char *name, bool months) {
        hle_lambda(rtc, nid, name, [=](Ctx &ctx) {
            if (!tick_ok(ctx.gpr[4]) || !tick_ok(ctx.gpr[5])) return finish(ctx, 0xFFFFFFFFu);
            DateTime t = from_ticks(load64(ctx.gpr[5]));
            const auto amount = static_cast<std::int32_t>(ctx.gpr[6]);
            if (months) {
                t.year = static_cast<std::int16_t>(t.year + amount / 12);
                t.month = static_cast<std::int16_t>(t.month + amount % 12);
                if (t.month < 1) {
                    t.month = static_cast<std::int16_t>(t.month + 12);
                    --t.year;
                }
                if (t.month > 12) {
                    t.month = static_cast<std::int16_t>(t.month - 12);
                    ++t.year;
                }
            } else {
                t.year = static_cast<std::int16_t>(t.year + amount);
            }
            if (valid_year(t)) {
                if (t.month == 2 && t.day == 29 && !leap(static_cast<std::uint32_t>(t.year))) t.day = 28;
                store64(ctx.gpr[4], to_ticks(t));
            }
            finish(ctx, 0u);
        });
    };
    add_calendar(0xDBF74F1Bu, "sceRtcTickAddMonths", true);
    add_calendar(0x42842C77u, "sceRtcTickAddYears", false);
    const auto format = [=, this](std::uint32_t nid, const char *name, bool rfc3339, bool local) {
        hle_lambda(rtc, nid, name, [=](Ctx &ctx) {
            if (!mem->contains(ctx.gpr[4], 32u) || !tick_ok(ctx.gpr[5])) return finish(ctx, 0xFFFFFFFFu);
            const int tz = local ? static_cast<int>(host_utc_offset_seconds() / 60) : static_cast<std::int32_t>(ctx.gpr[6]);
            const DateTime t = from_ticks(load64(ctx.gpr[5]) + static_cast<std::uint64_t>(static_cast<std::int64_t>(tz) * 60000000));
            static constexpr const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
            static constexpr const char *months_[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            const std::int64_t z = days_from_civil(t.year, static_cast<std::uint32_t>(t.month), static_cast<std::uint32_t>(t.day));
            char text[64];
            const int abs_tz = tz < 0 ? -tz : tz;
            if (rfc3339) {
                if (tz == 0) std::snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d.00Z", t.year, t.month, t.day, t.hour, t.minute, t.second);
                else std::snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d.00%c%02d:%02d", t.year, t.month, t.day, t.hour, t.minute,
                                   t.second, tz < 0 ? '-' : '+', abs_tz / 60, abs_tz % 60);
            } else {
                std::snprintf(text, sizeof text, "%s, %02d %s %04d %02d:%02d:%02d %c%02d%02d", days[((z % 7) + 11) % 7], t.day,
                              months_[(t.month + 11) % 12], t.year, t.hour, t.minute, t.second, tz < 0 ? '-' : '+', abs_tz / 60, abs_tz % 60);
            }
            for (std::size_t i = 0; i < 32u; ++i) {
                mem->store8(ctx.gpr[4] + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(text[i]));
                if (text[i] == '\0') break;
            }
            finish(ctx, 0u);
        });
    };
    format(0xC663B3B9u, "sceRtcFormatRFC2822", false, false);
    format(0x7DE6711Bu, "sceRtcFormatRFC2822LocalTime", false, true);
    format(0x0498FB3Cu, "sceRtcFormatRFC3339", true, false);
    format(0x27F98543u, "sceRtcFormatRFC3339LocalTime", true, true);
    hle_lambda(rtc, 0x62685E98u, "sceRtcGetLastAdjustedTime", [=](Ctx &ctx) {
        if (tick_ok(ctx.gpr[4])) store64(ctx.gpr[4], kLastAdjustedTick);
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0x203CEB0Du, "sceRtcGetLastReincarnatedTime", [=](Ctx &ctx) {
        if (tick_ok(ctx.gpr[4])) store64(ctx.gpr[4], kLastReincarnatedTick);
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0xDFBC5F16u, "sceRtcParseDateTime", [=, this](Ctx &ctx) {
        if (!tick_ok(ctx.gpr[4]) || !mem->contains(ctx.gpr[5], 1u)) return finish(ctx, 0xFFFFFFFFu);
        const std::string text = read_string(ctx.gpr[5], 64);
        DateTime date;
        int offset = 0;
        if (!parse_rfc3339(text.c_str(), date, offset) && !parse_rfc2822(text.c_str(), date, offset)) return finish(ctx, 0xFFFFFFFFu);
        store64(ctx.gpr[4], to_ticks(date) - static_cast<std::uint64_t>(static_cast<std::int64_t>(offset) * 60000000));
        finish(ctx, 0u);
    });
    hle_lambda(rtc, 0x81FCDA34u, "sceRtcIsAlarmed", [](Ctx &ctx) { finish(ctx, 0u); });
    hle_lambda(rtc, 0xC2DDBEB5u, "sceRtcGetAlarmTick", [=](Ctx &ctx) {
        if (tick_ok(ctx.gpr[4])) store64(ctx.gpr[4], 0u);
        finish(ctx, 0u);
    });
}

} // namespace pspweb
