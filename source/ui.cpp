#include "ui.hpp"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui {
namespace {

// ─── palette ────────────────────────────────────────────────────────────────

struct Col { Uint8 r, g, b, a; };

Col A(Col c, int a) { c.a = Uint8(std::clamp(a, 0, 255)); return c; }
Col lerp(Col a, Col b, float t) {
    auto m = [t](Uint8 x, Uint8 y) { return Uint8(std::lround(x + (int(y) - int(x)) * t)); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

const Col kWhite{255, 255, 255, 255};
const Col kText{244, 246, 255, 255};
const Col kText2{226, 232, 252, 178};
const Col kText3{214, 222, 248, 116};
const Col kAccentA{82, 130, 255, 255};
const Col kAccentB{156, 92, 255, 255};
const Col kGreen{70, 224, 156, 255};
const Col kAmber{255, 186, 80, 255};
const Col kRed{255, 98, 114, 255};
const Col kBlue{128, 176, 255, 255};

// ─── state ──────────────────────────────────────────────────────────────────

SDL_Renderer* R = nullptr;
SDL_Texture* g_bg = nullptr;
SDL_Texture* g_frost = nullptr;

enum Font { F_Cap, F_Key, F_Small, F_Body, F_Row, F_Btn, F_Item, F_Icon, F_H2, F_Title, F_Ver, F_Hero, F_N };
struct FontSpec { const char* file; int px; };
const FontSpec kFonts[F_N] = {
    {"Inter-SemiBold.ttf", 13}, {"Inter-Bold.ttf", 14},     {"Inter-Medium.ttf", 15},
    {"Inter-Regular.ttf", 17},  {"Inter-Medium.ttf", 18},   {"Inter-SemiBold.ttf", 19},
    {"Inter-SemiBold.ttf", 21}, {"Inter-Bold.ttf", 23},     {"Inter-SemiBold.ttf", 26},
    {"Inter-SemiBold.ttf", 26}, {"Inter-SemiBold.ttf", 40}, {"Inter-Bold.ttf", 42},
};
TTF_Font* g_font[F_N] = {};

float s_sel_y = -1;
float s_prog[3] = {0, 0, 0};

// ─── geometry: anti-aliased rounded rects via SDL_RenderGeometry ───────────

constexpr float kPi = 3.14159265f;
std::vector<SDL_FPoint> g_ring[4];
std::vector<SDL_Vertex> g_v;
std::vector<int> g_i;

int segs_for(float r) { return std::clamp(int(r * 0.45f), 3, 14); }

void perim(std::vector<SDL_FPoint>& o, float x, float y, float w, float h, float r, float off, int seg) {
    o.clear();
    const float cx[4] = {x + r, x + w - r, x + w - r, x + r};
    const float cy[4] = {y + r, y + r, y + h - r, y + h - r};
    const float a0[4] = {kPi, 1.5f * kPi, 0.f, 0.5f * kPi};
    float rad = std::max(0.f, r + off);
    for (int k = 0; k < 4; k++)
        for (int i = 0; i <= seg; i++) {
            float a = a0[k] + 0.5f * kPi * float(i) / float(seg);
            o.push_back({cx[k] + std::cos(a) * rad, cy[k] + std::sin(a) * rad});
        }
}

struct Grad { Col a, b; bool horiz; float x, y, w, h; };
SDL_Color at(const Grad& g, SDL_FPoint p, float amul) {
    float t = g.horiz ? (p.x - g.x) / g.w : (p.y - g.y) / g.h;
    Col c = lerp(g.a, g.b, std::clamp(t, 0.f, 1.f));
    return SDL_Color{c.r, c.g, c.b, Uint8(c.a * amul)};
}

void flush(SDL_Texture* tex = nullptr) {
    if (!g_i.empty()) SDL_RenderGeometry(R, tex, g_v.data(), int(g_v.size()), g_i.data(), int(g_i.size()));
    g_v.clear();
    g_i.clear();
}

void fill_rr(float x, float y, float w, float h, float r, Col top, Col bot, bool horiz = false) {
    if (w <= 0 || h <= 0) return;
    r = std::min(r, std::min(w, h) * 0.5f);
    int seg = segs_for(r);
    Grad g{top, bot, horiz, x, y, w, h};
    perim(g_ring[0], x, y, w, h, r, -0.5f, seg);
    perim(g_ring[1], x, y, w, h, r, 0.5f, seg);
    int n = int(g_ring[0].size());
    SDL_FPoint c{x + w / 2, y + h / 2};
    g_v.push_back({c, at(g, c, 1), {0, 0}});
    for (auto& p : g_ring[0]) g_v.push_back({p, at(g, p, 1), {0, 0}});
    for (auto& p : g_ring[1]) g_v.push_back({p, at(g, p, 0), {0, 0}});
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        g_i.insert(g_i.end(), {0, 1 + i, 1 + j});
        g_i.insert(g_i.end(), {1 + i, 1 + n + i, 1 + n + j, 1 + i, 1 + n + j, 1 + j});
    }
    flush();
}

void stroke_rr(float x, float y, float w, float h, float r, float t, Col top, Col bot) {
    if (w <= 0 || h <= 0) return;
    r = std::min(r, std::min(w, h) * 0.5f);
    int seg = segs_for(r);
    Grad g{top, bot, false, x, y, w, h};
    const float off[4] = {0.5f, -0.5f, std::min(-0.5f, 0.5f - t), -0.5f - t};
    const float al[4] = {0, 1, 1, 0};
    int n = 0;
    for (int k = 0; k < 4; k++) {
        perim(g_ring[k], x, y, w, h, r, off[k], seg);
        n = int(g_ring[k].size());
        for (auto& p : g_ring[k]) g_v.push_back({p, at(g, p, al[k]), {0, 0}});
    }
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n, a = k * n + i, b = k * n + j, c = (k + 1) * n + i, d = (k + 1) * n + j;
            g_i.insert(g_i.end(), {a, b, d, a, d, c});
        }
    flush();
}

// Blurred copy of the background, mapped through the shape: real frosted glass.
void frost_rr(float x, float y, float w, float h, float r) {
    r = std::min(r, std::min(w, h) * 0.5f);
    perim(g_ring[0], x, y, w, h, r, 0, segs_for(r));
    auto uv = [](float px, float py) {   // SDL rejects UVs outside 0..1
        return SDL_FPoint{std::clamp(px / kW, 0.f, 1.f), std::clamp(py / kH, 0.f, 1.f)};
    };
    const SDL_Color white{255, 255, 255, 255};
    g_v.push_back({{x + w / 2, y + h / 2}, white, uv(x + w / 2, y + h / 2)});
    for (auto& p : g_ring[0]) g_v.push_back({p, white, uv(p.x, p.y)});
    int n = int(g_ring[0].size());
    for (int i = 0; i < n; i++) g_i.insert(g_i.end(), {0, 1 + i, 1 + (i + 1) % n});
    flush(g_frost);
}

void circle(float cx, float cy, float r, Col c) { fill_rr(cx - r, cy - r, 2 * r, 2 * r, r, c, c); }

void shadow(float x, float y, float w, float h, float r, float dy = 14.f, int alpha = 11) {
    const Col c{0, 0, 0, Uint8(alpha)};
    for (int i = 6; i >= 1; i--) {
        float s = i * 4.f;
        fill_rr(x - s, y - s + dy, w + 2 * s, h + 2 * s, r + s, c, c);
    }
}

void glass(float x, float y, float w, float h, float r, int top = 24, int bot = 9) {
    shadow(x, y, w, h, r);
    frost_rr(x, y, w, h, r);
    fill_rr(x, y, w, h, r, A(kWhite, top), A(kWhite, bot));
    stroke_rr(x, y, w, h, r, 1.f, A(kWhite, 72), A(kWhite, 16));
}

// ─── text (white glyphs cached per font+string, tinted at draw time) ───────

struct Tex { SDL_Texture* t = nullptr; int w = 0, h = 0; Uint32 used = 0; };
std::unordered_map<std::string, Tex> g_cache;
Uint32 g_frame = 0;

const Tex* tex_for(Font f, const std::string& s) {
    if (s.empty() || !g_font[f]) return nullptr;
    std::string key(1, char('a' + f));
    key += s;
    auto it = g_cache.find(key);
    if (it != g_cache.end()) { it->second.used = g_frame; return &it->second; }
    if (g_cache.size() > 700)
        for (auto i = g_cache.begin(); i != g_cache.end();) {
            if (g_frame - i->second.used > 90) { SDL_DestroyTexture(i->second.t); i = g_cache.erase(i); }
            else ++i;
        }
    SDL_Surface* surf = TTF_RenderUTF8_Blended(g_font[f], s.c_str(), SDL_Color{255, 255, 255, 255});
    if (!surf) return nullptr;
    Tex t;
    t.t = SDL_CreateTextureFromSurface(R, surf);
    t.w = surf->w;
    t.h = surf->h;
    t.used = g_frame;
    SDL_FreeSurface(surf);
    if (!t.t) return nullptr;
    SDL_SetTextureBlendMode(t.t, SDL_BLENDMODE_BLEND);
    return &(g_cache[key] = t);
}

float text_w(Font f, const std::string& s) {
    int w = 0, h = 0;
    if (!s.empty()) TTF_SizeUTF8(g_font[f], s.c_str(), &w, &h);
    return float(w);
}

enum Align { Left, Center, Right };

// cy = vertical centre of the line (Inter's box centre sits on its cap centre).
float text(Font f, const std::string& s, float x, float cy, Col c, Align al = Left) {
    const Tex* t = tex_for(f, s);
    if (!t) return 0;
    float dx = al == Left ? x : al == Center ? x - t->w / 2.f : x - t->w;
    SDL_FRect dst{std::round(dx), std::round(cy - t->h / 2.f), float(t->w), float(t->h)};
    SDL_SetTextureColorMod(t->t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t->t, c.a);
    SDL_RenderCopyF(R, t->t, nullptr, &dst);
    return float(t->w);
}

void pop_utf8(std::string& s) {
    while (!s.empty() && (Uint8(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
}

std::string fit(Font f, std::string s, float maxw) {
    if (text_w(f, s) <= maxw) return s;
    while (!s.empty()) {
        pop_utf8(s);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        if (text_w(f, s + "…") <= maxw) return s + "…";
    }
    return "…";
}

std::vector<std::string> wrap(Font f, const std::string& para, float maxw) {
    std::vector<std::string> lines;
    std::string line, word;
    auto push_word = [&] {
        if (word.empty()) return;
        std::string cand = line.empty() ? word : line + " " + word;
        if (line.empty() || text_w(f, cand) <= maxw) line = cand;
        else { lines.push_back(line); line = word; }
        word.clear();
    };
    for (char ch : para) {
        if (ch == ' ') push_word();
        else word += ch;
    }
    push_word();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// ─── release notes: markdown → wrapped lines (cached) ──────────────────────

std::string inline_md(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if ((c == '[' || (c == '!' && i + 1 < s.size() && s[i + 1] == '['))) {
            size_t open = c == '!' ? i + 1 : i;
            size_t cb = s.find(']', open);
            if (cb != std::string::npos && cb + 1 < s.size() && s[cb + 1] == '(') {
                size_t cp = s.find(')', cb);
                if (cp != std::string::npos) {
                    if (c == '[') o += inline_md(s.substr(open + 1, cb - open - 1));
                    i = cp;
                    continue;
                }
            }
        }
        if (c == '<') {
            size_t gt = s.find('>', i);
            if (gt != std::string::npos && gt - i < 48) { i = gt; continue; }
        }
        if (c == '*' || c == '`' || c == '~') continue;
        if (c == '_' && i + 1 < s.size() && s[i + 1] == '_') { i++; continue; }
        o += (c == '\t') ? ' ' : c;
    }
    return o;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

struct NoteLine { std::string s; Font f; Col c; float indent; float gap; bool bullet; };
std::unordered_map<std::string, std::vector<NoteLine>> g_notes;

const std::vector<NoteLine>& notes_layout(const std::string& md, float maxw) {
    auto it = g_notes.find(md);
    if (it != g_notes.end()) return it->second;
    std::vector<NoteLine> out;
    float pending_gap = 0;
    size_t a = 0;
    while (a <= md.size()) {
        size_t b = md.find('\n', a);
        if (b == std::string::npos) b = md.size();
        std::string ln = trim(md.substr(a, b - a));
        a = b + 1;
        if (ln.empty()) { if (!out.empty()) pending_gap = 10; continue; }
        bool head = false, bullet = false;
        if (ln[0] == '#') { head = true; ln.erase(0, ln.find_first_not_of('#')); }
        else if (ln.size() > 1 && (ln[0] == '-' || ln[0] == '*' || ln[0] == '+') && ln[1] == ' ') { bullet = true; ln.erase(0, 2); }
        else if (ln[0] == '>') ln.erase(0, 1);
        if (ln == "---" || ln == "***" || ln == "___") continue;
        ln = trim(inline_md(ln));
        if (ln.empty()) continue;
        Font f = head ? F_Row : F_Body;
        Col c = head ? kText : kText2;
        float indent = bullet ? 20.f : 0.f;
        bool first = true;
        for (auto& w : wrap(f, ln, maxw - indent)) {
            out.push_back({w, f, c, indent, first ? (head && !out.empty() ? 12.f : pending_gap) : 0.f, bullet && first});
            first = false;
            pending_gap = 0;
        }
    }
    return g_notes[md] = std::move(out);
}

// ─── generated background + its blurred twin ───────────────────────────────

SDL_Surface* make_bg() {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, kW, kH, 32, SDL_PIXELFORMAT_ARGB8888);
    struct Orb { float x, y, r, cr, cg, cb, a; };
    const Orb orbs[] = {
        { 120,   40, 640,  60, 104, 255, 0.52f},
        {1190,  170, 560, 146,  80, 255, 0.40f},
        { 990,  860, 660,  24, 196, 182, 0.30f},
        { 250,  830, 470, 255,  74, 146, 0.20f},
    };
    uint32_t seed = 0x9E3779B9u;
    SDL_LockSurface(s);
    for (int y = 0; y < kH; y++) {
        Uint32* row = reinterpret_cast<Uint32*>(static_cast<Uint8*>(s->pixels) + y * s->pitch);
        float t = float(y) / kH;
        float br = 13 - 8 * t, bgc = 17 - 10 * t, bb = 42 - 26 * t;
        for (int x = 0; x < kW; x++) {
            float r = br, g = bgc, b = bb;
            for (const Orb& o : orbs) {
                float dx = x - o.x, dy = y - o.y;
                float d2 = (dx * dx + dy * dy) / (o.r * o.r);
                if (d2 >= 1) continue;
                float f = 1 - d2;
                f = f * f * f * o.a;
                r += o.cr * f; g += o.cg * f; b += o.cb * f;
            }
            seed = seed * 1664525u + 1013904223u;
            float n = float((seed >> 16) & 0xFF) / 255.f;   // dither against banding
            Uint32 R8 = Uint32(std::clamp(int(r + n), 0, 255));
            Uint32 G8 = Uint32(std::clamp(int(g + n), 0, 255));
            Uint32 B8 = Uint32(std::clamp(int(b + n), 0, 255));
            row[x] = 0xFF000000u | (R8 << 16) | (G8 << 8) | B8;
        }
    }
    SDL_UnlockSurface(s);
    return s;
}

SDL_Surface* make_frost(SDL_Surface* bg) {
    const int S = 4, w = kW / S, h = kH / S;
    std::vector<float> img(size_t(w) * h * 3, 0.f), tmp(img.size());
    SDL_LockSurface(bg);
    for (int y = 0; y < kH; y++) {
        const Uint32* row = reinterpret_cast<const Uint32*>(static_cast<Uint8*>(bg->pixels) + y * bg->pitch);
        for (int x = 0; x < kW; x++) {
            float* d = &img[(size_t(y / S) * w + x / S) * 3];
            d[0] += (row[x] >> 16) & 0xFF; d[1] += (row[x] >> 8) & 0xFF; d[2] += row[x] & 0xFF;
        }
    }
    SDL_UnlockSurface(bg);
    for (float& v : img) v /= float(S * S);

    const int rad = 5;
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 3; c++) {
                    float sum = 0;
                    for (int k = -rad; k <= rad; k++) sum += img[(size_t(y) * w + std::clamp(x + k, 0, w - 1)) * 3 + c];
                    tmp[(size_t(y) * w + x) * 3 + c] = sum / (2 * rad + 1);
                }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 3; c++) {
                    float sum = 0;
                    for (int k = -rad; k <= rad; k++) sum += tmp[(size_t(std::clamp(y + k, 0, h - 1)) * w + x) * 3 + c];
                    img[(size_t(y) * w + x) * 3 + c] = sum / (2 * rad + 1);
                }
    }

    // Upscale ourselves (bilinear) so every renderer samples it 1:1.
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, kW, kH, 32, SDL_PIXELFORMAT_ARGB8888);
    uint32_t seed = 0x2545F491u;
    SDL_LockSurface(s);
    for (int y = 0; y < kH; y++) {
        Uint32* row = reinterpret_cast<Uint32*>(static_cast<Uint8*>(s->pixels) + y * s->pitch);
        float fy = (y + 0.5f) / S - 0.5f;
        int y0 = std::clamp(int(std::floor(fy)), 0, h - 1), y1 = std::min(y0 + 1, h - 1);
        float ty = std::clamp(fy - y0, 0.f, 1.f);
        for (int x = 0; x < kW; x++) {
            float fx = (x + 0.5f) / S - 0.5f;
            int x0 = std::clamp(int(std::floor(fx)), 0, w - 1), x1 = std::min(x0 + 1, w - 1);
            float tx = std::clamp(fx - x0, 0.f, 1.f);
            seed = seed * 1664525u + 1013904223u;
            float n = float((seed >> 16) & 0xFF) / 255.f;
            Uint32 px = 0xFF000000u;
            for (int c = 0; c < 3; c++) {
                float a = img[(size_t(y0) * w + x0) * 3 + c], b = img[(size_t(y0) * w + x1) * 3 + c];
                float d = img[(size_t(y1) * w + x0) * 3 + c], e = img[(size_t(y1) * w + x1) * 3 + c];
                float v = (a + (b - a) * tx) * (1 - ty) + (d + (e - d) * tx) * ty;
                px |= Uint32(std::clamp(int(v + n), 0, 255)) << (16 - 8 * c);
            }
            row[x] = px;
        }
    }
    SDL_UnlockSurface(s);
    return s;
}

