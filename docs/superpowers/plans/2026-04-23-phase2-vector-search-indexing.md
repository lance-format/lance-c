# Phase 2: Vector Search & Indexing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring lance-c to feature parity with Lance's vector search and indexing surface — vector/scalar index creation, list/drop, k-NN and BM25 search via the existing scanner builder, plus C++ wrappers.

**Architecture:** Two surface areas mirroring Lance's split. New `src/index.rs` for index lifecycle (mutates dataset). Extensions to `src/scanner.rs` for vector search (`nearest`) and FTS (`full_text_search`). The `LanceDataset.inner` field changes from `Arc<Dataset>` to `RwLock<Arc<Dataset>>` so create/drop operations can mutate while existing scanners keep their snapshot view.

**Tech Stack:** Rust 2024 edition + `lance` 3.0.1 (`DatasetIndexExt`, `VectorIndexParams`, `ScalarIndexParams`, `FullTextSearchQuery`), Arrow 57.0.0 C Data Interface, C/C++ via `extern "C"` + header-only RAII wrappers.

**Spec:** [`docs/superpowers/specs/2026-04-23-phase2-vector-search-indexing-design.md`](../specs/2026-04-23-phase2-vector-search-indexing-design.md)

---

## Phasing — 3 PRs

- **PR 1 (Tasks 1–15):** Mutability refactor + index creation/management + C++ wrappers for index lifecycle.
- **PR 2 (Tasks 16–22):** Vector search (`nearest` + knobs) + C++ wrappers.
- **PR 3 (Tasks 23–26):** Full-text search + mutual exclusion + C++ wrappers.

Each PR ends with all `cargo test` and the C/C++ compile test passing.

---

# PR 1 — Mutability Refactor + Index Lifecycle

### Task 1: Add `snapshot()` and `with_mut()` accessors to `LanceDataset`

**Files:**
- Modify: `src/dataset.rs`

The struct field changes shape and we add two helpers. All read sites in dataset.rs are updated in this task; scanner.rs is updated in Task 2. After this task, `cargo check --all-targets` will fail because scanner.rs accesses `.inner` directly — that's expected.

- [ ] **Step 1: Replace the struct field and add helper methods**

Replace lines 20–23 of `src/dataset.rs`:

```rust
use std::sync::{Arc, RwLock};

/// Opaque handle representing an opened Lance dataset.
pub struct LanceDataset {
    pub(crate) inner: RwLock<Arc<Dataset>>,
}

impl LanceDataset {
    /// Take a consistent snapshot of the inner dataset.
    /// Returns a cloned Arc so the caller can hold it without keeping the lock.
    pub(crate) fn snapshot(&self) -> Arc<Dataset> {
        self.inner.read().expect("dataset rwlock poisoned").clone()
    }

    /// Mutate the inner dataset under an exclusive write lock.
    /// `Arc::make_mut` performs a cheap shallow clone if other Arc refs exist
    /// (existing scanners keep their snapshot view).
    pub(crate) fn with_mut<F, R>(&self, f: F) -> R
    where
        F: FnOnce(&mut Dataset) -> R,
    {
        let mut guard = self.inner.write().expect("dataset rwlock poisoned");
        let ds = Arc::make_mut(&mut *guard);
        f(ds)
    }
}
```

Remove the existing `use std::sync::Arc;` line if it duplicates.

- [ ] **Step 2: Update `lance_dataset_open` constructor**

In `src/dataset.rs`, find the `let handle = LanceDataset { inner: Arc::new(dataset), };` block in `open_dataset_inner` and replace with:

```rust
    let handle = LanceDataset {
        inner: RwLock::new(Arc::new(dataset)),
    };
```

- [ ] **Step 3: Update all read sites in `dataset.rs`**

Replace every `ds.inner.<method>` call with `ds.snapshot().<method>` (or split across lines for clarity). Specifically:

- `ds.inner.version().version` → `ds.snapshot().version().version`
- `ds.inner.count_rows(None)` → `ds.snapshot().count_rows(None)`
- `ds.inner.latest_version_id()` → `ds.snapshot().latest_version_id()`
- `ds.inner.schema()` → `let snap = ds.snapshot(); let lance_schema = snap.schema();` (chain owns the Arc)
- `ds.inner.take(idx_slice, projection)` → `ds.snapshot().take(idx_slice, projection)`
- `ds.inner.count_fragments()` → `ds.snapshot().count_fragments()`
- `ds.inner.get_fragments()` → `ds.snapshot().get_fragments()`

For `dataset_take_inner`, the projection construction also touches the schema — keep one snapshot in scope:

```rust
    let snap = ds.snapshot();
    let projection = match &col_names {
        Some(cols) => lance::dataset::ProjectionRequest::from_columns(cols.iter(), snap.schema()),
        None => lance::dataset::ProjectionRequest::from_schema(snap.schema().clone()),
    };
    let batch = block_on(snap.take(idx_slice, projection))?;
```

- [ ] **Step 4: Run `cargo check` to verify dataset.rs compiles**

Run: `cargo check --all-targets 2>&1 | head -40`
Expected: errors only in `src/scanner.rs` referring to `ds.inner.clone()` and `dataset.scan()`. dataset.rs itself should compile cleanly.

### Task 2: Update `scanner.rs` to use `snapshot()`

**Files:**
- Modify: `src/scanner.rs`

- [ ] **Step 1: Replace `ds.inner.clone()` and re-shape struct field**

In `src/scanner.rs`:
- The struct already stores `dataset: Arc<Dataset>` — keep that, just change how we obtain it.
- In `scanner_new_inner`, replace `LanceScanner::new(ds.inner.clone())` with `LanceScanner::new(ds.snapshot())`.
- `materialize_stream` and `build_scanner` use `self.dataset.scan()` which is fine because `self.dataset` is already a `Arc<Dataset>` snapshot.

No other changes needed.

- [ ] **Step 2: Run check + tests to confirm Phase 1 is unbroken**

```bash
cargo check --all-targets
cargo test --lib
cargo test --test c_api_test
```

All existing tests should pass. Expected output: `test result: ok. ... passed`.

- [ ] **Step 3: Commit the refactor**

```bash
git add src/dataset.rs src/scanner.rs
git commit -m "$(cat <<'EOF'
refactor(dataset): wrap inner Arc<Dataset> in RwLock for mutation

Adds LanceDataset::snapshot() (clones Arc under read lock) and
with_mut() (Arc::make_mut under write lock). All read sites now go
through snapshot(); a follow-up commit will introduce write sites
(create_index, drop_index).

Existing scanners keep their consistent Arc snapshot when the
dataset mutates, mirroring how Lance handles dataset versioning
internally.
EOF
)"
```

### Task 3: Add Phase 2 type enums to `lance.h`

**Files:**
- Modify: `include/lance.h`

- [ ] **Step 1: Add new enums and forward decls**

Insert these definitions in `include/lance.h` after the existing `LanceErrorCode` enum (around line 81), before the opaque-handle section:

```c
/* ─── Index types (Phase 2) ─── */

typedef enum {
    LANCE_INDEX_IVF_FLAT      = 101,
    LANCE_INDEX_IVF_SQ        = 102,
    LANCE_INDEX_IVF_PQ        = 103,
    LANCE_INDEX_IVF_HNSW_SQ   = 104,
    LANCE_INDEX_IVF_HNSW_PQ   = 105,
    LANCE_INDEX_IVF_HNSW_FLAT = 106,
} LanceVectorIndexType;

typedef enum {
    LANCE_SCALAR_BTREE      = 1,
    LANCE_SCALAR_BITMAP     = 2,
    LANCE_SCALAR_LABEL_LIST = 3,
    LANCE_SCALAR_INVERTED   = 4,
} LanceScalarIndexType;

typedef enum {
    LANCE_METRIC_L2      = 0,
    LANCE_METRIC_COSINE  = 1,
    LANCE_METRIC_DOT     = 2,
    LANCE_METRIC_HAMMING = 3,
} LanceMetricType;

typedef enum {
    LANCE_DTYPE_FLOAT32 = 0,
    LANCE_DTYPE_FLOAT16 = 1,
    LANCE_DTYPE_FLOAT64 = 2,
    LANCE_DTYPE_UINT8   = 3,
    LANCE_DTYPE_INT8    = 4,
} LanceDataType;

typedef struct {
    LanceVectorIndexType index_type;
    LanceMetricType      metric;
    uint32_t num_partitions;        /* IVF; 0 = default (lance internal) */
    uint32_t num_sub_vectors;       /* PQ;  0 = default */
    uint32_t num_bits;              /* PQ/RQ; 0 = 8 */
    uint32_t max_iterations;        /* IVF kmeans; 0 = 50 */
    uint32_t hnsw_m;                /* HNSW; 0 = default */
    uint32_t hnsw_ef_construction;  /* HNSW; 0 = default */
    uint32_t sample_rate;           /* IVF; 0 = 256 */
} LanceVectorIndexParams;
```

Then add the function declarations at the end of the file, before `#ifdef __cplusplus` closing block:

```c
/* ─── Index lifecycle (Phase 2) ─── */

/**
 * Create a vector index on a column.
 * @param dataset    Open dataset (mutated; same handle remains valid).
 * @param column     Column name (must be FixedSizeList<float32|float16|uint8|int8>).
 * @param index_name Optional index name; NULL → "<column>_idx".
 * @param params     Vector index params; index_type field selects the variant.
 * @param replace    If true, replace any existing index of the same name.
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_create_vector_index(
    LanceDataset* dataset,
    const char* column,
    const char* index_name,
    const LanceVectorIndexParams* params,
    bool replace
);

/**
 * Create a scalar index on a column.
 * @param params_json Optional JSON params string (e.g. inverted tokenizer config), or NULL.
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_create_scalar_index(
    LanceDataset* dataset,
    const char* column,
    const char* index_name,
    LanceScalarIndexType index_type,
    const char* params_json,
    bool replace
);

/** Drop an index by name. Returns -1 (NOT_FOUND) if no such index. */
int32_t lance_dataset_drop_index(LanceDataset* dataset, const char* name);

/** Number of user indexes (excludes system indexes). Returns 0 on error. */
uint64_t lance_dataset_index_count(const LanceDataset* dataset);

/**
 * JSON array describing all user indexes.
 * Caller must free the returned string with lance_free_string().
 * Returns NULL on error.
 */
const char* lance_dataset_index_list_json(const LanceDataset* dataset);
```

