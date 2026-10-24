// AtmoUpdate - fetch and install Atmosphere + Hekate releases on-device.
#include "http.hpp"
#include "gh.hpp"
#include "install.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <switch.h>
#include <curl/curl.h>

#include <atomic>
#include <cmath>
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
    const char* repo;
    const char* saved_key;
    const char* asset_prefix;
    const char* asset_contains;

    std::string installed;
    std::string latest;
    std::string status;
    std::string err;
    std::string zip_url;
    std::string zip_name;
    double progress = 0.0;
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

TTF_Font* g_font_hero  = nullptr;
TTF_Font* g_font_big   = nullptr;
TTF_Font* g_font_med   = nullptr;
TTF_Font* g_font_small = nullptr;
TTF_Font* g_font_tiny  = nullptr;
PlFontData g_font_data;

SDL_Texture* g_panel     = nullptr;
SDL_Texture* g_panel_focus = nullptr;
SDL_Texture* g_glow      = nullptr;

// ─── drawing helpers ─────────────────────────────────────────────────────────

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

void draw_text_right(SDL_Renderer* r, TTF_Font* f, const std::string& s, int x_right, int y, SDL_Color c) {
    int w = 0, h = 0;
    SDL_Texture* t = text_tex(r, f, s, c, &w, &h);
    if (!t) return;
    SDL_Rect dst{x_right - w, y, w, h};
    SDL_RenderCopy(r, t, nullptr, &dst);
    SDL_DestroyTexture(t);
}

void fill_rect(SDL_Renderer* r, SDL_Rect rect, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_RenderFillRect(r, &rect);
}

// Software-generate a translucent rounded glass panel with soft inner highlight + border.
SDL_Texture* build_panel(SDL_Renderer* r, int w, int h, int radius, SDL_Color border) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_LockSurface(s);
    Uint32* px = static_cast<Uint32*>(s->pixels);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            // Nearest corner distance (for rounded alpha).
            double dist_outside = 0.0;
            bool in_corner_zone = false;
            int cx = x, cy = y;
            if (x < radius && y < radius)            { cx = radius; cy = radius; in_corner_zone = true; }
            else if (x >= w - radius && y < radius)  { cx = w - radius - 1; cy = radius; in_corner_zone = true; }
            else if (x < radius && y >= h - radius)  { cx = radius; cy = h - radius - 1; in_corner_zone = true; }
            else if (x >= w - radius && y >= h - radius) { cx = w - radius - 1; cy = h - radius - 1; in_corner_zone = true; }
            double d = 0.0;
            if (in_corner_zone) {
                double dx = x - cx, dy = y - cy;
                d = std::sqrt(dx*dx + dy*dy);
                dist_outside = d - radius;
            }

            // Edge-of-rect proximity for straight sides.
            int edge_px = std::min({x, y, w - 1 - x, h - 1 - y});

            // Alpha of the whole panel at this pixel (soft AA on rounded edge).
            double coverage = 1.0;
            if (in_corner_zone) {
                if (dist_outside > 0.5) coverage = 0.0;
                else if (dist_outside > -0.5) coverage = 0.5 - dist_outside;
            }
            if (coverage <= 0.0) { px[y*w + x] = 0; continue; }

            // Vertical gradient for the fill: brighter at top for a glass highlight.
            double v = static_cast<double>(y) / h;
            double top_boost = std::exp(-v * 4.0);          // 1→~0.02 top→bottom
            Uint8 base = static_cast<Uint8>(22 + top_boost * 22);     // 44 at top, 22 at bottom
            Uint8 fill_r = 255, fill_g = 255, fill_b = 255;

            // Border detection: within 1.4 px of the rounded perimeter.
            bool on_border = false;
            if (in_corner_zone) {
                if (dist_outside > -1.4 && dist_outside <= 0.0) on_border = true;
            } else {
                if (edge_px <= 1) on_border = true;
            }

            Uint8 R, G, B, A;
            if (on_border) {
                R = border.r; G = border.g; B = border.b;
                A = static_cast<Uint8>(border.a * coverage);
            } else {
                R = fill_r; G = fill_g; B = fill_b;
                A = static_cast<Uint8>(base * coverage);
            }
            px[y*w + x] = (Uint32(A) << 24) | (Uint32(R) << 16) | (Uint32(G) << 8) | Uint32(B);
        }
    }
    SDL_UnlockSurface(s);
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
    SDL_FreeSurface(s);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

