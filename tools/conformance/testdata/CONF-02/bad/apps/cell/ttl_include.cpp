#include <nats.h>
#include "lease_setup.h"

// nats.c's own include spelling (as in engine/server/src/nats_bus.cpp), and a TTL on a config whose
// type and bucket this file never names: the lint cannot resolve the bucket.
void shortenLease(LeaseSetup& s) { s.cfg.TTL = 3000; }
