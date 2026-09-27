#include <SDL3/SDL.h>

SDL_Renderer* launcherRenderer(SDL_Window* w) { return SDL_CreateRenderer(w, nullptr); }