- [ ] **Step 2: Verify the header compiles standalone**

```bash
cc -I include -fsyntax-only -xc - <<'EOF'
#include "lance.h"
int main(void) {
    LanceVectorIndexParams p = {LANCE_INDEX_IVF_PQ, LANCE_METRIC_L2, 256, 16, 8, 50, 0, 0, 256};
    return p.num_partitions != 256;
}
EOF
```

Expected: exit code 0, no output.

### Task 4: Create `src/index.rs` skeleton + register in `lib.rs`

**Files:**
- Create: `src/index.rs`
- Modify: `src/lib.rs`

- [ ] **Step 1: Create the new module skeleton**

Create `src/index.rs`:

```rust
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Index lifecycle C API: create vector / scalar indexes, drop, list.
//!
//! Index creation mutates the dataset under an exclusive write lock; existing
//! scanners that already cloned the inner Arc keep their snapshot view.

use std::ffi::{CString, c_char};

use lance_core::Result;
use lance_index::scalar::{BuiltinIndexType, ScalarIndexParams};
use lance_index::{DatasetIndexExt, IndexType};

use crate::dataset::LanceDataset;
use crate::error::{LanceErrorCode, ffi_try, set_last_error};
use crate::helpers;
use crate::runtime::block_on;
```

- [ ] **Step 2: Register the module in `src/lib.rs`**

In `src/lib.rs`, after `mod helpers;` add `mod index;`. After `pub use fragment_writer::*;` add `pub use index::*;`.

- [ ] **Step 3: Verify it compiles**

```bash
cargo check --all-targets
```

Expected: compiles with warnings about unused imports — that's fine, tasks 5–13 will use them.

### Task 5: Implement `lance_dataset_create_scalar_index` (BTree happy path) — TDD

**Files:**
- Modify: `src/index.rs`
- Modify: `tests/c_api_test.rs`

- [ ] **Step 1: Write the failing test**

At the bottom of `tests/c_api_test.rs`, add:

```rust
#[test]
fn test_create_scalar_index_btree() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let column = c_str("id");
    let rc = unsafe {
        lance_dataset_create_scalar_index(
            ds,
            column.as_ptr(),
            ptr::null(),                 /* default name */
            LanceScalarIndexType::BTree as i32,
            ptr::null(),                 /* no params */
            false,
        )
    };
    assert_eq!(rc, 0, "create_scalar_index returned {} ({:?})", rc, unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });

    let count = unsafe { lance_dataset_index_count(ds) };
    assert_eq!(count, 1);

    unsafe { lance_dataset_close(ds) };
}
```

You also need to make `LanceScalarIndexType` visible in Rust. Add this to `src/index.rs` (it mirrors the C enum):

```rust
/// Scalar index type, matching the C enum `LanceScalarIndexType`.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceScalarIndexType {
    BTree = 1,
    Bitmap = 2,
    LabelList = 3,
    Inverted = 4,
}

impl LanceScalarIndexType {
    fn from_c(v: i32) -> Result<Self> {
        match v {
            1 => Ok(Self::BTree),
            2 => Ok(Self::Bitmap),
            3 => Ok(Self::LabelList),
            4 => Ok(Self::Inverted),
            _ => Err(lance_core::Error::InvalidInput {
                source: format!("invalid scalar index type: {}", v).into(),
                location: snafu::location!(),
            }),
        }
    }

    fn to_builtin(self) -> BuiltinIndexType {
        match self {
            Self::BTree => BuiltinIndexType::BTree,
            Self::Bitmap => BuiltinIndexType::Bitmap,
            Self::LabelList => BuiltinIndexType::LabelList,
            Self::Inverted => BuiltinIndexType::Inverted,
        }
    }

    fn to_index_type(self) -> IndexType {
        match self {
            Self::BTree => IndexType::BTree,
            Self::Bitmap => IndexType::Bitmap,
            Self::LabelList => IndexType::LabelList,
            Self::Inverted => IndexType::Inverted,
        }
    }
}
```

- [ ] **Step 2: Run test to confirm it fails to compile**

```bash
cargo test --test c_api_test test_create_scalar_index_btree 2>&1 | head -20
```

Expected: error `cannot find function lance_dataset_create_scalar_index` and `cannot find function lance_dataset_index_count`.

- [ ] **Step 3: Implement `lance_dataset_create_scalar_index` and `lance_dataset_index_count`**

Append to `src/index.rs`:

```rust
/// Create a scalar index on a column.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_create_scalar_index(
    dataset: *mut LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    index_type: i32,
    params_json: *const c_char,
    replace: bool,
) -> i32 {
    ffi_try!(
        unsafe {
            create_scalar_index_inner(dataset, column, index_name, index_type, params_json, replace)
        },
        neg
    )
}

unsafe fn create_scalar_index_inner(
    dataset: *mut LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    index_type: i32,
    params_json: *const c_char,
    replace: bool,
) -> Result<i32> {
    if dataset.is_null() || column.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "dataset and column must not be NULL".into(),
            location: snafu::location!(),
        });
    }
    let ds = unsafe { &*dataset };
    let column_str = unsafe { helpers::parse_c_string(column)? }.ok_or_else(|| {
        lance_core::Error::InvalidInput {
            source: "column must not be empty".into(),
            location: snafu::location!(),
        }
    })?;
    let name = unsafe { helpers::parse_c_string(index_name)? }.map(|s| s.to_string());
    let params_str = unsafe { helpers::parse_c_string(params_json)? };

    let scalar_type = LanceScalarIndexType::from_c(index_type)?;

    let mut params = ScalarIndexParams::for_builtin(scalar_type.to_builtin());
    if let Some(json) = params_str {
        params.params = Some(json.to_string());
    }

    block_on(async {
        ds.with_mut(|d| {
            block_on(d.create_index(&[column_str], scalar_type.to_index_type(), name, &params, replace))
        })
    })?;

    Ok(0)
}

/// Number of user indexes (excludes system indexes).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_index_count(dataset: *const LanceDataset) -> u64 {
    if dataset.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "dataset is NULL");
        return 0;
    }
    let ds = unsafe { &*dataset };
    let snap = ds.snapshot();
    match block_on(snap.load_indices()) {
        Ok(indices) => {
            crate::error::clear_last_error();
            indices.iter().filter(|i| !lance_index::is_system_index(i)).count() as u64
        }
        Err(err) => {
            crate::error::set_lance_error(&err);
            0
        }
    }
}
```

Note the nested `block_on` in `with_mut`: `with_mut` takes a sync closure; the inner `block_on` drives the async `create_index` future. The outer `block_on(async { ... })` is needed because `with_mut`'s closure must own its async — but since `with_mut` is sync, we can simplify:

```rust
    ds.with_mut(|d| {
        block_on(d.create_index(&[column_str], scalar_type.to_index_type(), name, &params, replace))
    })?;
```

Drop the outer `block_on(async { ... })` wrapper (the snippet above already shows this).

- [ ] **Step 4: Run test to verify it passes**

```bash
cargo test --test c_api_test test_create_scalar_index_btree 2>&1 | tail -15
```

Expected: `test result: ok. 1 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/index.rs src/lib.rs include/lance.h tests/c_api_test.rs
git commit -m "feat(index): add scalar index creation + index_count C API"
```

### Task 6: Add tests for the other three scalar index types

**Files:**
- Modify: `tests/c_api_test.rs`

- [ ] **Step 1: Add three more tests**

The fixture has a `name: Utf8` column we can use for Bitmap (low-cardinality) and Inverted. LabelList wants a list-of-strings — for that we need a new fixture.

Add a new helper in `tests/c_api_test.rs`:

```rust
fn create_label_list_dataset() -> (tempfile::TempDir, String) {
    use arrow_array::ListArray;
    use arrow_array::builder::{ListBuilder, StringBuilder};

    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ll_ds").to_str().unwrap().to_string();

    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new(
            "tags",
            DataType::List(Arc::new(Field::new("item", DataType::Utf8, true))),
            true,
        ),
    ]));

    let mut tag_builder = ListBuilder::new(StringBuilder::new());
    tag_builder.values().append_value("rust");
    tag_builder.values().append_value("ffi");
    tag_builder.append(true);
    tag_builder.values().append_value("cpp");
    tag_builder.append(true);
    let tags: ListArray = tag_builder.finish();

    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![Arc::new(Int32Array::from(vec![1, 2])), Arc::new(tags)],
    )
    .unwrap();

    lance_c::runtime::block_on(async {
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
            &uri,
            None,
        )
        .await
        .unwrap();
    });

    (tmp, uri)
}
```

Then the three tests:

```rust
#[test]
fn test_create_scalar_index_bitmap() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("name");
    let rc = unsafe {
        lance_dataset_create_scalar_index(
            ds, column.as_ptr(), ptr::null(),
            LanceScalarIndexType::Bitmap as i32, ptr::null(), false,
        )
    };
    assert_eq!(rc, 0);
    assert_eq!(unsafe { lance_dataset_index_count(ds) }, 1);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_create_scalar_index_inverted() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("name");
    let rc = unsafe {
        lance_dataset_create_scalar_index(
            ds, column.as_ptr(), ptr::null(),
            LanceScalarIndexType::Inverted as i32, ptr::null(), false,
        )
    };
    assert_eq!(rc, 0, "{}", unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });
    assert_eq!(unsafe { lance_dataset_index_count(ds) }, 1);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_create_scalar_index_label_list() {
    let (_tmp, uri) = create_label_list_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("tags");
    let rc = unsafe {
        lance_dataset_create_scalar_index(
            ds, column.as_ptr(), ptr::null(),
            LanceScalarIndexType::LabelList as i32, ptr::null(), false,
        )
    };
    assert_eq!(rc, 0, "{}", unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });
    assert_eq!(unsafe { lance_dataset_index_count(ds) }, 1);
    unsafe { lance_dataset_close(ds) };
}
```

- [ ] **Step 2: Run all three tests**

