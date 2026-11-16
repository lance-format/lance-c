# Cross-SDK index metadata acceptance

This opt-in interoperability test proves three narrow paths against one exact
SDK pair:

1. A C11 program uses the public lance-c API on native Lance 14.0.2 to create
   two-fragment datasets and build uncommitted, fragment-scoped BTree version-0
   segments, IVF_PQ version-1 segments, and Inverted FTS version-2 segments.
2. Java decodes the exact C-produced protobuf `IndexMetadata` bytes with a
   standard generated protobuf parser and maps them to the Java 12
   `Index.Builder`.
3. `commitExistingIndexSegments` publishes both segments in one Java dataset
   version.
4. The dataset is closed and reopened. Coverage, UUIDs, full `Any` bytes, index
   version, and the commit-generated file list are checked.
5. BTree filters, IVF_PQ k-NN queries, and FTS match, phrase, Boolean, and rare
   term queries are compared with independent expected results and
   Java-to-Java one-shot builds on equivalent pre-commit copies. `ScanStats`
   must report index activity.

The vector path trains one IVF/PQ model in C, injects that model into both
fragment-scoped segment builds, and checks the generated `VectorIndexDetails`
with the same standard protobuf parser. Its queries set L2 explicitly, probe
all four IVF partitions, and overfetch all 256 fixture rows for reranking, so
the fixed expected IDs and distances test semantic fidelity rather than
stochastic approximate recall.

The FTS path indexes one ordinary string field over 128 rows in two real
fragments. It uses the simple analyzer, English language settings, lower-casing,
token positions, equal tokenization settings on both sides, and the default
128-document posting block. The explicit FTS `format_version=2` is also the
default for this analyzer and block size at both versions. Its queries check
rows from both fragments, a positional phrase, Boolean `MUST` / `MUST_NOT`, and
a token present in only the final row.

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

`run.sh` refuses to reuse an existing work directory. It runs the BTree,
IVF_PQ, and FTS workflows and records their producer logs, metadata
hashes, tool versions, Maven dependency tree, resolved Java classpath, and Java
acceptance logs. Set `LANCE_C_LIB` to an already-built `liblance_c.so` to skip
the Cargo build.

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
the omitted input. Vector `Any` values remain opaque at this adapter boundary
and are byte-compared before and after commit; the acceptance separately
decodes `VectorIndexDetails` to verify the metric and PQ shape. FTS `Any`
values are likewise retained byte-for-byte and separately decoded to verify the
analyzer, position, block-size, document-granularity, and posting-format
settings.

It rejects v14 values that the Java 12 API cannot represent:

- any present `clustering_id`;
- non-empty v14 index `metadata`;
- unsigned dataset versions, fragment IDs, base IDs, or creation times outside
  the corresponding signed Java range;
- missing UUID, fields, name, fragment coverage, or index details.

## Scope and limits

This proves BTree version 0, one L2 IVF_PQ version-1 configuration, and one
Inverted FTS version-2 configuration on datasets without stable row IDs. The
vector fixture has two fragments, 256 rows, dimension 8, four IVF partitions,
two 4-bit PQ subvectors, and one shared C-trained model. The FTS fixture has
two fragments, 128 rows, the simple analyzer, token positions, 128-document
posting blocks, row document granularity, and no stemming or stop-word removal.
Both the v12 consumer and v14 producer use default Lance data-file format 2.2
here; the writer format is not downgraded.

It does not prove other vector types, metrics, quantization shapes, stable-row
ID BTree version 1, other FTS analyzers or formats, FTS format v3, list-element
documents, result-score equality across native versions, Bitmap, NGram, RTree,
Doris Arrow 19 dependency management, the Doris glibc JNI replacement, Doris
BE/native production alignment, Doris end-to-end index mutation, or general
distributed FTS shard merge.
