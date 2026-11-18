# Core 14 transaction fixture

`producer.c` writes the historical dataset used by
`tests/core14_transaction_fixture_test.rs`. It must be built against the last
`lance-c` source pinned to Lance 14.0.2:

- lance-c commit: `8cc29073329f5bef1e9ba121b385a4dd5f04e701`, the `origin/main`
  base of the core-15 adoption PR #224
- Lance core: `40f15ed1b71b66950961b175829fdf17a763f108`
- Rust: `1.94.0`, the pinned commit's `rust-version`

Run these commands from the repository root. Build the pinned library and
producer outside this checkout:

```bash
git worktree add /tmp/lance-c-core14 8cc29073329f5bef1e9ba121b385a4dd5f04e701
(cd /tmp/lance-c-core14 && cargo +1.94.0 build --release --locked)

cc -std=c11 -O2 tests/fixtures/core14_transaction_fixture/producer.c \
  -I /tmp/lance-c-core14/include \
  -L /tmp/lance-c-core14/target/release -llance_c \
  -Wl,-rpath,/tmp/lance-c-core14/target/release \
  -o /tmp/core14_transaction_producer

/tmp/core14_transaction_producer /tmp/core14-txn-fixture.lance
```

The producer prints the attached and detached versions as JSON. Use the
detached version in the ignored test:

```bash
LANCE_CORE14_FIXTURE_URI=/tmp/core14-txn-fixture.lance \
LANCE_CORE14_FIXTURE_DETACHED_VERSION=<detached version> \
cargo test --locked --test core14_transaction_fixture_test -- --ignored
```

The fixture is historical compatibility evidence, not part of normal CI. Keep
the producer here while this test remains maintained.
