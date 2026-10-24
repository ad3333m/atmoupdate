// AtmoUpdate - fetch and install Atmosphere + Hekate + Firmware releases on-device.
#include "http.hpp"
#include "gh.hpp"
#include "install.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <switch.h>
#include <curl/curl.h>

#include <algorithm>
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

enum class Kind { Atmosphere, Hekate, Firmware };

struct Candidate {
    std::string version;
    std::string asset_url;
    std::string asset_name;
};

struct Card {
    const char* title;
    const char* repo;
    const char* saved_key;
    const char* asset_prefix;
    const char* asset_contains;
    Kind kind;
    bool is_list;           // Firmware: render scrollable list of candidates.

    std::string installed;
    std::string status;
    std::string err;
    std::vector<Candidate> cands;
    int selected = 0;
    int scroll   = 0;
    double progress = 0.0;
    bool busy = false;
    bool need_refresh = true;
    bool want_update = false;
};

Card g_cards[] = {
    {"Atmosphère", "Atmosphere-NX/Atmosphere", "atmosphere",
     "atmosphere-", ".zip", Kind::Atmosphere, false},
    {"Hekate",     "CTCaer/hekate",            "hekate",
     "hekate_ctcaer_", ".zip", Kind::Hekate, false},
    {"Firmware",   "THZoria/NX_Firmware",      "firmware",
     "", ".zip", Kind::Firmware, true},
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

SDL_Texture* g_panel       = nullptr;
SDL_Texture* g_panel_focus = nullptr;
SDL_Texture* g_pill        = nullptr;
SDL_Texture* g_pill_focus  = nullptr;
SDL_Texture* g_glow        = nullptr;

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

void draw_text_width(SDL_Renderer* r, TTF_Font* f, const std::string& s, int x, int y, int max_w, SDL_Color c) {
    int w = 0, h = 0;
    SDL_Texture* t = text_tex(r, f, s, c, &w, &h);
    if (!t) return;
    if (w <= max_w) {
        SDL_Rect dst{x, y, w, h};
        SDL_RenderCopy(r, t, nullptr, &dst);
    } else {
        SDL_Rect src{0, 0, max_w, h};
        SDL_Rect dst{x, y, max_w, h};
        SDL_RenderCopy(r, t, &src, &dst);
    }
    SDL_DestroyTexture(t);
}

void fill_rect(SDL_Renderer* r, SDL_Rect rect, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_RenderFillRect(r, &rect);
}

SDL_Texture* build_panel(SDL_Renderer* r, int w, int h, int radius, SDL_Color border, int border_px) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_LockSurface(s);
    Uint32* px = static_cast<Uint32*>(s->pixels);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double dist_outside = 0.0;
            bool in_corner = false;
            int cx = x, cy = y;
            if (x < radius && y < radius)                 { cx = radius; cy = radius; in_corner = true; }
            else if (x >= w - radius && y < radius)       { cx = w - radius - 1; cy = radius; in_corner = true; }
            else if (x < radius && y >= h - radius)       { cx = radius; cy = h - radius - 1; in_corner = true; }
            else if (x >= w - radius && y >= h - radius)  { cx = w - radius - 1; cy = h - radius - 1; in_corner = true; }
            double d = 0.0;
            if (in_corner) {
                double dx = x - cx, dy = y - cy;
                d = std::sqrt(dx*dx + dy*dy);
                dist_outside = d - radius;
            }
            int edge_px = std::min({x, y, w - 1 - x, h - 1 - y});

            double coverage = 1.0;
            if (in_corner) {
                if (dist_outside > 0.5) coverage = 0.0;
                else if (dist_outside > -0.5) coverage = 0.5 - dist_outside;
            }
            if (coverage <= 0.0) { px[y*w + x] = 0; continue; }

            double v = static_cast<double>(y) / h;
            double top_boost = std::exp(-v * 4.0);
            Uint8 base = static_cast<Uint8>(22 + top_boost * 24);

            bool on_border = false;
            double border_f = static_cast<double>(border_px);
            if (in_corner) {
                if (dist_outside > -border_f && dist_outside <= 0.0) on_border = true;
            } else {
                if (edge_px < border_px) on_border = true;
            }

            Uint8 R, G, B, A;
            if (on_border) {
                R = border.r; G = border.g; B = border.b;
                A = static_cast<Uint8>(border.a * coverage);
            } else {
                R = 255; G = 255; B = 255;
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

SDL_Texture* build_glow(SDL_Renderer* r, int size) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_LockSurface(s);
    Uint32* px = static_cast<Uint32*>(s->pixels);
    double cx = size / 2.0;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double dx = x - cx, dy = y - cx;
            double d = std::sqrt(dx*dx + dy*dy) / cx;
            double a = std::max(0.0, 1.0 - d);
            a = a * a * a;
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
    if (g_glow) {
        double t = ticks / 1000.0;
        SDL_SetTextureColorMod(g_glow, 90, 140, 255);
        SDL_SetTextureAlphaMod(g_glow, 110);
        int sz = 1100;
        SDL_Rect a{int(-240 + std::sin(t * 0.4) * 60),
                   int(-240 + std::cos(t * 0.3) * 60), sz, sz};
        SDL_RenderCopy(r, g_glow, nullptr, &a);

        SDL_SetTextureColorMod(g_glow, 180, 110, 230);
        SDL_SetTextureAlphaMod(g_glow, 95);
        SDL_Rect b{int(ow - 820 + std::sin(t * 0.25) * 50),
                   int(oh - 620 + std::cos(t * 0.35) * 50), sz, sz};
        SDL_RenderCopy(r, g_glow, nullptr, &b);
    }
}

void draw_panel(SDL_Renderer* r, SDL_Rect rect, bool focus, Uint32 ticks) {
    if (g_panel) {
        SDL_SetTextureColorMod(g_panel, 0, 0, 0);
        SDL_SetTextureAlphaMod(g_panel, 90);
        SDL_Rect sh{rect.x - 4, rect.y + 10, rect.w + 8, rect.h + 10};
        SDL_RenderCopy(r, g_panel, nullptr, &sh);
        SDL_SetTextureColorMod(g_panel, 255, 255, 255);
        SDL_SetTextureAlphaMod(g_panel, 255);
    }
    SDL_Texture* tex = focus ? g_panel_focus : g_panel;
    if (!tex) return;
    SDL_RenderCopy(r, tex, nullptr, &rect);
    if (focus && g_panel_focus) {
        double pulse = 0.5 + 0.5 * std::sin(ticks / 450.0);
        SDL_SetTextureAlphaMod(g_panel_focus, static_cast<Uint8>(80 + pulse * 120));
        SDL_RenderCopy(r, g_panel_focus, nullptr, &rect);
        SDL_SetTextureAlphaMod(g_panel_focus, 255);
    }
}

void draw_pill(SDL_Renderer* r, int x_right, int y, const std::string& text, SDL_Color dot, SDL_Color fg) {
    int tw = 0, th = 0;
    SDL_Texture* tx = text_tex(r, g_font_small, text, fg, &tw, &th);
    int pad_x = 12, pad_y = 6, dot_w = 10, dot_gap = 10;
    int bgw = tw + pad_x * 2 + dot_w + dot_gap;
    int bgh = th + pad_y * 2;
    SDL_Rect bg{x_right - bgw, y, bgw, bgh};
    fill_rect(r, bg, SDL_Color{255, 255, 255, 28});
    fill_rect(r, SDL_Rect{bg.x + pad_x, bg.y + bgh/2 - dot_w/2, dot_w, dot_w}, dot);
    if (tx) {
        SDL_Rect dst{bg.x + pad_x + dot_w + dot_gap, bg.y + pad_y, tw, th};
        SDL_RenderCopy(r, tx, nullptr, &dst);
        SDL_DestroyTexture(tx);
    }
}

void draw_progress(SDL_Renderer* r, SDL_Rect rect, double p) {
    fill_rect(r, rect, SDL_Color{255, 255, 255, 32});
    SDL_Rect fill{rect.x, rect.y, int(rect.w * p), rect.h};
    fill_rect(r, fill, SDL_Color{110, 190, 255, 240});
    fill_rect(r, SDL_Rect{rect.x, rect.y, rect.w, 2}, SDL_Color{255, 255, 255, 70});
}

const Candidate* current_pick(const Card& c) {
    if (c.cands.empty()) return nullptr;
    int idx = std::max(0, std::min(c.selected, int(c.cands.size()) - 1));
    return &c.cands[idx];
}

// Scan a release's asset list and pick the right zip based on card's filter.
const gh::Asset* pick_asset(const Card& c, const gh::Release& rel) {
    for (auto& a : rel.assets) {
        if (c.asset_prefix && c.asset_prefix[0] && a.name.rfind(c.asset_prefix, 0) != 0) continue;
        if (c.asset_contains && c.asset_contains[0]) {
            if (a.name.find(c.asset_contains) == std::string::npos) continue;
        }
        return &a;
    }
    return nullptr;
}

std::string norm_tag(const std::string& t) {
    if (!t.empty() && (t[0] == 'v' || t[0] == 'V')) return t.substr(1);
    return t;
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
            std::vector<gh::Release> rels;
            std::string err;
            if (c.is_list) {
                if (!gh::releases(c.repo, rels, err)) {
                    std::lock_guard<std::mutex> lk(g_mu);
                    c.err = err; c.status = "Error"; c.busy = false; continue;
                }
            } else {
                gh::Release one;
                if (!gh::latest(c.repo, one, err)) {
                    std::lock_guard<std::mutex> lk(g_mu);
                    c.err = err; c.status = "Error"; c.busy = false; continue;
                }
                rels.push_back(std::move(one));
            }

            std::vector<Candidate> cands;
            cands.reserve(rels.size());
            for (auto& rel : rels) {
                const gh::Asset* a = pick_asset(c, rel);
                if (!a) continue;
                cands.push_back(Candidate{norm_tag(rel.tag), a->url, a->name});
            }

            std::string installed;
            switch (c.kind) {
                case Kind::Atmosphere: installed = install::detect_atmosphere_version(); break;
                case Kind::Hekate:     installed = install::detect_hekate_version();     break;
                case Kind::Firmware:   installed = install::detect_firmware_version();   break;
            }
            if (installed.empty()) installed = install::read_saved(c.saved_key);

            std::lock_guard<std::mutex> lk(g_mu);
            c.installed = installed;
            c.cands = std::move(cands);
            c.selected = 0;
            c.scroll = 0;
            if (c.cands.empty())                                       c.status = "No release";
            else if (installed.empty())                                c.status = "Not installed";
            else if (installed == c.cands.front().version)             c.status = "Up to date";
            else                                                       c.status = "Update available";
            c.busy = false;
        } else if (do_update) {
            std::string url, name, ver;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                const Candidate* p = current_pick(c);
                if (!p) { c.err = "no selection"; c.status = "Error"; c.busy = false; continue; }
                url = p->asset_url; name = p->asset_name; ver = p->version;
                c.status = "Downloading…"; c.progress = 0.0; c.err.clear();
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
            std::string dest_root = "sdmc:/";
            if (c.kind == Kind::Firmware) dest_root = "sdmc:/firmware/" + ver + "/";
            if (!install::extract_zip_to(tmp, dest_root, lg, err)) {
                std::lock_guard<std::mutex> lk(g_mu);
                c.err = err; c.status = "Install failed"; c.busy = false; continue;
            }
            if (c.kind == Kind::Hekate) {
                std::string pay = "sdmc:/hekate_ctcaer_" + ver + ".bin";
                std::string reboot = "sdmc:/atmosphere/reboot_payload.bin";
                std::string e2;
                install::copy_file(pay, reboot, e2);
            }
            install::write_saved(c.saved_key, ver);
            std::remove(tmp.c_str());
            std::lock_guard<std::mutex> lk(g_mu);
            if (c.kind != Kind::Firmware) c.installed = ver;
            c.status = (c.kind == Kind::Firmware) ? "Downloaded — install via Daybreak" : "Installed";
            c.busy = false;
        }
    }
}

