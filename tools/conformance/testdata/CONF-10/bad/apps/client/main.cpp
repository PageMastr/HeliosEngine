#include <SDL3/SDL.h>

int main() {
    // Looked up by name, which is still a call.
    void* fn = SDL_LoadFunction(nullptr, "SDL_CreateRenderer");
    return fn != nullptr;
}
