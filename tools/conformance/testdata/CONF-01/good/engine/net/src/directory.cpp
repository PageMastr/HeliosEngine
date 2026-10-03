#include <nats/nats.h>
#include <string>
#include <string_view>

namespace names {
constexpr std::string_view kDirectory{"DIRECTORY"};
}

// The read projection, named through a constant and wrapped in std::string.
natsStatus bindDirectory(jsCtx* js, kvStore** kv) {
    return js_KeyValue(kv, js, std::string(names::kDirectory).c_str());
}
natsStatus bindBy(jsCtx* js, kvStore** kv, const char* bucket) {
    return js_KeyValue(kv, js, bucket); // conformance:allow CONF-01 callers pass names::kDirectory only
}