void init_fonts() {
    plGetSharedFontByType(&g_font_data, PlSharedFontType_Standard);
    auto mk = [](int px){ return TTF_OpenFontRW(SDL_RWFromMem(g_font_data.address, g_font_data.size), 1, px); };
    g_font_hero  = mk(48);
    g_font_big   = mk(30);
    g_font_med   = mk(22);
    g_font_small = mk(18);
    g_font_tiny  = mk(15);
}

SDL_Color status_dot(const Card& c) {
    if (!c.err.empty())                                 return SDL_Color{255, 140, 140, 255};
    if (c.status == "Up to date" || c.status == "Installed") return SDL_Color{150, 230, 180, 255};
    if (c.status == "Update available")                 return SDL_Color{140, 200, 255, 255};
    if (c.busy)                                         return SDL_Color{140, 200, 255, 255};
    if (c.status.rfind("Downloaded", 0) == 0)           return SDL_Color{150, 230, 180, 255};
    return SDL_Color{170, 185, 215, 255};
}

void draw_card(SDL_Renderer* r, int idx, SDL_Rect rect, Uint32 ticks) {
    Card snap;
    { std::lock_guard<std::mutex> lk(g_mu); snap = g_cards[idx]; }
    bool focus = (idx == g_focus);
    draw_panel(r, rect, focus, ticks);

    SDL_Color white{240, 244, 255, 255};
    SDL_Color dim{170, 185, 215, 255};
    SDL_Color accent{140, 200, 255, 255};
    SDL_Color err{255, 140, 140, 255};

    int inner_x = rect.x + 24;
    int inner_w = rect.w - 48;

    draw_text(r, g_font_big, snap.title, inner_x, rect.y + 22, white);
    draw_pill(r, rect.x + rect.w - 24, rect.y + 30, snap.status, status_dot(snap), white);

    fill_rect(r, SDL_Rect{inner_x, rect.y + 80, inner_w, 1}, SDL_Color{255, 255, 255, 36});

    int ry = rect.y + 100;
    draw_text(r, g_font_small, "Installed", inner_x, ry, dim);
    draw_text_right(r, g_font_med, snap.installed.empty() ? "—" : snap.installed,
                    rect.x + rect.w - 24, ry - 2, white);

    if (!snap.is_list) {
        ry += 40;
        draw_text(r, g_font_small, "Latest", inner_x, ry, dim);
        std::string latest = snap.cands.empty() ? "—" : snap.cands.front().version;
        draw_text_right(r, g_font_med, latest, rect.x + rect.w - 24, ry - 2, accent);
    } else {
        // Version list.
        int list_y0 = ry + 36;
        int list_h = rect.y + rect.h - 100 - list_y0;
        int row_h = 32;
        int visible = std::max(1, list_h / row_h);
        int total = int(snap.cands.size());

        // Clamp scroll to keep selection visible.
        int sc = snap.scroll;
        if (snap.selected < sc) sc = snap.selected;
        if (snap.selected >= sc + visible) sc = snap.selected - visible + 1;
        sc = std::max(0, std::min(sc, std::max(0, total - visible)));

        for (int i = 0; i < visible && (sc + i) < total; i++) {
            int gi = sc + i;
            int rowy = list_y0 + i * row_h;
            SDL_Rect row{inner_x, rowy, inner_w, row_h - 4};
            bool sel = (gi == snap.selected);
            fill_rect(r, row, sel ? SDL_Color{110, 180, 255, 70} : SDL_Color{255, 255, 255, 14});
            fill_rect(r, SDL_Rect{row.x, row.y, 3, row.h}, sel ? SDL_Color{140, 200, 255, 255} : SDL_Color{255, 255, 255, 40});
            SDL_Color txt = sel ? white : dim;
            draw_text(r, g_font_small, snap.cands[gi].version, row.x + 14, row.y + 5, txt);
            if (gi == 0) {
                draw_text_right(r, g_font_tiny, "LATEST", row.x + row.w - 10, row.y + 8, accent);
            }
        }

        // Scroll hint.
        if (total > visible) {
            draw_text_right(r, g_font_tiny,
                            std::to_string(snap.selected + 1) + " / " + std::to_string(total),
                            rect.x + rect.w - 24, list_y0 - 20, dim);
            draw_text(r, g_font_tiny, "▲ ▼ scroll", inner_x, list_y0 - 20, dim);
        }
    }

    if (!snap.err.empty()) {
        draw_text_width(r, g_font_tiny, snap.err.substr(0, 60),
                        inner_x, rect.y + rect.h - 100, inner_w, err);
    }

    // Progress bar.
    if (snap.busy && (snap.status == "Downloading…" || snap.status == "Installing…")) {
        SDL_Rect pb{inner_x, rect.y + rect.h - 82, inner_w, 8};
        draw_progress(r, pb, snap.progress);
        char pct[32];
        std::snprintf(pct, sizeof(pct), "%d%%", int(snap.progress * 100));
        draw_text_right(r, g_font_tiny, pct, rect.x + rect.w - 24, rect.y + rect.h - 106, dim);
    }

    // Button.
    SDL_Rect btn{inner_x, rect.y + rect.h - 64, inner_w, 44};
    fill_rect(r, btn, focus ? SDL_Color{110, 170, 255, 90} : SDL_Color{255, 255, 255, 28});
    fill_rect(r, SDL_Rect{btn.x, btn.y, btn.w, 1}, SDL_Color{255, 255, 255, 80});

    const char* label = "A • Update";
    if (snap.busy)                                 label = "Working…";
    else if (snap.status == "Up to date")          label = "A • Reinstall";
    else if (snap.status == "Not installed")       label = "A • Install";
    else if (snap.kind == Kind::Firmware)          label = "A • Download selected";
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
    draw_text(r, g_font_hero, "AtmoUpdate", 44, 32, white);
    draw_text(r, g_font_small, "Atmosphère • Hekate • Firmware — on-device, no PC",
              46, 90, dim);
}

