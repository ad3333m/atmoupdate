// AtmoUpdate - update Atmosphère, hekate and firmware on the console itself.
#include "gh.hpp"
#include "http.hpp"
#include "install.hpp"
#include "model.hpp"
#include "reboot.hpp"
#include "ui.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<Card> g_cards = {
    {"Atmosphère", "Atmosphere-NX/Atmosphere", "atmosphere", "atmosphere-",    ".zip", Kind::Atmosphere},
    {"hekate",     "CTCaer/hekate",            "hekate",     "hekate_ctcaer_", ".zip", Kind::Hekate},
    {"Firmware",   "THZoria/NX_Firmware",      "firmware",   "",               ".zip", Kind::Firmware},
};

std::mutex g_mu;
std::condition_variable g_cv;
std::atomic<bool> g_quit{false};
std::atomic<bool> g_online{true};

std::string norm_tag(const std::string& t) {
    return (!t.empty() && (t[0] == 'v' || t[0] == 'V')) ? t.substr(1) : t;
}

std::string friendly(const std::string& err) {
    if (err.find("resolve") != std::string::npos || err.find("connect") != std::string::npos)
        return "No internet connection. Check Wi-Fi and press Y.";
    if (err.find("HTTP 403") != std::string::npos || err.find("HTTP 429") != std::string::npos)
        return "GitHub's rate limit was hit. Try again in a few minutes.";
    if (err.find("imeout") != std::string::npos) return "GitHub took too long to answer. Press Y to retry.";
    return err;
}

const gh::Asset* pick_asset(const Card& c, const gh::Release& rel) {
    for (auto& a : rel.assets) {
        if (c.asset_prefix[0] && a.name.rfind(c.asset_prefix, 0) != 0) continue;
        if (a.name.find(c.asset_contains) == std::string::npos) continue;
        return &a;
    }
    return nullptr;
}

bool is_busy(const Card& c) {
    return c.phase == Phase::Checking || c.phase == Phase::Downloading || c.phase == Phase::Installing;
}

void refresh(Card& c) {
    { std::lock_guard<std::mutex> lk(g_mu); c.phase = Phase::Checking; c.err.clear(); }

    NifmInternetConnectionType net_type{};
    NifmInternetConnectionStatus net{};
    u32 strength = 0;
    g_online = R_SUCCEEDED(nifmGetInternetConnectionStatus(&net_type, &strength, &net)) &&
               net == NifmInternetConnectionStatus_Connected;

    std::vector<gh::Release> rels;
    std::string err;
    bool ok;
    if (c.kind == Kind::Firmware) ok = gh::releases(c.repo, rels, err);
    else {
        gh::Release one;
        ok = gh::latest(c.repo, one, err);
        if (ok) rels.push_back(std::move(one));
    }

    std::string installed;
    switch (c.kind) {
        case Kind::Atmosphere: installed = install::detect_atmosphere_version(); break;
        case Kind::Hekate:     installed = install::detect_hekate_version();     break;
        case Kind::Firmware:   installed = install::detect_firmware_version();   break;
    }
    if (installed.empty() && c.kind != Kind::Firmware) installed = install::read_saved(c.saved_key);

    std::vector<Candidate> cands;
    for (auto& r : rels)
        if (const gh::Asset* a = pick_asset(c, r))
            cands.push_back({norm_tag(r.tag), a->url, a->name, gh::pretty_date(r.published), r.body, a->size});
    bool pending = c.kind == Kind::Atmosphere && install::ams_update_pending();

    std::lock_guard<std::mutex> lk(g_mu);
    c.installed = installed;
    if (ok) c.cands = std::move(cands);
    if (!ok) { c.phase = Phase::Error; c.err = friendly(err); }
    else c.phase = pending ? Phase::RebootPending : Phase::Idle;
}

void fail(Card& c, const std::string& err) {
    std::lock_guard<std::mutex> lk(g_mu);
    c.phase = Phase::Error;
    c.err = friendly(err);
}

void install_card(Card& c) {
    Candidate pick;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (c.target < 0 || c.target >= int(c.cands.size())) { c.phase = Phase::Error; c.err = "Nothing to install"; return; }
        pick = c.cands[c.target];
        c.phase = Phase::Downloading;
        c.progress = 0;
        c.dl_now = c.dl_total = 0;
        c.err.clear();
    }
    std::string tmp = "sdmc:/switch/AtmoUpdate/" + pick.asset_name, err;
    install::mkdirs_for(tmp);
    http::ProgressFn cb = [&c](double now, double total) {
        std::lock_guard<std::mutex> lk(g_mu);
        c.dl_now = (long long)now;
        c.dl_total = (long long)total;
        c.progress = total > 0 ? now / total : 0;
        return !g_quit.load();
    };
    if (!http::get_file(pick.asset_url, tmp, cb, err)) { std::remove(tmp.c_str()); fail(c, err); return; }
    if (!install::is_zip(tmp)) { std::remove(tmp.c_str()); fail(c, "The download wasn't a zip file."); return; }

    { std::lock_guard<std::mutex> lk(g_mu); c.phase = Phase::Installing; c.progress = 1; }
    std::string root = c.kind == Kind::Firmware ? "sdmc:/firmware/" + pick.version + "/" : "sdmc:/";
    int staged = 0;
    bool ok = install::extract_zip_to(tmp, root, staged, err);
    std::remove(tmp.c_str());
    if (!ok) { fail(c, err); return; }

    if (c.kind == Kind::Hekate) {
        std::string bin = "sdmc:/hekate_ctcaer_" + pick.version + ".bin", e;
        if (install::exists(bin)) {
            install::copy_file(bin, "sdmc:/bootloader/update.bin", e);
            install::copy_file(bin, "sdmc:/atmosphere/reboot_payload.bin", e);
        }
    }
    install::write_saved(c.saved_key, pick.version);

    std::lock_guard<std::mutex> lk(g_mu);
    if (c.kind == Kind::Hekate) c.installed = pick.version;
    c.phase = staged > 0 ? Phase::RebootPending : Phase::Done;
}

