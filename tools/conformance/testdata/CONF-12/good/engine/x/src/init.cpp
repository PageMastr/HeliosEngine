// Ordinary initializers are fine: constructor(101) and later, and a comment about .preinit_array.
__attribute__((constructor(101))) static void lateEnough() {}
__attribute__((constructor)) static void unprioritized() {}
