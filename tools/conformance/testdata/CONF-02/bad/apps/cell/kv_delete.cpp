#include <nats.h>

// Deleting a KV bucket is a KV call too: a stream name the lint cannot resolve may be its KV_ stream's.
void dropBucket(jsCtx* js, jsStreamConfig* sc, const char* name) {
    js_DeleteKeyValue(js, name);
    sc->Name = name;
    sc->MaxAge = 3000;
}
