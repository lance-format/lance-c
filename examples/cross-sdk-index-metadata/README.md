# Cross-SDK index metadata PoC

This opt-in interoperability test proves one narrow path:

1. A C11 program uses the public lance-c API on native Lance 14.0.2 to create a
   two-fragment dataset and build two uncommitted, fragment-scoped BTree
   version-0 segments.
2. Java decodes the exact C-produced protobuf `IndexMetadata` bytes with a
   standard generated protobuf parser and maps them to the Java 12
   `Index.Builder`.
3. `commitExistingIndexSegments` publishes both segments in one Java dataset
   version.
4. The dataset is closed and reopened. Coverage, UUIDs, full `Any` bytes, index
   version, and the commit-generated file list are checked.
5. Three indexed filters are compared with independent expected IDs and a
   Java-to-Java one-shot BTree build on an equivalent pre-commit dataset copy.
   `ScanStats` must report index loads or comparisons.

## Exact pair

| Side | Identity |
|---|---|
| C producer | lance-c `0.1.9`, Rust `1.94.0` |
| Native producer | Lance `14.0.2`, `40f15ed1b71b66950961b175829fdf17a763f108` |
| Java consumer | `org.lance:lance-core:12.0.0` |
| Java source | Lance `12.0.0`, `cbeec97cb893a66ad6b3db87f25a57ab0093f358` |
| Protobuf | `protobuf-java` and `protoc` `4.33.4` |
| Roaring | `org.roaringbitmap:RoaringBitmap:0.8.13` |
| Arrow | `18.3.0`, resolved transitively from the Java 12 artifact POM |

The schema subset and its Apache-2.0 provenance are recorded in
[`PROVENANCE.md`](PROVENANCE.md).

## Run

Requirements on the execution host:

- Linux x86_64
- Rust `1.94.0`
- A C11 compiler
- JDK 17
- Maven 3.9.x
- Network access for Maven Central

From the repository root:

```bash
RUSTUP_TOOLCHAIN=1.94.0 cargo build --locked --release --lib
examples/cross-sdk-index-metadata/run.sh /tmp/lance-cross-sdk-metadata-poc
```

`run.sh` refuses to reuse an existing work directory. It records the producer
log, metadata hashes, tool versions, Maven dependency tree, resolved Java
classpath, and Java acceptance log in that directory. Set `LANCE_C_LIB` to an
already-built `liblance_c.so` to skip the Cargo build.

The Java project is not part of ordinary `cargo build` or `cargo test`; JDK and
Maven are required only when this example is run.

## Adapter boundary

The adapter preserves:

- the 16 UUID bytes without converting through text;
- field and covering-field IDs;
- dataset version;
- portable little-endian Roaring fragment coverage;
- the complete serialized `google.protobuf.Any`, not only its value;
- the physical index version;
- optional creation time and base ID when they fit Java 12's types.

It intentionally omits input files and `sizeBytes`. Java 12's native commit
re-lists `_indices/<uuid>/`; the acceptance checks that the committed and
reopened metadata has non-empty files and positive size instead of trusting
the omitted input.

It rejects v14 values that the Java 12 API cannot represent:

- any present `clustering_id`;
- non-empty v14 index `metadata`;
- unsigned dataset versions, fragment IDs, base IDs, or creation times outside
  the corresponding signed Java range;
- missing UUID, fields, name, fragment coverage, or index details.

## Scope and limits

This proves BTree version 0 on a dataset without stable row IDs. Both the v12
consumer and v14 producer use default Lance data-file format 2.2 here; the
writer format is not downgraded.

It does not prove IVF or vector indexes, FTS/Inverted, Bitmap, NGram, RTree,
stable-row-ID BTree version 1, Doris Arrow 19 dependency management, the Doris
glibc JNI replacement, Doris BE/native production alignment, or Doris
end-to-end index mutation. Current Doris mutation admission remains disabled.
