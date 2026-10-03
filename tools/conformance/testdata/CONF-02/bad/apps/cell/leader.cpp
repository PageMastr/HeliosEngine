#include <nats/nats.h>
#include <string>

#define LEADER_KEY "orch." "leader"
static const std::string kZoneKey = "zone.42";

natsStatus elect(kvStore* kv, uint64_t* rev, const char* key) {
    kvStore_Create(rev, kv, LEADER_KEY, "me", 2);
    kvStore_UpdateString(rev, kv,
                         kZoneKey.c_str(), "me", *rev);
    return kvStore_Update(rev, kv, key, "me", 2, *rev);
}
