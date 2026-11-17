# PR #50 release-path validation at `26cde77`

This directory validates the release path that PR
[#50](https://github.com/lance-format/lance-c/pull/50) installs, at the exact
head `26cde77e8c1acc538ec1a2f3be26f8eeea74373a`. It does not modify the product
tree: every script runs against a read-only clone of that commit.

Scope is the integrated release workflow, not the Rust or C API test suites:
the hosted checks already cover formatting, clippy, the Rust tests and the
C/C++ compilation tests on Linux, macOS and both Windows architectures.

## What runs where

`run.sh` is the host driver. It starts one pristine container from
`quay.io/pypa/manylinux_2_28_x86_64@sha256:d086f64cddcace250ef0f0073583c51b8298159303290a730c9f59bd91edfd2c`
(the digest the workflow pins), mounts the product clone read-only at
`/source`, and executes `body.sh` inside it as uid 1000. The image is used
unmodified: no extra packages are installed, matching the release workflow's
decision to drop its `apt-get` step.

`body.sh` replays the workflow's build job in order and records every phase in
`commands-status.txt` with its exit code and wall time:

1. `setup-protoc` action (`install.sh`, protoc 25.3 from the pinned URL and
   checksum), then `manylinux-install-clang -v <pin resolved from release.yml>`,
2. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=lib`,
   `cmake --build build --parallel`, `cmake --install build --prefix stage`,
3. `strip-release-shared.sh stage Linux`, `verify-shared-exports.sh`,
   `verify-linux-staging.sh stage x86_64-unknown-linux-gnu`,
   `verify-cmake-consumer.sh --from-install-prefix stage`,
4. `tar -C stage -cJf lance-c-v0.1.9-x86_64-unknown-linux-gnu.tar.xz .` with
   `XZ_OPT=-T0` plus the release checksum step, whose `shasum` branch the
   workflow skips on this image because `shasum` is absent,
5. extract the archive, move the staging prefix away, then run
   `verify-cmake-consumer.sh --from-install-prefix <extracted>` so the real-data
   consumer can only resolve the extracted tree,
6. layout, ELF, path-string and identity audits.

`probe.sh` (driven by `run-probe.sh`, same image, extracted archive mounted as
the consumer prefix) answers three questions the build job does not:

- which C++ standard a downstream that pins `CMAKE_CXX_STANDARD=17` really gets,
- how a shared-linkage consumer finds `liblance_c.so` when RPATH is skipped,
- which build-path strings survive inside the released binaries.

`inspect-arm64.sh` inspects the pinned aarch64 image without executing it:
architecture and manifest metadata, tool presence, glibc, and whether the clang
installer supports arm64.

## Results

See `summary.env` and `supplement/probe.env` in the evidence root. The main
points:

- the workflow sequence passes end to end from the pristine image,
- install layout is `lib/` only: no `lib64`, no symlinks, no `lib/` rewriting,
- `liblance_c.so` exports exactly the header's public `lance_*` set, its
  `DT_NEEDED` list is the allowed glibc set, and both it and a static consumer
  peak at `GLIBC_2.28`,
- installed and archive-extracted static and shared consumers read the
  historical 512-row dataset and pass the acceptance consumer,
- a C++17-pinned downstream still configures and builds, but CMake raises the
  target to `-std=gnu++20` because the installed package exports
  `INTERFACE_COMPILE_FEATURES cxx_std_20`,
- the released binaries only embed cargo build-script paths.

## Harness caveats

- The first two attempts at the full run failed for harness reasons and are
  not part of the record: the clang pin was copied from an obfuscated rendering
  of the workflow, and a wholesale `PATH` override dropped the image's
  `/opt/clang/bin`. `commands-status.txt` is truncated per run, so only the
  accepted run is listed there.
- In the accepted run, `p_consumer_extracted_explicit` still passed `-DCMAKE_SKIP_RPATH=ON`
  and therefore could not start the shared-linkage consumer; the phase logged
  exit 127 and did not fail. That phase has been rewritten, and the
  authoritative extracted-prefix evidence for both linkages is
  `verify-cmake-consumer.sh --from-install-prefix` plus `probe.sh`.
- The aarch64 archive cannot be produced or consumed on this host: the machine
  has no arm64 emulation, and the hosted workflows build no Linux arm64
  artifact. `inspect-arm64.sh` therefore records image-level evidence only.

## Reproduce

```
SOURCE=<clone at 26cde77> EVID_ROOT=<evidence dir> bash run.sh
SOURCE=<clone at 26cde77> EVID_ROOT=<evidence dir> bash run-probe.sh
SOURCE=<clone at 26cde77> EVID_ROOT=<evidence dir> bash inspect-arm64.sh
```