// Soft radial glow blob (used for background depth).
SDL_Texture* build_glow(SDL_Renderer* r, int size) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_LockSurface(s);
    Uint32* px = static_cast<Uint32*>(s->pixels);
    double cx = size / 2.0, cy = size / 2.0;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double dx = x - cx, dy = y - cy;
            double d = std::sqrt(dx*dx + dy*dy) / cx;
            double a = std::max(0.0, 1.0 - d);
            a = a * a * a;                   // steeper falloff
            Uint8 A = static_cast<Uint8>(a * 255);
            px[y*size + x] = (Uint32(A) << 24) | 0x00FFFFFF;
        }
    }
    SDL_UnlockSurface(s);
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
    SDL_FreeSurface(s);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

void draw_background(SDL_Renderer* r, Uint32 ticks) {
    // Deep navy vertical gradient fills the whole output.
    int ow, oh;
    SDL_GetRendererOutputSize(r, &ow, &oh);
    for (int y = 0; y < oh; y++) {
        double v = double(y) / oh;
        Uint8 R = static_cast<Uint8>(6  + v * 10);
        Uint8 G = static_cast<Uint8>(10 + v * 14);
        Uint8 B = static_cast<Uint8>(28 + v * 32);
        SDL_SetRenderDrawColor(r, R, G, B, 255);
        SDL_RenderDrawLine(r, 0, y, ow, y);
    }
    // Two slow-drifting glow blobs.
    if (g_glow) {
        double t = ticks / 1000.0;
        SDL_SetTextureColorMod(g_glow, 90, 140, 255);
        SDL_SetTextureAlphaMod(g_glow, 110);
        int sz = 1000;
        SDL_Rect a{int(-200 + std::sin(t * 0.4) * 60),
                   int(-200 + std::cos(t * 0.3) * 60), sz, sz};
        SDL_RenderCopy(r, g_glow, nullptr, &a);

        SDL_SetTextureColorMod(g_glow, 180, 100, 220);
        SDL_SetTextureAlphaMod(g_glow, 90);
        SDL_Rect b{int(ow - 800 + std::sin(t * 0.25) * 50),
                   int(oh - 600 + std::cos(t * 0.35) * 50), sz, sz};
        SDL_RenderCopy(r, g_glow, nullptr, &b);
    }
}

void draw_panel(SDL_Renderer* r, SDL_Rect rect, bool focus) {
    // Soft drop shadow.
    if (g_panel) {
        SDL_SetTextureColorMod(g_panel, 0, 0, 0);
        SDL_SetTextureAlphaMod(g_panel, 90);
        SDL_Rect sh{rect.x - 4, rect.y + 10, rect.w + 8, rect.h + 10};
        SDL_RenderCopy(r, g_panel, nullptr, &sh);
        SDL_SetTextureColorMod(g_panel, 255, 255, 255);
        SDL_SetTextureAlphaMod(g_panel, 255);
    }
    SDL_Texture* tex = focus ? g_panel_focus : g_panel;
    if (tex) SDL_RenderCopy(r, tex, nullptr, &rect);
}

void draw_pill(SDL_Renderer* r, int x, int y, const std::string& text, SDL_Color dot, SDL_Color fg) {
    int tw = 0, th = 0;
    SDL_Texture* tx = text_tex(r, g_font_small, text, fg, &tw, &th);
    int pad_x = 14, pad_y = 6;
    SDL_Rect bg{x, y, tw + pad_x * 2 + 20, th + pad_y * 2};
    fill_rect(r, bg, SDL_Color{255, 255, 255, 20});
    // dot
    fill_rect(r, SDL_Rect{x + pad_x, y + pad_y + th/2 - 5, 10, 10}, dot);
    if (tx) {
        SDL_Rect dst{x + pad_x + 20, y + pad_y, tw, th};
        SDL_RenderCopy(r, tx, nullptr, &dst);
        SDL_DestroyTexture(tx);
    }
}

