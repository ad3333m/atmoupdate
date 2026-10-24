// AtmoUpdate - fetch and install Atmosphere + Hekate releases on-device.
#include "http.hpp"
#include "gh.hpp"
#include "install.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <switch.h>
#include <curl/curl.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kW = 1280;
constexpr int kH = 720;

struct Card {
    const char* title;
    const char* repo;              // owner/repo
    const char* saved_key;         // key in versions.ini
    const char* asset_prefix;      // name prefix for the zip we want
    const char* asset_contains;    // additional substring filter (optional, nullable)

    // Live state (worker + main share, guarded by lock).
    std::string installed;
    std::string latest;
    std::string status;            // "Up to date" / "Update available" / "Downloading..." etc.
    std::string err;
    std::string zip_url;
    std::string zip_name;
    double progress = 0.0;         // 0..1 for downloads
    bool busy = false;
    bool need_refresh = true;
    bool want_update = false;
    bool done_install = false;
};

Card g_cards[] = {
    {"Atmosphère", "Atmosphere-NX/Atmosphere", "atmosphere",
     "atmosphere-", ".zip"},
    {"Hekate",     "CTCaer/hekate",            "hekate",
     "hekate_ctcaer_", ".zip"},
};
constexpr int kN = sizeof(g_cards) / sizeof(g_cards[0]);
int g_focus = 0;

std::mutex g_mu;
std::condition_variable g_cv;
std::atomic<bool> g_quit{false};

TTF_Font* g_font_big   = nullptr;
TTF_Font* g_font_med   = nullptr;
TTF_Font* g_font_small = nullptr;
PlFontData g_font_data;

SDL_Texture* text_tex(SDL_Renderer* r, TTF_Font* f, const std::string& s, SDL_Color c, int* w, int* h) {
    if (s.empty()) { if (w) *w = 0; if (h) *h = 0; return nullptr; }
    SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), c);
    if (!surf) { if (w) *w = 0; if (h) *h = 0; return nullptr; }
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, surf);
    if (w) *w = surf->w;
    if (h) *h = surf->h;
    SDL_FreeSurface(surf);
    return t;
}

void draw_text(SDL_Renderer* r, TTF_Font* f, const std::string& s, int x, int y, SDL_Color c) {
    int w = 0, h = 0;
    SDL_Texture* t = text_tex(r, f, s, c, &w, &h);
    if (!t) return;
    SDL_Rect dst{x, y, w, h};
    SDL_RenderCopy(r, t, nullptr, &dst);
    SDL_DestroyTexture(t);
}

void fill_rect(SDL_Renderer* r, SDL_Rect rect, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_RenderFillRect(r, &rect);
}

void stroke_rect(SDL_Renderer* r, SDL_Rect rect, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_RenderDrawRect(r, &rect);
}

void draw_background(SDL_Renderer* r) {
    for (int y = 0; y < kH; y++) {
        int t = (y * 255) / kH;
        SDL_SetRenderDrawColor(r, 10 + t / 10, 14 + t / 8, 28 + t / 5, 255);
        SDL_RenderDrawLine(r, 0, y, kW, y);
    }
    // Subtle accent glow
    for (int i = 0; i < 180; i++) {
        SDL_Color c{80, 130, 255, (Uint8)(40 - i / 5)};
        if (c.a == 0) break;
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_Rect gr{kW / 2 - 400 - i, 60 - i, 800 + 2*i, 10 + 2*i};
        SDL_RenderDrawRect(r, &gr);
    }
}

