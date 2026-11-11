// Temporary issue-209 benchmark: collect one UpdatedFragmentOffsets entry.

#include "lance/lance.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            std::fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__,         \
                         __LINE__, #condition);                                  \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

namespace bench_alloc {

std::atomic<uint64_t> allocation_calls{0};
std::atomic<uint64_t> allocation_bytes{0};
thread_local bool tracking = false;

void begin() {
    allocation_calls.store(0, std::memory_order_relaxed);
    allocation_bytes.store(0, std::memory_order_relaxed);
    tracking = true;
}

void end() { tracking = false; }

void record(std::size_t size) {
    if (!tracking) return;
    allocation_calls.fetch_add(1, std::memory_order_relaxed);
    allocation_bytes.fetch_add(size, std::memory_order_relaxed);
}

}  // namespace bench_alloc

void* operator new(std::size_t size) {
    void* allocation = std::malloc(size == 0 ? 1 : size);
    if (allocation == nullptr) throw std::bad_alloc();
    bench_alloc::record(size);
    return allocation;
}

void* operator new[](std::size_t size) {
    void* allocation = std::malloc(size == 0 ? 1 : size);
    if (allocation == nullptr) throw std::bad_alloc();
    bench_alloc::record(size);
    return allocation;
}

void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept {
    std::free(allocation);
}
void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept {
    std::free(allocation);
}

namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t offsets_hash(const std::vector<uint32_t>& offsets) {
    uint64_t hash = kFnvOffset;
    for (uint32_t offset : offsets) {
        hash ^= offset;
        hash *= kFnvPrime;
    }
    return hash;
}

void validate_offsets(const std::vector<uint32_t>& offsets,
                      std::size_t expected_count) {
    CHECK(offsets.size() == expected_count);
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        CHECK(offsets[index] == index);
    }
}

std::size_t parse_count(const char* raw) {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(raw, &end, 10);
    CHECK(end != raw && *end == '\0');
    CHECK(value > 0);
    CHECK(value <= std::numeric_limits<std::size_t>::max());
    return static_cast<std::size_t>(value);
}

std::size_t parse_sample(const char* raw) {
    const std::size_t value = parse_count(raw);
    CHECK(value <= std::numeric_limits<unsigned int>::max());
    return value;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4) {
            std::fprintf(stderr,
                         "usage: %s old|new OFFSET_COUNT SAMPLE\n", argv[0]);
            return 2;
        }
        const std::string workflow = argv[1];
        CHECK(workflow == "old" || workflow == "new");
        const std::size_t count = parse_count(argv[2]);
        const std::size_t sample = parse_sample(argv[3]);

        std::vector<uint32_t> values(count);
        std::iota(values.begin(), values.end(), uint32_t{0});
        lance::UpdatedFragmentOffsets offsets(
            std::vector<std::pair<uint64_t, std::vector<uint32_t>>>{
                {0, values}});
        CHECK(offsets.size() == 1);
        CHECK(offsets.fragment_id_at(0) == 0);
        CHECK(offsets.offset_count_at(0) == count);
        {
            auto warmup = offsets.offsets_at(0);
            validate_offsets(warmup, count);
        }

        bench_alloc::begin();
        const auto start = std::chrono::steady_clock::now();
        auto offsets_out = offsets.offsets_at(0);
        const auto stop = std::chrono::steady_clock::now();
        const uint64_t allocation_calls =
            bench_alloc::allocation_calls.load(std::memory_order_relaxed);
        const uint64_t allocation_bytes =
            bench_alloc::allocation_bytes.load(std::memory_order_relaxed);
        bench_alloc::end();

        validate_offsets(offsets_out, count);
        const auto nanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                .count();
        CHECK(nanos > 0);
        std::printf("%s,%zu,%zu,%llu,%llu,%llu,%zu,%016llx\n",
                    workflow.c_str(), count, sample,
                    static_cast<unsigned long long>(nanos),
                    static_cast<unsigned long long>(allocation_calls),
                    static_cast<unsigned long long>(allocation_bytes),
                    offsets_out.capacity(),
                    static_cast<unsigned long long>(offsets_hash(offsets_out)));
        return 0;
    } catch (const lance::Error& error) {
        std::fprintf(stderr, "lance::Error (%d): %s\n",
                     static_cast<int>(error.code), error.what());
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "std::exception: %s\n", error.what());
        return 1;
    }
}
