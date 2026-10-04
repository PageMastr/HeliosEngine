#include <nats/nats.h>

natsStatus openLeases(jsCtx* js, kvStore** kv) {
    kvConfig cfg;
    kvConfig_Init(&cfg);
    cfg.Bucket = "cell_leases";
    return js_CreateKeyValue(kv, js, &cfg);
}
natsStatus bindLeader(jsCtx* js, kvStore** kv) { return js_KeyValue(kv, js, "LEADER"); }
