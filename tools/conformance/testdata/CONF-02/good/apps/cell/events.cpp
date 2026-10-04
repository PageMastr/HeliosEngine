#include <nats.h>

// An ordinary stream's MaxAge is retention, not lease expiry, also in a nats.c file that sets no Bucket,
// and a per-key TTL through CAS on a zone key is not leadership.
void eventsStream(jsStreamConfig& sc) {
    sc.Name = "EVENTS";
    sc.MaxAge = 86400000000000;
}
natsStatus mark(kvStore* kv, uint64_t* rev) { return kvStore_CreateWithTTL(rev, kv, "zone.7", "x", 1, 60000); }