void draw_glass(SDL_Renderer* r, SDL_Rect rect, bool focused) {
    // outer soft shadow
    for (int i = 10; i > 0; i--) {
        SDL_Color sh{0, 0, 0, (Uint8)(16 - i)};
        SDL_Rect gr{rect.x - i, rect.y - i, rect.w + 2*i, rect.h + 2*i};
        stroke_rect(r, gr, sh);
    }
    // base translucent fill
    fill_rect(r, rect, SDL_Color{255, 255, 255, 18});
    // top highlight band
    SDL_Rect top{rect.x, rect.y, rect.w, 36};
    fill_rect(r, top, SDL_Color{255, 255, 255, 14});
    // border
    stroke_rect(r, rect, focused ? SDL_Color{140, 190, 255, 230} : SDL_Color{255, 255, 255, 70});
    if (focused) {
        SDL_Rect inner{rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2};
        stroke_rect(r, inner, SDL_Color{140, 190, 255, 120});
    }
}

void draw_progress(SDL_Renderer* r, SDL_Rect rect, double p) {
    fill_rect(r, rect, SDL_Color{255, 255, 255, 24});
    SDL_Rect fill{rect.x, rect.y, (int)(rect.w * p), rect.h};
    fill_rect(r, fill, SDL_Color{90, 180, 255, 220});
    stroke_rect(r, rect, SDL_Color{255, 255, 255, 100});
}

// Decide which asset to pick given the card's filters.
const gh::Asset* pick_asset(const Card& c, const gh::Release& rel) {
    for (auto& a : rel.assets) {
        if (a.name.rfind(c.asset_prefix, 0) != 0) continue;
        if (c.asset_contains && c.asset_contains[0]) {
            if (a.name.find(c.asset_contains) == std::string::npos) continue;
        }
        return &a;
    }
    return nullptr;
}

// Worker: handles refresh + update per-card sequentially.
void worker() {
    while (!g_quit.load()) {
        int work_i = -1;
        bool do_refresh = false;
        bool do_update = false;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [&] {
                if (g_quit.load()) return true;
                for (int i = 0; i < kN; i++) {
                    if (g_cards[i].need_refresh || g_cards[i].want_update) return true;
                }
                return false;
            });
            if (g_quit.load()) return;
            for (int i = 0; i < kN; i++) {
                if (g_cards[i].want_update)  { work_i = i; do_update = true;  break; }
                if (g_cards[i].need_refresh) { work_i = i; do_refresh = true; break; }
            }
            if (work_i < 0) continue;
            g_cards[work_i].busy = true;
            if (do_refresh) g_cards[work_i].need_refresh = false;
            if (do_update)  g_cards[work_i].want_update  = false;
        }

        Card& c = g_cards[work_i];
        if (do_refresh) {
            {
                std::lock_guard<std::mutex> lk(g_mu);
                c.status = "Checking…";
                c.err.clear();
            }
            gh::Release rel; std::string err;
            if (!gh::latest(c.repo, rel, err)) {
                std::lock_guard<std::mutex> lk(g_mu);
                c.err = err; c.status = "Error"; c.busy = false; continue;
            }
            const gh::Asset* a = pick_asset(c, rel);
            std::string installed;
            if (std::string(c.saved_key) == "hekate") installed = install::detect_hekate_version();
            if (installed.empty()) installed = install::read_saved(c.saved_key);
            std::string tag_norm = rel.tag;
            if (!tag_norm.empty() && (tag_norm[0] == 'v' || tag_norm[0] == 'V')) tag_norm.erase(0, 1);
            std::lock_guard<std::mutex> lk(g_mu);
            c.latest = tag_norm;
            c.installed = installed;
            if (a) { c.zip_url = a->url; c.zip_name = a->name; }
            if (installed.empty())             c.status = "Not installed";
            else if (installed == tag_norm)    c.status = "Up to date";
            else                               c.status = "Update available";
            c.busy = false;
        } else if (do_update) {
            std::string url, name;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                url = c.zip_url; name = c.zip_name;
                c.status = "Downloading…"; c.progress = 0.0; c.err.clear();
            }
            if (url.empty()) {
                std::lock_guard<std::mutex> lk(g_mu);
                c.err = "no asset"; c.status = "Error"; c.busy = false; continue;
            }
            std::string tmp = "sdmc:/switch/AtmoUpdate/" + name;
            install::mkdirs_for(tmp);
            std::string err;
            http::ProgressFn cb = [&c](double dl, double tot) {
                std::lock_guard<std::mutex> lk(g_mu);
                c.progress = (tot > 0) ? (dl / tot) : 0.0;
            };
            if (!http::get_file(url, tmp, cb, err)) {
                std::lock_guard<std::mutex> lk(g_mu);
                c.err = err; c.status = "Download failed"; c.busy = false; continue;
            }
            if (!install::is_zip(tmp)) {
                std::lock_guard<std::mutex> lk(g_mu);
                c.err = "not a zip"; c.status = "Error"; c.busy = false; continue;
            }
            {
                std::lock_guard<std::mutex> lk(g_mu);
                c.status = "Installing…"; c.progress = 1.0;
            }
            install::LogFn lg = [](const std::string&) {};
            if (!install::extract_zip_to_root(tmp, lg, err)) {
                std::lock_guard<std::mutex> lk(g_mu);
                c.err = err; c.status = "Install failed"; c.busy = false; continue;
            }
            // Side-effect: for hekate, mirror payload to /atmosphere/reboot_payload.bin.
            if (std::string(c.saved_key) == "hekate") {
                std::string pay = "sdmc:/hekate_ctcaer_" + c.latest + ".bin";
                std::string reboot = "sdmc:/atmosphere/reboot_payload.bin";
                std::string e2;
                install::copy_file(pay, reboot, e2);  // best-effort
            }
            install::write_saved(c.saved_key, c.latest);
            std::remove(tmp.c_str());
            std::lock_guard<std::mutex> lk(g_mu);
            c.installed = c.latest;
            c.status = "Installed";
            c.done_install = true;
            c.busy = false;
        }
    }
}

