# Raw File Workflow Validation

This branch records validation evidence for the raw file and shared storage
workflow implemented on `xuanwo/raw-file-storage-workflow`.

## Identities

- Product head: `02e7942b56e4e9f34766f283764669b830783515`
- Product tree: `e17f6a7b594e556c680c72b1edbe04ee78d963b4`
- Required dependency base: `9e344b3cd8b1d7770ee4df74dc19fab10f8c1fae`
- Validation host: Nier, Linux x86_64

## Commands

All commands used Rust 1.94.0 with `RUSTUP_TOOLCHAIN=1.94.0`,
`CARGO_BUILD_JOBS=8`, and the repository's normal Cargo configuration.

```text
cargo +1.94.0 fmt --all -- --check
cargo +1.94.0 check --all-targets --locked
cargo +1.94.0 clippy --all-targets --locked -- -D warnings
cargo +1.94.0 test --locked -- --test-threads=4
cargo +1.94.0 test --test compile_and_run_test --locked -- --ignored --test-threads=1
```

The raw file suite was also run directly:

```text
cargo +1.94.0 test --test file_api_test --locked -- --test-threads=4
```

## Results

- Formatting: passed.
- `check --all-targets --locked`: passed with zero compiler errors.
- `clippy --all-targets --locked -- -D warnings`: passed with zero diagnostics.
- Raw file workflow suite: 16 passed, 0 failed.
- Full Rust suite: 659 passed, 0 failed, 6 ignored.
- Ignored native consumers: 6 passed, 0 failed.
- Total Rust test result count: 665 passed, including the ignored native
  consumers.

The native-consumer suite includes the C11 and C++17 raw file consumers,
in addition to the existing C, C++, typed FTS, transaction, and static OSS
consumers.

## Coverage

The acceptance test covers file versions 2.0, 2.1, 2.2, and 2.3, declared,
lazy, and empty writers, schema metadata and global buffers, nested,
dictionary, extension, nullable, and binary fields, metadata and page buffer
descriptors, row count and projection errors, full/range/index streams, and
writer finish, abort, repeated-call, and failure behavior.

The raw Lance v1 rejection test uses an actual v1 file from the upstream Lance
`test_data/v0.7.5/with_deletions` fixture set. The checked-in fixture has
SHA-256
`316a91311965be9f14b1be359cbf94031c99f47ba3f225e2df44c97560c12da2`.
Zero-column writer behavior, including the native encoder panic with an
explicit data cache, is compared with a direct native oracle.

## Boundaries

- Validation ran only on Linux x86_64.
- No real cloud credentials were used.
- No cross-platform or performance experiments were run.
- Native S3 provider retention after the C handles close is asserted against
  the current native behavior; the binding does not force early provider
  release while a native object store still owns it.
- Native validation boundaries are compared with direct native oracles. The
  binding does not fabricate workarounds.
