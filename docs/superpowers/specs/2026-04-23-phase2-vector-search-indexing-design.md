# Phase 2: Vector Search & Indexing — Design

**Status:** Approved
**Date:** 2026-04-23
**Scope:** README "Phase 2" — vector search, full-text search, vector index creation, scalar index creation, index management, C++ wrappers.

## 1. Goal

Bring the lance-c bindings to feature parity with Lance's vector search and indexing surface, so that C/C++ callers can:

- Build IVF and HNSW vector indexes (IVF_FLAT, IVF_PQ, IVF_SQ, IVF_HNSW_FLAT, IVF_HNSW_PQ, IVF_HNSW_SQ).
- Build scalar indexes (BTree, Bitmap, LabelList, Inverted).
- List and drop indexes.
- Run k-nearest-neighbor vector search through the existing scanner builder.
- Run BM25 full-text search through the existing scanner builder.
- Use ergonomic C++ wrappers for all of the above.

## 2. Non-goals

- Compaction / fragment reuse / fragment cleanup (Phase 4).
- Write-path APIs beyond what already exists in `lance_write_fragments` (Phase 3).
- Compound boolean FTS queries (Boost / Boolean / Phrase composition). MVP exposes match + fuzzy; the composer can be added later without breaking changes.
- Hybrid (vector + FTS) search. Lance treats them as mutually exclusive; we surface that.
- Index optimization / re-training existing indexes. Only create / drop.

## 3. Architecture overview

Two surface areas, mirroring Lance's own split:

- **Index lifecycle** — new `src/index.rs`. Covers create vector / create scalar / drop / list. Mutates the dataset.
- **Search** — extends `src/scanner.rs`. `nearest()` and `full_text_search()` are scanner builder methods, so they reuse the existing iteration mechanisms (`to_arrow_stream`, `next`, `poll_next`, `scan_async`) without new code paths.

### File map

```
src/
  index.rs        NEW   create_vector_index, create_scalar_index, drop_index,
                        list_indices_json, index_count
  scanner.rs      EDIT  add nearest(), set_nprobes/refine/ef/metric/use_index/prefilter,
                        full_text_search()
  dataset.rs      EDIT  inner: Arc<Dataset> → RwLock<Arc<Dataset>>
  lib.rs          EDIT  re-export index::*
include/
  lance.h         EDIT  add LanceVectorIndexType, LanceScalarIndexType, LanceMetricType,
                        LanceDataType, LanceVectorIndexParams, function declarations
  lance.hpp       EDIT  add Dataset::create_*_index/drop/list, Scanner::nearest/fts
tests/
  c_api_test.rs   EDIT  add test fixtures and ~20 new tests
  cpp/test_cpp_api.cpp EDIT  add C++ smoke tests
```

## 4. Dataset mutability model

Currently `LanceDataset.inner` is `Arc<Dataset>`. `Dataset::create_index` and `drop_index` require `&mut Dataset`, which an `Arc` does not give us when other clones exist.

Change to:

```rust
pub struct LanceDataset {
    pub(crate) inner: RwLock<Arc<Dataset>>,
}
```

- **Reads** (scanner construction, schema, take, count_rows, fragments): take a read lock, clone the inner Arc, drop the lock, operate on the cloned Arc. Existing scanners that captured an Arc keep their snapshot view.
- **Writes** (create / drop index): take a write lock, `Arc::make_mut(&mut *guard)` to get `&mut Dataset` (cheap shallow clone if other Arc refs exist), call the mutation, drop the lock.
- After a successful mutation the same `LanceDataset*` handle reflects the new dataset version. No handle re-issuance for the C caller.

`Dataset` already implements `Clone`, so `Arc::make_mut` is well-defined.

## 5. Index creation API

### 5.1 Type enums

```c
typedef enum {
    LANCE_INDEX_IVF_FLAT     = 101,
    LANCE_INDEX_IVF_SQ       = 102,
    LANCE_INDEX_IVF_PQ       = 103,
    LANCE_INDEX_IVF_HNSW_SQ  = 104,
    LANCE_INDEX_IVF_HNSW_PQ  = 105,
    LANCE_INDEX_IVF_HNSW_FLAT = 106,
} LanceVectorIndexType;

typedef enum {
    LANCE_METRIC_L2      = 0,
    LANCE_METRIC_COSINE  = 1,
    LANCE_METRIC_DOT     = 2,
    LANCE_METRIC_HAMMING = 3,
} LanceMetricType;

typedef enum {
    LANCE_SCALAR_BTREE      = 1,
    LANCE_SCALAR_BITMAP     = 2,
    LANCE_SCALAR_LABEL_LIST = 3,
    LANCE_SCALAR_INVERTED   = 4,
} LanceScalarIndexType;
```

