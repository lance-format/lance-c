// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#define _POSIX_C_SOURCE 200809L

#include "lance/lance.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ARROW_FLAG_NULLABLE 2
#define VECTOR_DIM 8
#define VECTOR_FRAGMENTS 2
#define VECTOR_ROWS_PER_FRAGMENT 128

static void fail(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    const char *message = lance_last_error_message();
    if (message != NULL) {
        fprintf(stderr, ": %s", message);
        lance_free_string(message);
    }
    fputc('\n', stderr);
    exit(1);
}

#define ASSERT(condition, message)                                             \
    do {                                                                       \
        if (!(condition)) {                                                     \
            fail("assertion failed at %s:%d: %s", __FILE__, __LINE__, message); \
        }                                                                      \
    } while (0)

#define CHECK_OK()                                                             \
    do {                                                                       \
        if (lance_last_error_code() != LANCE_OK) {                             \
            fail("lance error at %s:%d", __FILE__, __LINE__);                  \
        }                                                                      \
    } while (0)

typedef struct {
    int64_t *values;
    struct ArrowArray top;
    struct ArrowArray child;
    struct ArrowArray *children[1];
    const void *top_buffers[1];
    const void *child_buffers[2];
} BatchAllocation;

typedef struct {
    struct ArrowSchema top;
    struct ArrowSchema child;
    struct ArrowSchema *children[1];
} SchemaAllocation;

typedef struct {
    int32_t *ids;
    float *embeddings;
    struct ArrowArray top;
    struct ArrowArray id;
    struct ArrowArray embedding;
    struct ArrowArray item;
    struct ArrowArray *top_children[2];
    struct ArrowArray *embedding_children[1];
    const void *top_buffers[1];
    const void *id_buffers[2];
    const void *embedding_buffers[1];
    const void *item_buffers[2];
} VectorBatchAllocation;

typedef struct {
    struct ArrowSchema top;
    struct ArrowSchema id;
    struct ArrowSchema embedding;
    struct ArrowSchema item;
    struct ArrowSchema *top_children[2];
    struct ArrowSchema *embedding_children[1];
} VectorSchemaAllocation;

typedef struct {
    int64_t start;
    size_t rows;
    int emitted;
} SourceStream;

typedef struct {
    int32_t start;
    size_t rows;
    int emitted;
} VectorSourceStream;

static void release_noop_array(struct ArrowArray *array) {
    if (array != NULL) {
        array->release = NULL;
    }
}

static void release_noop_schema(struct ArrowSchema *schema) {
    if (schema != NULL) {
        schema->release = NULL;
    }
}

static void release_batch(struct ArrowArray *array) {
    if (array == NULL || array->private_data == NULL) {
        return;
    }
    BatchAllocation *allocation = (BatchAllocation *)array->private_data;
    free(allocation->values);
    free(allocation);
    array->release = NULL;
    array->private_data = NULL;
}

static void release_schema(struct ArrowSchema *schema) {
    if (schema == NULL || schema->private_data == NULL) {
        return;
    }
    free(schema->private_data);
    schema->release = NULL;
    schema->private_data = NULL;
}

static void release_vector_batch(struct ArrowArray *array) {
    if (array == NULL || array->private_data == NULL) {
        return;
    }
    VectorBatchAllocation *allocation = (VectorBatchAllocation *)array->private_data;
    free(allocation->ids);
    free(allocation->embeddings);
    free(allocation);
    array->release = NULL;
    array->private_data = NULL;
}

static void release_vector_schema(struct ArrowSchema *schema) {
    if (schema == NULL || schema->private_data == NULL) {
        return;
    }
    free(schema->private_data);
    schema->release = NULL;
    schema->private_data = NULL;
}

static int source_get_schema(struct ArrowArrayStream *stream,
                             struct ArrowSchema *out) {
    (void)stream;
    SchemaAllocation *allocation = (SchemaAllocation *)calloc(1, sizeof(*allocation));
    if (allocation == NULL) {
        return -1;
    }

    allocation->children[0] = &allocation->child;
    allocation->top.format = "+s";
    allocation->top.n_children = 1;
    allocation->top.children = allocation->children;
    allocation->top.release = release_schema;
    allocation->top.private_data = allocation;

    allocation->child.format = "l";
    allocation->child.name = "id";
    allocation->child.flags = ARROW_FLAG_NULLABLE;
    allocation->child.release = release_noop_schema;

    *out = allocation->top;
    out->private_data = allocation;
    return 0;
}

