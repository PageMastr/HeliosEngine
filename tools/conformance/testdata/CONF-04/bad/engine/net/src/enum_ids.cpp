// Enumerators are constants too, also one defined through another.
enum Bits { kOffBits = 17, kPreShift = kOffBits + 5 };
unsigned long long a(unsigned long long p) { return p << kPreShift; }
