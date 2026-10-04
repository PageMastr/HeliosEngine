// A parenthesized shift amount is evaluated whole: (17 + 5) is 22.
unsigned long long blockId(unsigned long long prefix, unsigned long long shard, unsigned long long off) {
    return (prefix << (17 + 5)) | (shard << 17) | off;
}
