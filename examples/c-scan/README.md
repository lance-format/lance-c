# C scan example

This is a runnable C consumer for an installed `lance-c` package. It opens the
dataset path, creates a scanner, exports an `ArrowArrayStream`, and reads every
batch. It prints the scanned row count and the dataset version.

The example shows the ownership and error rules a C caller needs:

- `get_next` returning nonzero is a stream error, reported by
  `stream.get_last_error`.
- A successful `get_next` whose batch has `release == NULL` is end of stream.
- Every returned batch is released with its `release` callback, and the stream,
  scanner and dataset are released on success and failure paths.
- Last-error strings are freed with `lance_free_string`.

## Build and run

Build against an installed package whose prefix contains `include/` and `lib/`:

```bash
make -C examples/c-scan LANCE_C_PREFIX=/path/to/lance-c-install
./examples/c-scan/build/lance-c-scan test_data/v0.27.1/pq_in_schema
```

The Makefile adds the prefix's `lib` directory to the runtime search path, so
the example runs without a loader variable. If the prefix moves after the
build, set `LD_LIBRARY_PATH` on Linux or `DYLD_LIBRARY_PATH` on macOS to the new
`lib` directory.

With pkg-config, the same programs can be built without `LANCE_C_PREFIX`:

```bash
export PKG_CONFIG_PATH=/path/to/lance-c-install/lib/pkgconfig
cc -std=c11 -O2 -Wall -Wextra -Wpedantic main.c \
  $(pkg-config --cflags --libs lance-c) \
  -Wl,-rpath,"/path/to/lance-c-install/lib" \
  -o build/lance-c-scan
./build/lance-c-scan test_data/v0.27.1/pq_in_schema
```

For a static link, add `--static` to `pkg-config` and name
`/path/to/lance-c-install/lib/liblance_c.a` instead of `-llance_c`; the system
libraries in `Libs.private` are then included.

Expected output:

```text
rows: 512, version: 2
```

A missing dataset exits nonzero and prints the dataset path with the Lance
error:

```bash
./examples/c-scan/build/lance-c-scan /path/that/does/not/exist
```
