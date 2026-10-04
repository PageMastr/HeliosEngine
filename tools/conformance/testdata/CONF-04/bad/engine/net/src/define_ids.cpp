// Shift amounts through a #define and a static const: the Snowflake layout.
#define TIME_SHIFT 22
static const int kSeqShift = 12;
unsigned long long snow(unsigned long long ms, unsigned long long s) { return (ms << TIME_SHIFT) | (s << kSeqShift); }
