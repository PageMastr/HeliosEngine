#include <nats/nats.h>

// Members are not constants. d.name = "Cell" must not resolve kv_names.cpp's `name` parameters (lines 10
// and 16 still fail closed), and o.bucket is unresolved although a constant `bucket` exists.
namespace defaults {
constexpr const char* bucket = "DIRECTORY";
}
struct Desc { const char* name; const char* bucket; };
void init(Desc& d) { d.name = "Cell"; }
natsStatus bindDesc(jsCtx* js, kvStore** kv, const Desc& o) { return js_KeyValue(kv, js, o.bucket); }
