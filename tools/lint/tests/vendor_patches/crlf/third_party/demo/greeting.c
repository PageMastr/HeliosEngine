/* Fixture for tools/lint/vendor_patches.cmake: brackets, semicolons and backslashes. */
#define GREET(x) \
    do { puts(x); } while (0)

static const char* names[2] = {"a", "b"};

int greet(int i) {
    GREET(names[i]);
    return i + 1; /* patched */
}
