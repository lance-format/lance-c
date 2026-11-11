<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: Copyright The Lance Authors -->

# Schema provenance

`java/src/main/proto/index_metadata.proto` is an Apache-2.0 field-for-field
subset of the authoritative Lance schema at:

- Repository: <https://github.com/lancedb/lance>
- Commit: `40f15ed1b71b66950961b175829fdf17a763f108` (Lance v14.0.2)
- Source: `protos/table.proto`
- Source SHA-256:
  `8ba8a01dae10faac7eca7af82b2d67472a5d237cd2444ec5de936055b5f5b4f4`

The copied messages are `UUID`, `IndexMetadata`, and `IndexFile`. Field
numbers, protobuf scalar types, optional presence, repeated fields, and the
message-valued `google.protobuf.Any` are unchanged. Java package options only
change generated class names; they do not affect the wire format.

The adapter and harness compile this schema with the standard protobuf compiler
and parser pinned in `java/pom.xml`. Generated sources are not checked in.
