#include <nats.h>

// A stream name the lint cannot resolve, in a file that uses JetStream KV: its MaxAge may be a lease
// bucket's expiry.
void kvStream(jsStreamConfig* sc, kvStore* kv, const char* name) {
    sc->Name = name;
    sc->MaxAge = 3000;
    kvStore_PutString(nullptr, kv, "zone.1", "x");
}