static int source_get_next(struct ArrowArrayStream *stream,
                           struct ArrowArray *out) {
    SourceStream *source = (SourceStream *)stream->private_data;
    if (source->emitted != 0) {
        memset(out, 0, sizeof(*out));
        return 0;
    }

    BatchAllocation *allocation = (BatchAllocation *)calloc(1, sizeof(*allocation));
    if (allocation == NULL) {
        return -1;
    }
    allocation->values = (int64_t *)malloc(source->rows * sizeof(int64_t));
    if (allocation->values == NULL && source->rows != 0) {
        free(allocation);
        return -1;
    }
    for (size_t row = 0; row < source->rows; ++row) {
        allocation->values[row] = source->start + (int64_t)row;
    }

    allocation->child_buffers[1] = allocation->values;
    allocation->child.length = (int64_t)source->rows;
    allocation->child.n_buffers = 2;
    allocation->child.buffers = allocation->child_buffers;
    allocation->child.release = release_noop_array;
    allocation->child.private_data = allocation;

    allocation->children[0] = &allocation->child;
    allocation->top.length = (int64_t)source->rows;
    allocation->top.n_buffers = 1;
    allocation->top.buffers = allocation->top_buffers;
    allocation->top.n_children = 1;
    allocation->top.children = allocation->children;
    allocation->top.release = release_batch;
    allocation->top.private_data = allocation;

    *out = allocation->top;
    out->private_data = allocation;
    source->emitted = 1;
    return 0;
}

static const char *source_get_last_error(struct ArrowArrayStream *stream) {
    (void)stream;
    return "failed to allocate int64 C Data test batch";
}

static void release_stream(struct ArrowArrayStream *stream) {
    if (stream == NULL || stream->private_data == NULL) {
        return;
    }
    free(stream->private_data);
    stream->private_data = NULL;
    stream->release = NULL;
}

static struct ArrowArrayStream make_stream(int64_t start, size_t rows) {
    SourceStream *source = (SourceStream *)calloc(1, sizeof(*source));
    ASSERT(source != NULL, "allocate source stream");
    source->start = start;
    source->rows = rows;

    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    stream.get_schema = source_get_schema;
    stream.get_next = source_get_next;
    stream.get_last_error = source_get_last_error;
    stream.release = release_stream;
    stream.private_data = source;
    return stream;
}

static int vector_source_get_schema(struct ArrowArrayStream *stream,
                                    struct ArrowSchema *out) {
    (void)stream;
    VectorSchemaAllocation *allocation =
        (VectorSchemaAllocation *)calloc(1, sizeof(*allocation));
    if (allocation == NULL) {
        return -1;
    }

    allocation->top_children[0] = &allocation->id;
    allocation->top_children[1] = &allocation->embedding;
    allocation->embedding_children[0] = &allocation->item;

    allocation->top.format = "+s";
    allocation->top.n_children = 2;
    allocation->top.children = allocation->top_children;
    allocation->top.release = release_vector_schema;
    allocation->top.private_data = allocation;

    allocation->id.format = "i";
    allocation->id.name = "id";
    allocation->id.flags = ARROW_FLAG_NULLABLE;
    allocation->id.release = release_noop_schema;

    allocation->embedding.format = "+w:8";
    allocation->embedding.name = "embedding";
    allocation->embedding.flags = ARROW_FLAG_NULLABLE;
    allocation->embedding.n_children = 1;
    allocation->embedding.children = allocation->embedding_children;
    allocation->embedding.release = release_noop_schema;

    allocation->item.format = "f";
    allocation->item.name = "item";
    allocation->item.flags = ARROW_FLAG_NULLABLE;
    allocation->item.release = release_noop_schema;

    *out = allocation->top;
    out->private_data = allocation;
    return 0;
}

