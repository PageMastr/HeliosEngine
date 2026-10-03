// Not nats.c: another struct's TTL field.
struct Cache { int TTL; };
void expire(Cache& c) { c.TTL = 5; }