```bash
cargo test --test c_api_test test_create_scalar_index_ 2>&1 | tail -15
```

Expected: 4 tests pass (BTree from previous task plus the three new ones).

- [ ] **Step 3: Commit**

```bash
git add tests/c_api_test.rs
git commit -m "test(index): cover Bitmap, Inverted, LabelList scalar indexes"
```

### Task 7: Implement `lance_dataset_drop_index` — TDD

**Files:**
- Modify: `src/index.rs`
- Modify: `tests/c_api_test.rs`

- [ ] **Step 1: Write failing test**

Append to `tests/c_api_test.rs`:

```rust
#[test]
fn test_drop_index() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("id");
    let name = c_str("my_idx");

    unsafe {
        lance_dataset_create_scalar_index(
            ds, column.as_ptr(), name.as_ptr(),
            LanceScalarIndexType::BTree as i32, ptr::null(), false,
        );
    }
    assert_eq!(unsafe { lance_dataset_index_count(ds) }, 1);

    let rc = unsafe { lance_dataset_drop_index(ds, name.as_ptr()) };
    assert_eq!(rc, 0);
    assert_eq!(unsafe { lance_dataset_index_count(ds) }, 0);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_drop_missing_index() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let name = c_str("does_not_exist");
    let rc = unsafe { lance_dataset_drop_index(ds, name.as_ptr()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::NotFound);
    unsafe { lance_dataset_close(ds) };
}
```

- [ ] **Step 2: Confirm it fails to compile**

```bash
cargo test --test c_api_test test_drop_ 2>&1 | head -5
```

Expected: `cannot find function lance_dataset_drop_index`.

- [ ] **Step 3: Implement**

Append to `src/index.rs`:

```rust
/// Drop an index by name.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_drop_index(
    dataset: *mut LanceDataset,
    name: *const c_char,
) -> i32 {
    ffi_try!(unsafe { drop_index_inner(dataset, name) }, neg)
}

unsafe fn drop_index_inner(dataset: *mut LanceDataset, name: *const c_char) -> Result<i32> {
    if dataset.is_null() || name.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "dataset and name must not be NULL".into(),
            location: snafu::location!(),
        });
    }
    let ds = unsafe { &*dataset };
    let name_str = unsafe { helpers::parse_c_string(name)? }.unwrap();

    ds.with_mut(|d| block_on(d.drop_index(name_str)))?;
    Ok(0)
}
```

- [ ] **Step 4: Run tests to verify pass**

```bash
cargo test --test c_api_test test_drop_ 2>&1 | tail -10
```

Expected: 2 tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/index.rs tests/c_api_test.rs
git commit -m "feat(index): add lance_dataset_drop_index"
```

### Task 8: Implement `lance_dataset_index_list_json` — TDD

**Files:**
- Modify: `src/index.rs`
- Modify: `tests/c_api_test.rs`

- [ ] **Step 1: Write failing test**

```rust
#[test]
fn test_list_indices_json() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("id");
    let name = c_str("id_btree");
    unsafe {
        lance_dataset_create_scalar_index(
            ds, column.as_ptr(), name.as_ptr(),
            LanceScalarIndexType::BTree as i32, ptr::null(), false,
        );
    }

    let json_ptr = unsafe { lance_dataset_index_list_json(ds) };
    assert!(!json_ptr.is_null());
    let json = unsafe { std::ffi::CStr::from_ptr(json_ptr).to_str().unwrap().to_string() };
    unsafe { lance_free_string(json_ptr) };

    assert!(json.contains("\"name\":\"id_btree\""), "json was: {}", json);
    assert!(json.contains("\"columns\":[\"id\"]"), "json was: {}", json);
    assert!(json.contains("\"type\""), "json was: {}", json);

    unsafe { lance_dataset_close(ds) };
}
```

- [ ] **Step 2: Confirm fails**

```bash
cargo test --test c_api_test test_list_indices_json 2>&1 | head -5
```

Expected: `cannot find function lance_dataset_index_list_json`.

- [ ] **Step 3: Implement**

Append to `src/index.rs`:

```rust
/// JSON array describing all user indexes. Caller must free with lance_free_string().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_index_list_json(
    dataset: *const LanceDataset,
) -> *const c_char {
    if dataset.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "dataset is NULL");
        return std::ptr::null();
    }
    let ds = unsafe { &*dataset };
    let snap = ds.snapshot();

    let result = (|| -> Result<String> {
        let indices = block_on(snap.load_indices())?;
        let schema = snap.schema();
        let mut entries: Vec<String> = Vec::new();
        for idx in indices.iter() {
            if lance_index::is_system_index(idx) {
                continue;
            }
            let columns: Vec<String> = idx
                .fields
                .iter()
                .filter_map(|fid| schema.field_by_id(*fid).map(|f| f.name.clone()))
                .collect();
            // Determine type: try describe_indices for richer info; fall back to "Unknown".
            let type_str = describe_index_type(&snap, &idx.name);
            let cols_json = columns
                .iter()
                .map(|c| format!("\"{}\"", c.replace('"', "\\\"")))
                .collect::<Vec<_>>()
                .join(",");
            entries.push(format!(
                "{{\"name\":\"{}\",\"uuid\":\"{}\",\"columns\":[{}],\"type\":\"{}\",\"dataset_version\":{}}}",
                idx.name.replace('"', "\\\""),
                idx.uuid,
                cols_json,
                type_str,
                idx.dataset_version,
            ));
        }
        Ok(format!("[{}]", entries.join(",")))
    })();

    match result {
        Ok(json) => {
            crate::error::clear_last_error();
            CString::new(json)
                .map(|s| s.into_raw() as *const c_char)
                .unwrap_or(std::ptr::null())
        }
        Err(err) => {
            crate::error::set_lance_error(&err);
            std::ptr::null()
        }
    }
}

fn describe_index_type(ds: &lance::Dataset, name: &str) -> String {
    block_on(ds.describe_indices(None))
        .ok()
        .and_then(|descs| {
            descs.into_iter().find(|d| d.name() == name).map(|d| d.index_type().to_string())
        })
        .unwrap_or_else(|| "Unknown".to_string())
}
```

- [ ] **Step 4: Run test to verify pass**

```bash
cargo test --test c_api_test test_list_indices_json 2>&1 | tail -10
```

Expected: 1 test passes.

- [ ] **Step 5: Commit**

```bash
git add src/index.rs tests/c_api_test.rs
git commit -m "feat(index): add lance_dataset_index_list_json"
```

### Task 9: Implement `lance_dataset_create_vector_index` (IVF_FLAT) — TDD

**Files:**
- Modify: `src/index.rs`
- Modify: `tests/c_api_test.rs`

- [ ] **Step 1: Add vector dataset fixture**

Append to `tests/c_api_test.rs` near the other helpers:

```rust
fn create_vector_dataset(num_rows: i32, dim: i32) -> (tempfile::TempDir, String) {
    use arrow_array::FixedSizeListArray;
    use arrow_array::builder::{FixedSizeListBuilder, Float32Builder};

    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("vec_ds").to_str().unwrap().to_string();

    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new(
            "embedding",
            DataType::FixedSizeList(
                Arc::new(Field::new("item", DataType::Float32, true)),
                dim,
            ),
            false,
        ),
        Field::new("text", DataType::Utf8, true),
    ]));

    let mut emb_builder = FixedSizeListBuilder::new(Float32Builder::new(), dim);
    let texts: Vec<String> = (0..num_rows).map(|i| format!("doc {i}")).collect();
    let mut rng_seed: u32 = 1;
    for _ in 0..num_rows {
        for _ in 0..dim {
            // simple deterministic pseudo-random in [0,1)
            rng_seed = rng_seed.wrapping_mul(1664525).wrapping_add(1013904223);
            emb_builder.values().append_value((rng_seed as f32) / (u32::MAX as f32));
        }
        emb_builder.append(true);
    }
    let embeddings = emb_builder.finish();
    let text_refs: Vec<&str> = texts.iter().map(|s| s.as_str()).collect();

    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from((0..num_rows).collect::<Vec<_>>())),
            Arc::new(embeddings) as Arc<dyn arrow_array::Array>,
            Arc::new(StringArray::from(text_refs)),
        ],
    )
    .unwrap();

    lance_c::runtime::block_on(async {
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
            &uri,
            None,
        )
        .await
        .unwrap();
    });

    (tmp, uri)
}
```

- [ ] **Step 2: Write failing test**

```rust
#[test]
fn test_create_vector_index_ivf_flat() {
    let (_tmp, uri) = create_vector_dataset(256, 16);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("embedding");
    let params = LanceVectorIndexParams {
        index_type: LanceVectorIndexType::IvfFlat,
        metric: LanceMetricType::L2,
        num_partitions: 8,
        num_sub_vectors: 0,
        num_bits: 0,
        max_iterations: 0,
        hnsw_m: 0,
        hnsw_ef_construction: 0,
        sample_rate: 0,
    };
    let rc = unsafe {
        lance_dataset_create_vector_index(ds, column.as_ptr(), ptr::null(), &params, false)
    };
    assert_eq!(rc, 0, "{}", unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });
    assert_eq!(unsafe { lance_dataset_index_count(ds) }, 1);
    unsafe { lance_dataset_close(ds) };
}
```

- [ ] **Step 3: Confirm fails to compile**

```bash
cargo test --test c_api_test test_create_vector_index_ivf_flat 2>&1 | head -10
```

Expected: `cannot find function lance_dataset_create_vector_index`, `cannot find type LanceVectorIndexParams`.

- [ ] **Step 4: Add Rust mirrors of the C structs/enums and the function**

Append to `src/index.rs`:

```rust
use lance::index::vector::VectorIndexParams as LanceCoreVectorIndexParams;
use lance_index::vector::ivf::IvfBuildParams;
use lance_index::vector::hnsw::builder::HnswBuildParams;
use lance_index::vector::pq::PQBuildParams;
use lance_index::vector::sq::builder::SQBuildParams;
use lance_linalg::distance::DistanceType;

