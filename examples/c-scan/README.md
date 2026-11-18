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
LD_LIBRARY_PATH=/path/to/lance-c-install/lib \
  ./examples/c-scan/build/lance-c-scan test_data/v0.27.1/pq_in_schema
```

Expected output:

```text
rows: 512, version: 2
```

A missing dataset exits nonzero and prints the dataset path with the Lance
error:

```bash
./examples/c-scan/build/lance-c-scan /path/that/does/not/exist
```
