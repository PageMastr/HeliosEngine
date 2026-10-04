// A constexpr initialized with braces.
constexpr unsigned kBraced{22};
unsigned long long b(unsigned long long p) { return p << kBraced; }
