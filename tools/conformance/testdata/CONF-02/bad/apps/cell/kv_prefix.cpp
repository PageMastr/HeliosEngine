#include <nats.h>
#include <cstdio>

// A stream name built from "KV_" and a bucket the lint cannot resolve.
void prefixed(jsStreamConfig* sc, char* buf, const char* bucket) {
    std::snprintf(buf, 64, "KV_%s", bucket);
    sc->Name = buf;
    sc->MaxAge = 3000;
}
