// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

package org.lance.crosssdk;

import com.google.protobuf.Any;
import org.apache.arrow.memory.BufferAllocator;
import org.apache.arrow.memory.RootAllocator;
import org.apache.arrow.vector.BigIntVector;
import org.apache.arrow.vector.Float4Vector;
import org.apache.arrow.vector.IntVector;
import org.apache.arrow.vector.VectorSchemaRoot;
import org.apache.arrow.vector.ipc.ArrowReader;
import org.lance.Dataset;
import org.lance.Fragment;
import org.lance.ReadOptions;
import org.lance.crosssdk.proto.InvertedIndexDetailsProto.InvertedIndexDetails;
import org.lance.crosssdk.proto.InvertedIndexDetailsProto.InvertedIndexDetails.DocumentGranularity;
import org.lance.crosssdk.proto.VectorIndexDetailsProto.VectorIndexDetails;
import org.lance.crosssdk.proto.VectorIndexDetailsProto.VectorMetricType;
import org.lance.index.DistanceType;
import org.lance.index.Index;
import org.lance.index.IndexOptions;
import org.lance.index.IndexParams;
import org.lance.index.IndexType;
import org.lance.index.scalar.ScalarIndexParams;
import org.lance.index.vector.VectorIndexParams;
import org.lance.ipc.FullTextQuery;
import org.lance.ipc.LanceScanner;
import org.lance.ipc.Query;
import org.lance.ipc.ScanOptions;
import org.lance.ipc.ScanStats;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.stream.Collectors;

public final class CrossSdkAcceptance {
  private static final String BTREE_INDEX_NAME = "btree_cross_sdk";
  private static final String BTREE_COLUMN = "id";

  private static final String VECTOR_INDEX_NAME = "ivfpq_cross_sdk";
  private static final String VECTOR_COLUMN = "embedding";
  private static final int VECTOR_FIELD_ID = 1;
  private static final int VECTOR_DIMENSION = 8;
  private static final int VECTOR_PARTITIONS = 4;
  private static final int VECTOR_SUB_VECTORS = 2;
  private static final int VECTOR_BITS = 4;
  private static final int VECTOR_REFINE_FACTOR = 256;
  private static final int VECTOR_K = 4;
  private static final int[] VECTOR_MULTIPLIERS = {3, 5, 7, 11, 13, 17, 19, 23};
  private static final int[] VECTOR_MODULI = {29, 31, 37, 41, 43, 47, 53, 59};

  private static final String FTS_INDEX_NAME = "fts_cross_sdk";
  private static final String FTS_COLUMN = "text";
  private static final int FTS_FIELD_ID = 1;
  private static final int FTS_INDEX_VERSION = 2;
  private static final int FTS_TOTAL_ROWS = 128;
  private static final String FTS_PARAMS_JSON =
      "{\"base_tokenizer\":\"simple\",\"language\":\"English\","
          + "\"with_position\":true,\"max_token_length\":40,"
          + "\"lower_case\":true,\"stem\":false,\"remove_stop_words\":false,"
          + "\"format_version\":2}";

  private CrossSdkAcceptance() {}

  private static final class QueryCase {
    final String filter;
    final List<Long> expected;

    QueryCase(String filter, List<Long> expected) {
      this.filter = filter;
      this.expected = expected;
    }
  }

  private static final class QueryResult {
    final List<Long> ids;
    final ScanStats stats;

    QueryResult(List<Long> ids, ScanStats stats) {
      this.ids = ids;
      this.stats = stats;
    }
  }

  private static final class VectorQueryCase {
    final int keyId;
    final List<Integer> expectedIds;
    final List<Float> expectedDistances;

    VectorQueryCase(int keyId, List<Integer> expectedIds, List<Float> expectedDistances) {
      this.keyId = keyId;
      this.expectedIds = expectedIds;
      this.expectedDistances = expectedDistances;
    }
  }

  private static final class VectorQueryResult {
    final List<Integer> ids;
    final List<Float> distances;
    final ScanStats stats;

    VectorQueryResult(List<Integer> ids, List<Float> distances, ScanStats stats) {
      this.ids = ids;
      this.distances = distances;
      this.stats = stats;
    }
  }

  private static final class FtsQueryCase {
    final String name;
    final FullTextQuery query;
    final List<Integer> expected;

    FtsQueryCase(String name, FullTextQuery query, List<Integer> expected) {
      this.name = name;
      this.query = query;
      this.expected = expected;
    }
  }

  private static final class FtsQueryResult {
    final List<Integer> ids;
    final ScanStats stats;

    FtsQueryResult(List<Integer> ids, ScanStats stats) {
      this.ids = ids;
      this.stats = stats;
    }
  }

  private static void require(boolean condition, String message) {
    if (!condition) {
      throw new IllegalStateException(message);
    }
  }

  private static List<Long> range(long startInclusive, long endExclusive) {
    List<Long> values = new ArrayList<>();
    for (long value = startInclusive; value < endExclusive; ++value) {
      values.add(value);
    }
    return values;
  }

