#include <functional>

// A default argument after a lambda default is not a constant: leader.cpp's `key` (line 11) still fails
// closed.
void elect(kvStore* kv, std::function<void()> done = [] {}, const char* const key = "zone.1", int n = 2);
