#include <SDL3/SDL.h>

// Groups that are never compiled cannot create a renderer.
#if 0 // kept for reference
SDL_Renderer* old(SDL_Window* w) { return SDL_CreateRenderer(w, nullptr); }
#endif

#if 1
int kRhiOnly = 1;
#elif defined(HELIOS_SDL_OVERLAY)
SDL_Renderer* overlay(SDL_Window* w) { return SDL_CreateRenderer(w, nullptr); }
#else
SDL_Renderer* overlay(SDL_Window* w) { return SDL_CreateRenderer(w, nullptr); }
#endif

#ifdef HELIOS_TOOLS
#  if (0)
SDL_Renderer* nested(SDL_Window* w) { return SDL_CreateRenderer(w, nullptr); }
#  endif
#endif