// ─── widgets ────────────────────────────────────────────────────────────────

std::string pct(double p) { return std::to_string(int(std::lround(std::clamp(p, 0.0, 1.0) * 100))) + "%"; }
std::string mb(long long b) {
    char s[32];
    std::snprintf(s, sizeof s, b >= 1024LL * 1024 * 1024 ? "%.2f GB" : "%.1f MB",
                  b >= 1024LL * 1024 * 1024 ? b / 1073741824.0 : b / 1048576.0);
    return s;
}

struct StInfo { const char* text; Col col; };
StInfo status_info(const Card& c) {
    switch (status_of(c)) {
        case Status::Checking:        return {"Checking", kText3};
        case Status::UpToDate:        return {"Up to date", kGreen};
        case Status::UpdateAvailable: return {"Update available", kAmber};
        case Status::NotInstalled:    return {"Not installed", kBlue};
        case Status::NoRelease:       return {"No release", kText3};
        case Status::Downloading:     return {"Downloading", kBlue};
        case Status::Installing:      return {"Installing", kBlue};
        case Status::RebootPending:   return {"Reboot to finish", kAmber};
        case Status::Installed:       return {"Installed", kGreen};
        case Status::Downloaded:      return {"Ready for Daybreak", kGreen};
        case Status::Error:           return {"Error", kRed};
    }
    return {"", kText3};
}

