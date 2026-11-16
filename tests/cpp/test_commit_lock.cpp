// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

// C++17 consumer for the native commit-lock bridge. argv[1] is an existing
// dataset used as the Arrow stream source, argv[2] is a scratch directory.

#include "lance/lance.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

#define ASSERT(condition, message)                                            \
    do {                                                                      \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "ASSERT failed: %s: %s:%d\n", (message),      \
                         __FILE__, __LINE__);                                  \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

struct LockStats {
    int acquire = 0;
    int release = 0;
    int release_true = 0;
    int release_false = 0;
    int destroy = 0;
    int conflict_once = 0;
    int release_fail_once = 0;
    std::string uri;
};

class TestLease : public lance::CommitLease {
    LockStats* stats_;
    bool released_ = false;

public:
    explicit TestLease(LockStats* stats) : stats_(stats) {}

    void release(bool success) override {
        stats_->release++;
        if (success) {
            stats_->release_true++;
        } else {
            stats_->release_false++;
        }
        released_ = true;
        if (stats_->release_fail_once) {
            stats_->release_fail_once = 0;
            throw lance::Error(LANCE_ERR_INVALID_ARGUMENT,
                               "simulated cpp release failure");
        }
    }

    ~TestLease() noexcept override {
        ASSERT(released_, "lease destroyed without a release callback");
        stats_->destroy++;
    }
};

std::string source_uri;

ArrowArrayStream make_stream() {
    lance::Dataset source = lance::Dataset::open(source_uri);
    lance::Scanner scanner = source.scan();
    ArrowArrayStream stream = {};
    scanner.to_arrow_stream(&stream);
    return stream;
}

lance::Dataset write_locked(const std::string& uri, lance::WriteMode mode,
                            const lance::WriteOptions& options,
                            const lance::CommitLock* lock) {
    ArrowArrayStream stream = make_stream();
    return lance::Dataset::write_with_options_with_commit_lock(
        uri, &stream, mode, options, nullptr, nullptr, lock);
}

lance::CommitLock make_lock(LockStats* stats) {
    return lance::CommitLock([stats](uint64_t version) {
        stats->acquire++;
        ASSERT(version > 0, "C++ acquire observes a native base version");
        if (stats->conflict_once) {
            stats->conflict_once = 0;
            throw lance::Error(LANCE_ERR_COMMIT_CONFLICT,
                               "simulated cpp lock conflict");
        }
        return std::unique_ptr<lance::CommitLease>(new TestLease(stats));
    });
}

std::string scratch_path(const char* base, const char* name) {
    return std::string(base) + "/" + name;
}

void test_create_and_retained_owner(const char* base) {
    const std::string uri = scratch_path(base, "cpp_create");
    LockStats stats;
    stats.uri = uri;
    {
        lance::CommitLock lock = make_lock(&stats);
        lance::WriteOptions options;
        options.params.max_rows_per_file = 3;
        lance::Dataset dataset =
            write_locked(uri, lance::WriteMode::Create, options, &lock);
        ASSERT(stats.acquire == 1, "one C++ acquire");
        ASSERT(stats.release_true == 1, "one C++ release true");
        ASSERT(stats.destroy == 1, "one C++ lease destroyed exactly once");
        ASSERT(dataset.count_rows() == 20, "C++ create rows");
        ASSERT(dataset.fragment_count() == 7, "C++ create fragments");
        uint64_t version = dataset.version();

        // The lock object dies before the dataset; the dataset retains the
        // native handler owner and keeps committing.
        {
            lance::CommitLock moved = std::move(lock);
            (void)moved;
        }
        dataset.update_metadata_json({{"cpp_lock", "after_close"}});
        ASSERT(dataset.version() == version + 1,
               "C++ retained handler commits after lock close");
        ASSERT(dataset.count_rows() == 20, "C++ rows after mutation");
    }

    lance::Dataset reopened = lance::Dataset::open(uri);
    ASSERT(reopened.count_rows() == 20, "C++ reopened rows");
    ASSERT(reopened.fragment_count() == 7, "C++ reopened fragments");
}

void test_open_mutate_after_lock_close(const char* base) {
    const std::string uri = scratch_path(base, "cpp_open");
    {
        LockStats create_stats;
        create_stats.uri = uri;
        lance::CommitLock create_lock = make_lock(&create_stats);
        lance::Dataset created = write_locked(uri, lance::WriteMode::Create,
                                              lance::WriteOptions{}, &create_lock);
        ASSERT(created.count_rows() == 20, "C++ open fixture rows");
    }

    LockStats stats;
    stats.uri = uri;
    lance::CommitLock lock = make_lock(&stats);
    lance::Dataset opened = lance::Dataset::open_with_commit_lock(
        uri, nullptr, 0, nullptr, &lock);
    ASSERT(opened.count_rows() == 20, "C++ open rows");
    ASSERT(stats.acquire == 0, "C++ open does not acquire a lease");

    // Drop the only C handle; the dataset retains the native handler owner.
    {
        lance::CommitLock dropped(std::move(lock));
        (void)dropped;
    }
    uint64_t before = opened.version();
    opened.update_metadata_json({{"cpp_open", "mutated"}});
    ASSERT(opened.version() == before + 1, "C++ open mutation version");
    ASSERT(stats.acquire == 1, "C++ retained handler acquires after lock close");

    lance::Dataset reopened = lance::Dataset::open(uri);
    ASSERT(reopened.count_rows() == 20, "C++ open reopen rows");
}

