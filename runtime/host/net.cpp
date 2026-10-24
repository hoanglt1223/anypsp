// Network modules as an always-offline PSP: the WLAN switch reads "off", libraries
// initialise and terminate normally, ad hoc control never finds or joins a group, and
// every socket or matching context fails to open. Games then take their "no
// connection" paths instead of stopping on an unimplemented import.

#include "kernel.hpp"

#include <cstdio>
#include <functional>

namespace pspweb {
namespace {

constexpr std::uint32_t kErrorAdhocInvalidSocketId = 0x80410701u;
constexpr std::uint32_t kErrorAdhocNotConnected = 0x8041070Bu;
constexpr std::uint32_t kErrorMatchingInvalidArg = 0x80410806u;
constexpr std::uint32_t kErrorMatchingInvalidId = 0x80410807u;
constexpr std::uint32_t kErrorAdhocctlWlanSwitchOff = 0x80410B03u;
constexpr std::uint32_t kErrorAdhocctlInvalidArg = 0x80410B04u;
constexpr std::uint32_t kDialogResultCancel = 1u;

} // namespace

void Kernel::install_network() {
    auto &m = rt_.memory();
    auto add = [this](const char *library, std::uint32_t nid, const char *name, std::function<std::uint32_t(Ctx &)> body) {
        rt_.register_hle(library, nid, [this, body](psprecomp::Runtime &, Ctx &ctx) { finish(ctx, body(ctx)); });
        rt_.nids().add(library, nid, name);
        char key[64];
        std::snprintf(key, sizeof key, "%s:0x%08X", library, nid);
        registered_.insert(key);
    };
    auto value = [](std::uint32_t result) { return [result](Ctx &) { return result; }; };
    // Writes an empty list: *size = 0.
    auto empty_list = [&m](Ctx &ctx) {
        if (ctx.gpr[4] == 0u) return kErrorAdhocctlInvalidArg;
        m.store32(ctx.gpr[4], 0u);
        return 0u;
    };

    add("sceNet", 0x39AF39A6u, "sceNetInit", value(0u));
    add("sceNet", 0x281928A9u, "sceNetTerm", value(0u));

    add("sceWlanDrv", 0xD7763699u, "sceWlanGetSwitchState", value(0u));
    add("sceWlanDrv", 0x0C622081u, "sceWlanGetEtherAddr", [&m](Ctx &ctx) {
        constexpr std::uint8_t kMac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01}; // locally fixed, never sent anywhere
        if (ctx.gpr[4] != 0u)
            for (std::uint32_t i = 0; i < 6u; ++i) m.store8(ctx.gpr[4] + i, kMac[i]);
        return 0u;
    });

    const char *ctl = "sceNetAdhocctl";
    add(ctl, 0xE26F226Eu, "sceNetAdhocctlInit", value(0u));
    add(ctl, 0x9D689E13u, "sceNetAdhocctlTerm", value(0u));
    add(ctl, 0x20B317A0u, "sceNetAdhocctlAddHandler", value(0u));
    add(ctl, 0x6402490Bu, "sceNetAdhocctlDelHandler", value(0u));
    add(ctl, 0x75ECD386u, "sceNetAdhocctlGetState", [&m](Ctx &ctx) {
        if (ctx.gpr[4] == 0u) return kErrorAdhocctlInvalidArg;
        m.store32(ctx.gpr[4], 0u); // disconnected
        return 0u;
    });
    add(ctl, 0x08FFF7A0u, "sceNetAdhocctlScan", value(kErrorAdhocctlWlanSwitchOff));
    add(ctl, 0x0AD043EDu, "sceNetAdhocctlConnect", value(kErrorAdhocctlWlanSwitchOff));
    add(ctl, 0x5E7F79C9u, "sceNetAdhocctlJoin", value(kErrorAdhocctlWlanSwitchOff));
    add(ctl, 0x34401D65u, "sceNetAdhocctlDisconnect", value(0u));
    add(ctl, 0x81AEE1BEu, "sceNetAdhocctlGetScanInfo", empty_list);
    add(ctl, 0xE162CB14u, "sceNetAdhocctlGetPeerList", empty_list);
    add(ctl, 0x8916C003u, "sceNetAdhocctlGetNameByAddr", value(kErrorAdhocctlInvalidArg));

