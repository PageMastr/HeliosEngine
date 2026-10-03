#include <SDL3/SDL.h>

// A debug overlay that bypasses the RHI.
SDL_Renderer* makeOverlay(SDL_Window* window) {
    return SDL_CreateRenderer(window, nullptr);
}
SDL_Renderer* makeOverlay2(SDL_Window* w, SDL_PropertiesID props) { return SDL_CreateRendererWithProperties(props); }