void test_staged_and_batch(const char* base) {
    const std::string uri = scratch_path(base, "cpp_staged");
    LockStats stats;
    stats.uri = uri;
    lance::CommitLock lock = make_lock(&stats);
    {
        lance::Dataset created = write_locked(uri, lance::WriteMode::Create,
                                              lance::WriteOptions{}, &lock);
        (void)created;
    }

    auto stage = [&]() {
        ArrowArrayStream stream = make_stream();
        return lance::Dataset::write_uncommitted_with_commit_lock(
            uri, &stream, lance::WriteMode::Append,
            lance::UncommittedWriteOptions{}, nullptr, nullptr, &lock);
    };

    lance::Transaction first = stage();
    lance::Dataset single = lance::Dataset::commit_with_commit_lock(
        uri, first, lance::CommitOptions{}, nullptr, nullptr, &lock);
    ASSERT(single.count_rows() == 40, "C++ single staged rows");
    uint64_t version = single.version();

    lance::Transaction second = stage();
    lance::Transaction third = stage();
    std::vector<std::reference_wrapper<const lance::Transaction>> batch = {
        std::cref(second), std::cref(third)};
    lance::BatchCommitResult merged = lance::Dataset::commit_batch_with_commit_lock(
        uri, batch, lance::CommitOptions{}, nullptr, nullptr, &lock);
    ASSERT(merged.dataset.count_rows() == 80, "C++ batch rows");
    ASSERT(merged.dataset.version() == version + 1, "C++ batch version");
    // The native merged transaction is the batch input and keeps the base
    // version it was staged from; the committed dataset advances by one.
    ASSERT(merged.transaction.read_version() == version,
           "C++ merged transaction keeps the batch base version");
    (void)first;
    (void)second;
    (void)third;
    (void)single;
}

void test_exception_mapping(const char* base) {
    const std::string uri = scratch_path(base, "cpp_errors");
    LockStats stats;
    stats.uri = uri;
    lance::CommitLock lock = make_lock(&stats);
    {
        lance::Dataset created = write_locked(uri, lance::WriteMode::Create,
                                              lance::WriteOptions{}, &lock);
        (void)created;
    }

    // A thrown conflict is retried by the native blanket handler.
    int acquire_before = stats.acquire;
    stats.conflict_once = 1;
    ArrowArrayStream stream = make_stream();
    lance::Transaction transaction =
        lance::Dataset::write_uncommitted_with_commit_lock(
            uri, &stream, lance::WriteMode::Append,
            lance::UncommittedWriteOptions{}, nullptr, nullptr, &lock);
    lance::Dataset committed = lance::Dataset::commit_with_commit_lock(
        uri, transaction, lance::CommitOptions{}, nullptr, nullptr, &lock);
    ASSERT(stats.acquire == acquire_before + 2, "C++ conflict retried");
    ASSERT(committed.count_rows() == 40, "C++ conflict rows");

    // A release error after publication surfaces as an exception, but the
    // published manifest is visible on reopen.
    stream = make_stream();
    lance::Transaction published =
        lance::Dataset::write_uncommitted_with_commit_lock(
            uri, &stream, lance::WriteMode::Append,
            lance::UncommittedWriteOptions{}, nullptr, nullptr, &lock);
    stats.release_fail_once = 1;
    bool failed = false;
    try {
        lance::Dataset ignored = lance::Dataset::commit_with_commit_lock(
            uri, published, lance::CommitOptions{}, nullptr, nullptr, &lock);
        (void)ignored;
    } catch (const lance::Error& error) {
        failed = true;
        ASSERT(error.code == LANCE_ERR_INVALID_ARGUMENT,
               "C++ release failure code");
        ASSERT(std::strstr(error.what(), "simulated cpp release failure") !=
                   nullptr,
               "C++ release failure message");
    }
    ASSERT(failed, "C++ release failure throws");

    lance::Dataset reopened = lance::Dataset::open(uri);
    ASSERT(reopened.count_rows() == 60,
           "C++ release failure still published the append");
}

void test_stateless_lock(const char* base) {
    const std::string uri = scratch_path(base, "cpp_stateless");
    int acquires = 0;
    lance::CommitLock lock([&acquires](uint64_t) {
        acquires++;
        return std::unique_ptr<lance::CommitLease>();
    });
    lance::Dataset dataset =
        write_locked(uri, lance::WriteMode::Create, lance::WriteOptions{}, &lock);
    ASSERT(dataset.count_rows() == 20, "C++ stateless rows");
    ASSERT(acquires == 1, "C++ stateless acquire counted");
}

}  // namespace

int main(int argc, char** argv) {
    ASSERT(argc == 3, "usage: test_commit_lock_cpp <source-uri> <scratch-dir>");
    source_uri = argv[1];
    const char* base = argv[2];

    test_stateless_lock(base);
    test_create_and_retained_owner(base);
    test_open_mutate_after_lock_close(base);
    test_staged_and_batch(base);
    test_exception_mapping(base);

    std::printf("test_commit_lock_cpp: all checks passed\n");
    return 0;
}
