#include <nats/nats.h>

// A TTL on a kvConfig whose Bucket this file does not set: the lint cannot resolve the bucket.
void setTtl(kvConfig* cfg) { cfg->TTL = 3000; }