Numeric values match Lance's `IndexType` enum where applicable so cross-referencing manifests is trivial.

### 5.2 Vector index params

```c
typedef struct {
    LanceVectorIndexType index_type;
    LanceMetricType      metric;
    uint32_t num_partitions;       /* IVF; 0 = default */
    uint32_t num_sub_vectors;      /* PQ;  0 = default */
    uint32_t num_bits;             /* PQ/RQ; 0 → 8 */
    uint32_t max_iterations;       /* IVF kmeans; 0 = default (50) */
    uint32_t hnsw_m;               /* HNSW; 0 = default */
    uint32_t hnsw_ef_construction; /* HNSW; 0 = default */
    uint32_t sample_rate;          /* IVF; 0 = default */
} LanceVectorIndexParams;

int32_t lance_dataset_create_vector_index(
    LanceDataset* ds,
    const char* column,
    const char* index_name,        /* nullable; NULL → "<column>_idx" */
    const LanceVectorIndexParams* params,
    bool replace);
```

Mapping to Lance constructors:

| `index_type` | Lance constructor used | Required fields |
|---|---|---|
| IVF_FLAT | `VectorIndexParams::with_ivf_flat_params(metric, ivf)` | `num_partitions` |
| IVF_PQ | `VectorIndexParams::with_ivf_pq_params(metric, ivf, pq)` | `num_partitions`, `num_sub_vectors` |
| IVF_SQ | `VectorIndexParams::with_ivf_sq_params(metric, ivf, sq)` | `num_partitions` |
| IVF_HNSW_FLAT | `VectorIndexParams::ivf_hnsw(metric, ivf, hnsw)` | `num_partitions`, `hnsw_m` |
| IVF_HNSW_PQ | `VectorIndexParams::with_ivf_hnsw_pq_params(metric, ivf, hnsw, pq)` | `num_partitions`, `hnsw_m`, `num_sub_vectors` |
| IVF_HNSW_SQ | `VectorIndexParams::with_ivf_hnsw_sq_params(metric, ivf, hnsw, sq)` | `num_partitions`, `hnsw_m` |

The Rust core validates that the required fields for the chosen `index_type` are non-zero; otherwise it returns `LanceErrorCode::InvalidArgument` with a message naming the missing field. Per `CLAUDE.md`: never silently clamp; reject with descriptive errors.

### 5.3 Scalar index params

```c
int32_t lance_dataset_create_scalar_index(
    LanceDataset* ds,
    const char* column,
    const char* index_name,        /* nullable */
    LanceScalarIndexType index_type,
    const char* params_json,       /* nullable; passed through to ScalarIndexParams.params */
    bool replace);
```

Scalar index params upstream are already `Option<String>` JSON, with only the inverted index defining interesting params today (tokenizer, base tokenizer, language). Passing a JSON string keeps the C surface tiny and matches upstream verbatim — no inventing C structs we'd rip out later.

### 5.4 Behavior

- `index_name` NULL → Lance auto-generates as `<column>_idx`.
- `replace=false`: if an index of the same name exists, return `LANCE_ERR_INDEX` with the existing name in the message.
- `replace=true`: replace the existing index in the same commit.
- Returns 0 on success, -1 on error.
- After success, the same `LanceDataset*` handle reflects the new manifest version (see §4).

## 6. Search API (extends scanner)

### 6.1 Vector search

```c
typedef enum {
    LANCE_DTYPE_FLOAT32 = 0,
    LANCE_DTYPE_FLOAT16 = 1,
    LANCE_DTYPE_FLOAT64 = 2,
    LANCE_DTYPE_UINT8   = 3,
    LANCE_DTYPE_INT8    = 4,
} LanceDataType;

int32_t lance_scanner_nearest(
    LanceScanner* scanner,
    const char* column,
    const void* query_data,
    size_t query_len,
    LanceDataType element_type,
    uint32_t k);

int32_t lance_scanner_set_nprobes(LanceScanner*, uint32_t);
int32_t lance_scanner_set_refine_factor(LanceScanner*, uint32_t);
int32_t lance_scanner_set_ef(LanceScanner*, uint32_t);
int32_t lance_scanner_set_metric(LanceScanner*, LanceMetricType);
int32_t lance_scanner_set_use_index(LanceScanner*, bool);
int32_t lance_scanner_set_prefilter(LanceScanner*, bool);
```