std::string row_sub(const Card& c) {
    const std::string latest = c.cands.empty() ? "" : c.cands[0].version;
    switch (status_of(c)) {
        case Status::Checking:        return "Checking for updates…";
        case Status::UpToDate:        return "Up to date · " + c.installed;
        case Status::UpdateAvailable: return c.installed + "  →  " + latest;
        case Status::NotInstalled:    return latest + " available";
        case Status::NoRelease:       return "No release found";
        case Status::Downloading:     return "Downloading · " + pct(c.progress);
        case Status::Installing:      return "Installing…";
        case Status::RebootPending:   return "Reboot to finish";
        case Status::Installed:       return "Installed " + c.installed;
        case Status::Downloaded:      return "Saved · install with Daybreak";
        case Status::Error:           return "Couldn't finish · Y to retry";
    }
    return {};
}

float pill(float xr, float cy, const std::string& label, Col dot) {
    float tw = text_w(F_Small, label), h = 32, w = 14 + 8 + 9 + tw + 16, x = xr - w;
    fill_rr(x, cy - h / 2, w, h, h / 2, A(dot, 40), A(dot, 26));
    stroke_rr(x, cy - h / 2, w, h, h / 2, 1, A(dot, 96), A(dot, 44));
    circle(x + 18, cy, 4, dot);
    text(F_Small, label, x + 31, cy, kText);
    return x;
}

