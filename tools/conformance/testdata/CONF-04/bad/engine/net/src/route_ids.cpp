// A shift amount that sums parenthesized constants.
constexpr unsigned kOffBits = 17;
constexpr unsigned kShBits = 5;
constexpr unsigned kRouteShift = (kOffBits) + (kShBits);
unsigned long long routeId(unsigned long long prefix, unsigned long long off) { return (prefix << kRouteShift) | off; }
