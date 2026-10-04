#include <nats.h>

// A stream config whose Name this file does not set, in a file that uses JetStream KV: its MaxAge may be a
// lease bucket's expiry.
void retainKV(jsStreamConfig* sc, kvStore* kv) {
    sc->MaxAge = 3000;
    kvStore_PutString(nullptr, kv, "zone.1", "x");
}