void key_cap(float cx, float cy, const char* key, Col bg, Col fg) {
    circle(cx, cy, 12, bg);
    text(F_Key, key, cx, cy, fg, Center);
}

float button(float xr, float y, float h, const std::string& label, bool primary, const char* key) {
    float tw = text_w(F_Btn, label), kw = key ? 34.f : 0.f;
    float w = std::max(190.f, 30 + kw + tw + 32), x = xr - w;
    if (primary) {
        for (int i = 4; i >= 1; i--) {
            float s = i * 3.f;
            fill_rr(x - s, y - s + 7, w + 2 * s, h + 2 * s, h / 2 + s, A(kAccentA, 14), A(kAccentB, 14), true);
        }
        fill_rr(x, y, w, h, h / 2, kAccentA, kAccentB, true);
        fill_rr(x + 2, y + 2, w - 4, h * 0.46f, h * 0.23f, A(kWhite, 50), A(kWhite, 4));
        stroke_rr(x, y, w, h, h / 2, 1, A(kWhite, 110), A(kWhite, 30));
    } else {
        fill_rr(x, y, w, h, h / 2, A(kWhite, 34), A(kWhite, 16));
        stroke_rr(x, y, w, h, h / 2, 1, A(kWhite, 80), A(kWhite, 24));
    }
    float cx = x + (w - kw - tw) / 2;
    if (key) {
        key_cap(cx + 12, y + h / 2, key, A(kWhite, primary ? 240 : 215), primary ? Col{64, 74, 196, 255} : Col{24, 30, 56, 255});
        cx += kw;
    }
    text(F_Btn, label, cx, y + h / 2, kText);
    return x;
}