void draw_progress(SDL_Renderer* r, SDL_Rect rect, double p) {
    fill_rect(r, rect, SDL_Color{255, 255, 255, 28});
    SDL_Rect fill{rect.x, rect.y, int(rect.w * p), rect.h};
    fill_rect(r, fill, SDL_Color{110, 190, 255, 240});
    // Subtle inner highlight at top of bar.
    SDL_Rect hi{rect.x, rect.y, rect.w, 2};
    fill_rect(r, hi, SDL_Color{255, 255, 255, 60});
}

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
            { std::lock_guard<std::mutex> lk(g_mu); c.status = "Checking…"; c.err.clear(); }
            gh::Release rel; std::string err;
            if (!gh::latest(c.repo, rel, err)) {
                std::lock_guard<std::mutex> lk(g_mu);
                c.err = err; c.status = "Error"; c.busy = false; continue;
            }
            const gh::Asset* a = pick_asset(c, rel);
            std::string key = c.saved_key;
            std::string installed;
            if (key == "atmosphere") installed = install::detect_atmosphere_version();
            else if (key == "hekate") installed = install::detect_hekate_version();
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
            { std::lock_guard<std::mutex> lk(g_mu); c.status = "Installing…"; c.progress = 1.0; }
            install::LogFn lg = [](const std::string&) {};
            if (!install::extract_zip_to_root(tmp, lg, err)) {
                std::lock_guard<std::mutex> lk(g_mu);
                c.err = err; c.status = "Install failed"; c.busy = false; continue;
            }
            if (std::string(c.saved_key) == "hekate") {
                std::string pay = "sdmc:/hekate_ctcaer_" + c.latest + ".bin";
                std::string reboot = "sdmc:/atmosphere/reboot_payload.bin";
                std::string e2;
                install::copy_file(pay, reboot, e2);
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
    auto mk = [](int px){ return TTF_OpenFontRW(SDL_RWFromMem(g_font_data.address, g_font_data.size), 1, px); };
    g_font_hero  = mk(48);
    g_font_big   = mk(34);
    g_font_med   = mk(24);
    g_font_small = mk(20);
    g_font_tiny  = mk(16);
}

void draw_card(SDL_Renderer* r, int idx, SDL_Rect rect) {
    Card snap;
    { std::lock_guard<std::mutex> lk(g_mu); snap = g_cards[idx]; }
    bool focus = (idx == g_focus);
    draw_panel(r, rect, focus);

    SDL_Color white{240, 244, 255, 255};
    SDL_Color dim{170, 185, 215, 255};
    SDL_Color accent{140, 200, 255, 255};
    SDL_Color err{255, 140, 140, 255};
    SDL_Color ok{150, 230, 180, 255};

    int inner_x = rect.x + 32;
    int inner_w = rect.w - 64;

    // Title row.
    draw_text(r, g_font_big, snap.title, inner_x, rect.y + 26, white);

    // Status pill, top-right.
    SDL_Color dot = dim;
    if (!snap.err.empty()) dot = err;
    else if (snap.status == "Up to date" || snap.status == "Installed") dot = ok;
    else if (snap.status == "Update available") dot = accent;
    else if (snap.busy) dot = accent;
    int pw = 0, ph = 0;
    SDL_Texture* pt = text_tex(r, g_font_small, snap.status, white, &pw, &ph);
    if (pt) SDL_DestroyTexture(pt);
    draw_pill(r, rect.x + rect.w - 32 - (pw + 54), rect.y + 36, snap.status, dot, white);

    // Divider.
    SDL_Rect div{inner_x, rect.y + 92, inner_w, 1};
    fill_rect(r, div, SDL_Color{255, 255, 255, 35});

    // Version lines.
    int ry = rect.y + 118;
    draw_text(r, g_font_small, "Installed", inner_x, ry, dim);
    draw_text_right(r, g_font_med, snap.installed.empty() ? "—" : snap.installed,
                    rect.x + rect.w - 32, ry - 2, white);

    ry += 46;
    draw_text(r, g_font_small, "Latest", inner_x, ry, dim);
    draw_text_right(r, g_font_med, snap.latest.empty() ? "—" : snap.latest,
                    rect.x + rect.w - 32, ry - 2, accent);

    // Error detail.
    if (!snap.err.empty()) {
        draw_text(r, g_font_small, snap.err.substr(0, 46), inner_x, ry + 48, err);
    }

    // Progress bar when busy.
    if (snap.busy && (snap.status == "Downloading…" || snap.status == "Installing…")) {
        SDL_Rect pb{inner_x, rect.y + rect.h - 110, inner_w, 10};
        draw_progress(r, pb, snap.progress);
        char pct[32];
        std::snprintf(pct, sizeof(pct), "%d%%", int(snap.progress * 100));
        draw_text_right(r, g_font_tiny, pct, rect.x + rect.w - 32, rect.y + rect.h - 134, dim);
    }

    // Action button strip.
    SDL_Rect btn{inner_x, rect.y + rect.h - 76, inner_w, 48};
    fill_rect(r, btn, focus ? SDL_Color{110, 170, 255, 70} : SDL_Color{255, 255, 255, 22});
    // Inner highlight.
    fill_rect(r, SDL_Rect{btn.x, btn.y, btn.w, 1}, SDL_Color{255, 255, 255, 70});
    const char* label = "Press A to update";
    if (snap.busy) label = "Working…";
    else if (snap.status == "Up to date") label = "A • Reinstall";
    else if (snap.status == "Not installed") label = "A • Install";
    int lw = 0, lh = 0;
    SDL_Texture* lt = text_tex(r, g_font_med, label, white, &lw, &lh);
    if (lt) {
        SDL_Rect dst{btn.x + (btn.w - lw)/2, btn.y + (btn.h - lh)/2, lw, lh};
        SDL_RenderCopy(r, lt, nullptr, &dst);
        SDL_DestroyTexture(lt);
    }
}