void init_fonts() {
    plGetSharedFontByType(&g_font_data, PlSharedFontType_Standard);
    g_font_big   = TTF_OpenFontRW(SDL_RWFromMem(g_font_data.address, g_font_data.size), 1, 38);
    g_font_med   = TTF_OpenFontRW(SDL_RWFromMem(g_font_data.address, g_font_data.size), 1, 26);
    g_font_small = TTF_OpenFontRW(SDL_RWFromMem(g_font_data.address, g_font_data.size), 1, 20);
}

void draw_card(SDL_Renderer* r, int idx, SDL_Rect rect) {
    Card snap;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        snap = g_cards[idx];
    }
    bool focus = (idx == g_focus);
    draw_glass(r, rect, focus);

    SDL_Color white{235, 240, 255, 255};
    SDL_Color dim{180, 190, 215, 255};
    SDL_Color accent{140, 200, 255, 255};
    SDL_Color err{255, 140, 140, 255};
    SDL_Color ok{150, 230, 180, 255};

    draw_text(r, g_font_big, snap.title, rect.x + 24, rect.y + 18, white);

    int row_y = rect.y + 90;
    draw_text(r, g_font_small, "Installed", rect.x + 24, row_y, dim);
    draw_text(r, g_font_med, snap.installed.empty() ? "—" : snap.installed,
              rect.x + 160, row_y - 4, white);
    row_y += 44;
    draw_text(r, g_font_small, "Latest", rect.x + 24, row_y, dim);
    draw_text(r, g_font_med, snap.latest.empty() ? "—" : snap.latest,
              rect.x + 160, row_y - 4, accent);

    // Status
    row_y += 60;
    SDL_Color sc = white;
    if (!snap.err.empty()) sc = err;
    else if (snap.status == "Up to date" || snap.status == "Installed") sc = ok;
    else if (snap.status == "Update available") sc = accent;
    draw_text(r, g_font_med, snap.status, rect.x + 24, row_y, sc);
    if (!snap.err.empty()) {
        draw_text(r, g_font_small, snap.err.substr(0, 60),
                  rect.x + 24, row_y + 36, err);
    }

    // Progress bar when downloading/installing.
    if (snap.busy && (snap.status == "Downloading…" || snap.status == "Installing…")) {
        SDL_Rect pb{rect.x + 24, rect.y + rect.h - 92, rect.w - 48, 14};
        draw_progress(r, pb, snap.progress);
        char pct[32];
        std::snprintf(pct, sizeof(pct), "%d%%", (int)(snap.progress * 100));
        draw_text(r, g_font_small, pct, rect.x + rect.w - 70, rect.y + rect.h - 90 - 20, dim);
    }

    // Button hint strip.
    SDL_Rect btn{rect.x + 24, rect.y + rect.h - 56, rect.w - 48, 36};
    fill_rect(r, btn, focus ? SDL_Color{90, 160, 255, 60} : SDL_Color{255, 255, 255, 20});
    stroke_rect(r, btn, focus ? SDL_Color{150, 200, 255, 220} : SDL_Color{255, 255, 255, 70});
    const char* label = "Press A to update";
    if (snap.busy) label = "Working…";
    else if (snap.status == "Up to date") label = "A: Reinstall";
    draw_text(r, g_font_small, label, btn.x + 14, btn.y + 7, white);
}

