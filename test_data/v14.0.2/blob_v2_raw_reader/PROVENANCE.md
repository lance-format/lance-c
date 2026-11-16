# Blob v2 raw-reader fixture (Lance 14.0.2)

This directory carries a raw Lance file produced by the released Lance 14.0.2
file writer. It exists so `tests/file_api_test.rs` and the C/C++ raw-file
consumers exercise the public `lance_file_reader_*` API against a Blob v2
footer written before the descriptor children were normalized on read.

## Upstream source

- Repository: https://github.com/lancedb/lance
- Tag: `v15.0.0-beta.20` (release commit `86fff5f40f299a2ff6abb0c10210451f27e605eb`)
- Path: `test_data/v14.0.2/blob_v2_raw_reader/blob_v2_raw.lance`
- Git blob: `bffb17ee6b8664992bc9c7dffc7cc8ee9e9bfe0a`
- Size: 2530 bytes
- SHA256: `dcad3729bdfb4e6e7744f362fb09d60144a3af90739195a41fad06c8559acae6`

## Producer

The upstream producer is `test_data/v14.0.2/datagen.py` at the same release
commit (git blob `402a1e1f4061cb6a5d17b7ef041d38870a42cc81`, SHA256
`cfb207e36740a1e36c09adc9f2ba5790d1b5824a34a910670520a7a456c299a8`):

```bash
uv run --no-project --with pylance==14.0.2 --with pyarrow==26.0.1 datagen.py
```

The script asserts `lance.__version__ == "14.0.2"` and writes the file with
`LanceFileWriter` (file version 2.2) from three rows of a schema that holds
three Blob v2 columns: two top-level columns (`blob_a`, `blob_b`) and one
nested column (`info.payload`).

## Shape

- 3 rows, 4 root columns: `id` (Int32), `blob_a`, `blob_b`, `info` (struct with
  `payload`).
- Blob columns use the prepared Blob v2 Arrow layout
  (`kind`, `data`, `uri`, `blob_id`, `blob_size`, `position`) with metadata
  `lance-encoding:blob`, `lance-encoding:packed` and
  `ARROW:extension:name = lance.blob.v2`.
- The released 14.0.2 writer stores the descriptor children (`kind`,
  `position`, `size`, `blob_id`, `blob_uri`) as footer root fields, so the raw
  footer describes the Blob column itself as childless.
- `blob_a` payloads: `alpha`, null row, empty bytes; `blob_b` payloads: `red`,
  `green`, null row; `info.payload` payloads: `nested-0`, null row,
  `nested-2`.

The released 14.0.2 reader rejects this footer. Lance 15 normalizes the
descriptor children while decoding the footer, which is the behavior these
tests pin through the C API instead of a dataset scan.

Licensed under the Apache License, Version 2.0, as part of the Lance project.
