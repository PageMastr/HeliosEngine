#include <nats/nats.h>

// The bucket comes from the caller, so the lint cannot tell that it is not lease state.
void configure(kvConfig* cfg, const char* name) {
    cfg->Bucket = name;
    cfg->MaxAge = 60000;
}
