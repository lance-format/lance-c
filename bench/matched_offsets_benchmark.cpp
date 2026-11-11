// Temporary benchmark source for the matched-offsets acceptance task.

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
#include <vector>

#define main lance_c_fragment_mutation_test_main
#include "../tests/cpp/test_fragment_mutation.cpp"
#undef main

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

constexpr std::size_t kSamples = 3;
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

void create_dataset(const std::string& uri) {
    constexpr std::size_t kRows = 32 * 1024;
    std::vector<int64_t> ids(kRows);
    std::iota(ids.begin(), ids.end(), int64_t{0});
    std::vector<int32_t> values(kRows, 100);

    TwoColumnStream stream("id", "l", "value", "i", std::move(ids),
                           std::move(values));
    lance::WriteParams params;
    params.max_rows_per_file = kRows;
    params.max_rows_per_group = kRows;
    params.enable_stable_row_ids = true;
    auto dataset =
        lance::Dataset::write(uri, stream.get(), lance::WriteMode::Create,
                              params);
    CHECK(stream.get()->release == nullptr);
    CHECK(dataset.count_rows() == kRows);
    CHECK(dataset.fragment_count() == 1);
    std::printf("created dataset uri=%s rows=%zu fragments=%zu\n",
                uri.c_str(), kRows, dataset.fragment_count());
}

void run_benchmark(const std::string& uri, std::size_t expected_count,
                   const std::string& workflow) {
    CHECK(workflow == "old" || workflow == "new");

    auto dataset = lance::Dataset::open(uri);
    CHECK(dataset.fragment_count() == 1);
    auto fragment = dataset.open_fragment(dataset.fragment_ids()[0]);

    std::vector<int64_t> ids(expected_count);
    std::iota(ids.begin(), ids.end(), int64_t{0});
    std::vector<int32_t> values(expected_count, 123);
    TwoColumnStream update_stream("id", "l", "value", "i", std::move(ids),
                                  std::move(values));
    auto result =
        fragment.update_columns(update_stream.get(), "id", "id");
    CHECK(update_stream.get()->release == nullptr);

    {
        auto warmup = result.matched_offsets();
        validate_offsets(warmup, expected_count);
    }

    for (std::size_t sample = 1; sample <= kSamples; ++sample) {
        bench_alloc::begin();
        const auto start = std::chrono::steady_clock::now();
        auto offsets = result.matched_offsets();
        const auto stop = std::chrono::steady_clock::now();
        const uint64_t allocations =
            bench_alloc::allocation_calls.load(std::memory_order_relaxed);
        const uint64_t allocated_bytes =
            bench_alloc::allocation_bytes.load(std::memory_order_relaxed);
        bench_alloc::end();

        validate_offsets(offsets, expected_count);
        const auto nanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                .count();
        CHECK(nanos > 0);
        std::printf("%s,%zu,%zu,%llu,%llu,%llu,%zu,%016llx\n",
                    workflow.c_str(), expected_count, sample,
                    static_cast<unsigned long long>(nanos),
                    static_cast<unsigned long long>(allocations),
                    static_cast<unsigned long long>(allocated_bytes),
                    offsets.capacity(),
                    static_cast<unsigned long long>(offsets_hash(offsets)));
        std::fflush(stdout);
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "create") {
            create_dataset(argv[2]);
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "run") {
            run_benchmark(argv[2], parse_count(argv[3]), argv[4]);
            return 0;
        }
        std::fprintf(stderr,
                     "usage: %s create DATASET_URI\n"
                     "       %s run DATASET_URI MATCHED_COUNT old|new\n",
                     argv[0], argv[0]);
        return 2;
    } catch (const lance::Error& error) {
        std::fprintf(stderr, "lance::Error (%d): %s\n",
                     static_cast<int>(error.code), error.what());
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "std::exception: %s\n", error.what());
        return 1;
    }
}
