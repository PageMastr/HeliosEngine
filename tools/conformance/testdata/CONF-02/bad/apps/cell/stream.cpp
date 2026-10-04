#include <nats.h>

// A lease bucket's KV_ stream configured by hand: its MaxAge is the lease expiry.
void leaseStream(jsStreamConfig* sc) {
    sc->Name = "KV_leases";
    sc->MaxAge = 3000;
}
// Per-key TTLs through the compare-and-set calls: on a leader key, and on a key the lint cannot resolve.
natsStatus renew(kvStore* kv, uint64_t* rev, const char* key) {
    kvStore_CreateWithTTL(rev, kv, "leader", "me", 2, 3000);
    return kvStore_UpdateStringWithTTL(rev, kv, key, "me", *rev, 3000);
}
