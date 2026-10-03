#include <nats/nats.h>

static constexpr const char* kLeaseBucket = "LEASES";
static constexpr const char* kLeaderKey = "orch.leader";

void bindAll(jsCtx* js, kvStore** kv, kvStore* store, uint64_t* rev, const char* name) {
    js_KeyValue(kv, js, kLeaseBucket);
    js_KeyValue(kv, js,
                "leases");
    js_KeyValue(kv, js, name);
    kvStore_Create(rev, store, kLeaderKey, "me", 2);
    const char* subject = "$KV.LEASES.zone.7";
}

// A bucket set from a parameter, and a create whose kvConfig is filled elsewhere: both fail closed.
void configure(kvConfig* cfg, const char* name) { cfg->Bucket = name; }