void draw_footer(SDL_Renderer* r) {
    SDL_Color dim{180, 190, 215, 255};
    draw_text(r, g_font_small, "A  Update   Y  Refresh   D-Pad  Switch   +  Exit",
              40, kH - 36, dim);
}

void draw_header(SDL_Renderer* r) {
    SDL_Color white{235, 240, 255, 255};
    SDL_Color dim{160, 180, 210, 255};
    draw_text(r, g_font_big, "AtmoUpdate", 40, 28, white);
    draw_text(r, g_font_small, "Atmosphère & Hekate — right here on your Switch", 40, 72, dim);
}

}  // namespace

int main(int, char**) {
    socketInitializeDefault();
    plInitialize(PlServiceType_User);
    nifmInitialize(NifmServiceType_User);
    romfsInit();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    init_fonts();

    SDL_Window* win = SDL_CreateWindow("AtmoUpdate", 0, 0, kW, kH, 0);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, kW, kH);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    for (int i = 0; i < 2; i++) SDL_JoystickOpen(i);
    SDL_GameControllerOpen(0);

    std::thread w(worker);

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    bool running = true;
    while (running && appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus) running = false;
        if (down & (HidNpadButton_Left | HidNpadButton_StickLLeft))  g_focus = (g_focus + kN - 1) % kN;
        if (down & (HidNpadButton_Right | HidNpadButton_StickLRight)) g_focus = (g_focus + 1) % kN;
        if (down & HidNpadButton_A) {
            std::lock_guard<std::mutex> lk(g_mu);
            if (!g_cards[g_focus].busy && !g_cards[g_focus].latest.empty()) {
                g_cards[g_focus].want_update = true;
                g_cv.notify_one();
            }
        }
        if (down & HidNpadButton_Y) {
            std::lock_guard<std::mutex> lk(g_mu);
            for (auto& c : g_cards) if (!c.busy) c.need_refresh = true;
            g_cv.notify_one();
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
        }

        draw_background(ren);
        draw_header(ren);
        int cw = 540, ch = 420;
        int gap = 40;
        int total = cw * kN + gap * (kN - 1);
        int x0 = (kW - total) / 2;
        int y0 = 150;
        for (int i = 0; i < kN; i++) {
            SDL_Rect rect{x0 + i * (cw + gap), y0, cw, ch};
            draw_card(ren, i, rect);
        }
        draw_footer(ren);
        SDL_RenderPresent(ren);
    }

    g_quit = true;
    g_cv.notify_all();
    w.join();

    if (g_font_big) TTF_CloseFont(g_font_big);
    if (g_font_med) TTF_CloseFont(g_font_med);
    if (g_font_small) TTF_CloseFont(g_font_small);
    TTF_Quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    curl_global_cleanup();
    romfsExit();
    nifmExit();
    plExit();
    socketExit();
    return 0;
}
