#include <SDL3/SDL.h>

// A backslash-newline splice joins the name before the compiler sees it.
SDL_Renderer* spliced(SDL_Window* w) { return SDL_Create\
Renderer(w, nullptr); }

#if 0
SDL_Renderer* never(SDL_Window* w);
#else
SDL_Renderer* always(SDL_Window* w) { return SDL_CreateRenderer(w, nullptr); }
#endif

#ifdef HELIOS_DEBUG_OVERLAY
SDL_Renderer* debugOnly(SDL_Window* w) { return SDL_CreateRenderer(w, nullptr); }
#endif
