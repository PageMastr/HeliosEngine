void* r = SDL_CreateRenderer(w, nullptr); // conformance:allow CONF-10
void* s = SDL_CreateRenderer(w, nullptr); // conformance:allow because I said so
void* t = SDL_CreateRenderer(w, nullptr); /* conformance:allow CONF-10 */
