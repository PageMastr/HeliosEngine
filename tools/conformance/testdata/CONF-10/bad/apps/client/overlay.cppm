// A C++20 module unit is C++ too.
export module client.overlay;
import <SDL3/SDL.h>;
export SDL_Renderer* overlay(SDL_Window* w) { return SDL_CreateRenderer(w, nullptr); }
