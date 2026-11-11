# Publishing packed and dedicated blob writers

`PackedBlobWriter` and `DedicatedBlobWriter` write blob payloads to a sidecar
next to a caller-named data file. The data file is not published by the writer
itself. To make the blobs visible to readers, write a replacement data file
that contains the prepared descriptors and publish it with the normal
transaction APIs.

This is a replacement path for existing fragments. It does not append new rows;
new-row append requires fragment creation and `Append`, which is outside this
workflow.

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