static float fixture_vector_component(int32_t id, size_t component) {
    switch (component) {
    case 0:
        return (float)((id * 3) % 29);
    case 1:
        return (float)((id * 5) % 31);
    case 2:
        return (float)((id * 7) % 37);
    case 3:
        return (float)((id * 11) % 41);
    default:
        return 0.0f;
    }
}

static int vector_source_get_next(struct ArrowArrayStream *stream,
                                  struct ArrowArray *out) {
    VectorSourceStream *source = (VectorSourceStream *)stream->private_data;
    if (source->emitted != 0) {
        memset(out, 0, sizeof(*out));
        return 0;
    }

    VectorBatchAllocation *allocation =
        (VectorBatchAllocation *)calloc(1, sizeof(*allocation));
    if (allocation == NULL) {
        return -1;
    }
    allocation->ids =
        (int32_t *)malloc(source->rows * sizeof(*allocation->ids));
    allocation->embeddings =
        (float *)malloc(source->rows * VECTOR_DIM * sizeof(*allocation->embeddings));
    if ((allocation->ids == NULL && source->rows != 0) ||
        (allocation->embeddings == NULL && source->rows != 0)) {
        free(allocation->ids);
        free(allocation->embeddings);
        free(allocation);
        return -1;
    }
    for (size_t row = 0; row < source->rows; ++row) {
        int32_t id = source->start + (int32_t)row;
        allocation->ids[row] = id;
        for (size_t component = 0; component < VECTOR_DIM; ++component) {
            allocation->embeddings[row * VECTOR_DIM + component] =
                fixture_vector_component(id, component);
        }
    }

    allocation->id_buffers[1] = allocation->ids;
    allocation->id.length = (int64_t)source->rows;
    allocation->id.n_buffers = 2;
    allocation->id.buffers = allocation->id_buffers;
    allocation->id.release = release_noop_array;

    allocation->item_buffers[1] = allocation->embeddings;
    allocation->item.length = (int64_t)(source->rows * VECTOR_DIM);
    allocation->item.n_buffers = 2;
    allocation->item.buffers = allocation->item_buffers;
    allocation->item.release = release_noop_array;

    allocation->embedding_children[0] = &allocation->item;
    allocation->embedding.length = (int64_t)source->rows;
    allocation->embedding.n_buffers = 1;
    allocation->embedding.buffers = allocation->embedding_buffers;
    allocation->embedding.n_children = 1;
    allocation->embedding.children = allocation->embedding_children;
    allocation->embedding.release = release_noop_array;

    allocation->top_children[0] = &allocation->id;
    allocation->top_children[1] = &allocation->embedding;
    allocation->top.length = (int64_t)source->rows;
    allocation->top.n_buffers = 1;
    allocation->top.buffers = allocation->top_buffers;
    allocation->top.n_children = 2;
    allocation->top.children = allocation->top_children;
    allocation->top.release = release_vector_batch;
    allocation->top.private_data = allocation;

    *out = allocation->top;
    out->private_data = allocation;
    source->emitted = 1;
    return 0;
}

static const char *vector_source_get_last_error(struct ArrowArrayStream *stream) {
    (void)stream;
    return "failed to allocate vector C Data test batch";
}

static struct ArrowArrayStream make_vector_stream(int32_t start, size_t rows) {
    VectorSourceStream *source =
        (VectorSourceStream *)calloc(1, sizeof(*source));
    ASSERT(source != NULL, "allocate vector source stream");
    source->start = start;
    source->rows = rows;

    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    stream.get_schema = vector_source_get_schema;
    stream.get_next = vector_source_get_next;
    stream.get_last_error = vector_source_get_last_error;
    stream.release = release_stream;
    stream.private_data = source;
    return stream;
}

static void write_part(const char *uri, int32_t mode, int64_t start,
                       size_t rows) {
    struct ArrowArrayStream stream = make_stream(start, rows);
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(stream.get_schema(&stream, &schema) == 0, "export write schema");

    LanceWriteOptions options;
    memset(&options, 0, sizeof(options));
    LanceDataset *dataset = NULL;
    int32_t status = lance_dataset_write_with_options(
        uri, &schema, &stream, mode, &options, NULL, &dataset);
    if (schema.release != NULL) {
        schema.release(&schema);
    }
    if (stream.release != NULL) {
        stream.release(&stream);
    }
    ASSERT(status == 0, "write fixture fragment");
    CHECK_OK();
    ASSERT(dataset != NULL, "write returned no dataset");
    lance_dataset_close(dataset);
}

