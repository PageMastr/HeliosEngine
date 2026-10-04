// A time-prefixed composition through a name that aa_bits.h also defines (as 6).
constexpr unsigned kPrefixShift = 22;
unsigned long long zoneId(unsigned long long prefix, unsigned long long off) { return (prefix << kPrefixShift) | off; }
