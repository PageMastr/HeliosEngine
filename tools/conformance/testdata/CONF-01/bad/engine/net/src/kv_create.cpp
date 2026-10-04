#include <nats/nats.h>

natsStatus create(jsCtx* js, kvStore** kv, kvConfig* cfg) { return js_CreateKeyValue(kv, js, cfg); }