/// Vector index variant tag, mirroring the C enum `LanceVectorIndexType`.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceVectorIndexType {
    IvfFlat     = 101,
    IvfSq       = 102,
    IvfPq       = 103,
    IvfHnswSq   = 104,
    IvfHnswPq   = 105,
    IvfHnswFlat = 106,
}

#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceMetricType {
    L2      = 0,
    Cosine  = 1,
    Dot     = 2,
    Hamming = 3,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceVectorIndexParams {
    pub index_type: LanceVectorIndexType,
    pub metric: LanceMetricType,
    pub num_partitions: u32,
    pub num_sub_vectors: u32,
    pub num_bits: u32,
    pub max_iterations: u32,
    pub hnsw_m: u32,
    pub hnsw_ef_construction: u32,
    pub sample_rate: u32,
}

impl LanceMetricType {
    fn to_distance(self) -> DistanceType {
        match self {
            Self::L2 => DistanceType::L2,
            Self::Cosine => DistanceType::Cosine,
            Self::Dot => DistanceType::Dot,
            Self::Hamming => DistanceType::Hamming,
        }
    }
}

fn require_field(name: &str, value: u32) -> Result<u32> {
    if value == 0 {
        Err(lance_core::Error::InvalidInput {
            source: format!("{} is required for this index type and must be > 0", name).into(),
            location: snafu::location!(),
        })
    } else {
        Ok(value)
    }
}

fn build_ivf(p: &LanceVectorIndexParams) -> Result<IvfBuildParams> {
    let num_partitions = require_field("num_partitions", p.num_partitions)? as usize;
    let mut ivf = IvfBuildParams::new(num_partitions);
    if p.max_iterations != 0 {
        ivf.max_iters = p.max_iterations as usize;
    }
    if p.sample_rate != 0 {
        ivf.sample_rate = p.sample_rate as usize;
    }
    Ok(ivf)
}

fn build_pq(p: &LanceVectorIndexParams) -> Result<PQBuildParams> {
    let num_sub_vectors = require_field("num_sub_vectors", p.num_sub_vectors)? as usize;
    let num_bits = if p.num_bits == 0 { 8 } else { p.num_bits as usize };
    let max_iters = if p.max_iterations == 0 { 50 } else { p.max_iterations as usize };
    Ok(PQBuildParams {
        num_sub_vectors,
        num_bits,
        max_iters,
        ..Default::default()
    })
}

fn build_sq(p: &LanceVectorIndexParams) -> SQBuildParams {
    let mut sq = SQBuildParams::default();
    if p.num_bits != 0 {
        sq.num_bits = p.num_bits as u16;
    }
    if p.sample_rate != 0 {
        sq.sample_rate = p.sample_rate as usize;
    }
    sq
}

fn build_hnsw(p: &LanceVectorIndexParams) -> Result<HnswBuildParams> {
    let m = require_field("hnsw_m", p.hnsw_m)? as usize;
    let mut hnsw = HnswBuildParams::default();
    hnsw.m = m;
    if p.hnsw_ef_construction != 0 {
        hnsw.ef_construction = p.hnsw_ef_construction as usize;
    }
    Ok(hnsw)
}

fn build_vector_params(p: &LanceVectorIndexParams) -> Result<LanceCoreVectorIndexParams> {
    let metric = p.metric.to_distance();
    use LanceVectorIndexType::*;
    let core = match p.index_type {
        IvfFlat => {
            let ivf = build_ivf(p)?;
            LanceCoreVectorIndexParams::with_ivf_flat_params(metric, ivf)
        }
        IvfPq => {
            let ivf = build_ivf(p)?;
            let pq = build_pq(p)?;
            LanceCoreVectorIndexParams::with_ivf_pq_params(metric, ivf, pq)
        }
        IvfSq => {
            let ivf = build_ivf(p)?;
            let sq = build_sq(p);
            LanceCoreVectorIndexParams::with_ivf_sq_params(metric, ivf, sq)
        }
        IvfHnswFlat => {
            let ivf = build_ivf(p)?;
            let hnsw = build_hnsw(p)?;
            LanceCoreVectorIndexParams::ivf_hnsw(metric, ivf, hnsw)
        }
        IvfHnswPq => {
            let ivf = build_ivf(p)?;
            let hnsw = build_hnsw(p)?;
            let pq = build_pq(p)?;
            LanceCoreVectorIndexParams::with_ivf_hnsw_pq_params(metric, ivf, hnsw, pq)
        }
        IvfHnswSq => {
            let ivf = build_ivf(p)?;
            let hnsw = build_hnsw(p)?;
            let sq = build_sq(p);
            LanceCoreVectorIndexParams::with_ivf_hnsw_sq_params(metric, ivf, hnsw, sq)
        }
    };
    Ok(core)
}

/// Create a vector index on a column.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_create_vector_index(
    dataset: *mut LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    params: *const LanceVectorIndexParams,
    replace: bool,
) -> i32 {
    ffi_try!(
        unsafe { create_vector_index_inner(dataset, column, index_name, params, replace) },
        neg
    )
}

