#include <string>

// A default argument is not a constant, even after a braced default closes a `{}` in the parameter list:
// kv_names.cpp's `name` parameters (lines 10 and 16) still fail closed.
struct LogOptions { int level = 0; };
void logTo(LogOptions opts = {}, const std::string& name = "cell", int depth = 0);