void worker() {
    while (true) {
        int idx = -1;
        bool update = false;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] {
                if (g_quit) return true;
                for (auto& c : g_cards) if (c.want_update || c.need_refresh) return true;
                return false;
            });
            if (g_quit) return;
            for (int i = 0; i < int(g_cards.size()) && idx < 0; i++)
                if (g_cards[i].want_update) { idx = i; update = true; }
            for (int i = 0; i < int(g_cards.size()) && idx < 0; i++)
                if (g_cards[i].need_refresh) idx = i;
            g_cards[idx].want_update = false;
            g_cards[idx].need_refresh = false;
        }
        if (update) install_card(g_cards[idx]);
        else refresh(g_cards[idx]);
    }
}

}  // namespace

int main(int, char**) {
    socketInitializeDefault();
    nifmInitialize(NifmServiceType_User);
    romfsInit();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    SDL_Window* win = SDL_CreateWindow("AtmoUpdate", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ui::kW, ui::kH, 0);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, ui::kW, ui::kH);
    ui::init(ren, "romfs:/fonts/");

    std::thread worker_thread(worker);

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    int sel = 0, fw_sel = 0, repeat = 0;
    Zone zone = Zone::Sidebar;
    Modal modal = Modal::None;
    bool home_blocked = false, running = true;
    constexpr u64 kUp = HidNpadButton_Up | HidNpadButton_StickLUp;
    constexpr u64 kDown = HidNpadButton_Down | HidNpadButton_StickLDown;

    while (running && appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad), held = padGetButtons(&pad);
        if (held & (kUp | kDown)) { if (++repeat > 22 && repeat % 4 == 0) down |= held & (kUp | kDown); }
        else repeat = 0;

        View v;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            Card& cur = g_cards[sel];
            int nfw = int(g_cards[2].cands.size());
            bool installing = false;
            for (auto& c : g_cards) installing |= c.phase == Phase::Installing;

            if (modal != Modal::None) {
                if (down & HidNpadButton_A) {
                    if (modal == Modal::Reboot) {
                        std::string err;
                        reboot::finish_ams_update(err);   // only returns on failure
                        cur.err = err;
                    } else if (!is_busy(cur)) {
                        cur.target = cur.kind == Kind::Firmware ? fw_sel : 0;
                        cur.want_update = true;
                        g_cv.notify_one();
                    }
                    modal = Modal::None;
                } else if (down & HidNpadButton_B) modal = Modal::None;
            } else if (zone == Zone::List) {
                if (down & kUp) fw_sel = std::max(0, fw_sel - 1);
                if (down & kDown) fw_sel = std::min(std::max(0, nfw - 1), fw_sel + 1);
                if ((down & HidNpadButton_A) && nfw > 0 && !is_busy(cur)) modal = Modal::Confirm;
                if (down & (HidNpadButton_B | HidNpadButton_Left | HidNpadButton_StickLLeft)) zone = Zone::Sidebar;
            } else {
                if (down & kUp) sel = std::max(0, sel - 1);
                if (down & kDown) sel = std::min(int(g_cards.size()) - 1, sel + 1);
                Card& c = g_cards[sel];
                bool enter_list = c.kind == Kind::Firmware && !c.cands.empty() &&
                                  (down & (HidNpadButton_A | HidNpadButton_Right | HidNpadButton_StickLRight));
                if (enter_list) zone = Zone::List;
                else if ((down & HidNpadButton_A) && c.kind != Kind::Firmware && !is_busy(c)) {
                    if (c.phase == Phase::RebootPending) modal = Modal::Reboot;
                    else if (!c.cands.empty()) modal = Modal::Confirm;
                }
            }
            if (down & HidNpadButton_Y) {
                for (auto& c : g_cards) if (!is_busy(c)) c.need_refresh = true;
                g_cv.notify_one();
            }
            if ((down & HidNpadButton_Plus) && !installing) running = false;

            if (installing != home_blocked) {
                if (installing) appletBeginBlockingHomeButton(0);
                else appletEndBlockingHomeButton();
                home_blocked = installing;
            }

            v.cards = g_cards;
            v.sel = sel;
            v.zone = zone;
            v.fw_sel = fw_sel;
            v.modal = modal;
            v.online = g_online;
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) running = false;

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        ui::draw(v);
        SDL_RenderPresent(ren);
    }

    g_quit = true;
    g_cv.notify_all();
    worker_thread.join();
    if (home_blocked) appletEndBlockingHomeButton();

    ui::shutdown();
    TTF_Quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    curl_global_cleanup();
    romfsExit();
    nifmExit();
    socketExit();
    return 0;
}