float hint(float x, float cy, const char* key, const char* label) {
    key_cap(x + 12, cy, key, A(kWhite, 40), kText);
    stroke_rr(x, cy - 12, 24, 24, 12, 1, A(kWhite, 70), A(kWhite, 30));
    return x + 34 + text(F_Small, label, x + 34, cy, kText2) + 26;
}

float dpad_hint(float x, float cy, const char* label) {
    Col c = A(kWhite, 150);
    fill_rr(x + 8.5f, cy - 11, 7, 22, 2, c, c);
    fill_rr(x + 1, cy - 3.5f, 22, 7, 2, c, c);
    return x + 34 + text(F_Small, label, x + 34, cy, kText2) + 26;
}

void icon(float x, float y, float s, Kind k, const char* glyph = nullptr) {
    Col a, b;
    const char* letter;
    switch (k) {
        case Kind::Atmosphere: a = {84, 128, 255, 255}; b = {160, 90, 255, 255}; letter = "A"; break;
        case Kind::Hekate:     a = {255, 150, 72, 255}; b = {255, 72, 128, 255}; letter = "H"; break;
        default:               a = {28, 200, 176, 255}; b = {54, 128, 240, 255}; letter = "F"; break;
    }
    shadow(x, y, s, s, s * 0.3f, 6, 14);
    fill_rr(x, y, s, s, s * 0.3f, a, b);
    fill_rr(x + 2, y + 2, s - 4, s * 0.45f, s * 0.22f, A(kWhite, 60), A(kWhite, 0));
    stroke_rr(x, y, s, s, s * 0.3f, 1, A(kWhite, 120), A(kWhite, 30));
    text(F_Icon, glyph ? glyph : letter, x + s / 2, y + s / 2 + 1, kWhite, Center);
}

// ─── layout ─────────────────────────────────────────────────────────────────

constexpr float SX = 40, SY = 104, SW = 356, SH = 548;     // sidebar
constexpr float PX = 420, PY = 104, PW = 820, PH = 548;    // detail pane
constexpr float RX = SX + 12, RW = SW - 24, RH = 88, ROW0 = SY + 58, PITCH = 96;
constexpr float X0 = PX + 40, XR = PX + PW - 40, CW = XR - X0;
constexpr float BTN_Y = PY + PH - 36 - 52;

void header(const View& v) {
    icon(40, 30, 48, Kind::Atmosphere, "↑");
    text(F_Title, "AtmoUpdate", 104, 44, kText);
    text(F_Small, "Atmosphère, hekate and firmware, straight from your Switch", 105, 70, kText3);
    std::string net = v.online ? "Online" : "Offline";
    pill(1240, 54, net, v.online ? kGreen : kRed);
}

