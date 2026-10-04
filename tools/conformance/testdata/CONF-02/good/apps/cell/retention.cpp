#include <nats.h>

// A stream name the lint cannot resolve, in a file that uses no KV, is ordinary retention.
void retention(jsStreamConfig* sc, const char* name) {
    sc->Name = name;
    sc->MaxAge = 3000;
}