static void write_vector_part(const char *uri, int32_t mode, int32_t start,
                              size_t rows) {
    struct ArrowArrayStream stream = make_vector_stream(start, rows);
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(stream.get_schema(&stream, &schema) == 0, "export vector write schema");

    LanceWriteOptions options;
    memset(&options, 0, sizeof(options));
    LanceDataset *dataset = NULL;
    int32_t status = lance_dataset_write_with_options(
        uri, &schema, &stream, mode, &options, NULL, &dataset);
    if (schema.release != NULL) {
        schema.release(&schema);
    }
    if (stream.release != NULL) {
        stream.release(&stream);
    }
    ASSERT(status == 0, "write vector fixture fragment");
    CHECK_OK();
    ASSERT(dataset != NULL, "vector write returned no dataset");
    lance_dataset_close(dataset);
}

static void create_fixture(const char *uri) {
    write_part(uri, LANCE_WRITE_CREATE, 0, 1000);
    write_part(uri, LANCE_WRITE_APPEND, 1000, 1000);

    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open created fixture");
    CHECK_OK();
    uint64_t version = lance_dataset_version(dataset);
    CHECK_OK();
    uint64_t fragment_count = lance_dataset_fragment_count(dataset);
    CHECK_OK();
    ASSERT(version == 2, "fixture must have exactly two committed versions");
    ASSERT(fragment_count == 2, "fixture must have exactly two fragments");

    uint64_t fragment_ids[2] = {0, 0};
    ASSERT(lance_dataset_fragment_ids(dataset, fragment_ids) == 0,
           "list fixture fragment ids");
    CHECK_OK();
    printf("created_uri=%s\n", uri);
    printf("created_version=%" PRIu64 "\n", version);
    printf("fragment_count=%" PRIu64 "\n", fragment_count);
    printf("fragment_ids=%" PRIu64 ",%" PRIu64 "\n", fragment_ids[0],
           fragment_ids[1]);
    lance_dataset_close(dataset);
}

static void create_vector_fixture(const char *uri) {
    write_vector_part(uri, LANCE_WRITE_CREATE, 0, VECTOR_ROWS_PER_FRAGMENT);
    write_vector_part(uri, LANCE_WRITE_APPEND, VECTOR_ROWS_PER_FRAGMENT,
                      VECTOR_ROWS_PER_FRAGMENT);

    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open created vector fixture");
    CHECK_OK();
    uint64_t version = lance_dataset_version(dataset);
    CHECK_OK();
    uint64_t fragment_count = lance_dataset_fragment_count(dataset);
    CHECK_OK();
    ASSERT(version == 2, "vector fixture must have exactly two committed versions");
    ASSERT(fragment_count == VECTOR_FRAGMENTS,
           "vector fixture must have exactly two fragments");

    uint64_t fragment_ids[VECTOR_FRAGMENTS] = {0, 0};
    ASSERT(lance_dataset_fragment_ids(dataset, fragment_ids) == 0,
           "list vector fixture fragment ids");
    CHECK_OK();
    printf("created_uri=%s\n", uri);
    printf("created_version=%" PRIu64 "\n", version);
    printf("fragment_count=%" PRIu64 "\n", fragment_count);
    printf("fragment_ids=%" PRIu64 ",%" PRIu64 "\n", fragment_ids[0],
           fragment_ids[1]);
    printf("rows_per_fragment=%d\n", VECTOR_ROWS_PER_FRAGMENT);
    printf("vector_dimension=%d\n", VECTOR_DIM);
    lance_dataset_close(dataset);
}

static void format_uuid(const uint8_t uuid[16], char out[37]) {
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6],
             uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13],
             uuid[14], uuid[15]);
}

