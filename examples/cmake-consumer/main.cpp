// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include <lance/lance.hpp>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: consumer <dataset_uri>\n");
        return 2;
    }
    try {
        auto ds = lance::Dataset::open(argv[1]);
        std::printf("rows: %llu, version: %llu\n",
            static_cast<unsigned long long>(ds.count_rows()),
            static_cast<unsigned long long>(ds.version()));
        return 0;
    } catch (const lance::Error& e) {
        std::fprintf(stderr, "lance error %d: %s\n",
            static_cast<int>(e.code), e.what());
        return 1;
    }
}