- All knobs go into the existing `LanceScanner` struct as new optional fields.
- They are applied during `materialize_stream` / `build_scanner`, alongside the existing `limit`, `filter`, `fragment_ids` knobs.
- `nearest` validates `element_type` against the column's actual element type before delegating to Lance, surfacing a sharp error rather than a deep-stack panic.
- The output stream automatically includes a `_distance` column.

### 6.2 Full-text search

```c
int32_t lance_scanner_full_text_search(
    LanceScanner* scanner,
    const char* query,
    const char* const* columns,    /* NULL → all FTS-indexed columns */
    uint32_t max_fuzzy_distance);  /* 0 = exact; >0 = MatchQuery::with_fuzziness */
```

- Builds a `FullTextSearchQuery::new(query)` (or `new_fuzzy` if `max_fuzzy_distance > 0`), then `with_columns(...)` if the array is non-NULL.
- The output stream automatically includes a `_score` column.

### 6.3 Mutual exclusion

Vector search and FTS are mutually exclusive in Lance. We do not silently override; if the caller calls both, the second call returns `LANCE_ERR_INVALID_ARGUMENT` with a message saying which one is already set.

## 7. Index management

```c
/** Number of user indexes (excludes system indexes: FragmentReuse, MemWal). */
uint64_t lance_dataset_index_count(const LanceDataset* ds);

/**
 * JSON array of all user indexes:
 *   [{"name":"vec_idx","type":"IVF_PQ","columns":["embedding"],
 *     "metric":"l2","num_indexed_rows":1000,"num_unindexed_rows":0,
 *     "uuid":"..."}]
 * Caller must free with lance_free_string().
 * Returns NULL on error.
 */
const char* lance_dataset_index_list_json(const LanceDataset* ds);

/** Drop an index by name. Returns -1 with NOT_FOUND if no index of that name. */
int32_t lance_dataset_drop_index(LanceDataset* ds, const char* name);
```

JSON output keeps the C surface small while exposing all Lance metadata. A typed C struct enumerator would require a malloc'd array of strings per entry — overkill for an introspection API. Adding a typed surface later is non-breaking.

## 8. C++ wrappers (`lance.hpp`)

```cpp
namespace lance {

struct VectorIndexParams {
    LanceVectorIndexType index_type;
    LanceMetricType metric = LANCE_METRIC_L2;
    uint32_t num_partitions = 0;
    uint32_t num_sub_vectors = 0;
    uint32_t num_bits = 0;
    uint32_t max_iterations = 0;
    uint32_t hnsw_m = 0;
    uint32_t hnsw_ef_construction = 0;
    uint32_t sample_rate = 0;
};

class Dataset {
    // ... existing methods ...

    void create_vector_index(const std::string& column,
                             const VectorIndexParams& params,
                             const std::string& name = "",
                             bool replace = false);

    void create_scalar_index(const std::string& column,
                             LanceScalarIndexType index_type,
                             const std::string& name = "",
                             const std::string& params_json = "",
                             bool replace = false);

    void drop_index(const std::string& name);
    std::string list_indices_json() const;
    uint64_t index_count() const;
};

class Scanner {
    // ... existing fluent methods ...

    Scanner& nearest(const std::string& column,
                     const float* q, size_t dim, uint32_t k);   // f32 sugar
    Scanner& nearest(const std::string& column,
                     const void* q, size_t dim,
                     LanceDataType dtype, uint32_t k);          // typed
    Scanner& nprobes(uint32_t n);
    Scanner& refine_factor(uint32_t f);
    Scanner& ef(uint32_t e);
    Scanner& metric(LanceMetricType m);
    Scanner& use_index(bool b);
    Scanner& prefilter(bool b);

    Scanner& full_text_search(const std::string& query,
                              const std::vector<std::string>& columns = {},
                              uint32_t max_fuzzy_distance = 0);
};

} // namespace lance
```

The float-pointer overload covers the dominant case (Float32 embeddings). All other element types go through the typed overload.

## 9. Error handling

