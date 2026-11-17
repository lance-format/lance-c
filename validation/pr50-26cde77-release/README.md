# PR #50 release-path acceptance (head 26cde77e)

Validation-only probe for `lance-format/lance-c` PR #50. It reproduces the
`.github/workflows/release.yml` Linux x86_64 leg inside the digest-pinned
manylinux image and then exercises the extracted archive with real datasets.

Nothing here is product code: the scripts only read the product tree and write
to an external evidence directory.

## What it checks

1. `Load baseline floors` -> `.github/baseline.env`.
2. `.github/actions/setup-protoc/install.sh` (checksum-verified protoc 25.3).
3. `manylinux-install-clang -v <pinned version>`.
4. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=lib`,
   `cmake --build build --parallel`, `cmake --install build --prefix stage`.
5. `strip-release-shared.sh stage Linux`, `verify-shared-exports.sh`,
   `verify-linux-staging.sh`, `verify-cmake-consumer.sh --from-install-prefix stage`.
6. `tar -C stage -cJf <archive> .` plus both checksum branches of the workflow.
7. Archive extraction, then `verify-cmake-consumer.sh --from-install-prefix
   <extracted>` after the original staging prefix is retired, plus explicit
   static/shared consumers with rpath disabled.
8. Physical layout, glibc floor, exported symbol count, path leakage audits.
9. C++17 vs C++20 effect of the installed `INTERFACE_COMPILE_FEATURES`.

## What it does not cover

- macOS and Windows release legs are not executed here; their hosted runs are
  accepted separately.
- aarch64 release execution: `inspect-arm64.sh` only inspects the pinned image
  (platform metadata, tooling, clang installer arm64 mapping, glibc floor)
  because no arm64 host or emulation is available on NieR.

## Run

```bash
SOURCE=<lance-c clone at 26cde77e> EVID_ROOT=<evidence dir> ./run.sh
EVID_ROOT=<same evidence dir> ./inspect-arm64.sh
```