void draw_footer(SDL_Renderer* r) {
    int ow, oh; SDL_GetRendererOutputSize(r, &ow, &oh);
    fill_rect(r, SDL_Rect{40, oh - 64, ow - 80, 1}, SDL_Color{255, 255, 255, 30});
    SDL_Color dim{170, 185, 215, 255};
    draw_text(r, g_font_small,
              "A  Update   Y  Refresh   ◀ ▶  Card   ▲ ▼  List   +  Exit",
              46, oh - 44, dim);
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
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    constexpr int kCardW = 400, kCardH = 476;
    g_panel       = build_panel(ren, kCardW, kCardH, 24, SDL_Color{255, 255, 255, 95}, 1);
    g_panel_focus = build_panel(ren, kCardW, kCardH, 24, SDL_Color{150, 200, 255, 220}, 2);
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
        if (down & (HidNpadButton_Up    | HidNpadButton_StickLUp)) {
            std::lock_guard<std::mutex> lk(g_mu);
            Card& c = g_cards[g_focus];
            if (c.is_list && !c.cands.empty()) {
                c.selected = std::max(0, c.selected - 1);
            }
        }
        if (down & (HidNpadButton_Down  | HidNpadButton_StickLDown)) {
            std::lock_guard<std::mutex> lk(g_mu);
            Card& c = g_cards[g_focus];
            if (c.is_list && !c.cands.empty()) {
                c.selected = std::min(int(c.cands.size()) - 1, c.selected + 1);
            }
        }
        if (down & HidNpadButton_A) {
            std::lock_guard<std::mutex> lk(g_mu);
            Card& c = g_cards[g_focus];
            if (!c.busy && !c.cands.empty()) {
                c.want_update = true;
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
        Uint32 ticks = SDL_GetTicks();
        draw_background(ren, ticks);
        draw_header(ren);

        int gap = 40;
        int total = kCardW * kN + gap * (kN - 1);
        int x0 = (ow - total) / 2;
        int y0 = 150;
        for (int i = 0; i < kN; i++) {
            SDL_Rect rect{x0 + i * (kCardW + gap), y0, kCardW, kCardH};
            draw_card(ren, i, rect, ticks);
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
    if (g_font_hero)   TTF_CloseFont(g_font_hero);
    if (g_font_big)    TTF_CloseFont(g_font_big);
    if (g_font_med)    TTF_CloseFont(g_font_med);
    if (g_font_small)  TTF_CloseFont(g_font_small);
    if (g_font_tiny)   TTF_CloseFont(g_font_tiny);
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
