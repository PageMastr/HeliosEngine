// Standalone driver for the engine/asset fuzz target when not built with -fsanitize=fuzzer (the same
// driver as engine/net/fuzz's, with room for pak-sized inputs).
//
//   asset_fuzz_hpak_reader [--smoke N] [--seed S] [--make-seeds DIR] [file|dir ...]
//
// Replays every file given (directories: every regular file inside, sorted), or the built-in seeds
// when none are given, then runs N deterministic mutations (bit flips, byte overwrites, insertions,
// deletions, splices, truncation) of those inputs. --make-seeds writes the built-in seeds to DIR (how
// fuzz/corpus/hpak_reader/ was produced). Crashes and sanitizer reports abort the process, which fails
// the CTest. The same entry point links into libFuzzer for real campaigns (HELIOS_ASSET_LIBFUZZER=ON).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "helios/core/log.h"
#include "helios/core/random.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out);

namespace {

constexpr size_t kMaxInput = 1024 * 1024;

std::vector<uint8_t> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void mutate(helios::Xoshiro256& rng, std::vector<uint8_t>& d, const std::vector<std::vector<uint8_t>>& pool) {
    const int ops = 1 + static_cast<int>(rng.nextU32() % 4);
    for (int k = 0; k < ops; ++k) {
        const size_t n = d.size();
        switch (rng.nextU32() % 6) {
        case 0: // flip a bit; pak headers and TOCs are dense, so aim half the flips at the first 4 KiB
            if (n) d[(rng.nextU32() % 2 ? rng.nextU32() % std::min<size_t>(n, 4096) : rng.nextU32() % n)] ^=
                static_cast<uint8_t>(1u << (rng.nextU32() % 8));
            break;
        case 1: // overwrite with an interesting byte
            if (n) {
                static const uint8_t kInteresting[] = {0x00, 0x01, 0x02, 0x03, 0x0F, 0x10, 0x7F, 0x80, 0xFE, 0xFF};
                d[rng.nextU32() % n] = kInteresting[rng.nextU32() % sizeof(kInteresting)];
            }
            break;
        case 2: { // insert random bytes
            const size_t at = n ? rng.nextU32() % (n + 1) : 0;
            std::vector<uint8_t> ins(1 + rng.nextU32() % 16);
            for (auto& b : ins) b = static_cast<uint8_t>(rng.nextU32());
            d.insert(d.begin() + static_cast<std::ptrdiff_t>(at), ins.begin(), ins.end());
            break;
        }
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
    if (d.size() > kMaxInput) d.resize(kMaxInput);
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
        helios::log::setLevel(helios::log::Level::Info); // the target quiets logging while it runs
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
        } else {
            HELIOS_LOG_ERROR("no such corpus file or directory: {}", p.string());
            return 2;
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
    helios::log::setLevel(helios::log::Level::Info);
    HELIOS_LOG_INFO("replayed {} inputs and {} mutations without a crash", inputs.size(), smoke);
    return 0;
}
