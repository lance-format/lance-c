<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: Copyright The Lance Authors -->

# Schema provenance

The protobuf files under `java/src/main/proto` are Apache-2.0
field-for-field subsets of the authoritative Lance schema at:

- Repository: <https://github.com/lancedb/lance>
- Commit: `40f15ed1b71b66950961b175829fdf17a763f108` (Lance v14.0.2)

| Local file | Source | Source SHA-256 |
|---|---|---|
| `index_metadata.proto` | `protos/table.proto` | `8ba8a01dae10faac7eca7af82b2d67472a5d237cd2444ec5de936055b5f5b4f4` |
| `vector_index_details.proto` | `protos/index.proto` | `ef878dd98efb013d59b2748cfeb360c4a8b2e861b390863c1999b9a2a5d2f7f2` |

The copied table messages are `UUID`, `IndexMetadata`, and `IndexFile`. The
copied vector messages are `VectorMetricType`, `VectorIndexDetails` with its
nested quantization messages, and `HnswParameters`. Field numbers, protobuf
scalar types, optional presence, repeated fields, `oneof` members, and the
message-valued `google.protobuf.Any` are unchanged. Java package options only
change generated class names; they do not affect the wire format.

The adapter and harness compile this schema with the standard protobuf compiler
and parser pinned in `java/pom.xml`. Generated sources are not checked in.