  private static String sha256(byte[] bytes) {
    try {
      return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes));
    } catch (Exception e) {
      throw new IllegalStateException("SHA-256 is unavailable", e);
    }
  }

  private static byte[] readBytes(Path path) throws IOException {
    byte[] bytes = Files.readAllBytes(path);
    require(bytes.length > 0, "empty metadata payload: " + path);
    return bytes;
  }

  private static Set<Integer> coverage(Index index) {
    return new HashSet<>(index.fragments().orElseThrow(
        () -> new IllegalStateException("missing fragment coverage for " + index.uuid())));
  }

  private static Map<UUID, Index> byUuid(Collection<Index> indexes) {
    Map<UUID, Index> result = new HashMap<>();
    for (Index index : indexes) {
      require(result.put(index.uuid(), index) == null, "duplicate segment uuid " + index.uuid());
    }
    return result;
  }

  private static void assertArrayEqualsBytes(byte[] expected, byte[] actual, String message) {
    if (!Arrays.equals(expected, actual)) {
      throw new IllegalStateException(message);
    }
  }

  private static String anyTypeUrl(byte[] bytes) {
    try {
      return Any.parseFrom(bytes).getTypeUrl();
    } catch (Exception e) {
      throw new IllegalStateException("invalid serialized google.protobuf.Any", e);
    }
  }

  private static List<Index> decodeSegments(
      Path metadataPath,
      String indexName,
      long datasetVersion,
      List<Integer> expectedFragments,
      List<Integer> expectedFields,
      int expectedIndexVersion,
      String detailsSuffix)
      throws IOException {
    List<byte[]> payloads = new ArrayList<>();
    for (int segment = 0; segment < 2; ++segment) {
      byte[] bytes = readBytes(metadataPath.resolve("segment-" + segment + ".pb"));
      payloads.add(bytes);
      System.out.printf(
          "metadata segment=%d bytes=%d sha256=%s%n", segment, bytes.length, sha256(bytes));
    }

    List<Index> produced = new ArrayList<>();
    Set<Integer> union = new HashSet<>();
    for (int segment = 0; segment < payloads.size(); ++segment) {
      Index index = CrossSdkIndexMetadataAdapter.toJavaIndex(payloads.get(segment));
      produced.add(index);
      require(!index.getFiles().isPresent(), "test adapter must not supply input files");
      require(!index.getSizeBytes().isPresent(), "test adapter must not supply input sizeBytes");
      require(indexName.equals(index.name()), "unexpected decoded index name");
      require(index.datasetVersion() == datasetVersion, "decoded dataset version changed");
      require(index.fields().equals(expectedFields), "decoded field ids changed");
      require(index.coveringFields().isEmpty(), "covering fields must remain empty");
      require(index.indexVersion() == expectedIndexVersion, "decoded index version changed");
      require(index.fragments().isPresent(), "decoded fragment coverage is missing");
      require(
          index.indexDetails().isPresent()
              && anyTypeUrl(index.indexDetails().orElseThrow()).endsWith(detailsSuffix),
          "decoded Any type URL does not match " + detailsSuffix);
      Set<Integer> fragmentIds = coverage(index);
      require(fragmentIds.size() == 1, "each decoded segment must cover one fragment");
      require(Collections.disjoint(union, fragmentIds), "decoded segment coverage overlaps");
      union.addAll(fragmentIds);
      System.out.printf(
          "decoded segment=%d uuid=%s fields=%s covering=%s dataset_version=%d "
              + "fragments=%s index_version=%d any_bytes=%d created_at=%s%n",
          segment,
          index.uuid(),
          index.fields(),
          index.coveringFields(),
          index.datasetVersion(),
          index.fragments().orElseThrow(),
          index.indexVersion(),
          index.indexDetails().orElseThrow().length,
          index.createdAt());
    }

    require(
        !produced.get(0).uuid().equals(produced.get(1).uuid()),
        "C producer returned duplicate UUIDs");
    require(
        produced.get(0).fields().equals(produced.get(1).fields()),
        "C-produced segments disagree on indexed fields");
    require(
        union.equals(new HashSet<>(expectedFragments)),
        "C-produced coverage does not match fixture fragments: " + union);
    return produced;
  }

  private static void validateSegments(
      List<Index> segments,
      String indexName,
      List<Integer> expectedFragments,
      List<Integer> expectedFields,
      long datasetVersion,
      int expectedIndexVersion,
      IndexType expectedIndexType,
      String detailsSuffix,
      Map<UUID, Index> produced,
      boolean requirePublishedFiles) {
    require(segments.size() == 2, "expected two physical segments, got " + segments.size());
    require(
        segments.stream().map(Index::uuid).distinct().count() == 2,
        "physical segment UUIDs are not distinct");

    Set<Integer> union = new HashSet<>();
    for (Index segment : segments) {
      require(indexName.equals(segment.name()), "unexpected committed index name");
      require(segment.datasetVersion() == datasetVersion, "segment dataset version changed");
      require(
          segment.indexVersion() == expectedIndexVersion,
          "segment index version changed: expected " + expectedIndexVersion);
      require(segment.fields().equals(expectedFields), "indexed field ids changed");
      require(segment.coveringFields().isEmpty(), "covering fields must remain empty");
      require(segment.fragments().isPresent(), "fragment coverage is missing");
      require(
          segment.indexDetails().isPresent()
              && anyTypeUrl(segment.indexDetails().orElseThrow()).endsWith(detailsSuffix),
          "committed Any type URL does not match " + detailsSuffix);
      Set<Integer> fragmentIds = coverage(segment);
      require(fragmentIds.size() == 1, "each segment must cover one fixture fragment");
      require(Collections.disjoint(union, fragmentIds), "segment coverage overlaps");
      union.addAll(fragmentIds);

      Index input = produced.get(segment.uuid());
      if (input != null) {
        assertArrayEqualsBytes(
            input.indexDetails().orElseThrow(),
            segment.indexDetails().orElseThrow(),
            "full Any bytes changed during commit");
      }

      if (requirePublishedFiles) {
        require(segment.getFiles().isPresent(), "committed files are missing");
        require(!segment.getFiles().orElseThrow().isEmpty(), "committed file list is empty");
        require(segment.getSizeBytes().isPresent(), "committed sizeBytes is missing");
        require(segment.getSizeBytes().orElseThrow() > 0, "committed sizeBytes must be positive");
        require(
            segment.getFiles().orElseThrow().stream()
                .allMatch(file -> !file.getPath().isEmpty() && file.getSizeBytes() >= 0),
            "committed index file entry is invalid");
        require(segment.createdAt().isPresent(), "commit did not record createdAt");
        require(
            segment.indexType() == expectedIndexType,
            "committed index type is not " + expectedIndexType);
      }
    }
    require(
        union.equals(new HashSet<>(expectedFragments)),
        "committed coverage does not match fixture fragments: " + union);
  }

  private static List<Index> namedSegments(Dataset dataset, String indexName) {
    return dataset.getIndexes().stream()
        .filter(index -> indexName.equals(index.name()))
        .collect(Collectors.toList());
  }

  private static QueryResult runScalarQuery(Dataset dataset, String filter) {
    ScanOptions options =
        new ScanOptions.Builder()
            .columns(List.of(BTREE_COLUMN))
            .filter(filter)
            .batchSize(1024)
            .useScalarIndex(true)
            .fastSearch(true)
            .collectStats(true)
            .build();

    List<Long> ids = new ArrayList<>();
    ScanStats stats;
    try (LanceScanner scanner = dataset.newScan(options)) {
      try (ArrowReader reader = scanner.scanBatches()) {
        while (reader.loadNextBatch()) {
          VectorSchemaRoot root = reader.getVectorSchemaRoot();
          BigIntVector vector = (BigIntVector) root.getVector(BTREE_COLUMN);
          for (int row = 0; row < root.getRowCount(); ++row) {
            require(!vector.isNull(row), "fixture id column must not contain nulls");
            ids.add(vector.get(row));
          }
        }
      }
      stats =
          scanner
              .getStats()
              .orElseThrow(() -> new IllegalStateException("scan stats were not collected"));
    } catch (Exception e) {
      throw new IllegalStateException("scan failed for filter " + filter, e);
    }

    Collections.sort(ids);
    if (stats.getIndicesLoaded() == 0 && stats.getIndexComparisons() == 0) {
      throw new IllegalStateException(
          "query did not exercise a scalar index for filter " + filter + ": " + stats);
    }
    return new QueryResult(ids, stats);
  }

  private static void runScalarQueries(
      Dataset crossSdkDataset, Dataset baselineDataset, List<QueryCase> cases) {
    for (QueryCase query : cases) {
      QueryResult crossSdk = runScalarQuery(crossSdkDataset, query.filter);
      QueryResult baseline = runScalarQuery(baselineDataset, query.filter);

      require(
          crossSdk.ids.equals(query.expected),
          "C/Java path returned "
              + crossSdk.ids
              + " for "
              + query.filter
              + ", expected "
              + query.expected);
      require(
          baseline.ids.equals(query.expected),
          "Java one-shot returned "
              + baseline.ids
              + " for "
              + query.filter
              + ", expected "
              + query.expected);
      require(
          crossSdk.ids.equals(baseline.ids),
          "C/Java path differs from Java one-shot for " + query.filter);

      System.out.printf(
          "query filter=\"%s\" expected=%s cross_sdk=%s baseline=%s "
              + "cross_indices_loaded=%d cross_index_comparisons=%d "
              + "baseline_indices_loaded=%d baseline_index_comparisons=%d%n",
          query.filter,
          query.expected,
          crossSdk.ids,
          baseline.ids,
          crossSdk.stats.getIndicesLoaded(),
          crossSdk.stats.getIndexComparisons(),
          baseline.stats.getIndicesLoaded(),
          baseline.stats.getIndexComparisons());
      System.out.println("cross_scan_stats=" + crossSdk.stats);
      System.out.println("baseline_scan_stats=" + baseline.stats);
    }
  }

  private static float[] fixtureVector(int id) {
    float[] vector = new float[VECTOR_DIMENSION];
    for (int component = 0; component < VECTOR_DIMENSION; ++component) {
      vector[component] = (id * VECTOR_MULTIPLIERS[component]) % VECTOR_MODULI[component];
    }
    return vector;
  }

  private static VectorQueryResult runVectorQuery(Dataset dataset, float[] key) {
    Query query =
        new Query.Builder()
            .setColumn(VECTOR_COLUMN)
            .setKey(key)
            .setK(VECTOR_K)
            .setNprobes(VECTOR_PARTITIONS)
            .setRefineFactor(VECTOR_REFINE_FACTOR)
            .setDistanceType(DistanceType.L2)
            .setUseIndex(true)
            .build();
    ScanOptions options =
        new ScanOptions.Builder()
            .columns(List.of("id"))
            .nearest(query)
            .batchSize(1024)
            .fastSearch(true)
            .collectStats(true)
            .build();

    List<Integer> ids = new ArrayList<>();
    List<Float> distances = new ArrayList<>();
    ScanStats stats;
    try (LanceScanner scanner = dataset.newScan(options)) {
      try (ArrowReader reader = scanner.scanBatches()) {
        while (reader.loadNextBatch()) {
          VectorSchemaRoot root = reader.getVectorSchemaRoot();
          IntVector idVector = (IntVector) root.getVector("id");
          Float4Vector distanceVector = (Float4Vector) root.getVector("_distance");
          require(
              idVector.getValueCount() == distanceVector.getValueCount(),
              "vector id and distance columns have different row counts");
          for (int row = 0; row < idVector.getValueCount(); ++row) {
            require(!idVector.isNull(row), "vector fixture id must not contain nulls");
            require(!distanceVector.isNull(row), "vector distance must not contain nulls");
            ids.add(idVector.get(row));
            distances.add(distanceVector.get(row));
          }
        }
      }
      stats =
          scanner
              .getStats()
              .orElseThrow(() -> new IllegalStateException("scan stats were not collected"));
    } catch (Exception e) {
      throw new IllegalStateException("indexed vector scan failed", e);
    }

    if (stats.getPartsLoaded() == 0
        && stats.getIndicesLoaded() == 0
        && stats.getIndexComparisons() == 0) {
      throw new IllegalStateException("vector query did not load an index: " + stats);
    }
    return new VectorQueryResult(ids, distances, stats);
  }

  private static void requireExpectedDistances(
      List<Float> actual, List<Float> expected, String description) {
    require(
        actual.size() == expected.size(),
        description + " returned " + actual.size() + " distances, expected " + expected.size());
    for (int i = 0; i < expected.size(); ++i) {
      require(
          Math.abs(actual.get(i) - expected.get(i)) <= 1e-6f,
          description
              + " distance "
              + i
              + " was "
              + actual.get(i)
              + ", expected "
              + expected.get(i));
    }
  }

  private static void runVectorQueries(
      Dataset crossSdkDataset, Dataset baselineDataset, List<VectorQueryCase> cases) {
    for (VectorQueryCase query : cases) {
      VectorQueryResult crossSdk = runVectorQuery(crossSdkDataset, fixtureVector(query.keyId));
      VectorQueryResult baseline = runVectorQuery(baselineDataset, fixtureVector(query.keyId));

      require(
          crossSdk.ids.equals(query.expectedIds),
          "C/Java vector path returned "
              + crossSdk.ids
              + " for fixture id "
              + query.keyId
              + ", expected "
              + query.expectedIds);
      requireExpectedDistances(
          crossSdk.distances, query.expectedDistances, "C/Java vector path");
      require(
          baseline.ids.equals(query.expectedIds),
          "Java one-shot vector path returned "
              + baseline.ids
              + " for fixture id "
              + query.keyId
              + ", expected "
              + query.expectedIds);
      requireExpectedDistances(
          baseline.distances, query.expectedDistances, "Java one-shot vector path");
      require(
          crossSdk.ids.equals(baseline.ids),
          "C/Java vector path differs from Java one-shot for fixture id " + query.keyId);
      requireExpectedDistances(
          crossSdk.distances,
          baseline.distances,
          "C/Java vector distances differ from Java one-shot");

      System.out.printf(
          "vector_query key_id=%d expected_ids=%s expected_distances=%s "
              + "cross_ids=%s cross_distances=%s baseline_ids=%s baseline_distances=%s "
              + "cross_parts_loaded=%d cross_indices_loaded=%d "
              + "cross_index_comparisons=%d baseline_parts_loaded=%d "
              + "baseline_indices_loaded=%d baseline_index_comparisons=%d%n",
          query.keyId,
          query.expectedIds,
          query.expectedDistances,
          crossSdk.ids,
          crossSdk.distances,
          baseline.ids,
          baseline.distances,
          crossSdk.stats.getPartsLoaded(),
          crossSdk.stats.getIndicesLoaded(),
          crossSdk.stats.getIndexComparisons(),
          baseline.stats.getPartsLoaded(),
          baseline.stats.getIndicesLoaded(),
          baseline.stats.getIndexComparisons());
      System.out.println("cross_scan_stats=" + crossSdk.stats);
      System.out.println("baseline_scan_stats=" + baseline.stats);
    }
  }

  private static List<Integer> expectedFtsIds(int... residues) {
    Set<Integer> acceptedResidues = new HashSet<>();
    for (int residue : residues) {
      acceptedResidues.add(residue);
    }

    List<Integer> ids = new ArrayList<>();
    for (int id = 0; id < FTS_TOTAL_ROWS; ++id) {
      if (acceptedResidues.contains(id % 4)) {
        ids.add(id);
      }
    }
    return ids;
  }

  private static FtsQueryResult runFtsQuery(Dataset dataset, FullTextQuery query) {
    ScanOptions options =
        new ScanOptions.Builder()
            .columns(List.of("id"))
            .fullTextQuery(query)
            .batchSize(128)
            .collectStats(true)
            .build();

    List<Integer> ids = new ArrayList<>();
    ScanStats stats;
    try (LanceScanner scanner = dataset.newScan(options)) {
      try (ArrowReader reader = scanner.scanBatches()) {
        while (reader.loadNextBatch()) {
          VectorSchemaRoot root = reader.getVectorSchemaRoot();
          IntVector idVector = (IntVector) root.getVector("id");
          for (int row = 0; row < root.getRowCount(); ++row) {
            require(!idVector.isNull(row), "FTS fixture id must not contain nulls");
            ids.add(idVector.get(row));
          }
        }
      }
      stats =
          scanner
              .getStats()
              .orElseThrow(() -> new IllegalStateException("FTS scan stats were not collected"));
    } catch (Exception e) {
      throw new IllegalStateException("FTS scan failed", e);
    }

    Collections.sort(ids);
    require(ids.size() == new HashSet<>(ids).size(), "FTS scan returned duplicate row ids");

    long cacheLookups = stats.getIndexCacheHits() + stats.getIndexCacheMisses();
    long partitionsSearched = stats.getAllCounts().getOrDefault("partitions_searched", 0L);
    require(
        partitionsSearched > 0,
        "FTS query did not search an inverted-index partition: " + stats);
    require(cacheLookups > 0, "FTS query performed no inverted-index cache lookups: " + stats);
    return new FtsQueryResult(ids, stats);
  }

  private static void runFtsQueries(
      Dataset crossSdkDataset, Dataset baselineDataset, List<FtsQueryCase> cases) {
    for (FtsQueryCase query : cases) {
      FtsQueryResult crossSdk = runFtsQuery(crossSdkDataset, query.query);
      FtsQueryResult baseline = runFtsQuery(baselineDataset, query.query);

      require(
          crossSdk.ids.equals(query.expected),
          "C/Java FTS path returned "
              + crossSdk.ids
              + " for "
              + query.name
              + ", expected "
              + query.expected);
      require(
          baseline.ids.equals(query.expected),
          "Java one-shot FTS path returned "
              + baseline.ids
              + " for "
              + query.name
              + ", expected "
              + query.expected);
      require(
          crossSdk.ids.equals(baseline.ids),
          "C/Java FTS path differs from Java one-shot for " + query.name);

      System.out.printf(
          "fts_query name=%s expected=%s cross_sdk=%s baseline=%s "
              + "cross_indices_loaded=%d cross_parts_loaded=%d "
              + "cross_index_comparisons=%d cross_index_cache_hits=%d "
              + "cross_index_cache_misses=%d cross_partitions_searched=%d "
              + "baseline_indices_loaded=%d baseline_partitions_searched=%d%n",
          query.name,
          query.expected,
          crossSdk.ids,
          baseline.ids,
          crossSdk.stats.getIndicesLoaded(),
          crossSdk.stats.getPartsLoaded(),
          crossSdk.stats.getIndexComparisons(),
          crossSdk.stats.getIndexCacheHits(),
          crossSdk.stats.getIndexCacheMisses(),
          crossSdk.stats.getAllCounts().getOrDefault("partitions_searched", 0L),
          baseline.stats.getIndicesLoaded(),
          baseline.stats.getAllCounts().getOrDefault("partitions_searched", 0L));
      System.out.println("cross_scan_stats=" + crossSdk.stats);
      System.out.println("baseline_scan_stats=" + baseline.stats);
    }
  }

  private static void decodeFtsDetails(Index index, int segment) throws Exception {
    Any any = Any.parseFrom(index.indexDetails().orElseThrow());
    require(
        any.getTypeUrl().endsWith("InvertedIndexDetails"),
        "C-produced FTS Any has an unexpected type URL: " + any.getTypeUrl());
    InvertedIndexDetails details = InvertedIndexDetails.parseFrom(any.getValue());
    require(
        details.hasBaseTokenizer() && details.getBaseTokenizer().equals("simple"),
        "FTS base tokenizer is not simple");
    require(details.getLanguage().contains("English"), "FTS language is not English");
    require(details.getWithPosition(), "FTS index did not store token positions");
    require(
        details.hasMaxTokenLength() && details.getMaxTokenLength() == 40,
        "FTS max_token_length changed");
    require(details.getLowerCase(), "FTS lower_case is false");
    require(!details.getStem(), "FTS stemming is enabled");
    require(!details.getRemoveStopWords(), "FTS stop-word removal is enabled");
    require(details.getAsciiFolding(), "FTS ascii_folding is false");
    require(
        details.hasBlockSize() && details.getBlockSize() == 128,
        "FTS posting block size changed");
    require(
        details.getDocumentGranularity() == DocumentGranularity.ROW,
        "FTS document granularity is not row");
    require(
        details.hasPostingFormatVersion() && details.getPostingFormatVersion() == FTS_INDEX_VERSION,
        "FTS posting format version changed");
    require(!details.hasCodeConfig(), "FTS index unexpectedly contains code tokenizer config");
    System.out.printf(
        "fts_details segment=%d type_url=%s base_tokenizer=%s language=%s "
            + "positions=%s block_size=%d granularity=%s posting_format_version=%d%n",
        segment,
        any.getTypeUrl(),
        details.getBaseTokenizer(),
        details.getLanguage(),
        details.getWithPosition(),
        details.getBlockSize(),
        details.getDocumentGranularity(),
        details.getPostingFormatVersion());
  }

  private static List<Integer> datasetFragments(Dataset dataset) {
    return dataset.getFragments().stream()
        .map(Fragment::getId)
        .sorted()
        .collect(Collectors.toList());
  }

  private static void runBtree(Path crossSdkPath, Path baselinePath, Path metadataPath)
      throws Exception {
    List<Integer> fixtureFragments = List.of(0, 1);
    List<Index> produced =
        decodeSegments(
            metadataPath,
            BTREE_INDEX_NAME,
            2,
            fixtureFragments,
            List.of(0),
            0,
            "BTreeIndexDetails");
    Map<UUID, Index> producedByUuid = byUuid(produced);
    long beforeVersion;
    long afterVersion;

    try (RootAllocator allocator = new RootAllocator(Long.MAX_VALUE)) {
      ReadOptions readOptions = new ReadOptions.Builder().build();
      try (Dataset dataset = Dataset.open(allocator, crossSdkPath.toString(), readOptions)) {
        beforeVersion = dataset.version();
        List<Integer> fragmentIds = datasetFragments(dataset);
        require(beforeVersion == 2, "BTree fixture must start at dataset version 2");
        require(
            fragmentIds.equals(fixtureFragments),
            "BTree fixture fragment ids differ: " + fragmentIds);
        validateSegments(
            produced,
            BTREE_INDEX_NAME,
            fragmentIds,
            List.of(0),
            beforeVersion,
            0,
            IndexType.BTREE,
            "BTreeIndexDetails",
            Map.of(),
            false);

        List<Index> committed =
            dataset.commitExistingIndexSegments(BTREE_INDEX_NAME, BTREE_COLUMN, produced);
        afterVersion = dataset.version();
        require(
            afterVersion == beforeVersion + 1,
            "Java commit must publish exactly one dataset version");
        validateSegments(
            committed,
            BTREE_INDEX_NAME,
            fragmentIds,
            List.of(0),
            beforeVersion,
            0,
            IndexType.BTREE,
            "BTreeIndexDetails",
            producedByUuid,
            true);
        System.out.printf(
            "commit mode=btree status=success before_version=%d after_version=%d segments=%d%n",
            beforeVersion, afterVersion, committed.size());
      }

      try (Dataset reopened = Dataset.open(allocator, crossSdkPath.toString(), readOptions)) {
        require(
            reopened.version() == afterVersion,
            "reopened dataset version does not match the BTree commit");
        require(
            reopened.listIndexes().contains(BTREE_INDEX_NAME),
            "reopened BTree index name is missing");
        List<Index> reopenedSegments = namedSegments(reopened, BTREE_INDEX_NAME);
        validateSegments(
            reopenedSegments,
            BTREE_INDEX_NAME,
            fixtureFragments,
            List.of(0),
            beforeVersion,
            0,
            IndexType.BTREE,
            "BTreeIndexDetails",
            producedByUuid,
            true);
        System.out.printf(
            "reopen mode=btree status=success version=%d segments=%d coverage=%s files=%s%n",
            reopened.version(),
            reopenedSegments.size(),
            reopenedSegments.stream()
                .map(CrossSdkAcceptance::coverage)
                .collect(Collectors.toList()),
            reopenedSegments.stream()
                .map(index -> index.getFiles().orElseThrow().size())
                .collect(Collectors.toList()));

        try (Dataset baseline = Dataset.open(allocator, baselinePath.toString(), readOptions)) {
          require(
              baseline.version() == beforeVersion,
              "BTree baseline version differs before index build");
          IndexParams params =
              IndexParams.builder()
                  .setScalarIndexParams(ScalarIndexParams.create("btree"))
                  .build();
          IndexOptions options =
              IndexOptions.builder(List.of(BTREE_COLUMN), IndexType.BTREE, params)
                  .withIndexName(BTREE_INDEX_NAME)
                  .build();
          Index created = baseline.createIndex(options);
          require(
              baseline.version() == beforeVersion + 1,
              "Java one-shot BTree build must publish exactly one dataset version");
          require(created.indexVersion() == 0, "Java 12 BTree baseline must use version 0");
          require(
              baseline.listIndexes().contains(BTREE_INDEX_NAME),
              "baseline BTree index name is missing");
          System.out.printf(
              "baseline mode=btree status=success before_version=%d after_version=%d segments=%d%n",
              beforeVersion,
              baseline.version(),
              namedSegments(baseline, BTREE_INDEX_NAME).size());

          List<QueryCase> cases =
              List.of(
                  new QueryCase("id >= 10 AND id < 20", range(10, 20)),
                  new QueryCase("id >= 1010 AND id < 1020", range(1010, 1020)),
                  new QueryCase("id = 1999", List.of(1999L)));
          runScalarQueries(reopened, baseline, cases);
        }
      }
    }

    System.out.printf(
        "acceptance mode=btree status=passed producer=native14.0.2@40f15ed "
            + "consumer=org.lance:lance-core:12.0.0 source=cbeec97 "
            + "before_version=%d after_version=%d%n",
        beforeVersion, afterVersion);
  }

  private static void decodeVectorDetails(Index index, int segment) throws Exception {
    Any any = Any.parseFrom(index.indexDetails().orElseThrow());
    require(
        any.getTypeUrl().endsWith("VectorIndexDetails"),
        "C-produced vector Any has an unexpected type URL: " + any.getTypeUrl());
    VectorIndexDetails details = VectorIndexDetails.parseFrom(any.getValue());
    require(details.getMetricType() == VectorMetricType.L2, "vector index metric is not L2");
    require(details.hasPq(), "vector index compression is not PQ");
    require(details.getPq().getNumBits() == VECTOR_BITS, "vector PQ num_bits changed");
    require(
        details.getPq().getNumSubVectors() == VECTOR_SUB_VECTORS,
        "vector PQ num_sub_vectors changed");
    require(
        !details.hasHnswIndexConfig(), "IVF_PQ vector details unexpectedly contain HNSW");
    System.out.printf(
        "vector_details segment=%d type_url=%s metric=%s compression=PQ bits=%d "
            + "subvectors=%d runtime_hints=%d%n",
        segment,
        any.getTypeUrl(),
        details.getMetricType(),
        details.getPq().getNumBits(),
        details.getPq().getNumSubVectors(),
        details.getRuntimeHintsCount());
  }

  private static void runVector(Path crossSdkPath, Path baselinePath, Path metadataPath)
      throws Exception {
    List<Integer> fixtureFragments = List.of(0, 1);
    List<Integer> fixtureFields = List.of(VECTOR_FIELD_ID);
    // Java 12 exposes the generic VECTOR type; the concrete IVF_PQ shape is in Any.
    List<Index> produced =
        decodeSegments(
            metadataPath,
            VECTOR_INDEX_NAME,
            2,
            fixtureFragments,
            fixtureFields,
            1,
            "VectorIndexDetails");
    for (int segment = 0; segment < produced.size(); ++segment) {
      decodeVectorDetails(produced.get(segment), segment);
    }
    Map<UUID, Index> producedByUuid = byUuid(produced);
    long beforeVersion;
    long afterVersion;

    try (RootAllocator allocator = new RootAllocator(Long.MAX_VALUE)) {
      ReadOptions readOptions = new ReadOptions.Builder().build();
      try (Dataset dataset = Dataset.open(allocator, crossSdkPath.toString(), readOptions)) {
        beforeVersion = dataset.version();
        List<Integer> fragmentIds = datasetFragments(dataset);
        require(beforeVersion == 2, "vector fixture must start at dataset version 2");
        require(
            fragmentIds.equals(fixtureFragments),
            "vector fixture fragment ids differ: " + fragmentIds);
        validateSegments(
            produced,
            VECTOR_INDEX_NAME,
            fragmentIds,
            fixtureFields,
            beforeVersion,
            1,
            IndexType.VECTOR,
            "VectorIndexDetails",
            Map.of(),
            false);

        List<Index> committed =
            dataset.commitExistingIndexSegments(VECTOR_INDEX_NAME, VECTOR_COLUMN, produced);
        afterVersion = dataset.version();
        require(
            afterVersion == beforeVersion + 1,
            "Java vector commit must publish exactly one dataset version");
        validateSegments(
            committed,
            VECTOR_INDEX_NAME,
            fragmentIds,
            fixtureFields,
            beforeVersion,
            1,
            IndexType.VECTOR,
            "VectorIndexDetails",
            producedByUuid,
            true);
        System.out.printf(
            "commit mode=ivfpq status=success before_version=%d after_version=%d segments=%d%n",
            beforeVersion, afterVersion, committed.size());
      }

      try (Dataset reopened = Dataset.open(allocator, crossSdkPath.toString(), readOptions)) {
        require(
            reopened.version() == afterVersion,
            "reopened dataset version does not match the vector commit");
        require(
            reopened.listIndexes().contains(VECTOR_INDEX_NAME),
            "reopened vector index name is missing");
        List<Index> reopenedSegments = namedSegments(reopened, VECTOR_INDEX_NAME);
        validateSegments(
            reopenedSegments,
            VECTOR_INDEX_NAME,
            fixtureFragments,
            fixtureFields,
            beforeVersion,
            1,
            IndexType.VECTOR,
            "VectorIndexDetails",
            producedByUuid,
            true);
        System.out.printf(
            "reopen mode=ivfpq status=success version=%d segments=%d coverage=%s files=%s%n",
            reopened.version(),
            reopenedSegments.size(),
            reopenedSegments.stream()
                .map(CrossSdkAcceptance::coverage)
                .collect(Collectors.toList()),
            reopenedSegments.stream()
                .map(index -> index.getFiles().orElseThrow().size())
                .collect(Collectors.toList()));

        try (Dataset baseline = Dataset.open(allocator, baselinePath.toString(), readOptions)) {
          require(
              baseline.version() == beforeVersion,
              "vector baseline version differs before index build");
          VectorIndexParams vectorParams =
              VectorIndexParams.ivfPq(
                  VECTOR_PARTITIONS, VECTOR_BITS, VECTOR_SUB_VECTORS, DistanceType.L2, 4);
          IndexParams params =
              IndexParams.builder().setVectorIndexParams(vectorParams).build();
          IndexOptions options =
              IndexOptions.builder(List.of(VECTOR_COLUMN), IndexType.IVF_PQ, params)
                  .withIndexName(VECTOR_INDEX_NAME)
                  .build();
          Index created = baseline.createIndex(options);
          require(
              baseline.version() == beforeVersion + 1,
              "Java one-shot vector build must publish exactly one dataset version");
          require(created.indexVersion() == 1, "Java 12 IVF_PQ baseline must use version 1");
          require(
              baseline.listIndexes().contains(VECTOR_INDEX_NAME),
              "baseline vector index name is missing");
          System.out.printf(
              "baseline mode=ivfpq status=success before_version=%d after_version=%d segments=%d%n",
              beforeVersion,
              baseline.version(),
              namedSegments(baseline, VECTOR_INDEX_NAME).size());

          List<VectorQueryCase> cases =
              List.of(
                  new VectorQueryCase(
                      1, List.of(1, 255, 101, 34), List.of(0.0f, 232.0f, 403.0f, 516.0f)),
                  new VectorQueryCase(
                      199, List.of(199, 163, 124, 232), List.of(0.0f, 400.0f, 596.0f, 661.0f)));
          runVectorQueries(reopened, baseline, cases);
        }
      }
    }

    System.out.printf(
        "acceptance mode=ivfpq status=passed producer=native14.0.2@40f15ed "
            + "consumer=org.lance:lance-core:12.0.0 source=cbeec97 "
            + "before_version=%d after_version=%d partitions=%d subvectors=%d bits=%d "
            + "nprobes=%d refine_factor=%d k=%d%n",
        beforeVersion,
        afterVersion,
        VECTOR_PARTITIONS,
        VECTOR_SUB_VECTORS,
        VECTOR_BITS,
        VECTOR_PARTITIONS,
        VECTOR_REFINE_FACTOR,
        VECTOR_K);
  }

  private static void runFts(Path crossSdkPath, Path baselinePath, Path metadataPath)
      throws Exception {
    List<Integer> fixtureFragments = List.of(0, 1);
    List<Integer> fixtureFields = List.of(FTS_FIELD_ID);
    List<Index> produced =
        decodeSegments(
            metadataPath,
            FTS_INDEX_NAME,
            2,
            fixtureFragments,
            fixtureFields,
            FTS_INDEX_VERSION,
            "InvertedIndexDetails");
    for (int segment = 0; segment < produced.size(); ++segment) {
      decodeFtsDetails(produced.get(segment), segment);
    }
    Map<UUID, Index> producedByUuid = byUuid(produced);
    long beforeVersion;
    long afterVersion;

    try (RootAllocator allocator = new RootAllocator(Long.MAX_VALUE)) {
      ReadOptions readOptions = new ReadOptions.Builder().build();
      try (Dataset dataset = Dataset.open(allocator, crossSdkPath.toString(), readOptions)) {
        beforeVersion = dataset.version();
        List<Integer> fragmentIds = datasetFragments(dataset);
        require(beforeVersion == 2, "FTS fixture must start at dataset version 2");
        require(
            fragmentIds.equals(fixtureFragments),
            "FTS fixture fragment ids differ: " + fragmentIds);
        validateSegments(
            produced,
            FTS_INDEX_NAME,
            fragmentIds,
            fixtureFields,
            beforeVersion,
            FTS_INDEX_VERSION,
            IndexType.INVERTED,
            "InvertedIndexDetails",
            Map.of(),
            false);

        List<Index> committed =
            dataset.commitExistingIndexSegments(FTS_INDEX_NAME, FTS_COLUMN, produced);
        afterVersion = dataset.version();
        require(
            afterVersion == beforeVersion + 1,
            "Java FTS commit must publish exactly one dataset version");
        validateSegments(
            committed,
            FTS_INDEX_NAME,
            fragmentIds,
            fixtureFields,
            beforeVersion,
            FTS_INDEX_VERSION,
            IndexType.INVERTED,
            "InvertedIndexDetails",
            producedByUuid,
            true);
        System.out.printf(
            "commit mode=fts status=success before_version=%d after_version=%d segments=%d%n",
            beforeVersion, afterVersion, committed.size());
      }

      try (Dataset reopened = Dataset.open(allocator, crossSdkPath.toString(), readOptions)) {
        require(
            reopened.version() == afterVersion,
            "reopened dataset version does not match the FTS commit");
        require(
            reopened.listIndexes().contains(FTS_INDEX_NAME),
            "reopened FTS index name is missing");
        List<Index> reopenedSegments = namedSegments(reopened, FTS_INDEX_NAME);
        validateSegments(
            reopenedSegments,
            FTS_INDEX_NAME,
            fixtureFragments,
            fixtureFields,
            beforeVersion,
            FTS_INDEX_VERSION,
            IndexType.INVERTED,
            "InvertedIndexDetails",
            producedByUuid,
            true);
        for (int segment = 0; segment < reopenedSegments.size(); ++segment) {
          decodeFtsDetails(reopenedSegments.get(segment), segment);
        }
        System.out.printf(
            "reopen mode=fts status=success version=%d segments=%d coverage=%s files=%s%n",
            reopened.version(),
            reopenedSegments.size(),
            reopenedSegments.stream()
                .map(CrossSdkAcceptance::coverage)
                .collect(Collectors.toList()),
            reopenedSegments.stream()
                .map(index -> index.getFiles().orElseThrow().size())
                .collect(Collectors.toList()));

        try (Dataset baseline = Dataset.open(allocator, baselinePath.toString(), readOptions)) {
          require(
              baseline.version() == beforeVersion,
              "FTS baseline version differs before index build");
          ScalarIndexParams scalarParams = ScalarIndexParams.create("inverted", FTS_PARAMS_JSON);
          IndexParams params =
              IndexParams.builder().setScalarIndexParams(scalarParams).build();
          IndexOptions options =
              IndexOptions.builder(List.of(FTS_COLUMN), IndexType.INVERTED, params)
                  .withIndexName(FTS_INDEX_NAME)
                  .build();
          Index created = baseline.createIndex(options);
          require(
              baseline.version() == beforeVersion + 1,
              "Java one-shot FTS build must publish exactly one dataset version");
          require(
              created.indexVersion() == FTS_INDEX_VERSION,
              "Java 12 FTS baseline must use index version " + FTS_INDEX_VERSION);
          require(
              baseline.listIndexes().contains(FTS_INDEX_NAME),
              "baseline FTS index name is missing");
          System.out.printf(
              "baseline mode=fts status=success before_version=%d after_version=%d segments=%d%n",
              beforeVersion,
              baseline.version(),
              namedSegments(baseline, FTS_INDEX_NAME).size());

          FullTextQuery.MatchQuery alpha =
              (FullTextQuery.MatchQuery) FullTextQuery.match("alpha", FTS_COLUMN);
          FullTextQuery.MatchQuery gamma =
              (FullTextQuery.MatchQuery) FullTextQuery.match("gamma", FTS_COLUMN);
          FullTextQuery alphaWithoutGamma =
              FullTextQuery.booleanQuery(
                  List.of(
                      new FullTextQuery.BooleanClause(FullTextQuery.Occur.MUST, alpha),
                      new FullTextQuery.BooleanClause(FullTextQuery.Occur.MUST_NOT, gamma)));
          List<FtsQueryCase> cases =
              List.of(
                  new FtsQueryCase(
                      "match-alpha", FullTextQuery.match("alpha", FTS_COLUMN), expectedFtsIds(0, 1)),
                  new FtsQueryCase(
                      "phrase-beta-gamma",
                      FullTextQuery.phrase("beta gamma", FTS_COLUMN, 0),
                      expectedFtsIds(0, 2)),
                  new FtsQueryCase(
                      "boolean-alpha-not-gamma",
                      alphaWithoutGamma,
                      expectedFtsIds(1)),
                  new FtsQueryCase(
                      "rare-tail-token", FullTextQuery.match("needle", FTS_COLUMN), List.of(127)));
          runFtsQueries(reopened, baseline, cases);
        }
      }
    }

    System.out.printf(
        "acceptance mode=fts status=passed producer=native14.0.2@40f15ed "
            + "consumer=org.lance:lance-core:12.0.0 source=cbeec97 "
            + "before_version=%d after_version=%d index_version=%d "
            + "posting_format_version=%d block_size=128 rows_per_fragment=64 "
            + "fragments=2%n",
        beforeVersion,
        afterVersion,
        FTS_INDEX_VERSION,
        FTS_INDEX_VERSION);
  }

  public static void main(String[] args) throws Exception {
    if (args.length != 4) {
      throw new IllegalArgumentException(
          "usage: CrossSdkAcceptance <btree|ivfpq|fts> <cross-sdk-dataset> "
              + "<baseline-dataset> <metadata-directory>");
    }
    String mode = args[0];
    Path crossSdkPath = Path.of(args[1]).toAbsolutePath();
    Path baselinePath = Path.of(args[2]).toAbsolutePath();
    Path metadataPath = Path.of(args[3]).toAbsolutePath();

    switch (mode) {
      case "btree":
        runBtree(crossSdkPath, baselinePath, metadataPath);
        break;
      case "ivfpq":
        runVector(crossSdkPath, baselinePath, metadataPath);
        break;
      case "fts":
        runFts(crossSdkPath, baselinePath, metadataPath);
        break;
      default:
        throw new IllegalArgumentException("unknown mode: " + mode);
    }
  }
}
