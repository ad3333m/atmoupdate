#pragma once
#include "model.hpp"
#include <string>

struct SDL_Renderer;

// Platform-free renderer (SDL2 + SDL2_ttf). The Switch app and the desktop
// preview tool both draw through this, so CI screenshots match the console.
namespace ui {

constexpr int kW = 1280, kH = 720;

bool init(SDL_Renderer* r, const std::string& font_dir);
void shutdown();
void reset_anim();
void draw(const View& v);

}
