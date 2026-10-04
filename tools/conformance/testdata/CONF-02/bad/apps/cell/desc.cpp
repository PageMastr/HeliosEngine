#include <nats/nats.h>

// Neither a member set elsewhere nor a pointer that can be re-pointed is a constant, so config.cpp's
// `name` and leader.cpp's `key` parameters stay unresolved (config.cpp:6 and leader.cpp:11 still fail).
struct Desc { const char* name; };
void init(Desc& d) { d.name = "Cell"; }
const char* key = "zone.1";
