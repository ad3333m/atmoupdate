#include "reboot.hpp"
#include "install.hpp"
#include <switch.h>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kPayload = "romfs:/swap_payload.bin";
constexpr size_t kIramPayloadSize = 0x24000;  // what Atmosphère's bpc:ams copies to IRAM
alignas(0x1000) u8 g_payload[kIramPayloadSize];

std::string hex(Result rc) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%X", rc);
    return b;
}

// Icosa (retail) and Copper (dev) are Erista; everything newer is Mariko.
bool is_erista() {
    u64 hw = 0;
    if (R_FAILED(splInitialize())) return true;
    Result rc = splGetConfig(SplConfigItem_HardwareType, &hw);
    splExit();
    return R_FAILED(rc) || hw <= 1;
}

Result ams_set_reboot_payload(const void* buf, size_t size) {
    Handle h;
    Result rc = svcConnectToNamedPort(&h, "bpc:ams");
    if (R_FAILED(rc)) return rc;
    Service srv;
    serviceCreate(&srv, h);
    rc = serviceDispatch(&srv, 65001,
                         .buffer_attrs = {SfBufferAttr_In | SfBufferAttr_HipcMapAlias},
                         .buffers = {{buf, size}});
    serviceClose(&srv);
    return rc;
}

}

namespace reboot {

bool finish_ams_update(std::string& err) {
    if (is_erista()) {
        FILE* f = std::fopen(kPayload, "rb");
        if (!f) { err = "Swap payload missing from app"; return false; }
        std::memset(g_payload, 0, sizeof(g_payload));
        std::fread(g_payload, 1, sizeof(g_payload), f);
        std::fclose(f);

        Result rc = spsmInitialize();
        if (R_FAILED(rc)) { err = "spsm " + hex(rc); return false; }
        smExit();
        rc = ams_set_reboot_payload(g_payload, sizeof(g_payload));
        if (R_FAILED(rc)) {
            smInitialize();
            spsmExit();
            err = "Reboot-to-payload unavailable (" + hex(rc) + ")";
            return false;
        }
        spsmShutdown(true);
        err = "Reboot failed";
        return false;
    }

    // Mariko: the modchip boots /payload.bin. Park the swap payload there; it puts
    // payload.bin.aio back (hekate if present) and reboots once the rename is done.
    std::string restore = install::exists("sdmc:/bootloader/update.bin") ? "sdmc:/bootloader/update.bin"
                                                                         : "sdmc:/payload.bin";
    if (!install::exists(restore)) { err = "No /payload.bin to return to"; return false; }
    if (!install::copy_file(restore, "sdmc:/payload.bin.aio", err)) return false;
    if (!install::copy_file(kPayload, "sdmc:/payload.bin", err)) return false;

    Result rc = spsmInitialize();
    if (R_FAILED(rc)) { err = "spsm " + hex(rc); return false; }
    spsmShutdown(true);
    err = "Reboot failed";
    return false;
}

}