void sidebar(const View& v) {
    glass(SX, SY, SW, SH, 28);
    text(F_Cap, "COMPONENTS", SX + 28, SY + 34, kText3);

    float target = ROW0 + v.sel * PITCH;
    if (s_sel_y < 0) s_sel_y = target;
    s_sel_y += (target - s_sel_y) * 0.3f;
    if (std::fabs(target - s_sel_y) < 0.5f) s_sel_y = target;
    if (v.zone == Zone::Sidebar && v.modal == Modal::None) {
        fill_rr(RX, s_sel_y, RW, RH, 22, Col{90, 136, 255, 62}, Col{150, 96, 255, 40});
        stroke_rr(RX, s_sel_y, RW, RH, 22, 1, Col{170, 196, 255, 150}, Col{170, 150, 255, 50});
    } else {
        fill_rr(RX, s_sel_y, RW, RH, 22, A(kWhite, 22), A(kWhite, 12));
    }

    for (int i = 0; i < int(v.cards.size()); i++) {
        const Card& c = v.cards[i];
        float y = ROW0 + i * PITCH;
        icon(RX + 16, y + 20, 48, c.kind);
        text(F_Item, c.title, RX + 80, y + 33, kText);
        text(F_Small, fit(F_Small, row_sub(c), RW - 80 - 40), RX + 80, y + 60, kText2);
        if (status_of(c) != Status::Checking) circle(RX + RW - 22, y + RH / 2, 4.5f, status_info(c).col);
    }

    fill_rr(SX + 24, SY + 372, SW - 48, 1, 0, A(kWhite, 26), A(kWhite, 26));
    text(F_Cap, "THIS CONSOLE", SX + 28, SY + 400, kText3);
    const char* keys[] = {"Firmware", "Atmosphère", "hekate"};
    const int idx[] = {2, 0, 1};
    for (int k = 0; k < 3; k++) {
        float cy = SY + 436 + k * 34;
        const std::string& val = v.cards[idx[k]].installed;
        text(F_Small, keys[k], SX + 28, cy, kText3);
        text(F_Small, val.empty() ? "—" : val, SX + SW - 28, cy, kText2, Right);
    }
}

void action_row(const View& v, const Card& c, int ci) {
    Status s = status_of(c);
    if (s == Status::Downloading || s == Status::Installing) {
        if (s_prog[ci] < 0) s_prog[ci] = float(c.progress);
        else s_prog[ci] += (float(c.progress) - s_prog[ci]) * 0.25f;
        std::string l = s == Status::Downloading ? "Downloading  " + pct(c.progress) : "Installing…";
        text(F_Row, l, X0, BTN_Y + 12, kText);
        if (s == Status::Downloading && c.dl_total > 0)
            text(F_Small, mb(c.dl_now) + " of " + mb(c.dl_total), XR, BTN_Y + 12, kText3, Right);
        fill_rr(X0, BTN_Y + 34, CW, 10, 5, A(kWhite, 30), A(kWhite, 18));
        float w = std::max(10.f, CW * std::clamp(s_prog[ci], 0.f, 1.f));
        int glow = s == Status::Installing ? int(190 + 65 * std::sin(SDL_GetTicks() / 260.0)) : 255;
        fill_rr(X0, BTN_Y + 34, w, 10, 5, A(kAccentA, glow), A(kAccentB, glow), true);
        return;
    }
    s_prog[ci] = 0;

    const std::string latest = c.cands.empty() ? "" : c.cands[0].version;
    std::string label;
    bool primary = true;
    if (s == Status::RebootPending) label = "Reboot to finish";
    else if (s == Status::Checking || s == Status::NoRelease || c.cands.empty()) label.clear();
    else if (c.kind == Kind::Firmware) {
        if (v.zone == Zone::List) label = "Download " + c.cands[std::clamp(v.fw_sel, 0, int(c.cands.size()) - 1)].version;
        else { label = "Choose a version"; primary = false; }
    } else if (s == Status::Error) label = "Try again";
    else if (s == Status::UpToDate || s == Status::Installed) { label = "Reinstall " + latest; primary = false; }
    else if (s == Status::NotInstalled) label = "Install " + latest;
    else label = "Update to " + latest;

    float bx = XR + 1;
    if (!label.empty()) bx = button(XR, BTN_Y, 52, label, primary, "A");

    std::string help;
    Col hc = kText3;
    if (!c.err.empty()) { help = c.err; hc = kRed; }
    else if (c.kind == Kind::Atmosphere) help = s == Status::RebootPending ? "New files are staged. A reboot swaps them in, then starts hekate."
                                                                           : "Installs to your SD card. A quick reboot finishes it.";
    else if (c.kind == Kind::Hekate) help = "Your hekate_ipl.ini is kept. Also set as the reboot payload.";
    else help = s == Status::Downloaded ? "Saved in /firmware. Open Daybreak to install it."
                                        : "Downloads to /firmware/<version> for Daybreak.";
    float maxw = bx - X0 - 24;
    auto lines = wrap(F_Small, help, maxw);
    if (lines.size() > 2) { lines.resize(2); lines[1] = fit(F_Small, lines[1] + " …", maxw); }
    float cy = BTN_Y + 26 - (lines.size() - 1) * 10.f;
    for (auto& l : lines) { text(F_Small, l, X0, cy, hc); cy += 20; }
}

void tile(float x, float y, float w, float h, const char* label, const std::string& value,
          const std::string& sub, bool hot) {
    if (hot) {
        fill_rr(x, y, w, h, 22, Col{92, 136, 255, 54}, Col{150, 96, 255, 30});
        stroke_rr(x, y, w, h, 22, 1, Col{170, 196, 255, 120}, Col{170, 150, 255, 40});
    } else {
        fill_rr(x, y, w, h, 22, Col{0, 0, 0, 56}, Col{0, 0, 0, 34});
        stroke_rr(x, y, w, h, 22, 1, A(kWhite, 30), A(kWhite, 10));
    }
    text(F_Cap, label, x + 24, y + 27, kText3);
    if (!sub.empty()) text(F_Small, sub, x + w - 24, y + 27, kText3, Right);
    text(F_Ver, value.empty() ? "—" : value, x + 22, y + h - 33, kText);
}

