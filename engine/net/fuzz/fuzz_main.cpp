// Standalone driver for engine/net fuzz targets when not built with -fsanitize=fuzzer.
//
//   net_fuzz_<target> [--smoke N] [--seed S] [--make-seeds DIR] [file|dir ...]
//
// Replays every file given (directories: every regular file inside, sorted), or the target's
// built-in seeds when none are given, then runs N deterministic mutations (bit flips, byte
// overwrites, insertions, deletions, splices) of those inputs. --make-seeds writes the built-in
// seeds to DIR (how fuzz/corpus/<target>/ was produced). Crashes and sanitizer reports abort the
// process, which fails the CTest. The same entry point links into libFuzzer for real campaigns
// (HELIOS_NET_LIBFUZZER=ON; 1 h nightly per target, 04 §9).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "fuzz_common.h"
#include "helios/core/log.h"
#include "helios/core/random.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

std::vector<uint8_t> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void mutate(helios::Xoshiro256& rng, std::vector<uint8_t>& d, const std::vector<std::vector<uint8_t>>& pool) {
    const int ops = 1 + static_cast<int>(rng.nextU32() % 4);
    for (int k = 0; k < ops; ++k) {
        const uint32_t op = rng.nextU32() % 6;
        const size_t n = d.size();
        switch (op) {
        case 0: // flip a bit
            if (n) d[rng.nextU32() % n] ^= static_cast<uint8_t>(1u << (rng.nextU32() % 8));
            break;
        case 1: // overwrite with an interesting byte
            if (n) {
                static const uint8_t kInteresting[] = {0x00, 0x01, 0x0F, 0x10, 0x1F, 0x20, 0x3F, 0x7F, 0x80, 0xFE, 0xFF};
                d[rng.nextU32() % n] = kInteresting[rng.nextU32() % sizeof(kInteresting)];
            }
            break;
        case 2: // insert random bytes
            {
                const size_t at = n ? rng.nextU32() % (n + 1) : 0;
                const size_t len = 1 + rng.nextU32() % 16;
                std::vector<uint8_t> ins(len);
                for (auto& b : ins) b = static_cast<uint8_t>(rng.nextU32());
                d.insert(d.begin() + static_cast<std::ptrdiff_t>(at), ins.begin(), ins.end());
            }
            break;
        case 3: // delete a range
            if (n) {
                const size_t at = rng.nextU32() % n;
                const size_t len = std::min<size_t>(n - at, 1 + rng.nextU32() % 32);
                d.erase(d.begin() + static_cast<std::ptrdiff_t>(at), d.begin() + static_cast<std::ptrdiff_t>(at + len));
            }
            break;
        case 4: // splice with another input
            if (!pool.empty()) {
                const auto& other = pool[rng.nextU32() % pool.size()];
                if (!other.empty()) {
                    const size_t from = rng.nextU32() % other.size();
                    const size_t len = std::min<size_t>(other.size() - from, 1 + rng.nextU32() % 64);
                    const size_t at = n ? rng.nextU32() % (n + 1) : 0;
                    d.insert(d.begin() + static_cast<std::ptrdiff_t>(at), other.begin() + static_cast<std::ptrdiff_t>(from),
                             other.begin() + static_cast<std::ptrdiff_t>(from + len));
                }
            }
            break;
        default: // truncate
            if (n) d.resize(rng.nextU32() % n);
            break;
        }
    }
    if (d.size() > 64 * 1024) d.resize(64 * 1024);
}

} // namespace

int main(int argc, char** argv) {
    long long smoke = 0;
    uint64_t seed = 0x5EEDF00Dull;
    std::string makeSeeds;
    std::vector<std::filesystem::path> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--smoke" && i + 1 < argc) smoke = std::stoll(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
        else if (a == "--make-seeds" && i + 1 < argc) makeSeeds = argv[++i];
        else paths.emplace_back(a);
    }
    std::vector<std::vector<uint8_t>> inputs;
    if (!makeSeeds.empty()) {
        heliosFuzzSeeds(inputs);
        std::filesystem::create_directories(makeSeeds);
        for (size_t i = 0; i < inputs.size(); ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "seed_%03zu.bin", i);
            std::ofstream out(std::filesystem::path(makeSeeds) / name, std::ios::binary);
            out.write(reinterpret_cast<const char*>(inputs[i].data()), static_cast<std::streamsize>(inputs[i].size()));
        }
        HELIOS_LOG_INFO("wrote {} seeds to {}", inputs.size(), makeSeeds);
        return 0;
    }
    for (const auto& p : paths) {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            std::vector<std::filesystem::path> files;
            for (const auto& e : std::filesystem::directory_iterator(p))
                if (e.is_regular_file()) files.push_back(e.path());
            std::sort(files.begin(), files.end());
            for (const auto& f : files) inputs.push_back(readFile(f));
        } else if (std::filesystem::is_regular_file(p, ec)) {
            inputs.push_back(readFile(p));
        }
    }
    if (inputs.empty()) heliosFuzzSeeds(inputs);
    for (const auto& in : inputs) LLVMFuzzerTestOneInput(in.data(), in.size());
    helios::Xoshiro256 rng(seed);
    std::vector<uint8_t> scratch;
    for (long long i = 0; i < smoke; ++i) {
        scratch = inputs.empty() ? std::vector<uint8_t>{} : inputs[rng.nextU32() % inputs.size()];
        mutate(rng, scratch, inputs);
        LLVMFuzzerTestOneInput(scratch.data(), scratch.size());
    }
    HELIOS_LOG_INFO("replayed {} inputs and {} mutations without a crash", inputs.size(), smoke);
    return 0;
}
