#pragma section(".CRT$XLA1", long, read)
__attribute__((section(".preinit_array"), used)) static void (*early)(void) = nullptr;
__attribute__((constructor(50))) static void tooEarly() {}
struct Big { Big(); } big __attribute__((init_priority(90)));
