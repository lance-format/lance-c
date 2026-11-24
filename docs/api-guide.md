# C and C++ API guide

See the [quickstart](../README.md#quickstart) for installation and a first read. The core sections below cover writing, scanning, search, mutation, indices, cloud storage, and errors. Later sections cover specialized APIs. The [C header](../include/lance/lance.h) documents the ownership and error contract of each function; see [Generated C/C++ API reference](api-reference.md) to render the headers as searchable HTML.

The core workflows use public calls present in the current source and were exercised against a published stable release. The [latest stable release](https://github.com/lance-format/lance-c/releases/latest) provides supported downloads, and the [releases page](https://github.com/lance-format/lance-c/releases) records the changes in each release. A package may not contain entry points added after its release: check the installed header before using one.

## Write Arrow batches

A dataset write takes a caller-owned `ArrowSchema` that describes the stream and an `ArrowArrayStream` that is consumed. The stream must deliver record batches matching the schema. `LANCE_WRITE_CREATE` fails if the URI already contains a dataset, `LANCE_WRITE_APPEND` requires a compatible schema, and `LANCE_WRITE_OVERWRITE` replaces an existing dataset or creates a new one.

```c
LanceDataset* dataset = NULL;
int32_t status = lance_dataset_write(
    "file:///tmp/demo.lance",
    &schema,              /* caller owns; this call does not release it */
    &stream,              /* consumed on success and failure */
    LANCE_WRITE_CREATE,
    NULL,                 /* static storage options, or NULL */
    &dataset);
if (status != 0) {
    /* Read the thread-local error before the next C call. */
}
/* Release the schema with schema.release(&schema); do not release the stream again. */
lance_dataset_close(dataset);
```

The C++ `Dataset::write` overload reads the schema from the stream itself, so the stream must be self-describing. The stream is consumed on every return path, including when the call throws:

```cpp
ArrowArrayStream stream = make_stream();  /* a self-describing Arrow stream */
lance::Dataset dataset = lance::Dataset::write(
    "file:///tmp/demo.lance", &stream, lance::WriteMode::Create);
```

`lance_dataset_write` and `Dataset::write` retain the legacy 20-version / 14-day auto-cleanup policy. Use `lance_dataset_write_with_options` or `Dataset::write_with_options` to select native `WriteParams::default()`. The [owned native write options](#owned-native-write-options) section covers the full write-options surface.

## Scan, filter, project and take

`lance_scanner_new(dataset, columns, filter)` copies the NULL-terminated column list and SQL filter when it is called. A NULL column list selects all columns and a NULL filter selects all rows. Use `lance_scanner_set_projection()` for aliases and SQL expressions; the setter replaces the initial column list, copies its strings, and must be called before the scan starts. `lance_scanner_set_limit()` and `lance_scanner_set_offset()` apply the result window. `lance_scanner_count_rows()` executes the native count plan and rejects a configured limit, offset, or ordering.

```c
LanceScanner* scanner = lance_scanner_new(dataset, NULL, "score >= 10");
if (scanner == NULL) {
    /* Read lance_last_error_code()/message(). */
}

LanceProjectionColumn projection[] = {
    {"id", "id"},
    {"upper_name", "upper(name)"},
};
if (lance_scanner_set_projection(scanner, projection, 2) != 0 ||
    lance_scanner_set_limit(scanner, 100) != 0 ||
    lance_scanner_set_offset(scanner, 20) != 0) {
    /* Invalid configuration leaves the previous scanner state in place. */
}

struct ArrowArrayStream stream = {0};
if (lance_scanner_to_arrow_stream(scanner, &stream) != 0) {
    /* Read the error, then release the scanner. */
}
/* Consume batches, then stream.release(&stream); lance_scanner_close(scanner). */
```

```cpp
auto scanner = dataset.scan();
scanner.project({{"id", "id"}, {"upper_name", "upper(name)"}})
       .limit(100)
       .offset(20)
       .batch_size(64);

ArrowArrayStream raw{};
scanner.to_arrow_stream(&raw);
lance::ArrowStream stream(raw);
/* Read batches from stream->get_next(). */
```

`take` reads rows by zero-based offset and `take_rows` reads rows by dataset row ID from the `_rowid` column. Both return a caller-owned Arrow stream whose non-NULL `release` callback must be called exactly once; the schema is validated before the stream callbacks are exposed. `take_rows` may omit missing or deleted row IDs and otherwise preserves input order and duplicates. `take` preserves the requested offsets.

```c
const uint64_t offsets[] = {0, 4, 9};
struct ArrowArrayStream rows = {0};
if (lance_dataset_take(dataset, offsets, 3, NULL, &rows) != 0) {
    /* Read the error. */
}
/* rows.release(&rows) exactly once. */
```

```cpp
const uint64_t offsets[] = {0, 4, 9};
ArrowArrayStream raw{};
dataset.take(offsets, 3, &raw);  /* the overload accepts a column list */
lance::ArrowStream rows(raw);
```

`take_rows` also has a borrowing overload that takes
`std::span<const uint64_t>`. `std::vector`, `std::array` and C arrays convert
to it directly. The range is read only during the call, and an empty range
(even one with a null data pointer) reads zero rows. The pointer/count
overloads keep working.

```cpp
std::vector<uint64_t> row_ids{0, 4, 9};
ArrowArrayStream raw{};
dataset.take_rows(row_ids, &raw);  /* all columns */
lance::ArrowStream all_columns(raw);

ArrowArrayStream projected_raw{};
dataset.take_rows(row_ids, {"id", "name"}, &projected_raw);
lance::ArrowStream projected(projected_raw);
```

The same read methods have overloads that return `lance::ArrowStream` directly:
`Scanner::to_arrow_stream()`, `Dataset::take()`, `Dataset::take_rows()`, and
`Fragment::take()`. They forward the pointer/count range, projection, and
empty-projection behavior of the corresponding raw-out overload. The returned
owner releases the native stream exactly once, from `reset()` or its
destructor; moving it transfers ownership and leaves the source empty. Passing
an `ArrowArrayStream*` keeps the existing caller-owned contract. `std::vector`,
`std::array`, `std::span`, and C arrays bind to the range overloads.

```cpp
const uint64_t row_ids[] = {0, 4, 9};
auto all_columns = dataset.take(row_ids, 3);
auto named = dataset.take_rows(row_ids, {"id"});

auto scanner = dataset.scan({"id"});
auto scanned = scanner.to_arrow_stream();
all_columns.reset();  /* early release; destruction would also release once */
```

## Vector and full-text search

A vector query sets `k` on the scanner and can be combined with a SQL filter, projection, limit, offset, and ordering. The query vector length is the column dimension, not the byte length. The default scoring path adds `_distance` to the output; `Scanner::disable_scoring_autoprojection()` or `lance_scanner_disable_scoring_autoprojection()` keeps it only when named explicitly. `prefilter(true)` applies the filter before ranking; postfiltering can return fewer than `k` rows. An indexed query remains approximate. Use `use_index(false)` for an exact scan, still subject to `k`.

```c
float query[] = {0.0f, 0.0f};
LanceScanner* scanner = lance_scanner_new(dataset, NULL, "id >= 4");
if (scanner == NULL ||
    lance_scanner_nearest(scanner, "embedding", query, 2,
                          LANCE_DTYPE_FLOAT32, 5) != 0 ||
    lance_scanner_set_metric(scanner, LANCE_METRIC_L2) != 0 ||
    lance_scanner_set_prefilter(scanner, true) != 0 ||
    lance_scanner_set_limit(scanner, 2) != 0) {
    /* Read the error and rebuild the scanner if needed. */
}
```

```cpp
const float query[] = {0.0f, 0.0f};
auto scanner = dataset.scan(std::nullopt, "id >= 4");
scanner.nearest("embedding", query, 2, 5)
       .metric(LANCE_METRIC_L2)
       .prefilter(true)
       .limit(2);
```

Full-text search uses BM25 over an inverted scalar index. Build the index first, then either name the indexed columns or pass NULL to search all FTS-indexed columns. `max_fuzzy_distance == 0` selects exact term matching; a positive value enables native fuzzy matching. The default scoring path adds `_score`. FTS and nearest queries are mutually exclusive on one scanner. See [Scanner projection and scoring columns](#scanner-projection-and-scoring-columns), [Distance-bounded vector search](#distance-bounded-vector-search), and [Multi-vector search](#multi-vector-search) for the specialized forms.

```c
const char* fts_columns[] = {"text", NULL};
LanceScanner* fts = lance_scanner_new(dataset, NULL, NULL);
if (fts == NULL ||
    lance_scanner_full_text_search(fts, "apple", fts_columns, 0) != 0 ||
    lance_scanner_set_limit(fts, 5) != 0) {
    /* Read the error. */
}
```

```cpp
auto fts = dataset.scan();
fts.full_text_search("apple", {"text"}, 0).limit(5);
```

## Mutations and indices

`delete`, `update`, and `merge_insert` mutate the same dataset handle in place and commit a new manifest. Existing scanners and independently opened handles keep their previous snapshot. A failed mutation can leave uncommitted data files behind; reopen the URI when the commit outcome is uncertain. `merge_insert` consumes its source stream on every return path except a NULL source.

```c
uint64_t deleted = 0;
if (lance_dataset_delete(dataset, "id = 6", &deleted) != 0) {
    /* Read the error. */
}

const char* update_columns[] = {"text"};
const char* update_values[] = {"'updated'"};
uint64_t updated = 0;
if (lance_dataset_update(dataset, "id = 5", update_columns,
                         update_values, 1, &updated) != 0) {
    /* Read the error. */
}

const char* keys[] = {"id"};
LanceMergeInsertParams params = {0};
params.when_matched = LANCE_MERGE_WHEN_MATCHED_UPDATE_ALL;
params.when_not_matched = LANCE_MERGE_WHEN_NOT_MATCHED_INSERT_ALL;
LanceMergeInsertResult result = {0};
if (lance_dataset_merge_insert(dataset, keys, 1, &source, &params,
                               &result) != 0) {
    /* source was consumed even on this error path. */
}
```

```cpp
uint64_t deleted = dataset.delete_rows("id = 6");
uint64_t updated = dataset.update("id = 5", {{"text", "'updated'"}});
auto find_or_create = dataset.merge_insert({"id"}, &source);
auto upserted = dataset.upsert({"id"}, &upsert_source);
```

Scalar indices are created with `lance_dataset_create_scalar_index()` or `Dataset::create_scalar_index()`. BTree is the ordinary equality/range index; `LANCE_SCALAR_INVERTED` with tokenizer JSON is the FTS index. Vector indices use `LanceVectorIndexParams` / `VectorIndexOptions`; the legacy parameter struct and the additive options entry point share native validation and defaults. Index creation commits its own version. Native Lance commits before reloading index metadata, so an error can be returned after a version was published; reopen the dataset to inspect the committed state.

```c
lance_dataset_create_scalar_index(dataset, "id", "id_btree",
                                  LANCE_SCALAR_BTREE, NULL, false);

const char* fts_params =
    "{\"base_tokenizer\":\"simple\",\"language\":\"English\"}";
lance_dataset_create_scalar_index(dataset, "text", "text_fts",
                                  LANCE_SCALAR_INVERTED, fts_params, false);

LanceVectorIndexParams vector = {0};
vector.index_type = LANCE_INDEX_IVF_FLAT;
vector.metric = LANCE_METRIC_L2;
vector.num_partitions = 16;
lance_dataset_create_vector_index(dataset, "embedding", "embedding_ivf",
                                  &vector, false);
```

```cpp
dataset.create_scalar_index("id", LANCE_SCALAR_BTREE, "id_btree");
dataset.create_scalar_index(
    "text", LANCE_SCALAR_INVERTED, "text_fts",
    R"({"base_tokenizer":"simple","language":"English"})");

lance::VectorIndexOptions vector;
vector.index_type = LANCE_INDEX_IVF_FLAT;
vector.metric = LANCE_METRIC_L2;
vector.num_partitions = 16;
dataset.create_vector_index_with_options(
    "embedding", vector, "embedding_ivf");
```

## Cloud object storage and credentials

The dataset URI selects the object store (`s3://`, `gs://`, `az://`, `file://`, `memory://`, and the other schemes supported by the pinned Lance core). Static options are passed unchanged to the native object-store provider. Use the provider's own key names: for example, S3 commonly uses `aws_access_key_id`, `aws_secret_access_key`, `aws_region`, and `aws_endpoint` for a custom endpoint; GCS and Azure use their corresponding native credential keys. The binding does not rename provider keys or add a second credential cache.

```c
const char* storage_options[] = {
    "aws_access_key_id", "...",
    "aws_secret_access_key", "...",
    "aws_region", "us-east-1",
    "aws_endpoint", "https://s3.example.com",
    NULL,
};
LanceDataset* dataset = lance_dataset_open(
    "s3://bucket/data.lance", storage_options, 0);
```

```cpp
auto dataset = lance::Dataset::open(
    "s3://bucket/data.lance",
    {{"aws_access_key_id", "..."},
     {"aws_secret_access_key", "..."},
     {"aws_region", "us-east-1"},
     {"aws_endpoint", "https://s3.example.com"}});
```

For rotating credentials, wrap a `LanceStorageOptionsProvider` in a `LanceStorageOptionsAccessor` and pass the accessor to the open or write options. The dataset retains the accessor and provider, so closing the C handles after a successful open does not invalidate later I/O. Native code owns refresh, expiration, caching, and object-store registry identity.

Set `expires_at_millis` to the credential response's expiry as a decimal string of Unix epoch milliseconds. The pinned native accessor refreshes within `refresh_offset_millis` (default 60 seconds) before that time; an absent or non-numeric `expires_at_millis` means no expiry. Temporary AWS credentials also need `aws_session_token`. Do not substitute a human-readable timestamp or a duration. A provider callback may run on a native worker thread: it must return normally, must not use `lance_last_error_*()`, and reports C failures through `LanceCallbackError`. In C++, throwing `lance::Error` is converted to that callback error.

The C fetch result is borrowed only until `release_snapshot` runs. The callback owns its keys, values, and snapshot storage; the library copies the complete map before releasing it exactly once. The example duplicates the full credential strings and returns a typed callback error if a copy or the epoch-millisecond formatting fails, so it never returns truncated credentials.

```c
typedef struct {
    char* access_key_id;
    char* secret_access_key;
    char* session_token;
    char* expires_at_millis;
    const char* keys[4];
    const char* values[4];
} CredentialSnapshot;

static char* copy_string(const char* value) {
    size_t length = strlen(value) + 1;
    char* copy = malloc(length);
    if (copy != NULL) memcpy(copy, value, length);
    return copy;
}

static void release_credential_snapshot(void* context) {
    CredentialSnapshot* snapshot = context;
    free(snapshot->access_key_id);
    free(snapshot->secret_access_key);
    free(snapshot->session_token);
    free(snapshot->expires_at_millis);
    free(snapshot);
}

static int32_t callback_io_error(LanceCallbackError* error) {
    error->code = LANCE_ERR_IO;
    error->message.data = NULL;
    error->message.release = NULL;
    return -1;
}

/* Return the credential response's real Unix epoch millisecond expiry. */
uint64_t temporary_credentials_expire_at_millis(void);

static int32_t fetch_storage_options(
    void* context, LanceStorageOptionsFetchResult* out,
    LanceCallbackError* error) {
    (void)context;
    /* Supply the complete strings from the real credential response. */
    const char* access_key_id = "<temporary-access-key>";
    const char* secret_access_key = "<temporary-secret-key>";
    const char* session_token = "<temporary-session-token>";
    char expires_at_millis[32];

    int written = snprintf(
        expires_at_millis, sizeof(expires_at_millis), "%llu",
        (unsigned long long)temporary_credentials_expire_at_millis());
    if (written < 0 || (size_t)written >= sizeof(expires_at_millis)) {
        return callback_io_error(error);
    }

    CredentialSnapshot* snapshot = calloc(1, sizeof(*snapshot));
    if (snapshot == NULL) return callback_io_error(error);

    snapshot->access_key_id = copy_string(access_key_id);
    snapshot->secret_access_key = copy_string(secret_access_key);
    snapshot->session_token = copy_string(session_token);
    snapshot->expires_at_millis = copy_string(expires_at_millis);
    if (snapshot->access_key_id == NULL ||
        snapshot->secret_access_key == NULL ||
        snapshot->session_token == NULL ||
        snapshot->expires_at_millis == NULL) {
        release_credential_snapshot(snapshot);
        return callback_io_error(error);
    }

    snapshot->keys[0] = "aws_access_key_id";
    snapshot->keys[1] = "aws_secret_access_key";
    snapshot->keys[2] = "aws_session_token";
    snapshot->keys[3] = "expires_at_millis";
    snapshot->values[0] = snapshot->access_key_id;
    snapshot->values[1] = snapshot->secret_access_key;
    snapshot->values[2] = snapshot->session_token;
    snapshot->values[3] = snapshot->expires_at_millis;

    out->has_options = 1;
    out->keys = snapshot->keys;
    out->values = snapshot->values;
    out->count = 4;
    out->snapshot_ctx = snapshot;
    out->release_snapshot = release_credential_snapshot;
    return 0;
}

LanceStorageOptionsProviderSpec spec = {0};
spec.fetch = fetch_storage_options;
spec.provider_id = "sts-refresh";
LanceStorageOptionsProvider* provider =
    lance_storage_options_provider_new(&spec);
LanceStorageOptionsAccessor* accessor =
    lance_storage_options_accessor_new(NULL, provider);

LanceDatasetOpenOptions open_options = {0};
open_options.storage_options_accessor = accessor;
LanceDataset* dataset = lance_dataset_open_with_options(
    "s3://bucket/data.lance", &open_options, NULL);

/* The dataset retains the accessor and provider. */
lance_storage_options_accessor_close(accessor);
lance_storage_options_provider_close(provider);
```

```cpp
struct TemporaryCredentials {
    std::string access_key_id;
    std::string secret_access_key;
    std::string session_token;
    uint64_t expires_at_millis = 0; /* Unix epoch milliseconds */
};

/* Return the credential source's current temporary credentials. */
TemporaryCredentials fetch_temporary_credentials();

lance::StorageOptionsProvider provider(
    "sts-refresh",
    []() -> std::optional<lance::StorageOptions> {
        auto credentials = fetch_temporary_credentials();
        return lance::StorageOptions{
            {"aws_access_key_id", credentials.access_key_id},
            {"aws_secret_access_key", credentials.secret_access_key},
            {"aws_session_token", credentials.session_token},
            {"expires_at_millis",
             std::to_string(credentials.expires_at_millis)},
        };
    });
lance::StorageOptionsAccessor accessor(
    lance::StorageOptions{{"aws_region", "us-east-1"}}, &provider);
lance::DatasetOpenOptions options;
options.storage_options_accessor = &accessor;
auto dataset = lance::Dataset::open_with_options(
    "s3://bucket/data.lance", options);
```

The same static list is the `storage_opts` argument on the write entry points. For a dynamic accessor, use `lance_dataset_write_with_options_and_accessor()` or `Dataset::write_with_options_and_accessor()`; the returned dataset retains the accessor. A NULL provider and an empty map are different native values, and a provider may return native None for one fetch. Those distinctions are preserved.

## Runnable write/index/query example

`examples/cmake-consumer/workflow.cpp` is a complete C++20 program against an installed package. It writes `id`, `text`, and `embedding` columns, creates BTree, inverted FTS, and IVF vector indices, reopens the dataset, and runs a filtered vector query and an FTS query. The fixture is built directly with the Arrow C Data Interface, so it also shows the release callbacks and stream ownership rules.

```bash
cmake -S examples/cmake-consumer -B build/workflow \
  -DCMAKE_PREFIX_PATH=/path/to/lance-c-install
cmake --build build/workflow --target workflow
./build/workflow/workflow file:///tmp/lance-c-workflow.lance
```

Expected output on a fresh path:

```text
write: rows=6 version=1
indices: count=3 version=4
vector query: 4 5
fts query: 1 2 4
workflow: PASS
```

The example uses only calls and behavior present in the stable release you installed. A development build may add entry points that the archive does not contain; check its header.

## Errors and panic recovery

C entry points report failures through their documented sentinel (NULL, -1, or 0 for selected accessors). The error code and message are thread-local: read them on the thread that made the failing call, before another call can replace them. `lance_last_error_message()` takes the pending message, so the caller must free it with `lance_free_string()`; taking the message also clears the pending error code. If you need the code later, copy it before taking the message.

```c
LanceDataset* dataset = lance_dataset_open("/path/that/does/not/exist", NULL, 0);
if (dataset == NULL) {
    LanceErrorCode code = lance_last_error_code();
    const char* message = lance_last_error_message();
    fprintf(stderr, "lance error %d: %s\n",
            (int)code, message != NULL ? message : "(no message)");
    lance_free_string(message);
}
```

C++ methods throw `lance::Error`; `code` carries the same `LanceErrorCode` value and `what()` carries the message.

```cpp
try {
    auto dataset = lance::Dataset::open("/path/that/does/not/exist");
} catch (const lance::Error& error) {
    std::fprintf(stderr, "lance error %d: %s\n",
                 static_cast<int>(error.code), error.what());
}
```

`lance::check_error()` exposes the same poll to code that calls the C API
directly: it returns normally when `lance_last_error_code()` is `LANCE_OK` and
throws `lance::Error` otherwise.

A panic inside Lance or Arrow is caught at the FFI boundary and reported as `LANCE_ERR_PANIC`. A scanner is poisoned after that: close it and build a fresh scanner instead of retrying it. A dataset handle remains usable, and a mutation that panics before commit leaves the last committed snapshot intact. C++ callers receive `lance::Error` with `code == LANCE_ERR_PANIC`. Panics during close/free are logged and cleanup may leak by design. The contract does not cover a double panic, a panic in a destructor during unwinding, stack overflow, or allocation failure, and callbacks passed into the library must not unwind. See the panic section in [error.h](../include/lance/error.h) for the complete boundary.

## Header inclusion

`<lance/lance.h>` is the umbrella C header and keeps the complete 0.2 public
ABI. C callers that use only the dataset read path can include the focused
domain headers instead:

- `<lance/dataset.h>` — dataset open and close, snapshots, metadata and version history
- `<lance/scanner.h>` — scanner configuration, planning and iteration
- `<lance/batch.h>` — `LanceBatch` conversion to and from the Arrow C Data Interface

Each domain header includes `<lance/arrow_abi.h>` and `<lance/fwd.h>`.
Include `<lance/error.h>` as well when calling `lance_last_error_code()`,
`lance_last_error_message()` or `lance_free_string()`, and
`<lance/version.h>` for the version macros or `lance_c_version()`. The umbrella
already brings in every header. For example, a read consumer that reports
errors includes:

```c
#include <lance/dataset.h>
#include <lance/scanner.h>
#include <lance/error.h>
```

Every public header is self-contained: it can be the first include in a C11 or
C++20 translation unit.

Selective includes change neither the ABI nor the behavior, and the umbrella
remains the safe choice for code that uses several domains. Existing code does
not need to change.

The C++ wrapper `<lance/lance.hpp>` is a large header-only interface and still
includes the umbrella C header. Include it only in the `.cpp` translation units
that use the C++ API, rather than in widely included project headers, to limit
compile time and rebuild scope where practical.

## Binding version

`LANCE_C_VERSION_MAJOR`, `LANCE_C_VERSION_MINOR`, `LANCE_C_VERSION_PATCH`,
`LANCE_C_VERSION_PRE`, and `LANCE_C_VERSION_STRING` are synchronized with the
package version in `Cargo.toml`. This is the lance-c release version, not the
Lance core dataset/file writer version or a Git pin.

`LANCE_C_VERSION` is a numeric encoding for compile-time checks. Major, minor,
and patch must each be less than 1000. The pre-release suffix is not encoded,
so a pre-release compares equal to its base release; use
`LANCE_C_VERSION_PRE` or `LANCE_C_VERSION_STRING` for exact identity.

```c
#if LANCE_C_VERSION >= LANCE_C_VERSION_ENCODE(0, 2, 0)
/* Use the 0.2 API. */
#endif

if (strcmp(lance_c_version(), LANCE_C_VERSION_STRING) != 0) {
    /* The loaded library and headers disagree. */
}
```

```cpp
if (lance::version() != LANCE_C_VERSION_STRING) {
    /* The loaded library and headers disagree. */
}
```

`lance_c_version()` / `lance::version()` return the full package version,
including a pre-release suffix when present. The string is owned by the library,
remains valid for the process lifetime, and must not be freed.

## Dataset metadata

Table metadata is available through `lance_dataset_metadata_json()` or
`Dataset::metadata_json()`. Updates return the complete post-commit map:

```c
const char* keys[] = {"owner", "retired", "empty"};
const char* values[] = {"c-consumer", NULL, ""};
const char* updated = lance_dataset_update_metadata_json(
    dataset, keys, values, 3, LANCE_METADATA_UPDATE_MERGE);
if (!updated) {
    /* Check lance_last_error_code()/message. */
}
lance_free_string(updated);
```

```cpp
ds.update_metadata_json({
    {"owner", std::string("c++")},
    {"retired", std::nullopt},
    {"empty", std::string("")},
}, lance::MetadataUpdateMode::Merge);

ds.update_schema_metadata_json({
    {"schema-owner", std::string("c++")},
});
```

A NULL value removes the key. An empty string remains a real empty value.
`LANCE_METADATA_UPDATE_REPLACE` / `MetadataUpdateMode::Replace` replaces the
whole map; an empty update in replace mode clears it. Updates mutate the same
dataset handle under Lance's native snapshot and commit semantics: the handle
sees the new version, while existing dataset snapshots and scanners keep their
prior view. Table and schema maps are independent.

The exported Arrow schema is still the schema metadata read path. Use
`lance_dataset_schema()` or `Dataset::schema()` to inspect the committed schema
metadata; no duplicate C-side metadata cache is maintained.

Field metadata uses one native batch and one commit. Each entry selects a field
by ID or path and carries its own merge or replace mode:

```cpp
auto id = ds.resolve_field_id(lance::FieldReference::by_path("id"));
ds.update_field_metadata({
    {lance::FieldReference::by_id(id),
     {{"owner", std::string("c++")}, {"empty", std::string("")}},
     lance::MetadataUpdateMode::Merge},
    {lance::FieldReference::by_path("profile.name"),
     {{"nested", std::string("value")}},
     lance::MetadataUpdateMode::Replace},
});
```

A NULL value removes a key, an empty string remains a real value, and duplicate
references to the same resolved field follow the native builder's
last-update-wins behavior. Read field metadata through the Arrow schema's field
metadata. `lance_dataset_data_storage_version()`,
`lance_dataset_has_stable_row_ids()`, `lance_dataset_field_id_count()` /
`lance_dataset_field_id_at()`, `lance_dataset_resolve_field_id()` and
`lance_dataset_field_path()` expose the current native snapshot; the C++
wrappers provide the corresponding `Dataset` methods. `lance_dataset_uri()` /
`Dataset::uri()` return the fully qualified URI resolved by native
`Dataset::uri()`; for a namespace-opened dataset this is the `describe_table()`
location, not the table ID.

## Dataset configuration, bases and files

`lance_dataset_update_config_json()` and `Dataset::update_config_json()` apply
native config merge or replace semantics and return the complete resulting map:

```cpp
auto updated = ds.update_config_json({
    {"owner", std::string("c++")},
    {"retired", std::nullopt},
    {"empty", std::string("")},
    {"dup", std::string("first")},
    {"dup", std::string("second")},
});

auto cleared =
    ds.update_config_json({}, lance::MetadataUpdateMode::Replace);
```

A NULL or `std::nullopt` value removes a key, an empty string remains a value,
and duplicate keys follow the native builder's last-write-wins behavior. An
empty replace clears the map. The existing merge-only
`lance_dataset_update_config()` and `Dataset::update_config()` entry points keep
their original signatures.

Base paths are copied from the current manifest as JSON keyed by decimal ID.
Registration maps to native `Dataset::add_bases`, including optional transaction
properties:

```cpp
auto registry = ds.base_paths_json();

lance::AddBasesOptions options;
options.transaction_properties =
    std::vector<std::pair<std::string, std::string>>{{"owner", "c++"}};
ds.add_bases({
    lance::BasePath{0, alternate_uri, std::string("alternate"), false},
}, options);
```

ID zero requests native allocation. `name == std::nullopt` means native `None`;
an engaged empty string is preserved. An unset transaction-properties optional
maps to native `None`, while an engaged empty vector maps to
`Some(empty map)`. Native duplicate/conflict validation remains authoritative.

`lance_dataset_tracked_files_stream()` and
`lance_dataset_all_files_stream()` export the corresponding native Arrow
streams. `Dataset::tracked_files_stream()` and `Dataset::all_files_stream()`
return the move-only `lance::ArrowStream` owner. Tracked files follow native
manifest walking across versions and registered bases. `all_files` lists the
primary root only and includes untracked physical files there.

```cpp
auto tracked = ds.tracked_files_stream(
    /*min_version=*/2,
    [](uint64_t processed, std::optional<uint64_t> total) {
        /* Runs on a native background task; do not block or throw. */
    });
auto files = ds.all_files_stream();
```

The progress `total` is an optional hint. Native reports it only after its
manifest listing finishes, and that listing races the walk, so an update may
arrive without a total even when it completes the last manifest.

An exported stream retains what it needs after the source dataset closes. The
stream `release` callback may return before a native progress callback finishes;
the C++ wrapper keeps the callback capture until the native emitter releases it.
In C, `LanceTrackedFilesProgress` transfers ownership of `callback_ctx` to the
library, which calls its `release` exactly once, including on factory errors or
early stream release.

## Dataset maintenance

Manual version cleanup and its read-only explanation bind the native
`CleanupPolicy`, `CleanupOperation`, `Dataset::cleanup_old_versions`,
`Dataset::validate`, `Dataset::count_deleted_rows` and
`Dataset::num_small_files` operations:

```cpp
lance::CleanupPolicy policy;
policy.versions = std::vector<uint64_t>{2, 3};
policy.before_version = 5;

lance::CleanupOperationOptions operation;
operation.max_candidate_files = 100;

auto preview = ds.explain_cleanup(policy, operation);
auto removed = ds.cleanup(policy, operation);

ds.validate();
uint64_t deleted_rows = ds.count_deleted_rows();
uint64_t small_files = ds.num_small_files(/*max_rows_per_group=*/1000);
```

Policy selectors combine with AND. A default-constructed policy inherits native
defaults rather than the legacy writer's automatic 20-version / 14-day policy.
`std::nullopt` versus an engaged empty vector distinguishes absent from explicit
empty version selection, and `retain_versions` uses Lance's asynchronous native
builder. Use `CleanupOldVersionsOptions` /
`lance_dataset_cleanup_old_versions()` for relative-duration cleanup; Lance core
owns the clock cutoff.

Explanations copy all aggregate stats, candidate files, truncation state,
referenced branches and warnings, so they remain valid after the dataset handle
closes. They do not reserve files. Execution re-evaluates current references
and storage state, protects current/tagged/branch/unverified data according to
Lance, and reports partial deletion through `failed_deletes`; it does not roll
back successful deletes. Historical snapshot handles do not own a storage lease.

## Scanner ordering

Use `lance_scanner_set_order_by()` or the fluent C++ `Scanner::order_by()` to
apply Lance's native `Scanner::order_by` behavior:

```cpp
auto scanner = dataset.scan();
scanner.order_by({
    {"category", LANCE_ORDER_ASCENDING, LANCE_ORDER_NULLS_LAST},
    {"score", LANCE_ORDER_DESCENDING, LANCE_ORDER_NULLS_FIRST},
    {"id", LANCE_ORDER_ASCENDING, LANCE_ORDER_NULLS_LAST},
});
```

Tie ordering must be explicit when rows can compare equal; the binding does not
add a row-id or stable-order tie-breaker. Lance validates referenced columns
when the scan starts. With limit/offset, Lance sorts all rows before applying
the window. With nearest or full-text search, Lance sorts the rows selected by
the query.

Pass `std::nullopt` or `lance_scanner_set_order_by(scanner, NULL, 0)` to clear
the ordering. An explicit empty vector is the native empty ordering and has the
same scan effect. Invalid enum values or a NULL column name are rejected without
replacing the previous ordering. Multi-vector search is the one unsupported
mode: `nearest_multivector()` always restores distance order for its result
window, so combining it with a non-empty ordering fails when the scan starts
instead of silently ignoring the ordering.

## Scanner projection and scoring columns

`lance_scanner_set_projection()` and the C++ `Scanner::project()` bind Lance's
native `Scanner::project_with_transform()`:

```cpp
auto scanner = dataset.scan();
scanner.project({
    {"id_alias", "id"},
    {"doubled", "id * 2"},
    {"x", "metadata.location.x"},
});
```

Entries preserve input order. `output_name` is the output field name and may be
an alias; `expression` is a Lance SQL expression. The binder accepts stored
columns, metadata columns, nested field paths and search-generated scoring
columns. Pass `std::nullopt` or `lance_scanner_set_projection(scanner, NULL, 0)`
to restore the unset projection and scan all dataset columns. An empty vector is
an explicit empty projection. The arrays and strings are copied before the
setter returns; invalid entries leave the previous projection unchanged. The
older column-list argument to `lance_scanner_new()` keeps its escaped
field-path behavior and is unchanged.

By default, vector and full-text searches still append `_distance` or `_score`
to an explicit projection, matching the published legacy behavior. Call
`Scanner::disable_scoring_autoprojection()` or
`lance_scanner_disable_scoring_autoprojection()` before starting the scan to
include those columns only when the projection names them explicitly. The
setting is one-way for that scanner.

Multi-vector search keeps the hidden distance required for its final result
window internally. It is removed from output when scoring autoprojection is
disabled and the caller did not request `_distance`.

## Segment-scoped array label filters

Ordinary scans can use a `LabelList` index for array membership through the
existing SQL and Substrait filter interfaces. For example, on a `List<Utf8>`
column named `labels`:

```sql
array_contains(labels, 'red')
array_contains(labels, 'red') AND array_contains(labels, 'blue')
array_contains(labels, 'red') OR array_contains(labels, 'blue')
array_has_all(labels, ['red', 'blue'])
array_has_any(labels, ['red', 'blue'])
```

Configure `lance_scanner_set_fragment_ids` and
`lance_scanner_set_scalar_index_segment` with the selected LabelList segment.
Pass a SQL filter to `lance_scanner_new`, or attach a serialized Substrait
`ExtendedExpression` with `lance_scanner_set_substrait_filter`. The Substrait
schema must describe the list field and its element type; replacing that field
with an unsupported-type placeholder cannot express a label predicate. Use
Lance/DataFusion's `array_has` (the canonical name of `array_contains`),
`array_has_all`, or `array_has_any` functions with correctly typed arguments.
Substrait takes precedence over the primary SQL filter; use
`lance_scanner_additional_sql_filter` when an additional condition must be ANDed
with it.

AND/OR membership expressions can reuse the same selected LabelList segment.
Other columns remain residual filters, evaluated before LIMIT/OFFSET. This API
selects one physical segment; it does not intersect indices on different columns.
Incomplete segment coverage falls back to scanning the entire explicit fragment
scope. Check `scalar_segments_searched` and `scalar_segment_fallbacks` in the
statistics callback to distinguish index acceleration from filter execution.

Bindings preserve Lance's function semantics, not the semantics of similarly
named functions in another SQL engine. In particular, a NULL search value in
`array_contains` does not match NULL array elements. `array_has_all` tests set
containment, not an ordered contiguous subsequence. The pinned Lance version
also treats an empty all-label query as true even for NULL lists. Integrators
should initially push only non-NULL constant labels with matching element types
and retain conditions whose semantics have not been verified in the calling engine.

## Runtime BTree segment selection

Ordinary segment-scoped scans can opt in to comparing BTree page costs at scan
preparation time. Configure `lance_scanner_set_scalar_index_auto_selection(scanner,
true)`, or call `Scanner::scalar_index_auto_selection(true)` in C++, together with
`scalar_index_segment` and an explicit, nonempty fragment list. The selected
segment remains the default when estimation is unavailable. The option is disabled
by default and does not affect vector or full-text searches.

The estimator reads `page_lookup.lance` min/max and NULL-count metadata for equality,
range, bounded IN, IS NULL, and supported AND/OR expressions. Estimated work is
matching pages times the index's configured rows per page. It can select a cheaper
BTree segment only when its known coverage contains the entire read domain. A best
estimate at least as large as the domain's physical row count selects a scoped
non-indexed scan. The full filter, deletion handling, explicit fragment scope and
LIMIT/OFFSET remain in effect on every path; estimates never remove result rows.

Each preparation considers at most eight physical segments, an aggregate 8 MiB
of lookup files, and a separate 8 MiB allowance for decoded lookup buffers. The
decoded allowance is checked from schema and row count before materialization and
charged for both cached and newly loaded lookups. Only fixed-width scalar lookup
columns have a known bound with the current reader API. Variable-width keys such
as strings/binary therefore have unknown costs: a default on such a key is kept,
and an alternative is skipped without opening its lookup for estimation. Such
indices remain usable through the caller's existing segment-selection path.

One lookup is limited to 65,536 pages, IN predicates to 256 values, and one estimate
to 1,000,000 page/predicate checks. Lookup batches share the Session's bounded
metadata cache. Unknown manifest file sizes, unsupported formats, probe errors,
or an unestimable default retain the original segment path. Bitmap and LabelList
segments retain their existing behavior and are not cost-ranked.

Statistics callbacks report `scalar_index_auto_candidates`,
`scalar_index_auto_changed`, `scalar_index_auto_estimated_pages`,
`scalar_index_auto_estimated_rows`, and `scalar_index_auto_full_scans`. Estimated
rows are a page-level approximation, not an exact result count or measured latency.
The model does not account for cache residency, projected-row width, or random
row-fetch cost. Warm lookups avoid storage I/O but still require estimation work.

## Distance-bounded vector search

After configuring a single-vector nearest-neighbor query, use
`lance_scanner_set_distance_range` or C++ `Scanner::distance_range` to restrict
results to `lower_bound <= _distance < upper_bound`:

```cpp
const float query[] = {1.0f, 0.0f};
auto scanner = dataset.scan();
scanner.nearest("embedding", query, 2, 100)
       .metric(LANCE_METRIC_L2)
       .distance_range(std::nullopt, 0.5f);
```

This returns **at most 100** neighbors with distance below `0.5`. It is a
range-constrained Top-K search, not an unbounded enumeration of all matches.
For an annulus, use `.distance_range(0.2f, 0.5f)`; for a lower bound only, use
`.distance_range(0.2f)`; `.distance_range()` clears both bounds. In C, pass
pointers to bounds and `NULL` for an unbounded side:

```c
float upper_bound = 0.5f;
int32_t status = lance_scanner_set_distance_range(scanner, NULL, &upper_bound);
/* Check status and lance_last_error_* before starting the scan. */
```

Bounds are copied and must be finite. When both are set, the lower bound must
be strictly smaller than the upper bound. Negative bounds are allowed (for
example, Dot distances can be negative). Distances use the selected metric's
units: L2 reports **squared Euclidean distance**, so a geometric radius `r`
corresponds to an upper bound of `r * r`, with the boundary excluded. Index-based
search retains Lance's approximate candidate selection; the range does not
guarantee exhaustive recall. Use `.use_index(false)` for an exact scan, still
subject to `k`.

Set bounds after `nearest` and before starting the scan. Replacing the nearest
query clears the bounds; an invalid range leaves the previous range unchanged.
Multi-vector queries are not supported by this setter because their scores
aggregate distances across subvectors.

## Multi-vector search

Use `lance_scanner_nearest_multivector` or the C++ `Scanner::nearest_multivector`
method for a `List<FixedSizeList<float16|float32|float64, D>>` column:

```cpp
const float query[] = {1.0f, 0.0f, 0.0f, 1.0f};
auto scanner = dataset.scan();
scanner.nearest_multivector("embeddings", query, 2, 2, LANCE_DTYPE_FLOAT32, 10)
       .metric(LANCE_METRIC_COSINE)
       .prefilter(true);
```

The copied, row-major matrix is **one query** containing two subvectors. Results
rank logical rows by the sum of each query subvector's minimum distance to a
stored subvector. Empty or null outer rows do not rank. Inner vectors must be
non-nullable; actual stored null or non-finite elements encountered during
scoring fail the stream. Float types and dimensions must match the column.
Cosine pairs with zero norm have undefined distance and are ignored. A row is
excluded if any query subvector has no defined match; a zero-norm query subvector
therefore produces no results. Column names use Lance field-path syntax,
including nested paths such as `payload.embeddings` and backtick-quoted names.

L2 is the default on every fragment. Cosine multi-vector indexes are supported
by the pinned Lance version; incompatible metrics use exact search. Indexed
candidates are refined against stored values (`refine_factor` defaults to 1).
ANN candidate selection remains approximate. Limit and offset apply after
restoring distance order, including fragment-scoped searches. Strict row batching
is applied after that final result window, preserving full batches except the last.

Queries accept at most 128 subvectors. Both `num_vectors * k` and
`refine_factor * k` must be at most 100,000 to bound plan expansion and candidate
allocation. The existing single-vector API and its defaults are unchanged.

## Batch nearest-neighbor search

Use `lance_scanner_nearest_batch` or `Scanner::nearest_batch` to search a
`FixedSizeList<T, dimension>` column with independent query vectors:

```cpp
const float queries[] = {1.0f, 0.0f, 0.0f, 1.0f};
auto scanner = dataset.scan();
scanner.nearest_batch("embedding", queries, 2, 2, 10)
       .use_index(false);
```

Here there are two queries of dimension two, each returning up to ten rows.
The result includes the zero-based `query_index` (`int32`) and, under the legacy
scoring default, `_distance`, alongside the requested dataset columns. A batch
of one query still includes `query_index`. After
`disable_scoring_autoprojection()`, `_distance` appears only when named
explicitly in the projection. Duplicate query vectors retain distinct indices;
queries with no matching rows produce no result rows. Result row order is not
part of the API contract.

This differs from `nearest_multivector`: that API scores one logical query
against a `List<FixedSizeList<...>>` column and returns one ranked result set.
Batch input requires a fixed-size vector column and rejects a dataset with a
reserved `query_index` column. Float16, Float32, Float64, Int8, and UInt8 are accepted;
the query element type must match the column. UInt8 uses the Hamming metric.
Floating-point query values must be finite. Values are copied before the setter
returns, so the caller can immediately release its input buffer.

Configure the query before scanning. Successful nearest-query setters replace
the previous query; an invalid batch request leaves it unchanged. FTS and
nearest queries remain mutually exclusive. Distance ranges currently require a
single-vector query; replacing it with a batch clears its bounds. All queries
share the scanner's metric, filter, index selection, and tuning parameters.
Existing prefilter and postfilter semantics apply; postfiltering can return
fewer than `k` rows.

The scanner's global `limit` and `offset` settings are rejected in batch mode,
in either configuration order: they cannot represent independent result windows.
For per-query pagination, request enough candidates with `k` and apply the window
separately to each `query_index` in the caller.

Bounds are 128 queries, 64 MiB of copied query values, and 100,000 candidates for
`num_queries * k * refine_factor` (use 1 when refinement is unset). Explicit
refinement must be positive. These bounds do not cap total index/payload memory;
callers should also configure scanner resource controls. Arrow streams use the
existing ownership and cancellation contract, including early stream release.

Execution delegates to the pinned Lance scanner. Flat search can share data
scans. Eligible IVF queries can share partition scans; HNSW, refinement, adaptive
partition probing, and other ineligible plans use Lance's per-query fallback.
The batch API does not guarantee shared execution or a particular speedup.

## Owned native write options

`lance_native_write_options_new` copies one `LanceNativeWriteOptionsSpec` into
an opaque `LanceNativeWriteOptions` owner. The handle maps the full representable
native `WriteParams` surface: output shape, auto-cleanup, stable row IDs,
`data_storage_version`, `enable_v2_manifest_paths`, session, transaction
properties, base registration and placement, per-base storage params, external
blob policy, blob pack-file threshold, current-format file-writer options, and
fragment/cumulative progress callbacks. `lance_dataset_write_with_native_options`
uses it for a committing write;
`lance_dataset_write_uncommitted_stream_with_native_options` uses it for a
staged write that is published through the existing transaction commit APIs.

```c
LanceWriteOptions write = {0};
write.params.max_rows_per_file = 1;

LanceNativeWriteOptionsSpec spec = {0};
spec.write_options = &write;
spec.has_enable_v2_manifest_paths = 1;
spec.enable_v2_manifest_paths = false;
spec.has_target_all_bases = 1;
spec.target_all_bases = true;

LanceNativeWriteOptions* options = lance_native_write_options_new(&spec);
LanceDataset* out = NULL;
if (lance_dataset_write_with_native_options(
        "data.lance", &schema, &stream, LANCE_WRITE_CREATE, options, &out) != 0) {
    /* read lance_last_error_* */
}
lance_native_write_options_close(options);
```

The C++ RAII wrapper is `lance::NativeWriteOptions` with
`lance::NativeWriteOptionsSpec`; use `Dataset::write_with_native_options` and
`Dataset::write_uncommitted_with_native_options`.

The returned staged transaction owns only uncommitted data-file metadata. It
does not retain the options handle, session, storage accessor or commit lock;
pass those owners to the later commit when commit I/O needs them.

Option presence is preserved. A NULL list pointer maps to native `None`; a
non-NULL list pointer with count zero maps to `Some(empty)`. An explicit empty
target list and all native target-base conflicts are reported by Lance core,
not reconstructed here. `target_all_bases` resolves primary storage as ID 0
and registered bases by ascending ID, with primary first when included.

Progress callbacks receive actual native events. Fragment begin/complete
callbacks receive a borrowed `LanceWriteFragmentProgressInfo`; the write-stats
callback receives cumulative native `WriteStats`. Fragment callbacks may report
an error and may run concurrently on native worker threads. Write-stats has no
native error channel. Ownership of the progress context transfers only when
`lance_native_write_options_new` succeeds, and the release callback runs
exactly once when the final native owner drops. Closing the options handle does
not invalidate already-committed datasets or returned transactions.

Native trait-object hooks that cannot be represented in C remain explicit gaps:
custom Rust object stores and store wrappers, custom commit handlers other than
`LanceCommitLock`, and feature-gated AWS credential providers. Global
`ObjectStoreParams` scalar fields beyond the storage accessor are also not
exposed; per-base `ObjectStoreParams` supports the accessor, block size, S3
credential refresh offset, constant-size upload parts, and lexical ordering.
Native object-store resolution, validation, defaults and errors remain
authoritative.

## Native transactions

`Dataset::write_uncommitted` maps to `InsertBuilder::execute_uncommitted_stream`
and returns an owned native transaction. The stream is consumed, but data files
remain uncommitted until the transaction is published:

```cpp
ArrowArrayStream stream = source_stream();
lance::UncommittedWriteOptions write_options;
write_options.write.params.max_rows_per_file = 1;
write_options.transaction_properties =
    std::vector<std::pair<std::string, std::string>>{{"engine", "cpp"}};

auto transaction = lance::Dataset::write_uncommitted(
    "data.lance", &stream, lance::WriteMode::Append, write_options);
auto operation = transaction.operation();
assert(operation.kind() == lance::OperationKind::Append);
assert(operation.fragment_count() > 0);

auto bytes = transaction.to_proto();
auto parsed = lance::Transaction::from_proto(bytes);
auto dataset = lance::Dataset::commit("data.lance", parsed);
auto committed = dataset.read_transaction();
auto version_one = dataset.read_transaction_by_version(1);
```

Fragment deletion writes native deletion files before publication. The returned
fragment metadata is used by its scanner, so staged reads, takes and counts see
the deletion immediately:

```cpp
auto fragment = dataset.open_fragment(dataset.fragment_ids()[0]);
auto updated = fragment.delete_if("id = 3");  // nullopt if every row is deleted
if (updated) {
    auto operation = lance::Operation::make_delete(
        {&*updated}, {}, "id = 3");
    lance::TransactionOptions options;
    options.tag = "fragment-delete";
    options.transaction_properties =
        std::vector<std::pair<std::string, std::string>>{
            {"owner=team", "analytics"},
            {"empty", ""},
        };
    auto transaction = lance::Transaction::from_operation(
        dataset.version(), operation, options);
    dataset = lance::Dataset::commit("data.lance", transaction);
}
```

Deleting every row returns `std::nullopt`; put that fragment ID in
`Operation::make_delete`'s deleted-ID list instead. An explicit
`Scanner::fragment_ids()` replaces the staged metadata selection; a zero-length
selection selects no fragments. The C entry points are
`lance_fragment_delete`, `lance_operation_delete_new` and
`lance_transaction_new`. Native commit validation and conflict handling remain
authoritative.

Append batches use native `CommitBuilder::execute_batch` and return both the
actual merged transaction and a new dataset:

```cpp
std::vector<std::reference_wrapper<const lance::Transaction>> batch{
    std::cref(first), std::cref(second)};
auto result = lance::Dataset::commit_batch("data.lance", batch);
```

The C entry points are `lance_dataset_write_uncommitted_stream`,
`lance_transaction_commit`, `lance_transaction_commit_batch`,
`lance_dataset_read_transaction`, and
`lance_dataset_read_transaction_by_version`. Native defaults remain omitted in
`LanceCommitOptions`; explicit zero retries, zero retry budget, disabled timeout
and explicit timeout values are forwarded without reinterpretation.

This delivery exposes writer-generated Append and Overwrite operations plus a
typed Delete constructor for the native fragment-delete workflow. Other typed
Update/Merge/Restore/Clone/Rewrite/index/schema operation constructors and
callback-based commit locks remain later issue #112 scope.
Transaction values are independent snapshots: closing their source dataset,
stream, or derived operation handle does not invalidate them.

Native protobuf conversion is the transport contract. `Operation::Unknown`
decodes but cannot be re-encoded or committed, empty tag/property maps normalize
to absent, and the pinned core does not preserve Overwrite schema metadata
through its separate protobuf field. The pinned core's CommitBuilder also does
not apply its separate `with_transaction_properties` field in `execute` or
`execute_batch`; this binding therefore carries properties on the uncommitted
transaction.

A failed commit performs no binding-side rollback. Data files may remain, and
an I/O failure can leave publication uncertain. Reopen the URI to establish the
actual version before retrying. Detached commits follow native behavior: they do
not appear in main history, and the pinned core rejects using one to create a
new dataset.

## Share metadata and index caches

Use a session to share Lance's metadata and index caches across dataset
handles. Cache limits are bytes; zero requests zero capacity for the
corresponding cache.

```c
LanceSession* session = lance_session_new(
    6ULL * 1024 * 1024 * 1024,
    1ULL * 1024 * 1024 * 1024);
LanceDataset* ds =
    lance_dataset_open_with_session("data.lance", NULL, 0, session);

LanceSessionCacheStats stats;
lance_session_get_cache_stats(session, &stats);

lance_session_close(session);  /* ds remains valid */
lance_dataset_close(ds);
```

```cpp
lance::Session session(
    6ULL * 1024 * 1024 * 1024,
    1ULL * 1024 * 1024 * 1024);
auto ds = lance::Dataset::open_with_session(session, "data.lance");
auto stats = session.cache_stats();
```

Each tier can independently use the native default, an explicit capacity, a
backend URI, or a backend kind plus options. `Default` and an explicit zero
capacity are different choices.

```c
LanceSessionCacheSpec index = {0};
index.kind = LANCE_SESSION_CACHE_SPEC_URI;
index.backend_uri = "moka://?capacity=67108864";

LanceSessionCacheSpec metadata = {0};
metadata.kind = LANCE_SESSION_CACHE_SPEC_SIZE;
metadata.size_bytes = 0; /* explicit zero capacity */

LanceSession* session = lance_session_new_with_cache_specs(&index, &metadata);
uint64_t size_bytes = 0;
uint64_t approx_items = 0;
lance_session_get_size_bytes(session, &size_bytes);
lance_session_get_approx_num_items(session, &approx_items);
```

```cpp
lance::Session session(
    lance::SessionCacheSpec::from_uri("moka://?capacity=67108864"),
    lance::SessionCacheSpec::with_size(0));
auto size_bytes = session.size_bytes();
auto approx_items = session.approx_num_items();
```

`size_bytes()` is the native deep size of live session state, not an RSS
measurement or a configured limit. `approx_num_items()` is explicitly
approximate and may change as cache maintenance runs.

## Thread safety

The C pointer is a lifetime token, not a lock, and a `const` qualifier does not
by itself permit concurrent calls. The [umbrella header](../include/lance/lance.h)
defines the complete contract; the main cases are:

- `LanceDataset` handles are shareable. Snapshot reads and in-place mutations
  may overlap; mutations serialize on the handle's internal write lock for the
  whole native call, and readers that already hold a snapshot keep their old
  view. `restore` and `checkout_*` work from a snapshot and return a new
  handle instead of taking the source handle's write lock; restore still
  follows native commit-conflict rules.
- `LanceSession` inspection calls may run concurrently, and one session may
  back concurrent opens. Cache statistics are current values, not one atomic
  snapshot across the index and metadata caches.
- `LanceFileReader` read-only calls may overlap. Each `read_all`, `read_range`,
  or `read_indices` call creates an independent stream.
- `LanceNamespace` ordinary calls may overlap. Custom operation and dynamic
  context-provider callbacks must be thread-safe; they must not close or
  re-enter the namespace that is invoking them.
- `LanceScanner` and `LanceFileWriter` require caller serialization of all
  calls on one handle. A scanner's exported streams are independent of the
  scanner and of each other, but callbacks on one stream are serialized.
- All four callbacks (`get_schema`, `get_next`, `get_last_error`, `release`) on
  one `ArrowArrayStream` must be serialized. `get_last_error` returns a borrowed
  pointer valid only until the next operation on that stream or release.

Every `close`/`free` takes exclusive ownership of its pointer and must not
overlap any other use of that pointer. Closing a producer releases only that
handle; children that retain native ownership, such as scanners, fragments,
streams and snapshots, stay valid where their own documentation says so.
Borrowed pointers retain only their documented lifetime and may be invalidated
by a concurrent mutable call or close, even on a shareable handle.

Synchronous callbacks run inside the native call that invoked them. They must
not close or re-enter the handle whose call is still on the stack; use an
independent handle or a snapshot acquired before the call.

## Vector index build options

`lance_dataset_create_vector_index` and
`lance_index_segment_builder_new_vector` retain the original
`LanceVectorIndexParams` ABI and its zero-as-default rules. IVF_RQ is value 107;
its legacy `num_bits == 0` selects the native RQ default of 5.

Use `lance_dataset_create_vector_index_with_options` and
`lance_index_segment_builder_new_vector_with_options` when a build needs native
presence semantics. `LanceVectorIndexOptions` distinguishes an absent value from
an explicit zero, supports automatic or explicit IVF sizing, training/sampling
and streaming controls, precomputed partitions/shuffle inputs, storage and
runtime hints, PQ parameters/models, native IVF/PQ Arrow model pairs, RQ JSON,
HNSW tuning, index file version, transpose control, and the native train flag.
`options->has_train == false` retains the native train=true default.

```c
LanceVectorIndexOptions options = {0};
options.index_type = LANCE_INDEX_IVF_RQ;
options.metric = LANCE_METRIC_L2;
options.num_partitions = (LanceOptionalU64){256, true}; /* absent: native auto */
options.num_bits = (LanceOptionalU64){5, true};
const char* rq_model = lance_index_build_rq_model(128, 0); /* 0 -> native 5 */
options.rabitq_model_json = rq_model;
int32_t status = lance_dataset_create_vector_index_with_options(
    dataset, "embedding", "embedding_idx", &options, false);
lance_free_string(rq_model);
```

```cpp
lance::VectorIndexOptions options;
options.index_type = LANCE_INDEX_IVF_RQ;
options.num_partitions = 256;
options.rabitq_model_json = lance::build_rq_model(128);
dataset.create_vector_index_with_options("embedding", options);
```

String maps must contain valid UTF-8. Duplicate keys use Rust HashMap
last-write-wins semantics. Fragment selection preserves native None versus
Some(empty); Lance decides duplicate and missing-ID validation at build time.
Native Arrow model pointers are borrowed synchronously and may be replaced while
a live caller-owned equivalent remains in place. `IndexModel::adopt` transfers
caller-produced native Arrow structs and clears the input structs, allowing
model dtypes beyond the legacy trainer's Float32 representation.

Uncommitted builders copy their dataset snapshot, strings, maps, paths, fragment
IDs and UUIDs. The committed options entry point follows native publication:
Lance commits first and then reloads index metadata, so an error can be returned
after a new dataset version was published. Reopen the dataset to determine its
current version; the binding does not claim storage rollback on such errors.

## Prewarm an index synchronously

Prewarm a logical index before serving queries to move cold index reads out of
foreground query execution. The call blocks until the Rust SDK's
`Dataset::prewarm_index` finishes for all segments with that name in the dataset
handle's snapshot. It does not create an index or change the dataset version.

```c
LanceSession* session = lance_session_new(64 * 1024 * 1024, 16 * 1024 * 1024);
if (!session) return -1;
LanceDataset* ds = lance_dataset_open_with_session("data.lance", NULL, 0, session);
if (!ds) {
    lance_session_close(session);
    return -1;
}
/* Assumes an index named embedding_idx already exists. */
int32_t status = lance_dataset_prewarm_index(ds, "embedding_idx");
if (status != 0) {
    const char* message = lance_last_error_message();
    fprintf(stderr, "%s\n", message);
    lance_free_string(message);
}
lance_dataset_close(ds);
/* Keep session alive and reuse it when opening subsequent query datasets. */
lance_session_close(session);
```

```cpp
lance::Session session(64 * 1024 * 1024, 16 * 1024 * 1024);
auto ds = lance::Dataset::open_with_session(session, "data.lance");
ds.prewarm_index("embedding_idx"); // Blocks; throws lance::Error on failure.
```

The destination is the dataset's process-local session index cache. Reusing the
same session across handles preserves warmed entries after a dataset closes.
Independent sessions and other processes do not share that cache. Entries remain
evictable: success guarantees completion, not that the entire index fits in the
cache or remains resident. Replacing an index creates new segments that must be
warmed separately; historical handles still use their own snapshots. A failure
can leave partially warmed entries in the cache; retrying is safe.

This binding uses the SDK's index-specific prewarm behavior without additional
options. Tests cover multi-segment IVF-Flat and B-tree indexes; other index types
and formats follow the underlying SDK's support and error behavior. It does not
prewarm ordinary data columns or eliminate result-row reads, and it adds no
persistent cache, automatic refresh, or background task management.

## Open at a specific version

`lance_dataset_open` takes a `version` argument — `0` means the latest, any
other value checks out that specific version id (e.g. one returned by
`lance_dataset_versions`):

```c
LanceDataset* ds = lance_dataset_open("data.lance", NULL, 42);
```

```cpp
auto ds = lance::Dataset::open("data.lance", {}, /*version=*/42);
```

Native refs can also select a version, the latest version on a branch, or a tag:

```c
LanceRef* ref = lance_ref_tag("release-2026-11");
LanceDataset* ds = lance_dataset_checkout_ref(source, ref);
lance_ref_close(ref);
```

```cpp
auto ds = source.checkout(lance::Ref::tag("release-2026-11"));
auto tags = ds.tags();
```

`lance_dataset_current_version()`, `lance_dataset_version_refs()` and
`lance_dataset_tags()` return copied snapshots. They remain valid after the
originating dataset handle is closed. Timestamp accessors expose native
nanosecond precision; the existing millisecond version accessor is unchanged.
Nanosecond accessors report an error when a timestamp does not fit i64
nanoseconds. Snapshot creation and millisecond reads retain the native range.
There is no C-side timestamp/asof selector.

Branches use the public Lance 14.0.2 branch lifecycle:

```cpp
auto branch = source.create_branch(
    "feature/next", lance::Ref::version(
        std::optional<std::string>("main"), std::optional<uint64_t>(2)));
auto checked_out = source.checkout_branch("feature/next");
branch.replace_branch_metadata("feature/next", {{"owner", "team"}});
auto branches = source.branches_json();
auto contents = source.get_branch_json("feature/next");
auto identifier = branch.branch_identifier_json();
```

`lance_dataset_branch_create()` accepts the source dataset's store params when
its storage-options pointer is `NULL`; a non-`NULL`, empty array requests
explicit empty object-store params. Native creation is a non-atomic shallow
clone followed by a `BranchContents` write. If creation leaves a branch
dataset without `BranchContents`, use `lance_dataset_branch_force_delete()` to
remove it. Normal deletion still rejects branches referenced by a child branch
or a tag. Reusing an existing branch name may fail in the native shallow-clone
phase rather than reaching the `BranchContents` conflict check.

Branch JSON is the selected Lance core's serde representation, not a new C
metadata model. The list is an object keyed by branch name; each
`BranchContents` includes `parentBranch`, `identifier.version_mapping`,
`parentVersion`, `createAt` in Unix seconds, `manifestSize`, and `metadata`.
Map enumeration order is unspecified. Branch metadata replacement writes the
whole native map, so an empty map clears all entries.

## Deep clone a dataset

`lance_dataset_deep_clone` copies one source snapshot into a new target dataset
using native `Dataset::deep_clone`. A `NULL` reference clones the source
handle's current snapshot; pass a `LanceRef` to select the current head, a
version, a tag, or a branch:

```c
LanceDataset* copy = NULL;
if (lance_dataset_deep_clone(ds, "copy.lance", NULL, NULL, &copy) != 0) {
    /* handle error */
}

LanceRef* ref = lance_ref_version_number(7);
LanceDataset* at_seven = NULL;
int32_t rc = lance_dataset_deep_clone(ds, "copy-v7.lance", ref, NULL, &at_seven);
lance_ref_close(ref);
```

```cpp
lance::Dataset copy = ds.deep_clone("copy.lance");
auto ref = lance::Ref::version_number(7);
lance::Dataset at_seven = ds.deep_clone("copy-v7.lance", &ref);
```

The target must not exist yet. The returned handle is a normal dataset whose
version id is the cloned snapshot's version; the source handle keeps its
snapshot and no source write lock is held during copying. The optional storage
options accessor contributes its native `Arc` without resolving or copying
credentials; NULL, or an accessor created with neither static options nor a
provider, passes native `None`. For datasets with additional base paths, native
deep clone reads them through the source dataset's object store; per-base
distinct source credentials are not supported. Native clone commits resolve the
target URI through the default object-store registry, so target schemes
registered only on a custom session are rejected. A copy or commit error does
not guarantee that the target remains unpublished; reopen the target to check
its state. Copied objects may remain under the target path and are not rolled
back.

## Diagnostic logging

Native diagnostics are opt-in. Until the host calls `lance_init_logging`, the
library installs no logger or tracing subscriber and emits no diagnostic output;
`RUST_LOG` alone has no effect.

```c
if (lance_init_logging("warn,lance=debug,opendal=debug") != 0) {
    /* Check lance_last_error_code()/message. */
}
```

```cpp
lance::init_logging("warn,lance=debug,opendal=debug");
```

The argument uses Rust `RUST_LOG` / tracing `EnvFilter` syntax, but the library
does not read the environment itself: comma-separated `target=level` directives
with an optional bare default level. NULL selects `info`; an empty string is
invalid. The call installs a stderr formatter for `tracing` events and, unless
the host already installed a `log` logger, a bridge for `log` records; both use
the same filter. A host global keeps its own filtering. For example,
`lance_init_logging(NULL)` enables info and above for every target.

The `tracing` subscriber and `log` logger are process globals that can be
installed only once. `lance_init_logging` preserves a logger or subscriber the
host already installed instead of replacing it, so the host global keeps
receiving its events. Calls are thread-safe and idempotent: the first successful
call's filter is final and later calls do nothing. An invalid filter returns
`-1` with `LANCE_ERR_INVALID_ARGUMENT` and leaves logging unconfigured, so a
later valid call can still enable it.

## Tokio runtime worker threads

`lance-c` creates one process-wide, multi-thread Tokio runtime lazily on the
first runtime-backed call. It does not add a binding-specific worker-count API
or environment variable. The locked Tokio version (1.53.1 in `Cargo.lock`)
reads `TOKIO_WORKER_THREADS` when that runtime is built because `lance-c` does
not call `Builder::worker_threads`.

- When unset, Tokio uses `std::thread::available_parallelism()`, the parallelism
  available to the process, normally one worker per available CPU.
- When set, it must parse as a positive `usize`. Zero, a non-numeric value, or a
  non-Unicode value makes Tokio panic while building the runtime; `lance-c` does
  not clamp or fall back. The panic is caught by the FFI panic boundary, so a
  runtime-backed call fails instead of running with a different count.
- The value is read once, when the runtime is first built. Set it before the
  first runtime-backed `lance_*` call; changing it later has no effect.

This variable controls the runtime's core worker threads only. Tokio's blocking
pool cap is `max_blocking_threads + worker_threads` (512 plus the worker count
by default), and the async callback dispatcher uses its own thread. So
`TOKIO_WORKER_THREADS` does not bound every thread the library may create.

## Process lifetime and embedding

The process-global state described above lasts for the whole process: the
shared runtime and the diagnostic globals installed by `lance_init_logging`
are created lazily and are never taken down. The callback dispatcher is
attempted on the first callback dispatch and, if its thread starts, normally
remains for the process lifetime. The C API exposes no shutdown, reset, or
reinitialization call for any of them.

- The runtime is built on first use and reused by every later call. Its core
  worker threads stay alive until process exit, including while the library is
  idle. Blocking-pool threads are separate: Tokio retires them after an idle
  keep-alive (10 seconds by default), and they can be recreated later.
- The async callback dispatcher is started on demand: the binding attempts to
  start its thread on the first callback dispatch. If the thread starts, it
  normally remains for the process lifetime. If startup fails or a completion
  cannot be queued, that completion is delivered on the thread attempting to
  dispatch it, which is not necessarily the thread that made the original API
  call.
- `lance_init_logging` installs its globals once. There is no API to remove or
  reload them, and later calls are no-ops, so the configuration from the first
  successful call lasts until process exit.

Hosts that manage process lifetime themselves should account for this state:

- **`fork()`**: `lance-c` provides no post-fork reset or reinitialization API
  and registers no `pthread_atfork` handler of its own. Behavior of inherited
  runtime, dispatcher, or logging state in a forked child is not part of the
  documented contract. A host that forks after its first runtime-backed call or
  after `lance_init_logging` has no binding API that restores a clean state in
  the child.
- **`dlopen`/`dlclose`**: because there is no shutdown call, an unload path has
  no supported way to stop the runtime or dispatcher or to uninstall the
  logging globals before the library is unloaded. Unloading the library after
  it has used the runtime is not a documented support path.
