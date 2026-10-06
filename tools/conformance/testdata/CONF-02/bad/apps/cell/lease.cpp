#include <nats/nats.h>

void configure(kvConfig* cfg) {
    cfg->Bucket = "leases";
    cfg->TTL = 3000;
}
natsStatus takeLeader(kvStore* kv, uint64_t* rev) { return kvStore_Create(rev, kv, "leader", "me", 2); }