unsafe fn create_vector_index_inner(
    dataset: *mut LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    params: *const LanceVectorIndexParams,
    replace: bool,
) -> Result<i32> {
    if dataset.is_null() || column.is_null() || params.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "dataset, column, and params must not be NULL".into(),
            location: snafu::location!(),
        });
    }
    let ds = unsafe { &*dataset };
    let column_str = unsafe { helpers::parse_c_string(column)? }.unwrap();
    let name = unsafe { helpers::parse_c_string(index_name)? }.map(|s| s.to_string());
    let p = unsafe { &*params };
    let core_params = build_vector_params(p)?;

    ds.with_mut(|d| block_on(d.create_index(&[column_str], IndexType::Vector, name, &core_params, replace)))?;
    Ok(0)
}
```

Add `lance-linalg = "3.0.1"` to the `[dependencies]` block of `Cargo.toml`.

- [ ] **Step 5: Run test to verify pass**

```bash
cargo test --test c_api_test test_create_vector_index_ivf_flat 2>&1 | tail -10
```

Expected: 1 test passes.

- [ ] **Step 6: Commit**

```bash
git add src/index.rs tests/c_api_test.rs Cargo.toml Cargo.lock
git commit -m "feat(index): add vector index creation (IVF_FLAT)"
```

### Task 10: Add tests for IVF_PQ, IVF_HNSW_SQ, and required-field validation

**Files:**
- Modify: `tests/c_api_test.rs`

- [ ] **Step 1: Add three more tests**

```rust
#[test]
fn test_create_vector_index_ivf_pq() {
    let (_tmp, uri) = create_vector_dataset(256, 16);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("embedding");
    let params = LanceVectorIndexParams {
        index_type: LanceVectorIndexType::IvfPq,
        metric: LanceMetricType::L2,
        num_partitions: 8,
        num_sub_vectors: 4,
        num_bits: 8,
        max_iterations: 0,
        hnsw_m: 0,
        hnsw_ef_construction: 0,
        sample_rate: 0,
    };
    let rc = unsafe {
        lance_dataset_create_vector_index(ds, column.as_ptr(), ptr::null(), &params, false)
    };
    assert_eq!(rc, 0, "{}", unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_create_vector_index_ivf_hnsw_sq() {
    let (_tmp, uri) = create_vector_dataset(256, 16);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("embedding");
    let params = LanceVectorIndexParams {
        index_type: LanceVectorIndexType::IvfHnswSq,
        metric: LanceMetricType::L2,
        num_partitions: 8,
        num_sub_vectors: 0,
        num_bits: 0,
        max_iterations: 0,
        hnsw_m: 16,
        hnsw_ef_construction: 100,
        sample_rate: 0,
    };
    let rc = unsafe {
        lance_dataset_create_vector_index(ds, column.as_ptr(), ptr::null(), &params, false)
    };
    assert_eq!(rc, 0, "{}", unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_vector_index_missing_required_param() {
    let (_tmp, uri) = create_vector_dataset(256, 16);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("embedding");
    let params = LanceVectorIndexParams {
        index_type: LanceVectorIndexType::IvfPq,
        metric: LanceMetricType::L2,
        num_partitions: 8,
        num_sub_vectors: 0,    // missing!
        num_bits: 0, max_iterations: 0, hnsw_m: 0,
        hnsw_ef_construction: 0, sample_rate: 0,
    };
    let rc = unsafe {
        lance_dataset_create_vector_index(ds, column.as_ptr(), ptr::null(), &params, false)
    };
    assert_eq!(rc, -1);
    let msg = unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy().into_owned()
    };
    assert!(msg.contains("num_sub_vectors"), "msg was: {}", msg);
    unsafe { lance_dataset_close(ds) };
}
```

- [ ] **Step 2: Run all three tests**

```bash
cargo test --test c_api_test test_create_vector_index_ test_vector_index_missing 2>&1 | tail -10
```

Expected: 3 tests pass.

- [ ] **Step 3: Commit**

```bash
git add tests/c_api_test.rs
git commit -m "test(index): cover IVF_PQ, IVF_HNSW_SQ, and missing-param validation"
```

### Task 11: Test replace=true / replace=false behavior

**Files:**
- Modify: `tests/c_api_test.rs`

- [ ] **Step 1: Add tests**

```rust
#[test]
fn test_create_index_replace_true() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("id");
    let name = c_str("dup");
    unsafe {
        lance_dataset_create_scalar_index(
            ds, column.as_ptr(), name.as_ptr(),
            LanceScalarIndexType::BTree as i32, ptr::null(), false,
        );
    }
    let rc = unsafe {
        lance_dataset_create_scalar_index(
            ds, column.as_ptr(), name.as_ptr(),
            LanceScalarIndexType::BTree as i32, ptr::null(), true,
        )
    };
    assert_eq!(rc, 0, "replace=true should succeed");
    assert_eq!(unsafe { lance_dataset_index_count(ds) }, 1);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_create_index_replace_false_conflicts() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("id");
    let name = c_str("dup2");
    unsafe {
        lance_dataset_create_scalar_index(
            ds, column.as_ptr(), name.as_ptr(),
            LanceScalarIndexType::BTree as i32, ptr::null(), false,
        );
    }
    let rc = unsafe {
        lance_dataset_create_scalar_index(
            ds, column.as_ptr(), name.as_ptr(),
            LanceScalarIndexType::BTree as i32, ptr::null(), false,
        )
    };
    assert_eq!(rc, -1);
    let code = lance_last_error_code();
    assert!(code == LanceErrorCode::IndexError || code == LanceErrorCode::InvalidArgument,
        "expected IndexError or InvalidArgument, got {:?}", code);
    unsafe { lance_dataset_close(ds) };
}
```

- [ ] **Step 2: Run + commit**

```bash
cargo test --test c_api_test test_create_index_replace 2>&1 | tail -10
```

Expected: 2 tests pass.

```bash
git add tests/c_api_test.rs
git commit -m "test(index): cover replace=true and replace=false conflict"
```

### Task 12: Add C++ wrappers for index lifecycle

**Files:**
- Modify: `include/lance.hpp`

- [ ] **Step 1: Add wrapper methods to the `lance::Dataset` class**

In `include/lance.hpp`, inside the `class Dataset { ... };` block (before the closing brace), append:

```cpp
    /// Create a vector index on a column.
    void create_vector_index(const std::string& column,
                             const LanceVectorIndexParams& params,
                             const std::string& name = "",
                             bool replace = false) {
        const char* name_c = name.empty() ? nullptr : name.c_str();
        if (lance_dataset_create_vector_index(handle_.get(), column.c_str(),
                                               name_c, &params, replace) != 0)
            check_error();
    }

    /// Create a scalar index on a column.
    void create_scalar_index(const std::string& column,
                             LanceScalarIndexType index_type,
                             const std::string& name = "",
                             const std::string& params_json = "",
                             bool replace = false) {
        const char* name_c = name.empty() ? nullptr : name.c_str();
        const char* json_c = params_json.empty() ? nullptr : params_json.c_str();
        if (lance_dataset_create_scalar_index(handle_.get(), column.c_str(),
                                               name_c, (int32_t)index_type,
                                               json_c, replace) != 0)
            check_error();
    }

    /// Drop an index by name.
    void drop_index(const std::string& name) {
        if (lance_dataset_drop_index(handle_.get(), name.c_str()) != 0)
            check_error();
    }

    /// Number of user indexes (excludes system indexes).
    uint64_t index_count() const {
        uint64_t n = lance_dataset_index_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return n;
    }

    /// JSON array describing all user indexes.
    std::string list_indices_json() const {
        const char* json = lance_dataset_index_list_json(handle_.get());
        if (!json) check_error();
        std::string out(json);
        lance_free_string(json);
        return out;
    }
```

Note: `lance_dataset_create_scalar_index` takes `int32_t index_type` in the C signature, so we cast the C++ enum.

- [ ] **Step 2: Verify the C/C++ compile-and-run test still builds**

```bash
cargo test --test compile_and_run_test -- --ignored 2>&1 | tail -20
```

Expected: PASS (existing C/C++ test exercises lance.hpp; this change only adds new methods).

- [ ] **Step 3: Commit**

```bash
git add include/lance.hpp
git commit -m "feat(cpp): add Dataset::create_*_index, drop_index, list_indices_json"
```

### Task 13: Add C++ smoke test for index lifecycle

**Files:**
- Modify: `tests/cpp/test_cpp_api.cpp`

- [ ] **Step 1: Read the existing test file to understand its structure**

```bash
head -80 tests/cpp/test_cpp_api.cpp
```

Note the existing pattern (test fixtures, assertion style).

- [ ] **Step 2: Append a new test for index creation**

Append to `tests/cpp/test_cpp_api.cpp` (before any `main()` if present, or in the test list):

```cpp
static void test_create_scalar_index_smoke(const std::string& uri) {
    auto ds = lance::Dataset::open(uri);
    ds.create_scalar_index("id", LANCE_SCALAR_BTREE, "id_idx");
    if (ds.index_count() != 1) {
        fprintf(stderr, "expected 1 index, got %llu\n",
                (unsigned long long)ds.index_count());
        std::exit(1);
    }
    auto json = ds.list_indices_json();
    if (json.find("id_idx") == std::string::npos) {
        fprintf(stderr, "json missing index name: %s\n", json.c_str());
        std::exit(1);
    }
    ds.drop_index("id_idx");
    if (ds.index_count() != 0) {
        fprintf(stderr, "expected 0 indexes after drop\n");
        std::exit(1);
    }
}
```

Add a call to `test_create_scalar_index_smoke(uri);` from the `main()` after the existing smoke calls. (If the file uses a different test discovery scheme, follow that.)

- [ ] **Step 3: Run the C++ compile test**

```bash
cargo test --test compile_and_run_test -- --ignored 2>&1 | tail -20
```

Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add tests/cpp/test_cpp_api.cpp
git commit -m "test(cpp): smoke test for index lifecycle"
```

### Task 14: Update README Phase 2 checklist

**Files:**
- Modify: `README.md:32-39`

- [ ] **Step 1: Tick the indexing rows**

Change the Phase 2 table rows that are now complete:

```markdown
| [ ] | Vector search | Nearest-neighbor via scanner with metric/k/nprobes |
| [ ] | Full-text search | FTS queries through scanner interface |
| [x] | Vector index creation | IVF_PQ, IVF_FLAT, IVF_SQ, HNSW variants |
| [x] | Scalar index creation | BTree, Bitmap, Inverted, Label-List indexes |
| [x] | Index management | List and drop index operations |
| [ ] | C++ wrappers | `create_vector_index()` and `create_scalar_index()` methods |
```

(C++ wrappers row stays unchecked because the search-side wrappers come in PR 2/3.)

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: tick Phase 2 indexing rows in README"
```

### Task 15: PR 1 — final integration check + open PR

- [ ] **Step 1: Run full check, lint, and tests**

```bash
cargo fmt
cargo clippy --all-targets -- -D warnings
cargo test
cargo test --test compile_and_run_test -- --ignored
```

All must pass.

- [ ] **Step 2: Push branch and open PR**

```bash
git push -u origin HEAD
gh pr create --title "feat(index): vector & scalar index lifecycle" --body "$(cat <<'EOF'
## Summary
- Add LanceDataset::snapshot() / with_mut() (RwLock<Arc<Dataset>>) so create/drop_index can mutate while existing scanners keep their Arc snapshot view.
- Add lance_dataset_create_vector_index (IVF_FLAT/PQ/SQ + IVF_HNSW_FLAT/PQ/SQ).
- Add lance_dataset_create_scalar_index (BTree, Bitmap, LabelList, Inverted).
- Add lance_dataset_drop_index, _index_count, _index_list_json.
- C++ wrappers in lance.hpp.

Spec: docs/superpowers/specs/2026-04-23-phase2-vector-search-indexing-design.md
Plan: docs/superpowers/plans/2026-04-23-phase2-vector-search-indexing.md (PR 1 = Tasks 1–15)

## Test plan
- [x] cargo test
- [x] cargo test --test compile_and_run_test -- --ignored
- [x] cargo clippy --all-targets -- -D warnings
EOF
)"
```

---

# PR 2 — Vector Search

> Wait until PR 1 is merged, then start this PR from main.

### Task 16: Add vector-search fields to `LanceScanner` + scanner setter knobs

**Files:**
- Modify: `src/scanner.rs`

- [ ] **Step 1: Extend the LanceScanner struct**

In `src/scanner.rs`, add new optional fields to `LanceScanner`:

```rust
pub struct LanceScanner {
    // ... existing fields ...
    nearest: Option<NearestQuery>,
    nprobes: Option<u32>,
    refine_factor: Option<u32>,
    ef: Option<u32>,
    metric_override: Option<crate::index::LanceMetricType>,
    use_index: Option<bool>,
    prefilter: bool,
}

struct NearestQuery {
    column: String,
    query: arrow_array::ArrayRef,
    k: u32,
}
```

Update `LanceScanner::new` to initialize all new fields to `None`/`false`.

- [ ] **Step 2: Implement the setter knob functions**

Append to `src/scanner.rs`:

```rust
macro_rules! scanner_set_u32 {
    ($name:ident, $field:ident) => {
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $name(scanner: *mut LanceScanner, value: u32) -> i32 {
            if scanner.is_null() {
                set_last_error(LanceErrorCode::InvalidArgument, "scanner is NULL");
                return -1;
            }
            unsafe { (*scanner).$field = Some(value); }
            crate::error::clear_last_error();
            0
        }
    };
}

scanner_set_u32!(lance_scanner_set_nprobes, nprobes);
scanner_set_u32!(lance_scanner_set_refine_factor, refine_factor);
scanner_set_u32!(lance_scanner_set_ef, ef);

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_set_metric(
    scanner: *mut LanceScanner,
    metric: i32,
) -> i32 {
    if scanner.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "scanner is NULL");
        return -1;
    }
    let m = match metric {
        0 => crate::index::LanceMetricType::L2,
        1 => crate::index::LanceMetricType::Cosine,
        2 => crate::index::LanceMetricType::Dot,
        3 => crate::index::LanceMetricType::Hamming,
        _ => {
            set_last_error(LanceErrorCode::InvalidArgument,
                format!("invalid metric: {}", metric));
            return -1;
        }
    };
    unsafe { (*scanner).metric_override = Some(m); }
    crate::error::clear_last_error();
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_set_use_index(
    scanner: *mut LanceScanner,
    enable: bool,
) -> i32 {
    if scanner.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "scanner is NULL");
        return -1;
    }
    unsafe { (*scanner).use_index = Some(enable); }
    crate::error::clear_last_error();
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_set_prefilter(
    scanner: *mut LanceScanner,
    enable: bool,
) -> i32 {
    if scanner.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "scanner is NULL");
        return -1;
    }
    unsafe { (*scanner).prefilter = enable; }
    crate::error::clear_last_error();
    0
}
```

- [ ] **Step 3: Add header declarations**

In `include/lance.h`, append (before the FTS section comes in PR 3):

```c
/* ─── Vector search (Phase 2) ─── */

/**
 * Set the k-NN query on the scanner.
 * @param column        Vector column (FixedSizeList<element_type>).
 * @param query_data    Pointer to a single query vector of length `query_len`.
 * @param query_len     Number of elements in the query (= column dim).
 * @param element_type  Element type of the query (must match column).
 * @param k             Number of nearest neighbors to return.
 * @return 0 on success, -1 on error.
 */