    const char *adhoc = "sceNetAdhoc";
    add(adhoc, 0xE1D621D7u, "sceNetAdhocInit", value(0u));
    add(adhoc, 0xA62C6F57u, "sceNetAdhocTerm", value(0u));
    add(adhoc, 0x6F92741Bu, "sceNetAdhocPdpCreate", value(kErrorAdhocNotConnected));
    add(adhoc, 0x877F6D66u, "sceNetAdhocPtpOpen", value(kErrorAdhocNotConnected));
    add(adhoc, 0xE08BDAC1u, "sceNetAdhocPtpListen", value(kErrorAdhocNotConnected));
    add(adhoc, 0xFC6FC07Bu, "sceNetAdhocPtpConnect", value(kErrorAdhocInvalidSocketId));
    add(adhoc, 0x9DF81198u, "sceNetAdhocPtpAccept", value(kErrorAdhocInvalidSocketId));
    add(adhoc, 0x4DA4C788u, "sceNetAdhocPtpSend", value(kErrorAdhocInvalidSocketId));
    add(adhoc, 0x8BEA2B3Eu, "sceNetAdhocPtpRecv", value(kErrorAdhocInvalidSocketId));
    add(adhoc, 0x157E6225u, "sceNetAdhocPtpClose", value(kErrorAdhocInvalidSocketId));
    add(adhoc, 0xABED3790u, "sceNetAdhocPdpSend", value(kErrorAdhocInvalidSocketId));
    add(adhoc, 0xDFE53E03u, "sceNetAdhocPdpRecv", value(kErrorAdhocInvalidSocketId));
    add(adhoc, 0x7F27BB5Eu, "sceNetAdhocPdpDelete", value(kErrorAdhocInvalidSocketId));

    const char *matching = "sceNetAdhocMatching";
    add(matching, 0x2A2A1E07u, "sceNetAdhocMatchingInit", value(0u));
    add(matching, 0x7945ECDAu, "sceNetAdhocMatchingTerm", value(0u));
    add(matching, 0xCA5EDA6Fu, "sceNetAdhocMatchingCreate", value(kErrorMatchingInvalidArg));
    add(matching, 0x93EF3843u, "sceNetAdhocMatchingStart", value(kErrorMatchingInvalidId));
    add(matching, 0x32B156B3u, "sceNetAdhocMatchingStop", value(kErrorMatchingInvalidId));
    add(matching, 0xF16EAF4Fu, "sceNetAdhocMatchingDelete", value(kErrorMatchingInvalidId));
    add(matching, 0x5E3D4B79u, "sceNetAdhocMatchingSelectTarget", value(kErrorMatchingInvalidId));
    add(matching, 0xEA3C6108u, "sceNetAdhocMatchingCancelTarget", value(kErrorMatchingInvalidId));
    add(matching, 0x8F58BEDFu, "sceNetAdhocMatchingCancelTargetWithOpt", value(kErrorMatchingInvalidId));

    // Network configuration and game sharing dialogs run their status sequence without
    // UI and end as if the user cancelled.
    auto dialog = [this, add, &m](const char *init, std::uint32_t init_nid, const char *status, std::uint32_t status_nid,
                                  const char *update, std::uint32_t update_nid, const char *shutdown,
                                  std::uint32_t shutdown_nid, UtilityDialog &state) {
        add("sceUtility", init_nid, init, [this, &m, &state](Ctx &ctx) {
            if (ctx.gpr[4] != 0u) m.store32(ctx.gpr[4] + 0x1Cu, kDialogResultCancel);
            state = UtilityDialog{1u, ctx.gpr[4]};
            return 0u;
        });
        rt_.register_hle("sceUtility", status_nid, [this, &state](psprecomp::Runtime &, Ctx &ctx) { dialog_get_status(ctx, state); });
        rt_.nids().add("sceUtility", status_nid, status);
        char key[64];
        std::snprintf(key, sizeof key, "sceUtility:0x%08X", status_nid);
        registered_.insert(key);
        add("sceUtility", update_nid, update, [](Ctx &) { return 0u; });
        add("sceUtility", shutdown_nid, shutdown, [&state](Ctx &) {
            state.status = 4u;
            return 0u;
        });
    };
    dialog("sceUtilityNetconfInitStart", 0x4DB1E739u, "sceUtilityNetconfGetStatus", 0x6332AA39u,
           "sceUtilityNetconfUpdate", 0x91E70E35u, "sceUtilityNetconfShutdownStart", 0xF88155F6u, netconf_dialog_);
    dialog("sceUtilityGameSharingInitStart", 0xC492F751u, "sceUtilityGameSharingGetStatus", 0x946963F3u,
           "sceUtilityGameSharingUpdate", 0x7853182Du, "sceUtilityGameSharingShutdownStart", 0xEFC6F80Fu,
           gamesharing_dialog_);
}

} // namespace pspweb
