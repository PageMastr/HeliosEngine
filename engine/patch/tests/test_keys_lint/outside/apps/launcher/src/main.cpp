// Fixture of lint_patch_test_keys: a launcher that opts in to the test-only roots.
int main() {
    TrustOptions o;
    o.allowTestKeys = true;
    return 0;
}