int32_t lance_scanner_nearest(
    LanceScanner* scanner,
    const char* column,
    const void* query_data,
    size_t query_len,
    LanceDataType element_type,
    uint32_t k
);

int32_t lance_scanner_set_nprobes(LanceScanner* scanner, uint32_t n);
int32_t lance_scanner_set_refine_factor(LanceScanner* scanner, uint32_t f);
int32_t lance_scanner_set_ef(LanceScanner* scanner, uint32_t e);
int32_t lance_scanner_set_metric(LanceScanner* scanner, LanceMetricType metric);
int32_t lance_scanner_set_use_index(LanceScanner* scanner, bool enable);
int32_t lance_scanner_set_prefilter(LanceScanner* scanner, bool enable);
```

- [ ] **Step 4: Verify it compiles**

```bash
cargo check --all-targets
```

Expected: compiles with warnings about unused fields (will be used in next task).

- [ ] **Step 5: Commit**

```bash
git add src/scanner.rs include/lance.h
git commit -m "feat(scanner): add vector search field plumbing + setter knobs"
```

### Task 17: Implement `lance_scanner_nearest` and apply at materialize time — TDD

**Files:**
- Modify: `src/scanner.rs`
- Modify: `tests/c_api_test.rs`

- [ ] **Step 1: Write failing test**

Append to `tests/c_api_test.rs`:

```rust
#[test]
fn test_scanner_nearest_brute_force() {
    let (_tmp, uri) = create_vector_dataset(64, 8);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    assert!(!scanner.is_null());

    let query: Vec<f32> = (0..8).map(|i| i as f32 * 0.1).collect();
    let column = c_str("embedding");
    let rc = unsafe {
        lance_scanner_nearest(
            scanner,
            column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void,
            query.len(),
            LanceDataType::Float32 as i32,
            5,
        )
    };
    assert_eq!(rc, 0, "{}", unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });

    let mut stream = FFI_ArrowArrayStream::empty();
    let rc2 = unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) };
    assert_eq!(rc2, 0);

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream as *mut _).unwrap() };
    let mut total = 0;
    let mut saw_distance = false;
    let schema = reader.schema();
    if schema.field_with_name("_distance").is_ok() {
        saw_distance = true;
    }
    for batch in reader {
        let b = batch.unwrap();
        total += b.num_rows();
    }
    assert!(saw_distance, "_distance column missing from schema");
    assert_eq!(total, 5, "expected k=5 results");

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}
```

You also need an `LanceDataType` enum visible to the test. Add to `src/scanner.rs`:

```rust
/// Data type tag for query vectors, mirroring the C enum `LanceDataType`.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceDataType {
    Float32 = 0,
    Float16 = 1,
    Float64 = 2,
    UInt8   = 3,
    Int8    = 4,
}
```

- [ ] **Step 2: Confirm fails**

```bash
cargo test --test c_api_test test_scanner_nearest_brute_force 2>&1 | head -10
```

Expected: `cannot find function lance_scanner_nearest`.

- [ ] **Step 3: Implement `lance_scanner_nearest`**

Append to `src/scanner.rs`:

```rust
/// Set the k-NN query on the scanner.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_nearest(
    scanner: *mut LanceScanner,
    column: *const c_char,
    query_data: *const c_void,
    query_len: usize,
    element_type: i32,
    k: u32,
) -> i32 {
    ffi_try!(
        unsafe { scanner_nearest_inner(scanner, column, query_data, query_len, element_type, k) },
        neg
    )
}

unsafe fn scanner_nearest_inner(
    scanner: *mut LanceScanner,
    column: *const c_char,
    query_data: *const c_void,
    query_len: usize,
    element_type: i32,
    k: u32,
) -> Result<i32> {
    if scanner.is_null() || column.is_null() || query_data.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "scanner, column, and query_data must not be NULL".into(),
            location: snafu::location!(),
        });
    }
    if k == 0 {
        return Err(lance_core::Error::InvalidInput {
            source: "k must be > 0".into(),
            location: snafu::location!(),
        });
    }
    let s = unsafe { &mut *scanner };
    let column_str = unsafe { helpers::parse_c_string(column)? }.unwrap();

    let dtype = match element_type {
        0 => LanceDataType::Float32,
        1 => LanceDataType::Float16,
        2 => LanceDataType::Float64,
        3 => LanceDataType::UInt8,
        4 => LanceDataType::Int8,
        _ => return Err(lance_core::Error::InvalidInput {
            source: format!("invalid element_type: {}", element_type).into(),
            location: snafu::location!(),
        }),
    };

    let query: arrow_array::ArrayRef = match dtype {
        LanceDataType::Float32 => {
            let slice = unsafe { std::slice::from_raw_parts(query_data as *const f32, query_len) };
            std::sync::Arc::new(arrow_array::Float32Array::from(slice.to_vec()))
        }
        LanceDataType::Float64 => {
            let slice = unsafe { std::slice::from_raw_parts(query_data as *const f64, query_len) };
            std::sync::Arc::new(arrow_array::Float64Array::from(slice.to_vec()))
        }
        LanceDataType::UInt8 => {
            let slice = unsafe { std::slice::from_raw_parts(query_data as *const u8, query_len) };
            std::sync::Arc::new(arrow_array::UInt8Array::from(slice.to_vec()))
        }
        LanceDataType::Int8 => {
            let slice = unsafe { std::slice::from_raw_parts(query_data as *const i8, query_len) };
            std::sync::Arc::new(arrow_array::Int8Array::from(slice.to_vec()))
        }
        LanceDataType::Float16 => {
            let raw = unsafe { std::slice::from_raw_parts(query_data as *const u16, query_len) };
            let values: Vec<half::f16> =
                raw.iter().map(|bits| half::f16::from_bits(*bits)).collect();
            std::sync::Arc::new(arrow_array::Float16Array::from(values))
        }
    };

    s.nearest = Some(NearestQuery {
        column: column_str.to_string(),
        query,
        k,
    });
    Ok(0)
}
```

(`half::f16` is already a transitive dep via arrow; if not, add `half = "2"` to `Cargo.toml` and `cargo build`.)

- [ ] **Step 4: Apply nearest in `build_scanner` and `materialize_stream`**

Inside `build_scanner` (and the parallel `materialize_stream`) in `src/scanner.rs`, add this block after the existing fragment_filter application:

```rust
        if let Some(n) = &self.nearest {
            scanner.nearest(&n.column, n.query.as_ref(), n.k as usize)?;
            if let Some(np) = self.nprobes {
                scanner.nprobes(np as usize);
            }
            if let Some(rf) = self.refine_factor {
                scanner.refine(rf);
            }
            if let Some(ef) = self.ef {
                scanner.ef(ef as usize);
            }
            if let Some(m) = self.metric_override {
                scanner.distance_metric(m.to_distance());
            }
            if let Some(ui) = self.use_index {
                scanner.use_index(ui);
            }
            if self.prefilter {
                scanner.prefilter(true);
            }
        }
```

You'll need `LanceMetricType::to_distance` to be `pub(crate)`. In `src/index.rs` change `fn to_distance(self)` to `pub(crate) fn to_distance(self)`.

The Lance Scanner does have a `prefilter(bool)` method — verify in `lance::dataset::scanner::Scanner`. (It's at `Scanner::prefilter`, used in many other code paths.)

- [ ] **Step 5: Run test to verify pass**

```bash
cargo test --test c_api_test test_scanner_nearest_brute_force 2>&1 | tail -10
```

Expected: 1 test passes.

- [ ] **Step 6: Commit**

```bash
git add src/scanner.rs src/index.rs tests/c_api_test.rs Cargo.toml Cargo.lock
git commit -m "feat(scanner): implement lance_scanner_nearest (k-NN search)"
```

### Task 18: Add tests for vector search with an IVF_PQ index, knobs, dim/type mismatch

**Files:**
- Modify: `tests/c_api_test.rs`

- [ ] **Step 1: Add tests**

```rust
#[test]
fn test_scanner_nearest_with_ivf_pq_index() {
    let (_tmp, uri) = create_vector_dataset(512, 16);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("embedding");
    let params = LanceVectorIndexParams {
        index_type: LanceVectorIndexType::IvfPq,
        metric: LanceMetricType::L2,
        num_partitions: 8,
        num_sub_vectors: 4,
        num_bits: 8,
        max_iterations: 0, hnsw_m: 0, hnsw_ef_construction: 0, sample_rate: 0,
    };
    unsafe {
        lance_dataset_create_vector_index(ds, column.as_ptr(), ptr::null(), &params, false);
    }

    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    let query: Vec<f32> = vec![0.5; 16];
    unsafe {
        lance_scanner_nearest(
            scanner, column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void, 16,
            LanceDataType::Float32 as i32, 10,
        );
        lance_scanner_set_nprobes(scanner, 4);
    }

    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) }, 0);
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream as *mut _).unwrap() };
    let mut total = 0;
    for batch in reader {
        total += batch.unwrap().num_rows();
    }
    assert_eq!(total, 10);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_nearest_dim_mismatch() {
    let (_tmp, uri) = create_vector_dataset(64, 8);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    let query: Vec<f32> = vec![0.0; 4]; // wrong dim
    let column = c_str("embedding");

    // nearest call accepts the query (dim check happens in build_scanner)
    unsafe {
        lance_scanner_nearest(
            scanner, column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void, 4,
            LanceDataType::Float32 as i32, 5,
        );
    }
    let mut stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) };
    assert_eq!(rc, -1);
    let msg = unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy().into_owned()
    };
    assert!(msg.to_lowercase().contains("dim"), "msg was: {}", msg);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_nearest_filter_postfilter() {
    let (_tmp, uri) = create_vector_dataset(64, 8);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let filter = c_str("id < 10");
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), filter.as_ptr()) };
    let query: Vec<f32> = vec![0.5; 8];
    let column = c_str("embedding");
    unsafe {
        lance_scanner_nearest(
            scanner, column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void, 8,
            LanceDataType::Float32 as i32, 20,
        );
    }
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) }, 0);
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream as *mut _).unwrap() };
    let mut total = 0;
    for b in reader { total += b.unwrap().num_rows(); }
    // Post-filter on top-20 nearest: count is 0..20 depending on data;
    // we just assert the call works and ≤ 20.
    assert!(total <= 20);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}