static void write_metadata(const char *path, const uint8_t *bytes, size_t len) {
    FILE *file = fopen(path, "wb");
    ASSERT(file != NULL, "open metadata output");
    size_t written = fwrite(bytes, 1, len, file);
    int close_status = fclose(file);
    ASSERT(written == len, "write complete metadata payload");
    ASSERT(close_status == 0, "close metadata output");
}

static void build_segments(const char *uri, const char *metadata_dir) {
    if (mkdir(metadata_dir, 0777) != 0 && errno != EEXIST) {
        fail("create metadata directory %s", metadata_dir);
    }

    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open fixture for segment build");
    CHECK_OK();

    uint64_t version = lance_dataset_version(dataset);
    CHECK_OK();
    uint64_t fragment_count = lance_dataset_fragment_count(dataset);
    CHECK_OK();
    ASSERT(version == 2, "segment build fixture version must be 2");
    ASSERT(fragment_count == 2, "segment build requires exactly two fragments");

    uint64_t fragment_ids[2] = {0, 0};
    ASSERT(lance_dataset_fragment_ids(dataset, fragment_ids) == 0,
           "list segment fixture fragments");
    CHECK_OK();

    char uuids[2][37] = {{0}};
    for (size_t segment = 0; segment < 2; ++segment) {
        ASSERT(fragment_ids[segment] <= UINT32_MAX,
               "fixture fragment id must fit uint32_t");
        uint32_t fragment_id = (uint32_t)fragment_ids[segment];

        LanceIndexSegmentBuildOptions options;
        memset(&options, 0, sizeof(options));
        options.fragment_ids = &fragment_id;
        options.fragment_count = 1;
        options.index_uuid = NULL;
        options.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;

        LanceScalarIndexOptions scalar_options = {true, true};
        LanceIndexSegmentBuilder *builder =
            lance_index_segment_builder_new_scalar_with_options(
                dataset, "id", "btree_cross_sdk", LANCE_SCALAR_BTREE, NULL,
                &scalar_options, &options);
        ASSERT(builder != NULL, "create BTree segment builder");
        CHECK_OK();

        uint8_t *bytes = NULL;
        size_t len = 0;
        ASSERT(lance_index_segment_builder_execute_uncommitted(
                   builder, &bytes, &len) == 0,
               "build uncommitted BTree segment");
        CHECK_OK();
        lance_index_segment_builder_free(builder);
        ASSERT(bytes != NULL && len > 0, "segment build returned no bytes");

        LanceIndexSegmentMetadata *metadata = NULL;
        ASSERT(lance_index_segment_metadata_parse(bytes, len, &metadata) == 0,
               "parse C-produced IndexMetadata");
        CHECK_OK();
        ASSERT(lance_index_segment_metadata_fragment_count(metadata) == 1,
               "segment must cover exactly one fragment");
        uint32_t covered[1] = {0};
        size_t covered_count = 0;
        ASSERT(lance_index_segment_metadata_fragment_ids(
                   metadata, covered, 1, &covered_count) == 0,
               "read segment coverage");
        CHECK_OK();
        ASSERT(covered_count == 1 && covered[0] == fragment_id,
               "segment coverage must match its assigned fragment");
        ASSERT(lance_index_segment_metadata_dataset_version(metadata) == version,
               "segment dataset version must match the fixture version");
        CHECK_OK();
        ASSERT(lance_index_segment_metadata_index_version(metadata) == 0,
               "non-stable-row-id BTree must use index version 0");
        CHECK_OK();
        ASSERT(lance_index_segment_metadata_index_type(metadata) ==
                   LANCE_SCALAR_BTREE,
               "segment metadata must report BTree");
        CHECK_OK();

        const char *type_url =
            lance_index_segment_metadata_index_details_type_url(metadata);
        ASSERT(type_url != NULL && strstr(type_url, "BTreeIndexDetails") != NULL,
               "segment metadata must contain BTree Any details");
        CHECK_OK();
        char type_url_copy[256];
        int type_url_len =
            snprintf(type_url_copy, sizeof(type_url_copy), "%s", type_url);
        ASSERT(type_url_len > 0 && (size_t)type_url_len < sizeof(type_url_copy),
               "copy Any type URL");
        const char *name = lance_index_segment_metadata_name(metadata);
        ASSERT(name != NULL && strcmp(name, "btree_cross_sdk") == 0,
               "segment metadata name");

        size_t field_count =
            lance_index_segment_metadata_field_count(metadata);
        CHECK_OK();
        ASSERT(field_count == 1, "C producer currently emits one keyed field");
        int32_t field_ids[1] = {0};
        size_t returned_fields = 0;
        ASSERT(lance_index_segment_metadata_field_ids(
                   metadata, field_ids, 1, &returned_fields) == 0,
               "read indexed field ids");
        CHECK_OK();
        ASSERT(returned_fields == 1 && field_ids[0] >= 0,
               "segment field id must be a non-negative Java int");

        uint8_t uuid[16] = {0};
        ASSERT(lance_index_segment_metadata_uuid(metadata, uuid) == 0,
               "read segment uuid");
        CHECK_OK();
        format_uuid(uuid, uuids[segment]);
        lance_index_segment_metadata_free(metadata);

        char path[4096];
        int written = snprintf(path, sizeof(path), "%s/segment-%zu.pb",
                               metadata_dir, segment);
        ASSERT(written > 0 && (size_t)written < sizeof(path),
               "metadata output path");
        write_metadata(path, bytes, len);

        printf("segment_%zu_file=%s\n", segment, path);
        printf("segment_%zu_bytes=%zu\n", segment, len);
        printf("segment_%zu_uuid=%s\n", segment, uuids[segment]);
        printf("segment_%zu_fragment_id=%" PRIu64 "\n", segment,
               fragment_ids[segment]);
        printf("segment_%zu_dataset_version=%" PRIu64 "\n", segment, version);
        printf("segment_%zu_index_version=0\n", segment);
        printf("segment_%zu_field_id=%d\n", segment, field_ids[0]);
        printf("segment_%zu_type_url=%s\n", segment, type_url_copy);
        lance_free_bytes(bytes);
    }

    ASSERT(strcmp(uuids[0], uuids[1]) != 0,
           "independent C builds must generate distinct segment UUIDs");
    uint64_t version_after = lance_dataset_version(dataset);
    CHECK_OK();
    ASSERT(version_after == version,
           "uncommitted segment builds must not publish a dataset version");
    printf("dataset_version_before_java_commit=%" PRIu64 "\n", version_after);
    lance_dataset_close(dataset);
}

