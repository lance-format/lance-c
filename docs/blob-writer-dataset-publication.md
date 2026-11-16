# Publishing packed and dedicated blob writers

`PackedBlobWriter` and `DedicatedBlobWriter` write blob payloads to a sidecar
next to a caller-named data file. The data file is not published by the writer
itself. To make the blobs visible to readers, write a replacement data file
that contains the prepared descriptors and publish it with the normal
transaction APIs.

The same finished files support two native publication paths:

- Existing fragments use `DataReplacement`, which is described first below.
- New rows use `Fragment::from_data_files` and `Append`, described under
  [Publishing new rows](#publishing-new-rows).

## Prerequisites

- The dataset uses the logical Blob v2 schema, normally with
  `blob_field_with_options(...)`.
- The replacement data file uses a compatible file version. For a V2.2
  dataset, pass `"2.2"` through `FileWriterOptions::version`.
- The caller owns file-name and blob-ID uniqueness. Choose a data-file name
  that is not already referenced by a committed fragment.

## Caller workflow

1. Open the dataset and read its current version and target fragment IDs.
2. Open one shared `FileSession` at `<dataset-uri>/data`. Do not construct
   sidecar paths or translate the dataset URI in the binding.
3. Choose one relative data-file name, for example
   `replacement-<unique>.lance`.
4. Open the packed or dedicated writer with that same name, and call
   `finish_array`. The returned descriptor field and array are owned by the
   caller. The field retains the native Blob v2 metadata and must be used as
   the blob column schema of the replacement file.
5. Build one replacement record batch containing the replacement data columns
   and the prepared descriptor column. It must have one descriptor row per
   physical row of the target fragment, in the original physical order. Import
   it with `Batch::from_arrow`; that call consumes the top-level array.
6. Write the batch to the same relative data-file name through
   `FileSession::open_writer`, with a compatible file version, and finish the
   raw writer explicitly.
7. Call `Dataset::create_data_file(data_file_name)` with no base ID. Native
   reads the finished file, validates the persisted field structure, and maps
   field names to dataset field IDs.
8. Construct `DataReplacementGroup(fragment_id, data_file)` for each target
   fragment, build one `Operation::make_data_replacement`, and commit one
   `Transaction` at the dataset version read in step 1.
9. Close the handles, reopen the dataset URI, and read the new version. Verify
   the payloads, physical layout, row IDs, and preservation of unreplaced
   fragments.

The C API follows the same sequence with `lance_file_session_open_*`,
`lance_*_finish_array`, `lance_file_writer_*`,
`lance_dataset_create_data_file`, and the existing transaction calls.

### C11 caller notes

The complete C11 example is `test_blob_writer_publication` in the
[native transaction consumer](../tests/cpp/test_transaction.c). It finishes the
raw writer before creating the `DataFile`, then constructs the replacement
operation and commits through the existing transaction APIs. The
[C++17 consumer](../tests/cpp/test_transaction.cpp) follows the same workflow.

`lance_batch_from_arrow` consumes the top-level array, including the prepared
descriptor child; do not close or release that child again. The top-level
schema is borrowed by `lance_batch_from_arrow`; the caller retains ownership
of that schema and the prepared descriptor field. `lance_*_finish_array` writes
the sidecar before it returns, so the raw data file must use the same relative
name and a compatible file version. `lance_dataset_create_data_file` and the
subsequent transaction calls are the only publication step.

## File layout and replacement rules

The raw file and the blob writer must use the same relative name:

```text
dataset/
  data/
    replacement-<unique>.lance
    replacement-<unique>/
      <native-obfuscated-blob-id>.blob
```

The native writer derives the sidecar path. The C/C++ layer does not add a
`data/` prefix or parse the dataset URI.

A data replacement supplies new base data files for an existing fragment.
Native requires the replacement file to describe the same physical rows as the
fragment. The file's top-level fields are matched by name to the dataset
schema; `create_data_file` produces the dataset field IDs and column indices.
Preserve the prepared blob field instead of rebuilding the descriptor schema
by hand.

Deletion vectors belong to the fragment, not to the replacement data file.
Replacing the base file must not drop a pending deletion vector. With stable
row IDs, existing live rows keep their row IDs, and an old snapshot continues
to expose the old data. One commit that carries several replacement groups
increases the dataset version once.

`DedicatedBlobWriter::finish_array` produces exactly one descriptor row. To
assemble a multi-row dedicated replacement, finish one writer per row and
append the one-row batches through the same raw file writer. Multiple
dedicated writers may use the same data-file name with different blob IDs;
their sidecar paths must remain distinct.

## Publishing new rows

New rows use the same writer and raw-file sequence, but publish a new fragment
instead of replacing an existing one:

1. Open the dataset and record its current version.
2. Write the Packed/Dedicated sidecars and the matching raw data file as
   described above. Every new fragment has its own data-file name.
3. Call `Dataset::create_data_file(name)` for each finished raw file.
4. Construct a detached fragment from the returned `DataFile` metadata:
   - C++: `Fragment::from_data_files(fragment_id, {&file}, physical_rows)`
   - C: `lance_fragment_new(fragment_id, files, count, physical_rows)`

   The constructor starts from native `Fragment::new`, clones each borrowed
   `DataFile`, and sets only the optional `physical_rows` value. Deletion
   files, overlays, row-ID metadata and row-version metadata keep their native
   defaults. Use fragment ID `0` to request native commit-time ID allocation.
   `physical_rows` is `std::nullopt` or `NULL` for native `None`; otherwise it
   must be the actual physical row count of the data file. The constructor
   does not infer that count or revalidate schema coverage.
5. Build one `Operation::Append` from one or more detached fragments and commit
   it through the normal `Transaction` path at the recorded version.
6. Close the `DataFile` handles after fragment construction and the fragment
   handles after operation construction; both constructors clone their inputs.
7. Reopen the dataset URI and verify the new rows, the increased row/fragment
   counts, preservation of old rows and row IDs, and the old snapshot.

`Fragment::from_data_files` and `lance_fragment_new` are metadata constructors,
not manifest writers. Native commit owns fragment-ID assignment, schema
coverage, conflict handling and manifest publication. The C11 example is
`test_blob_writer_append` in the
[native transaction consumer](../tests/cpp/test_transaction.c); the
[C++17 consumer](../tests/cpp/test_transaction.cpp) implements the same flow.

### Standalone raw-reader limitation

Dataset publication and dataset reads are supported. The selected Lance
14.0.2 core cannot reliably reconstruct a standalone raw v2.2 Blob footer:
default projection fails validation and explicit Blob projection fails
decoding. This affects raw `FileReader` consumers of the same file and is
tracked separately in #167; it does not change the dataset Append workflow
above.

## Lifecycle and failure boundaries

`finish_array` consumes the native writer. Precheck failures leave the writer
usable; once the native finish is entered, a storage error can leave the data
file or sidecar on storage. No rollback, cleanup, or preservation of an old
sidecar is promised. Use fresh names and treat a failed publication as an
unreferenced artifact rather than retrying into the same path.

Release each non-NULL prepared output exactly once. `Batch::from_arrow`
consumes the top-level array, including its child descriptor array; do not
release that child again. The descriptor field is borrowed by the replacement
schema and remains usable for the write.

The transaction APIs publish the resulting `DataFile`; the writer APIs never
commit a dataset version on their own.