```

- [ ] **Step 2: Run tests**

```bash
cargo test --test c_api_test test_scanner_nearest 2>&1 | tail -10
```

Expected: all 4 nearest tests pass.

- [ ] **Step 3: Commit**

```bash
git add tests/c_api_test.rs
git commit -m "test(scanner): nearest with IVF_PQ, knobs, dim mismatch, filter"
```

### Task 19: Multi-fragment + NULL-safety tests for nearest

**Files:**
- Modify: `tests/c_api_test.rs`

- [ ] **Step 1: Add tests**

```rust
#[test]
fn test_scanner_nearest_multi_fragment() {
    use arrow_array::FixedSizeListArray;
    use arrow_array::builder::{FixedSizeListBuilder, Float32Builder};

    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("multifrag").to_str().unwrap().to_string();
    let dim: i32 = 8;
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new(
            "embedding",
            DataType::FixedSizeList(Arc::new(Field::new("item", DataType::Float32, true)), dim),
            false,
        ),
    ]));

    let mut batches = Vec::new();
    for frag in 0..2 {
        let mut emb = FixedSizeListBuilder::new(Float32Builder::new(), dim);
        let ids: Vec<i32> = (0..32).map(|i| frag * 32 + i).collect();
        for _ in 0..32 {
            for _ in 0..dim { emb.values().append_value(0.5); }
            emb.append(true);
        }
        batches.push(
            RecordBatch::try_new(
                schema.clone(),
                vec![Arc::new(Int32Array::from(ids)), Arc::new(emb.finish())],
            ).unwrap()
        );
    }

    lance_c::runtime::block_on(async {
        let mut writer = Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batches[0].clone())], schema.clone()),
            &uri, None,
        ).await.unwrap();
        let _ = writer; // first write
        let mut params = lance::dataset::WriteParams::default();
        params.mode = lance::dataset::WriteMode::Append;
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batches[1].clone())], schema),
            &uri, Some(params),
        ).await.unwrap();
    });

    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    assert_eq!(unsafe { lance_dataset_fragment_count(ds) }, 2);

    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    let column = c_str("embedding");
    let query: Vec<f32> = vec![0.5; 8];
    unsafe {
        lance_scanner_nearest(
            scanner, column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void, 8,
            LanceDataType::Float32 as i32, 20,
        );
    }
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) }, 0);
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream as *mut _).unwrap() };
    let mut total = 0;
    for b in reader { total += b.unwrap().num_rows(); }
    assert_eq!(total, 20);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_nearest_null_safety() {
    let column = c_str("embedding");
    let query: Vec<f32> = vec![0.0; 8];
    // NULL scanner
    let rc = unsafe {
        lance_scanner_nearest(
            ptr::null_mut(), column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void, 8,
            LanceDataType::Float32 as i32, 5,
        )
    };
    assert_eq!(rc, -1);

    // Build a valid scanner
    let (_tmp, uri) = create_vector_dataset(8, 8);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };

    // NULL column
    let rc2 = unsafe {
        lance_scanner_nearest(
            scanner, ptr::null(),
            query.as_ptr() as *const std::ffi::c_void, 8,
            LanceDataType::Float32 as i32, 5,
        )
    };
    assert_eq!(rc2, -1);

    // NULL query_data
    let rc3 = unsafe {
        lance_scanner_nearest(
            scanner, column.as_ptr(),
            ptr::null(), 8,
            LanceDataType::Float32 as i32, 5,
        )
    };
    assert_eq!(rc3, -1);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}
```

- [ ] **Step 2: Run tests**

```bash
cargo test --test c_api_test test_scanner_nearest 2>&1 | tail -10
```

Expected: 6 nearest tests now pass.

- [ ] **Step 3: Commit**

```bash
git add tests/c_api_test.rs
git commit -m "test(scanner): multi-fragment + NULL-safety for nearest"
```

### Task 20: Add C++ wrappers for vector search + smoke test

**Files:**
- Modify: `include/lance.hpp`
- Modify: `tests/cpp/test_cpp_api.cpp`

- [ ] **Step 1: Extend `lance::Scanner` in `lance.hpp`**

Inside the `class Scanner { ... };` block, append (before the closing brace):

```cpp
    /// k-NN search (Float32 sugar).
    Scanner& nearest(const std::string& column, const float* q, size_t dim, uint32_t k) {
        if (lance_scanner_nearest(handle_.get(), column.c_str(),
                                   q, dim, LANCE_DTYPE_FLOAT32, k) != 0)
            check_error();
        return *this;
    }

    /// k-NN search (typed).
    Scanner& nearest(const std::string& column, const void* q, size_t dim,
                     LanceDataType dtype, uint32_t k) {
        if (lance_scanner_nearest(handle_.get(), column.c_str(),
                                   q, dim, dtype, k) != 0)
            check_error();
        return *this;
    }

    Scanner& nprobes(uint32_t n) {
        if (lance_scanner_set_nprobes(handle_.get(), n) != 0) check_error();
        return *this;
    }
    Scanner& refine_factor(uint32_t f) {
        if (lance_scanner_set_refine_factor(handle_.get(), f) != 0) check_error();
        return *this;
    }
    Scanner& ef(uint32_t e) {
        if (lance_scanner_set_ef(handle_.get(), e) != 0) check_error();
        return *this;
    }
    Scanner& metric(LanceMetricType m) {
        if (lance_scanner_set_metric(handle_.get(), m) != 0) check_error();
        return *this;
    }
    Scanner& use_index(bool enable) {
        if (lance_scanner_set_use_index(handle_.get(), enable) != 0) check_error();
        return *this;
    }
    Scanner& prefilter(bool enable) {
        if (lance_scanner_set_prefilter(handle_.get(), enable) != 0) check_error();
        return *this;
    }