static void print_metadata_common(
    size_t segment, const char *path, const uint8_t *bytes, size_t len,
    const char *uuid, uint64_t fragment_id, uint64_t dataset_version,
    int32_t index_version, const char *type_url, int32_t field_id) {
    printf("segment_%zu_file=%s\n", segment, path);
    printf("segment_%zu_bytes=%zu\n", segment, len);
    printf("segment_%zu_uuid=%s\n", segment, uuid);
    printf("segment_%zu_fragment_id=%" PRIu64 "\n", segment, fragment_id);
    printf("segment_%zu_dataset_version=%" PRIu64 "\n", segment,
           dataset_version);
    printf("segment_%zu_index_version=%d\n", segment, index_version);
    printf("segment_%zu_field_id=%d\n", segment, field_id);
    printf("segment_%zu_type_url=%s\n", segment, type_url);
    (void)bytes;
}

static void release_model(struct ArrowArray *array,
                          struct ArrowSchema *schema) {
    if (array->release != NULL) {
        array->release(array);
    }
    if (schema->release != NULL) {
        schema->release(schema);
    }
}

static void build_vector_segments(const char *uri, const char *metadata_dir) {
    if (mkdir(metadata_dir, 0777) != 0 && errno != EEXIST) {
        fail("create metadata directory %s", metadata_dir);
    }

    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open vector fixture for segment build");
    CHECK_OK();

    uint64_t version = lance_dataset_version(dataset);
    CHECK_OK();
    uint64_t fragment_count = lance_dataset_fragment_count(dataset);
    CHECK_OK();
    ASSERT(version == 2, "vector segment build fixture version must be 2");
    ASSERT(fragment_count == VECTOR_FRAGMENTS,
           "vector segment build requires exactly two fragments");

    uint64_t fragment_ids[VECTOR_FRAGMENTS] = {0, 0};
    ASSERT(lance_dataset_fragment_ids(dataset, fragment_ids) == 0,
           "list vector segment fixture fragments");
    CHECK_OK();

    struct ArrowArray centroids;
    struct ArrowSchema centroid_schema;
    struct ArrowArray codebook;
    struct ArrowSchema codebook_schema;
    memset(&centroids, 0, sizeof(centroids));
    memset(&centroid_schema, 0, sizeof(centroid_schema));
    memset(&codebook, 0, sizeof(codebook));
    memset(&codebook_schema, 0, sizeof(codebook_schema));

    LanceIndexTrainerOptions trainer;
    memset(&trainer, 0, sizeof(trainer));
    trainer.sample_rate.value = 256;
    trainer.sample_rate.is_set = true;
    trainer.max_iters.value = 4;
    trainer.max_iters.is_set = true;

    ASSERT(lance_index_train_ivf_model_with_options(
               dataset, "embedding", 4, LANCE_METRIC_L2, NULL, 0, &trainer,
               &centroids, &centroid_schema) == 0,
           "train shared IVF centroids");
    CHECK_OK();
    ASSERT(lance_index_train_pq_model_with_options(
               dataset, "embedding", 2, 4, LANCE_METRIC_L2, NULL, 0,
               &centroids, &centroid_schema, &trainer, &codebook,
               &codebook_schema) == 0,
           "train shared PQ codebook");
    CHECK_OK();

    LanceVectorIndexOptions options;
    memset(&options, 0, sizeof(options));
    options.index_type = LANCE_INDEX_IVF_PQ;
    options.metric = LANCE_METRIC_L2;
    options.num_partitions.value = 4;
    options.num_partitions.is_set = true;
    options.num_sub_vectors.value = 2;
    options.num_sub_vectors.is_set = true;
    options.num_bits.value = 4;
    options.num_bits.is_set = true;
    options.max_iters.value = 4;
    options.max_iters.is_set = true;
    options.sample_rate.value = 256;
    options.sample_rate.is_set = true;
    options.ivf_centroids = &centroids;
    options.ivf_centroids_schema = &centroid_schema;
    options.pq_codebook = &codebook;
    options.pq_codebook_schema = &codebook_schema;
    options.index_file_version = LANCE_INDEX_FILE_VERSION_V3;
    options.has_train = true;
    /* Native treats train=false as a definition-only build with empty coverage. */
    options.train = true;

    char uuids[VECTOR_FRAGMENTS][37] = {{0}};
    for (size_t segment = 0; segment < VECTOR_FRAGMENTS; ++segment) {
        ASSERT(fragment_ids[segment] <= UINT32_MAX,
               "vector fixture fragment id must fit uint32_t");
        uint32_t fragment_id = (uint32_t)fragment_ids[segment];

        LanceVectorIndexSegmentScope scope;
        memset(&scope, 0, sizeof(scope));
        scope.fragment_ids = &fragment_id;
        scope.fragment_count = 1;

        LanceIndexSegmentBuilder *builder =
            lance_index_segment_builder_new_vector_with_options(
                dataset, "embedding", "ivfpq_cross_sdk", &options, &scope);
        ASSERT(builder != NULL, "create IVF_PQ segment builder");
        CHECK_OK();

        uint8_t *bytes = NULL;
        size_t len = 0;
        ASSERT(lance_index_segment_builder_execute_uncommitted(
                   builder, &bytes, &len) == 0,
               "build uncommitted IVF_PQ segment");
        CHECK_OK();
        lance_index_segment_builder_free(builder);
        ASSERT(bytes != NULL && len > 0, "vector segment build returned no bytes");

        LanceIndexSegmentMetadata *metadata = NULL;
        ASSERT(lance_index_segment_metadata_parse(bytes, len, &metadata) == 0,
               "parse C-produced vector IndexMetadata");
        CHECK_OK();
        ASSERT(lance_index_segment_metadata_fragment_count(metadata) == 1,
               "vector segment must cover exactly one fragment");
        uint32_t covered[1] = {0};
        size_t covered_count = 0;
        ASSERT(lance_index_segment_metadata_fragment_ids(
                   metadata, covered, 1, &covered_count) == 0,
               "read vector segment coverage");
        CHECK_OK();
        ASSERT(covered_count == 1 && covered[0] == fragment_id,
               "vector segment coverage must match its assigned fragment");
        ASSERT(lance_index_segment_metadata_dataset_version(metadata) == version,
               "vector segment dataset version must match the fixture version");
        CHECK_OK();
        ASSERT(lance_index_segment_metadata_index_version(metadata) == 1,
               "IVF_PQ index version must be 1");
        CHECK_OK();
        ASSERT(lance_index_segment_metadata_index_type(metadata) ==
                   LANCE_INDEX_IVF_PQ,
               "vector segment metadata must report IVF_PQ");
        CHECK_OK();

        const char *type_url =
            lance_index_segment_metadata_index_details_type_url(metadata);
        ASSERT(type_url != NULL && strstr(type_url, "VectorIndexDetails") != NULL,
               "vector segment metadata must contain VectorIndexDetails Any");
        CHECK_OK();
        char type_url_copy[256];
        int type_url_len =
            snprintf(type_url_copy, sizeof(type_url_copy), "%s", type_url);
        ASSERT(type_url_len > 0 && (size_t)type_url_len < sizeof(type_url_copy),
               "copy vector Any type URL");
        const char *name = lance_index_segment_metadata_name(metadata);
        ASSERT(name != NULL && strcmp(name, "ivfpq_cross_sdk") == 0,
               "vector segment metadata name");

        size_t field_count = lance_index_segment_metadata_field_count(metadata);
        CHECK_OK();
        ASSERT(field_count == 1, "vector producer must emit one keyed field");
        int32_t field_ids[1] = {0};
        size_t returned_fields = 0;
        ASSERT(lance_index_segment_metadata_field_ids(
                   metadata, field_ids, 1, &returned_fields) == 0,
               "read vector indexed field ids");
        CHECK_OK();
        ASSERT(returned_fields == 1 && field_ids[0] == 1,
               "vector segment must index the embedding field id 1");

        uint8_t uuid[16] = {0};
        ASSERT(lance_index_segment_metadata_uuid(metadata, uuid) == 0,
               "read vector segment uuid");
        CHECK_OK();
        format_uuid(uuid, uuids[segment]);
        lance_index_segment_metadata_free(metadata);

        char path[4096];
        int written = snprintf(path, sizeof(path), "%s/segment-%zu.pb",
                               metadata_dir, segment);
        ASSERT(written > 0 && (size_t)written < sizeof(path),
               "vector metadata output path");
        write_metadata(path, bytes, len);
        print_metadata_common(segment, path, bytes, len, uuids[segment],
                              fragment_ids[segment], version, 1, type_url_copy,
                              field_ids[0]);
        lance_free_bytes(bytes);
    }

    ASSERT(strcmp(uuids[0], uuids[1]) != 0,
           "independent vector builds must generate distinct segment UUIDs");
    uint64_t version_after = lance_dataset_version(dataset);
    CHECK_OK();
    ASSERT(version_after == version,
           "uncommitted vector builds must not publish a dataset version");
    printf("dataset_version_before_java_commit=%" PRIu64 "\n", version_after);
    lance_dataset_close(dataset);
    release_model(&centroids, &centroid_schema);
    release_model(&codebook, &codebook_schema);
}

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s create <dataset-path>\n"
            "       %s build <dataset-path> <metadata-directory>\n"
            "       %s create-vector <dataset-path>\n"
            "       %s build-vector <dataset-path> <metadata-directory>\n",
            program, program, program, program);
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "create") == 0) {
        create_fixture(argv[2]);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "build") == 0) {
        build_segments(argv[2], argv[3]);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "create-vector") == 0) {
        create_vector_fixture(argv[2]);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "build-vector") == 0) {
        build_vector_segments(argv[2], argv[3]);
        return 0;
    }
    usage(argv[0]);
    return 2;
}
