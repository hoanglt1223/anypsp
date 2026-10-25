// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceUtilityOsk: the on-screen keyboard. Without interactive dialogs (headless runs) it is
// answered at once with the next scripted text (Kernel::set_osk_texts), or the game's initial
// text when no script is left. With them, the host shows a SystemDialog::Kind::Keyboard and the
// player types through dialog_move/focus/confirm (or dialog_type/dialog_erase from a real
// keyboard). The request layout and the result rules follow PPSSPP (Core/Dialog/PSPOskDialog.cpp,
// GPL-2.0-or-later).

#include "kernel.hpp"

#include <algorithm>
#include <iostream>

namespace pspweb {
namespace {

// SceUtilityOskParams: the common dialog header, then fieldCount, fields, state.
constexpr std::uint32_t kParamsFieldCount = 0x30u, kParamsFields = 0x34u, kParamsState = 0x38u;
// SceUtilityOskData.
constexpr std::uint32_t kFieldDesc = 0x1Cu, kFieldInText = 0x20u, kFieldOutLength = 0x24u, kFieldOutText = 0x28u,
                        kFieldResult = 0x2Cu, kFieldOutLimit = 0x30u;
constexpr std::uint32_t kResultCancelled = 1u, kResultChanged = 2u;
constexpr std::uint32_t kStateVisible = 3u, kStateFinished = 5u;
constexpr std::uint32_t kErrorInvalidStatus = 0x80110001u;

void append_utf8(std::string &out, char32_t c) {
    if (c < 0x80u) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (c >> 6u)));
        out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
    } else if (c < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (c >> 12u)));
        out.push_back(static_cast<char>(0x80u | ((c >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (c >> 18u)));
        out.push_back(static_cast<char>(0x80u | ((c >> 12u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((c >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
    }
}

std::u32string decode_utf8(std::string_view text) {
    std::u32string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        const int extra = b < 0x80u ? 0 : b < 0xE0u ? 1 : b < 0xF0u ? 2 : 3;
        char32_t c = extra == 0 ? b : extra == 1 ? (b & 0x1Fu) : extra == 2 ? (b & 0x0Fu) : (b & 0x07u);
        ++i;
        for (int k = 0; k < extra && i < text.size(); ++k, ++i) c = (c << 6u) | (static_cast<unsigned char>(text[i]) & 0x3Fu);
        out.push_back(c);
    }
    return out;
}

} // namespace

std::string Kernel::read_utf16(std::uint32_t address, std::size_t limit) const {
    std::string out;
    if (address == 0u) return out;
    auto &m = rt_.memory();
    for (std::size_t i = 0; i < limit && m.contains(address + static_cast<std::uint32_t>(i * 2u), 2u); ++i) {
        char32_t c = m.load16(address + static_cast<std::uint32_t>(i * 2u));
        if (c == 0u) break;
        if (c >= 0xD800u && c < 0xDC00u && i + 1u < limit) {
            const char32_t low = m.load16(address + static_cast<std::uint32_t>((i + 1u) * 2u));
            if (low >= 0xDC00u && low < 0xE000u) {
                c = 0x10000u + ((c - 0xD800u) << 10u) + (low - 0xDC00u);
                ++i;
            }
        }
        append_utf8(out, c);
    }
    return out;
}

void Kernel::install_osk() {
    const char *util = "sceUtility";
    hle(util, 0xF6269B82u, "sceUtilityOskInitStart", &Kernel::sceUtilityOskInitStart);
    hle(util, 0x3DFAEBA9u, "sceUtilityOskShutdownStart", &Kernel::sceUtilityOskShutdownStart);
    hle(util, 0x4B85C861u, "sceUtilityOskUpdate", &Kernel::return_zero);
    hle(util, 0xF3F76017u, "sceUtilityOskGetStatus", &Kernel::sceUtilityOskGetStatus);
}

void Kernel::sceUtilityOskInitStart(Ctx &ctx) {
    auto &m = rt_.memory();
    const std::uint32_t params = ctx.gpr[4];
    if (osk_dialog_.status != 0u) return finish(ctx, kErrorInvalidStatus);
    if (!m.contains(params, 0x3Cu)) return finish(ctx, 0xFFFFFFFFu);
    const std::uint32_t field = m.load32(params + kParamsFields);
    if (!m.contains(field, 0x34u)) return finish(ctx, 0xFFFFFFFFu);
    if (m.load32(params + kParamsFieldCount) != 1u)
        std::cerr << "[utility] keyboard with " << m.load32(params + kParamsFieldCount) << " fields: only the first is used\n";
    const std::string description = read_utf16(m.load32(field + kFieldDesc), 512);
    const std::string initial = read_utf16(m.load32(field + kFieldInText), 512);
    std::cerr << "[utility] keyboard: \"" << description << "\"\n";
    osk_dialog_ = UtilityDialog{1u, params};
    m.store32(params + kParamsState, kStateVisible);
    if (!interactive_dialogs_) {
        std::string text = initial;
        if (!osk_texts_.empty()) {
            text = osk_texts_.front();
            osk_texts_.erase(osk_texts_.begin());
        }
        osk_write(text, true);
        return finish(ctx, 0u);
    }
    SystemDialog d;
    d.kind = SystemDialog::Kind::Keyboard;
    d.text = description;
    d.input = initial;
    d.max_length = static_cast<int>(osk_max_length());
    d.cross_confirms = m.load32(params + 8u) == 1u;
    osk_dialog_.waiting = true;
    open_dialog(std::move(d), DialogOwner::Keyboard);
    finish(ctx, 0u);
}

// The longest text the field takes: outtextlimit when it is below the buffer, else the buffer.
std::uint32_t Kernel::osk_max_length() const {
    const auto &m = rt_.memory();
    const std::uint32_t field = m.load32(osk_dialog_.params + kParamsFields);
    const std::uint32_t length = m.load32(field + kFieldOutLength), limit = m.load32(field + kFieldOutLimit);
    if (length == 0u) return 0u;
    return limit == 0u || limit > length - 1u ? length - 1u : limit;
}

// Writes the answer: the text (UTF-16, cut to the field) and its terminator, and the results.
void Kernel::osk_write(const std::string &text, bool confirmed) {
    auto &m = rt_.memory();
    const std::uint32_t params = osk_dialog_.params;
    const std::uint32_t field = m.load32(params + kParamsFields);
    std::u16string units;
    for (const char32_t c : decode_utf8(text)) {
        if (c >= 0x10000u) {
            units.push_back(static_cast<char16_t>(0xD800u + ((c - 0x10000u) >> 10u)));
            units.push_back(static_cast<char16_t>(0xDC00u + ((c - 0x10000u) & 0x3FFu)));
        } else {
            units.push_back(static_cast<char16_t>(c));
        }
    }
    const std::uint32_t max = osk_max_length();
    const std::uint32_t out = m.load32(field + kFieldOutText);
    std::size_t end = m.load32(field + kFieldOutLength);
    if (end > units.size()) end = units.size() + 1u;
    if (end != 0u && m.contains(out, static_cast<std::uint32_t>(end * 2u)))
        for (std::size_t i = 0; i < end; ++i)
            m.store16(out + static_cast<std::uint32_t>(i * 2u), i < max && i < units.size() ? units[i] : 0u);
    m.store32(params + 0x1Cu, 0u);
    m.store32(field + kFieldResult, confirmed ? kResultChanged : kResultCancelled);
    m.store32(params + kParamsState, kStateFinished);
    osk_dialog_.waiting = false;
    if (osk_dialog_.status == 2u) osk_dialog_.status = 3u;
}

// Confirm on the focused key: type it, delete, or finish; going back cancels.
void Kernel::osk_answer(bool confirmed) {
    if (!confirmed) {
        std::cerr << "[utility] keyboard cancelled\n";
        osk_write(read_utf16(rt_.memory().load32(rt_.memory().load32(osk_dialog_.params + kParamsFields) + kFieldInText), 512), false);
        close_dialog();
        return;
    }
    const int key = dialog_.focus;
    if (key == kOskDone) {
        std::cerr << "[utility] keyboard answered: \"" << dialog_.input << "\"\n";
        osk_write(dialog_.input, true);
        close_dialog();
        return;
    }
    if (key == kOskDelete) dialog_erase();
    else if (key == kOskSpace) dialog_type(" ");
    else if (key >= 0 && key < kOskKeyCount) dialog_type(kOskKeys[key]);
}

void Kernel::dialog_type(std::string_view utf8) {
    if (dialog_owner_ != DialogOwner::Keyboard) return;
    std::u32string text = decode_utf8(dialog_.input);
    for (const char32_t c : decode_utf8(utf8)) {
        if (c < 0x20u || static_cast<int>(text.size()) >= dialog_.max_length) continue;
        text.push_back(c);
    }
    dialog_.input.clear();
    for (const char32_t c : text) append_utf8(dialog_.input, c);
    dialog_.serial = ++dialog_serial_;
}

void Kernel::dialog_erase() {
    if (dialog_owner_ != DialogOwner::Keyboard) return;
    std::u32string text = decode_utf8(dialog_.input);
    if (!text.empty()) text.pop_back();
    dialog_.input.clear();
    for (const char32_t c : text) append_utf8(dialog_.input, c);
    dialog_.serial = ++dialog_serial_;
}

void Kernel::sceUtilityOskGetStatus(Ctx &ctx) { dialog_get_status(ctx, osk_dialog_); }

void Kernel::sceUtilityOskShutdownStart(Ctx &ctx) {
    if (osk_dialog_.status != 3u && osk_dialog_.status != 2u) return finish(ctx, kErrorInvalidStatus);
    osk_dialog_.status = 4u;
    osk_dialog_.waiting = false;
    if (dialog_owner_ == DialogOwner::Keyboard) close_dialog();
    finish(ctx, 0u);
}

} // namespace pspweb