```

- [ ] **Step 2: Add C++ smoke test**

Append to `tests/cpp/test_cpp_api.cpp` (and call from `main`):

```cpp
static void test_nearest_smoke(const std::string& uri) {
    auto ds = lance::Dataset::open(uri);
    LanceVectorIndexParams params{};
    params.index_type = LANCE_INDEX_IVF_FLAT;
    params.metric = LANCE_METRIC_L2;
    params.num_partitions = 4;
    ds.create_vector_index("embedding", params);

    float q[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    ArrowArrayStream stream;
    ds.scan().nearest("embedding", q, 8, 5).nprobes(2).to_arrow_stream(&stream);
    if (stream.release) stream.release(&stream);
}
```

The test fixture for vector data needs to be created in C++ — for the smoke test you can write a small helper using `lance::Dataset::open` against a path that the harness pre-populates. Inspect `tests/cpp/test_cpp_api.cpp` to see how existing tests obtain a test URI; pass through the same mechanism.

- [ ] **Step 3: Run C++ compile test**

```bash
cargo test --test compile_and_run_test -- --ignored 2>&1 | tail -20
```

Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add include/lance.hpp tests/cpp/test_cpp_api.cpp
git commit -m "feat(cpp): Scanner::nearest and search knobs + smoke test"
```

### Task 21: Update README (vector search row)

**Files:**
- Modify: `README.md:32-39`

- [ ] **Step 1: Tick the row**

```markdown
| [x] | Vector search | Nearest-neighbor via scanner with metric/k/nprobes |
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: tick Phase 2 vector search row"
```

### Task 22: PR 2 — final check + open PR

- [ ] **Step 1: Full test sweep**

```bash
cargo fmt
cargo clippy --all-targets -- -D warnings
cargo test
cargo test --test compile_and_run_test -- --ignored
```

- [ ] **Step 2: Open PR**

```bash
git push -u origin HEAD
gh pr create --title "feat(scanner): k-NN vector search via scanner builder" --body "$(cat <<'EOF'
## Summary
- lance_scanner_nearest with typed query (f32/f16/f64/u8/i8).
- Knobs: nprobes, refine_factor, ef, metric, use_index, prefilter.
- C++ Scanner::nearest fluent overloads + smoke test.

Spec: docs/superpowers/specs/2026-04-23-phase2-vector-search-indexing-design.md
Plan: docs/superpowers/plans/2026-04-23-phase2-vector-search-indexing.md (PR 2 = Tasks 16–22)

## Test plan
- [x] cargo test
- [x] Multi-fragment + NULL safety covered.
- [x] C/C++ compile-and-run test passes.
EOF
)"
```

---

# PR 3 — Full-Text Search

> Wait until PR 2 is merged.

### Task 23: Add FTS field + `lance_scanner_full_text_search` — TDD

**Files:**
- Modify: `src/scanner.rs`
- Modify: `include/lance.h`
- Modify: `tests/c_api_test.rs`

- [ ] **Step 1: Add FTS field to LanceScanner**

In `src/scanner.rs`:

```rust
pub struct LanceScanner {
    // ... existing fields ...
    fts_query: Option<lance_index::scalar::FullTextSearchQuery>,
}
```

Initialize `fts_query: None` in `LanceScanner::new`.

Apply in `build_scanner` / `materialize_stream` after the `nearest` block:

```rust
        if let Some(fts) = &self.fts_query {
            scanner.full_text_search(fts.clone())?;
        }
```

- [ ] **Step 2: Add header declaration**

In `include/lance.h`:

```c
/* ─── Full-text search (Phase 2) ─── */

/**
 * Set a BM25 full-text search query on the scanner.
 * @param query              Query string (terms).
 * @param columns            NULL-terminated array of columns, or NULL for all FTS-indexed columns.
 * @param max_fuzzy_distance 0 = exact match; >0 = MatchQuery::with_fuzziness.
 * @return 0 on success, -1 on error.
 */
int32_t lance_scanner_full_text_search(
    LanceScanner* scanner,
    const char* query,
    const char* const* columns,
    uint32_t max_fuzzy_distance
);
```

- [ ] **Step 3: Write failing test**

Append to `tests/c_api_test.rs`:

```rust
#[test]
fn test_scanner_full_text_search() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("name");
    // Build inverted index on `name`
    unsafe {
        lance_dataset_create_scalar_index(
            ds, column.as_ptr(), ptr::null(),
            LanceScalarIndexType::Inverted as i32, ptr::null(), false,
        );
    }
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    let q = c_str("alice");
    let cols = [column.as_ptr(), ptr::null()];
    let rc = unsafe {
        lance_scanner_full_text_search(scanner, q.as_ptr(), cols.as_ptr(), 0)
    };
    assert_eq!(rc, 0, "{}", unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) }, 0);
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream as *mut _).unwrap() };
    let schema = reader.schema();
    assert!(schema.field_with_name("_score").is_ok(), "_score column missing");
    let mut total = 0;
    for b in reader { total += b.unwrap().num_rows(); }
    assert!(total >= 1, "expected at least 1 hit for 'alice'");
    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}
```

- [ ] **Step 4: Confirm fails**

```bash
cargo test --test c_api_test test_scanner_full_text_search 2>&1 | head -10
```

Expected: `cannot find function lance_scanner_full_text_search`.

- [ ] **Step 5: Implement `lance_scanner_full_text_search`**

Append to `src/scanner.rs`:

```rust
use lance_index::scalar::FullTextSearchQuery;

/// Set a BM25 full-text search query on the scanner.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_full_text_search(
    scanner: *mut LanceScanner,
    query: *const c_char,
    columns: *const *const c_char,
    max_fuzzy_distance: u32,
) -> i32 {
    ffi_try!(
        unsafe { fts_inner(scanner, query, columns, max_fuzzy_distance) },
        neg
    )
}

unsafe fn fts_inner(
    scanner: *mut LanceScanner,
    query: *const c_char,
    columns: *const *const c_char,
    max_fuzzy_distance: u32,
) -> Result<i32> {
    if scanner.is_null() || query.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "scanner and query must not be NULL".into(),
            location: snafu::location!(),
        });
    }
    let s = unsafe { &mut *scanner };
    let query_str = unsafe { helpers::parse_c_string(query)? }.unwrap().to_string();
    let cols = unsafe { helpers::parse_c_string_array(columns)? };

    let mut fts = if max_fuzzy_distance > 0 {
        FullTextSearchQuery::new_fuzzy(query_str, Some(max_fuzzy_distance))
    } else {
        FullTextSearchQuery::new(query_str)
    };

    if let Some(cols) = cols {
        if !cols.is_empty() {
            fts = fts.with_columns(&cols)?;
        }
    }

    s.fts_query = Some(fts);
    Ok(0)
}
```

- [ ] **Step 6: Run test to verify pass**

```bash
cargo test --test c_api_test test_scanner_full_text_search 2>&1 | tail -10
```

Expected: 1 test passes.

- [ ] **Step 7: Commit**

```bash
git add src/scanner.rs include/lance.h tests/c_api_test.rs
git commit -m "feat(scanner): add lance_scanner_full_text_search (BM25)"
```

### Task 24: Add fuzzy + mutual-exclusion tests

**Files:**
- Modify: `tests/c_api_test.rs`
- Modify: `src/scanner.rs`

- [ ] **Step 1: Add tests**

```rust
#[test]
fn test_fts_fuzzy() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("name");
    unsafe {
        lance_dataset_create_scalar_index(
            ds, column.as_ptr(), ptr::null(),
            LanceScalarIndexType::Inverted as i32, ptr::null(), false,
        );
    }
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    // "alise" within edit distance 2 of "alice"
    let q = c_str("alise");
    let cols = [column.as_ptr(), ptr::null()];
    let rc = unsafe {
        lance_scanner_full_text_search(scanner, q.as_ptr(), cols.as_ptr(), 2)
    };
    assert_eq!(rc, 0);
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) }, 0);
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream as *mut _).unwrap() };
    let mut total = 0;
    for b in reader { total += b.unwrap().num_rows(); }
    assert!(total >= 1, "expected fuzzy match for 'alise' → 'alice'");
    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_nearest_after_fts_is_rejected() {
    let (_tmp, uri) = create_vector_dataset(64, 8);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    let q = c_str("foo");
    unsafe { lance_scanner_full_text_search(scanner, q.as_ptr(), ptr::null(), 0); }

    let column = c_str("embedding");
    let query: Vec<f32> = vec![0.5; 8];
    let rc = unsafe {
        lance_scanner_nearest(
            scanner, column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void, 8,
            LanceDataType::Float32 as i32, 5,
        )
    };
    assert_eq!(rc, -1);
    let msg = unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy().into_owned()
    };
    assert!(msg.to_lowercase().contains("full_text") || msg.to_lowercase().contains("fts"),
        "msg was: {}", msg);
    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_fts_after_nearest_is_rejected() {
    let (_tmp, uri) = create_vector_dataset(64, 8);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    let column = c_str("embedding");
    let query: Vec<f32> = vec![0.5; 8];
    unsafe {
        lance_scanner_nearest(
            scanner, column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void, 8,
            LanceDataType::Float32 as i32, 5,
        );
    }
    let q = c_str("foo");
    let rc = unsafe { lance_scanner_full_text_search(scanner, q.as_ptr(), ptr::null(), 0) };
    assert_eq!(rc, -1);
    let msg = unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy().into_owned()
    };
    assert!(msg.to_lowercase().contains("nearest") || msg.to_lowercase().contains("vector"),
        "msg was: {}", msg);
    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}
```

- [ ] **Step 2: Implement mutual-exclusion checks**

In `src/scanner.rs`, modify `scanner_nearest_inner` to check at the top:

```rust
    let s = unsafe { &mut *scanner };
    if s.fts_query.is_some() {
        return Err(lance_core::Error::InvalidInput {
            source: "cannot call nearest after full_text_search; they are mutually exclusive".into(),
            location: snafu::location!(),
        });
    }
    // ... existing body ...
```

And `fts_inner`:

```rust
    let s = unsafe { &mut *scanner };
    if s.nearest.is_some() {
        return Err(lance_core::Error::InvalidInput {
            source: "cannot call full_text_search after nearest; they are mutually exclusive".into(),
            location: snafu::location!(),
        });
    }
    // ... existing body ...
```

- [ ] **Step 3: Run tests**

```bash
cargo test --test c_api_test test_fts test_nearest_after_fts test_fts_after_nearest 2>&1 | tail -15
```

Expected: 3 tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/scanner.rs tests/c_api_test.rs
git commit -m "feat(scanner): FTS fuzzy + mutual exclusion with nearest"
```

### Task 25: Add C++ FTS wrapper + smoke test

**Files:**
- Modify: `include/lance.hpp`
- Modify: `tests/cpp/test_cpp_api.cpp`

- [ ] **Step 1: Add wrapper**

In `lance::Scanner` (inside the class), append:

```cpp
    Scanner& full_text_search(const std::string& query,
                              const std::vector<std::string>& columns = {},
                              uint32_t max_fuzzy_distance = 0) {
        std::vector<const char*> col_ptrs;
        for (auto& c : columns) col_ptrs.push_back(c.c_str());
        col_ptrs.push_back(nullptr);
        const char* const* cols_c =
            columns.empty() ? nullptr : col_ptrs.data();
        if (lance_scanner_full_text_search(handle_.get(), query.c_str(),
                                            cols_c, max_fuzzy_distance) != 0)
            check_error();
        return *this;
    }
```

- [ ] **Step 2: Add C++ smoke test**

Append to `tests/cpp/test_cpp_api.cpp`:

```cpp
static void test_fts_smoke(const std::string& uri) {
    auto ds = lance::Dataset::open(uri);
    ds.create_scalar_index("name", LANCE_SCALAR_INVERTED);
    ArrowArrayStream stream;
    ds.scan().full_text_search("alice", {"name"}, 0).to_arrow_stream(&stream);
    if (stream.release) stream.release(&stream);
}
```

Wire into `main`.

- [ ] **Step 3: Run C++ compile test**

```bash
cargo test --test compile_and_run_test -- --ignored 2>&1 | tail -20
```

Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add include/lance.hpp tests/cpp/test_cpp_api.cpp
git commit -m "feat(cpp): Scanner::full_text_search + smoke test"
```

### Task 26: PR 3 — README update + final check + PR

**Files:**
- Modify: `README.md:32-39`

- [ ] **Step 1: Tick remaining Phase 2 rows**

```markdown
| [x] | Vector search | Nearest-neighbor via scanner with metric/k/nprobes |
| [x] | Full-text search | FTS queries through scanner interface |
| [x] | Vector index creation | IVF_PQ, IVF_FLAT, IVF_SQ, HNSW variants |
| [x] | Scalar index creation | BTree, Bitmap, Inverted, Label-List indexes |
| [x] | Index management | List and drop index operations |
| [x] | C++ wrappers | `create_vector_index()` and `create_scalar_index()` methods |
```

- [ ] **Step 2: Full test sweep**

```bash
cargo fmt
cargo clippy --all-targets -- -D warnings
cargo test
cargo test --test compile_and_run_test -- --ignored
```

- [ ] **Step 3: Open PR**

```bash
git add README.md
git commit -m "docs: tick all Phase 2 rows"
git push -u origin HEAD
gh pr create --title "feat(scanner): full-text search via scanner builder" --body "$(cat <<'EOF'
## Summary
- lance_scanner_full_text_search (BM25, exact + fuzzy).
- Mutual exclusion with nearest enforced (clear error message).
- C++ Scanner::full_text_search fluent API + smoke test.
- Phase 2 rows in README ticked.

Spec: docs/superpowers/specs/2026-04-23-phase2-vector-search-indexing-design.md
Plan: docs/superpowers/plans/2026-04-23-phase2-vector-search-indexing.md (PR 3 = Tasks 23–26)

## Test plan
- [x] cargo test
- [x] FTS exact + fuzzy + mutual-exclusion covered.
- [x] C/C++ compile-and-run test passes.
EOF
)"
```

---

## Done

After PR 3 merges, all six Phase 2 components in the README are checked. Scanner builder seamlessly supports k-NN and BM25 alongside the existing projection / filter / limit / fragment knobs, indexes can be created/listed/dropped, and the C++ wrappers cover the full surface.
