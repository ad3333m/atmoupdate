// Desktop screenshot harness: draws mock states through source/ui.cpp into PNGs.
// Build: g++ -std=c++20 -Isource tools/preview.cpp source/ui.cpp $(pkg-config --cflags --libs sdl2 SDL2_ttf SDL2_image)
#include "ui.hpp"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <cstdio>
#include <string>

namespace {

const char* kAmsNotes =
    "1.12.1 is Atmosphère's first official release with support for system version 23.0.1.\n\n"
    "## Features\n"
    "- Basic support was added for 23.0.1.\n"
    "  - The console should boot and work without issue, other than the [new features](https://example.com) not yet being implemented.\n"
    "- `fatal` was updated to show more information about **why** a crash happened.\n"
    "- Several improvements were made to `ams_mitm`'s handling of save data.\n\n"
    "## Bug fixes\n"
    "- A bug was fixed that could cause a hang when entering sleep mode on some units.\n"
    "- General system stability improvements to enhance the user's experience.\n";

const char* kHekNotes =
    "## hekate v6.5.4 & Nyx v1.9.4\n"
    "- Added support for 23.0.x HOS.\n"
    "- Improved emuMMC SD partition creation speed.\n"
    "- Fixed a rare issue where the backlight would not turn on.\n";

View base() {
    View v;
    Card ams{"Atmosphère", "Atmosphere-NX/Atmosphere", "atmosphere", "atmosphere-", ".zip", Kind::Atmosphere};
    ams.installed = "1.12.0";
    ams.cands = {{"1.12.1", "", "atmosphere-1.12.1.zip", "Oct 16, 2026", kAmsNotes, 30536712}};
    ams.phase = Phase::Idle;

    Card hek{"hekate", "CTCaer/hekate", "hekate", "hekate_ctcaer_", ".zip", Kind::Hekate};
    hek.installed = "6.5.4";
    hek.cands = {{"6.5.4", "", "hekate_ctcaer_6.5.4_Nyx_1.9.4.zip", "Sep 28, 2026", kHekNotes, 7340032}};
    hek.phase = Phase::Idle;

    Card fw{"Firmware", "THZoria/NX_Firmware", "firmware", "", ".zip", Kind::Firmware};
    fw.installed = "23.0.0";
    const char* vers[] = {"23.0.1", "23.0.0", "22.5.0", "22.1.0", "22.0.0", "21.2.0", "21.1.0", "21.0.1", "21.0.0", "20.5.0"};
    const char* dates[] = {"Oct 1, 2026", "Sep 10, 2026", "Jun 16, 2026", "May 2, 2026", "Mar 18, 2026",
                           "Feb 4, 2026", "Dec 9, 2025", "Nov 20, 2025", "Nov 4, 2025", "Sep 30, 2025"};
    for (int i = 0; i < 10; i++)
        fw.cands.push_back({vers[i], "", std::string("Firmware.") + vers[i] + ".zip", dates[i], "", 340422248LL - i * 3000000LL});
    fw.phase = Phase::Idle;

    v.cards = {ams, hek, fw};
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out = argc > 1 ? argv[1] : "preview";
    std::string fonts = argc > 2 ? argv[2] : "romfs/fonts/";
    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    IMG_Init(IMG_INIT_PNG);
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, ui::kW, ui::kH, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer* r = SDL_CreateSoftwareRenderer(surf);
    if (!r || !ui::init(r, fonts)) { std::fprintf(stderr, "init failed: %s\n", SDL_GetError()); return 1; }

    int failures = 0;
    auto shot = [&](const char* name, const View& v) {
        ui::reset_anim();
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        ui::draw(v);
        SDL_RenderPresent(r);
        std::string path = out + "/" + name + ".png";
        if (IMG_SavePNG(surf, path.c_str()) != 0) { std::fprintf(stderr, "save %s: %s\n", path.c_str(), IMG_GetError()); failures++; }
        else std::printf("wrote %s\n", path.c_str());
    };

    View v = base();
    shot("1_atmosphere_update", v);

    v = base(); v.sel = 1;
    shot("2_hekate_uptodate", v);

    v = base(); v.sel = 2; v.zone = Zone::List; v.fw_sel = 0;
    shot("3_firmware_list", v);

    v = base();
    v.cards[0].phase = Phase::Downloading; v.cards[0].progress = 0.42;
    v.cards[0].dl_now = 12830000; v.cards[0].dl_total = 30536712;
    shot("4_downloading", v);

    v = base(); v.cards[0].phase = Phase::RebootPending;
    shot("5_reboot_pending", v);

    v = base(); v.modal = Modal::Confirm;
    shot("6_confirm", v);

    v = base(); v.sel = 1; v.cards[1].phase = Phase::Error;
    v.cards[1].err = "No internet connection. Check Wi-Fi and press Y.";
    shot("7_error", v);

    v = base();
    for (auto& c : v.cards) { c.phase = Phase::Checking; c.cands.clear(); }
    v.cards[0].installed = "1.12.0"; v.cards[1].installed = ""; v.cards[2].installed = "23.0.0";
    shot("8_checking", v);

    v = base(); v.sel = 2; v.zone = Zone::List; v.fw_sel = 6; v.modal = Modal::Confirm;
    shot("9_firmware_confirm", v);

    ui::shutdown();
    SDL_DestroyRenderer(r);
    SDL_FreeSurface(surf);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
    return failures;
}