void draw_header(SDL_Renderer* r) {
    SDL_Color white{240, 244, 255, 255};
    SDL_Color dim{170, 185, 215, 255};
    draw_text(r, g_font_hero, "AtmoUpdate", 60, 38, white);
    draw_text(r, g_font_small, "Atmosphère & Hekate — on-device, no PC", 62, 100, dim);
}

void draw_footer(SDL_Renderer* r) {
    int ow, oh; SDL_GetRendererOutputSize(r, &ow, &oh);
    // Subtle divider line.
    fill_rect(r, SDL_Rect{40, oh - 70, ow - 80, 1}, SDL_Color{255, 255, 255, 30});
    SDL_Color dim{170, 185, 215, 255};
    draw_text(r, g_font_small,
              "A  Update   Y  Refresh   ◀ ▶  Switch card   +  Exit",
              60, oh - 48, dim);
}

}  // namespace

int main(int, char**) {
    socketInitializeDefault();
    plInitialize(PlServiceType_User);
    nifmInitialize(NifmServiceType_User);
    romfsInit();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    init_fonts();

    SDL_Window* win = SDL_CreateWindow("AtmoUpdate",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       kW, kH, 0);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1,
                                           SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    // No logical size — draw to full renderer output, so no letterbox bars.
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    // Pre-build glass panel textures sized for one card.
    constexpr int kCardW = 520, kCardH = 420;
    g_panel       = build_panel(ren, kCardW, kCardH, 26, SDL_Color{255, 255, 255, 90});
    g_panel_focus = build_panel(ren, kCardW, kCardH, 26, SDL_Color{150, 200, 255, 230});
    g_glow        = build_glow(ren, 384);

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
        if (down & (HidNpadButton_Left  | HidNpadButton_StickLLeft))  g_focus = (g_focus + kN - 1) % kN;
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
        while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) running = false;

        int ow, oh; SDL_GetRendererOutputSize(ren, &ow, &oh);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        draw_background(ren, SDL_GetTicks());
        draw_header(ren);

        int gap = 48;
        int total = kCardW * kN + gap * (kN - 1);
        int x0 = (ow - total) / 2;
        int y0 = 190;
        for (int i = 0; i < kN; i++) {
            SDL_Rect rect{x0 + i * (kCardW + gap), y0, kCardW, kCardH};
            draw_card(ren, i, rect);
        }
        draw_footer(ren);
        SDL_RenderPresent(ren);
    }

    g_quit = true;
    g_cv.notify_all();
    w.join();

    if (g_panel)       SDL_DestroyTexture(g_panel);
    if (g_panel_focus) SDL_DestroyTexture(g_panel_focus);
    if (g_glow)        SDL_DestroyTexture(g_glow);
    if (g_font_hero)  TTF_CloseFont(g_font_hero);
    if (g_font_big)   TTF_CloseFont(g_font_big);
    if (g_font_med)   TTF_CloseFont(g_font_med);
    if (g_font_small) TTF_CloseFont(g_font_small);
    if (g_font_tiny)  TTF_CloseFont(g_font_tiny);
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