- All new APIs use the existing thread-local error mechanism: `lance_last_error_code()` / `lance_last_error_message()`.
- New error code values reused from the existing enum:
  - Invalid params (e.g. missing required field, bad column, dim mismatch) → `LANCE_ERR_INVALID_ARGUMENT`.
  - Index not found on drop → `LANCE_ERR_NOT_FOUND`.
  - Index name conflict on create with `replace=false` → `LANCE_ERR_INDEX`.
  - Underlying I/O / commit failures → `LANCE_ERR_IO` / `LANCE_ERR_COMMIT_CONFLICT`.
- No new error codes added — Phase 1's `LanceErrorCode` covers Phase 2.

## 10. Testing

Per `CLAUDE.md`: every feature must have tests, cover NULL/empty edge cases, and include multi-fragment scenarios.

### Rust integration tests (`tests/c_api_test.rs`)

New fixture: `create_vector_dataset(num_rows, dim) -> (TempDir, String)` writing a dataset with an `embedding` column of `FixedSizeList<Float32, dim>`, an `id: Int32`, and a `text: Utf8` column.

Test cases (each maps to one `#[test]`):

1. **Vector index lifecycle (IVF_FLAT)** — create, list shows it, drop, list empty.
2. **Vector index lifecycle (IVF_PQ)** — same with PQ params.
3. **Vector index lifecycle (IVF_HNSW_SQ)** — same with HNSW params.
4. **Scalar index lifecycle (BTree, Bitmap, LabelList, Inverted)** — one test per type.
5. **Vector search with index** — build IVF_PQ, `nearest(query, k=10)`, assert distances non-decreasing, k results, `_distance` column present.
6. **Vector search knobs** — `nprobes`, `refine_factor`, `ef` accepted; `metric` override changes ordering.
7. **Vector search without index** — brute-force path returns correct results.
8. **Vector search + filter (post-filter)** — filter applied after nearest.
9. **Vector search + filter (prefilter)** — `set_prefilter(true)` changes counts.
10. **FTS** — build inverted index, `full_text_search("alice")` returns matches with `_score`.
11. **FTS fuzzy** — `max_fuzzy_distance=2` matches near-misses.
12. **Required-param validation** — IVF_PQ with `num_sub_vectors=0` returns InvalidArgument naming the field.
13. **Element-type mismatch** — Float32 query against u8 column returns InvalidArgument.
14. **Dim mismatch** — query length ≠ column dim returns InvalidArgument.
15. **Drop missing index** — returns NotFound.
16. **Replace=true** — recreating with replace=true succeeds.
17. **Replace=false on conflict** — returns IndexError.
18. **Mutual exclusion** — calling `nearest` after `full_text_search` (or vice versa) returns InvalidArgument.
19. **Multi-fragment** — write 2 fragments, build vector index, nearest still returns correct top-k spanning fragments.
20. **NULL safety** — NULL scanner, NULL column, NULL query all handled.

### C++ smoke tests (`tests/cpp/test_cpp_api.cpp`)

- Create vector index + search via fluent API.
- Create scalar index + drop + list.
- FTS search.

## 11. Implementation phasing

1. **PR 1 — Mutability refactor + index creation/management** (combined per user direction; the refactor has no other consumer)
   - `inner: Arc<Dataset>` → `RwLock<Arc<Dataset>>` and update all read sites in dataset.rs / scanner.rs.
   - `src/index.rs` with create_vector_index, create_scalar_index, drop_index, list_indices_json, index_count.
   - Header + C++ wrapper additions for index lifecycle.
   - Tests 1–4, 12, 15, 16, 17 from §10.
2. **PR 2 — Vector search**
   - `nearest()` and knobs in `scanner.rs`.
   - Header + C++ wrapper additions.
   - Tests 5–9, 13, 14, 19, 20 (vector half).
3. **PR 3 — Full-text search**
   - `full_text_search()` in `scanner.rs`.
   - Header + C++ wrapper additions.
   - Tests 10, 11, 18, 20 (FTS half). Test 18 (mutual exclusion) lands here since it requires both APIs to be present.

Each PR is independently mergeable, has its own tests, and leaves the README Phase 2 checklist with progressively more boxes ticked.

## 12. Open questions

None. All design decisions resolved during brainstorming:

- Scope: full Phase 2, one spec.
- Vector index params: tagged-enum struct.
- Query vector encoding: typed pointer + `LanceDataType`.
- FTS surface: simple match + fuzzy at MVP.
- Implementation order: 3 PRs (mutability+indexing combined, then vector search, then FTS).