void detail_release(const View& v, const Card& c, int ci) {
    Status s = status_of(c);
    const Candidate* latest = c.cands.empty() ? nullptr : &c.cands[0];

    float ty = PY + 126, tw = (CW - 60) / 2, th = 92;
    tile(X0, ty, tw, th, "INSTALLED", c.installed, "", false);
    circle(X0 + tw + 30, ty + th / 2, 19, A(kWhite, 26));
    stroke_rr(X0 + tw + 11, ty + th / 2 - 19, 38, 38, 19, 1, A(kWhite, 60), A(kWhite, 20));
    text(F_Item, "→", X0 + tw + 30, ty + th / 2 - 1, kText, Center);
    tile(X0 + tw + 60, ty, tw, th, "LATEST", latest ? latest->version : "", latest ? latest->date : "",
         s == Status::UpdateAvailable || s == Status::NotInstalled);

    float ny = PY + 248;
    text(F_Cap, "WHAT'S NEW", X0, ny, kText3);
    float bx = X0, by = ny + 16, bw = CW, bh = BTN_Y - 22 - by;
    fill_rr(bx, by, bw, bh, 18, Col{0, 0, 0, 46}, Col{0, 0, 0, 30});
    stroke_rr(bx, by, bw, bh, 18, 1, A(kWhite, 22), A(kWhite, 8));
    if (s == Status::Checking) { text(F_Body, "Loading release notes…", bx + 22, by + 30, kText3); }
    else if (!latest || trim(latest->notes).empty()) { text(F_Body, "No release notes.", bx + 22, by + 30, kText3); }
    else {
        const auto& lines = notes_layout(latest->notes, bw - 44);
        float y = by + 18, bottom = by + bh - 14;
        const float lh = 24;
        for (size_t i = 0; i < lines.size(); i++) {
            const NoteLine& l = lines[i];
            y += l.gap;
            if (y + lh > bottom) break;
            bool last_fit = (y + 2 * lh > bottom) && i + 1 < lines.size();
            float x = bx + 22 + l.indent;
            if (l.bullet) circle(bx + 22 + 6, y + lh / 2, 2.5f, kText3);
            text(l.f, last_fit ? fit(l.f, l.s + " …", bw - 44 - l.indent) : l.s, x, y + lh / 2, l.c);
            y += lh;
            if (last_fit) break;
        }
    }
    action_row(v, c, ci);
}

void detail_firmware(const View& v, const Card& c, int ci) {
    float top = PY + 128, bottom = BTN_Y - 28, rh = 40, pitch = 46;
    int vis = int((bottom - top + (pitch - rh)) / pitch);
    int n = int(c.cands.size());
    if (status_of(c) == Status::Checking && n == 0) {
        text(F_Body, "Loading versions…", X0, top + 20, kText3);
    } else if (n == 0) {
        text(F_Body, "No firmware releases found.", X0, top + 20, kText3);
    }
    int sel = std::clamp(v.fw_sel, 0, std::max(0, n - 1));
    int first = std::clamp(sel - (vis - 1) / 2, 0, std::max(0, n - vis));
    bool focus = v.zone == Zone::List && v.modal == Modal::None;
    for (int k = 0; k < vis && first + k < n; k++) {
        int i = first + k;
        const Candidate& cd = c.cands[i];
        float y = top + k * pitch;
        bool is_sel = i == sel;
        if (is_sel && focus) {
            fill_rr(X0, y, CW, rh, 14, Col{90, 136, 255, 66}, Col{150, 96, 255, 42});
            stroke_rr(X0, y, CW, rh, 14, 1, Col{170, 196, 255, 150}, Col{170, 150, 255, 50});
        } else {
            fill_rr(X0, y, CW, rh, 14, A(kWhite, is_sel ? 30 : 10), A(kWhite, is_sel ? 18 : 6));
        }
        float x = X0 + 20 + text(F_Row, cd.version, X0 + 20, y + rh / 2, is_sel ? kText : kText2) + 14;
        auto chip = [&](const char* t, Col col) {
            float w = text_w(F_Cap, t) + 20;
            fill_rr(x, y + rh / 2 - 11, w, 22, 11, A(col, 46), A(col, 30));
            text(F_Cap, t, x + 10, y + rh / 2, col);
            x += w + 8;
        };
        if (i == 0) chip("LATEST", kBlue);
        if (cd.version == c.installed) chip("INSTALLED", kGreen);
        float rx = XR - 20;
        if (cd.size > 0) rx -= text(F_Small, mb(cd.size), rx, y + rh / 2, kText3, Right) + 18;
        text(F_Small, cd.date, rx, y + rh / 2, kText3, Right);
    }
    if (n > vis) {
        float th = bottom - top, bar = std::max(30.f, th * vis / n);
        float by = top + (th - bar) * first / float(std::max(1, n - vis));
        fill_rr(XR + 16, top, 4, th, 2, A(kWhite, 16), A(kWhite, 16));
        fill_rr(XR + 16, by, 4, bar, 2, A(kWhite, 90), A(kWhite, 90));
    }
    action_row(v, c, ci);
}

void detail(const View& v) {
    glass(PX, PY, PW, PH, 28);
    const Card& c = v.cards[v.sel];
    text(F_Hero, c.title, X0, PY + 62, kText);
    std::string sub = c.kind == Kind::Firmware
        ? (c.installed.empty() ? std::string("THZoria/NX_Firmware") : "Running " + c.installed + "  ·  THZoria/NX_Firmware")
        : std::string("github.com/") + c.repo;
    text(F_Small, sub, X0 + 2, PY + 100, kText3);
    StInfo st = status_info(c);
    pill(XR, PY + 62, st.text, st.col);
    if (c.kind == Kind::Firmware) detail_firmware(v, c, v.sel);
    else detail_release(v, c, v.sel);
}

void footer(const View& v) {
    float x = 44, cy = 686;
    if (v.modal != Modal::None) {
        x = hint(x, cy, "A", "Confirm");
        hint(x, cy, "B", "Cancel");
    } else if (v.zone == Zone::List) {
        x = dpad_hint(x, cy, "Choose");
        x = hint(x, cy, "A", "Download");
        x = hint(x, cy, "B", "Back");
        x = hint(x, cy, "Y", "Refresh");
        hint(x, cy, "+", "Exit");
    } else {
        const Card& c = v.cards[v.sel];
        Status s = status_of(c);
        x = dpad_hint(x, cy, "Navigate");
        if (c.kind == Kind::Firmware) x = hint(x, cy, "A", "Choose");
        else if (s == Status::RebootPending) x = hint(x, cy, "A", "Reboot");
        else if (s != Status::Checking && s != Status::Downloading && s != Status::Installing && !c.cands.empty())
            x = hint(x, cy, "A", "Update");
        x = hint(x, cy, "Y", "Refresh");
        hint(x, cy, "+", "Exit");
    }
    text(F_Small, "v1.1", 1240, cy, kText3, Right);
}

void modal(const View& v) {
    fill_rr(0, 0, kW, kH, 0, Col{3, 5, 14, 150}, Col{3, 5, 14, 170});
    const Card& c = v.cards[v.sel];
    int ti = c.kind == Kind::Firmware ? std::clamp(v.fw_sel, 0, std::max(0, int(c.cands.size()) - 1)) : 0;
    const Candidate* cd = c.cands.empty() ? nullptr : &c.cands[ti];
    const std::string ver = cd ? cd->version : "";
    bool fresh = c.installed.empty();

    std::string title, line, body, ok;
    if (v.modal == Modal::Reboot) {
        title = "Reboot to finish?";
        body = "Your Switch restarts, swaps the new Atmosphère files into place, then boots hekate. It only takes a few seconds.";
        ok = "Reboot";
    } else if (c.kind == Kind::Firmware) {
        title = "Download firmware " + ver + "?";
        line = cd && cd->size > 0 ? mb(cd->size) + " download" : "";
        body = "It's saved to /firmware/" + ver + ". Open Daybreak afterwards to install it.";
        ok = "Download";
    } else {
        title = std::string(fresh ? "Install " : "Update ") + c.title + "?";
        line = fresh ? ver : c.installed + "   →   " + ver;
        body = c.kind == Kind::Atmosphere
            ? "Files install to your SD card now. A quick reboot afterwards finishes the update."
            : "hekate is replaced and set as your reboot payload. Your hekate_ipl.ini is kept.";
        ok = fresh ? "Install" : "Update";
    }

    float w = 580, h = 300, x = (kW - w) / 2, y = (kH - h) / 2 - 10;
    glass(x, y, w, h, 30, 44, 22);
    text(F_H2, title, x + 36, y + 52, kText);
    float by = y + 86;
    if (!line.empty()) { text(F_Item, line, x + 36, by + 8, kBlue); by += 38; }
    for (auto& l : wrap(F_Body, body, w - 72)) { text(F_Body, l, x + 36, by + 10, kText2); by += 25; }
    float bx = button(x + w - 36, y + h - 36 - 52, 52, ok, true, "A");
    button(bx - 12, y + h - 36 - 52, 52, "Cancel", false, "B");
}

}  // namespace

bool init(SDL_Renderer* r, const std::string& font_dir) {
    R = r;
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < F_N; i++) {
        g_font[i] = TTF_OpenFont((font_dir + kFonts[i].file).c_str(), kFonts[i].px);
        if (!g_font[i]) return false;
        TTF_SetFontHinting(g_font[i], TTF_HINTING_LIGHT);
    }
    SDL_Surface* bg = make_bg();
    SDL_Surface* fr = make_frost(bg);
    g_bg = SDL_CreateTextureFromSurface(R, bg);
    g_frost = SDL_CreateTextureFromSurface(R, fr);
    SDL_FreeSurface(bg);
    SDL_FreeSurface(fr);
    return g_bg && g_frost;
}

void shutdown() {
    for (auto& e : g_cache) SDL_DestroyTexture(e.second.t);
    g_cache.clear();
    for (auto& f : g_font) if (f) { TTF_CloseFont(f); f = nullptr; }
    if (g_bg) SDL_DestroyTexture(g_bg);
    if (g_frost) SDL_DestroyTexture(g_frost);
    g_bg = g_frost = nullptr;
}

void reset_anim() {
    s_sel_y = -1;
    for (float& p : s_prog) p = -1;
}

void draw(const View& v) {
    g_frame++;
    SDL_RenderCopy(R, g_bg, nullptr, nullptr);
    header(v);
    sidebar(v);
    detail(v);
    footer(v);
    if (v.modal != Modal::None) modal(v);
}

}  // namespace ui
