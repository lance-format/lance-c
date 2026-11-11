// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

package org.lance.crosssdk;

import org.apache.arrow.memory.BufferAllocator;
import org.apache.arrow.memory.RootAllocator;
import org.apache.arrow.vector.BigIntVector;
import org.apache.arrow.vector.VectorSchemaRoot;
import org.apache.arrow.vector.ipc.ArrowReader;
import org.lance.Dataset;
import org.lance.Fragment;
import org.lance.ReadOptions;
import org.lance.index.Index;
import org.lance.index.IndexOptions;
import org.lance.index.IndexParams;
import org.lance.index.IndexType;
import org.lance.index.scalar.ScalarIndexParams;
import org.lance.ipc.LanceScanner;
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
  private static final String INDEX_NAME = "btree_cross_sdk";
  private static final String COLUMN = "id";

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

  private static void validateSegments(
      List<Index> segments,
      List<Integer> expectedFragments,
      List<Integer> expectedFields,
      long datasetVersion,
      Map<UUID, Index> produced,
      boolean requirePublishedFiles) {
    require(segments.size() == 2, "expected two physical segments, got " + segments.size());
    require(
        segments.stream().map(Index::uuid).distinct().count() == 2,
        "physical segment UUIDs are not distinct");

    Set<Integer> union = new HashSet<>();
    for (Index segment : segments) {
      require(INDEX_NAME.equals(segment.name()), "unexpected committed index name");
      require(segment.datasetVersion() == datasetVersion, "segment dataset version changed");
      require(segment.indexVersion() == 0, "BTree index version must remain 0");
      require(segment.fields().equals(expectedFields), "indexed field ids changed");
      require(segment.coveringFields().isEmpty(), "covering fields must remain empty");
      require(segment.fragments().isPresent(), "fragment coverage is missing");
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
        require(segment.indexType() == IndexType.BTREE, "committed index type is not BTREE");
      }
    }
    require(
        union.equals(new HashSet<>(expectedFragments)),
        "committed coverage does not match fixture fragments: " + union);
  }

  private static void assertArrayEqualsBytes(byte[] expected, byte[] actual, String message) {
    if (!Arrays.equals(expected, actual)) {
      throw new IllegalStateException(message);
    }
  }

  private static List<Index> namedSegments(Dataset dataset) {
    return dataset.getIndexes().stream()
        .filter(index -> INDEX_NAME.equals(index.name()))
        .collect(Collectors.toList());
  }

  private static QueryResult runQuery(Dataset dataset, String filter) {
    ScanOptions options =
        new ScanOptions.Builder()
            .columns(List.of(COLUMN))
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
          BigIntVector vector = (BigIntVector) root.getVector(COLUMN);
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
          "query did not exercise a scalar index for filter "
              + filter
              + ": "
              + stats);
    }
    return new QueryResult(ids, stats);
  }

  private static void runQueries(
      Dataset crossSdkDataset, Dataset baselineDataset, List<QueryCase> cases) {
    for (QueryCase query : cases) {
      QueryResult crossSdk = runQuery(crossSdkDataset, query.filter);
      QueryResult baseline = runQuery(baselineDataset, query.filter);

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

  public static void main(String[] args) throws Exception {
    if (args.length != 3) {
      throw new IllegalArgumentException(
          "usage: CrossSdkAcceptance <cross-sdk-dataset> <baseline-dataset> <metadata-directory>");
    }
    Path crossSdkPath = Path.of(args[0]).toAbsolutePath();
    Path baselinePath = Path.of(args[1]).toAbsolutePath();
    Path metadataPath = Path.of(args[2]).toAbsolutePath();

    List<byte[]> payloads = new ArrayList<>();
    for (int segment = 0; segment < 2; ++segment) {
      byte[] bytes = readBytes(metadataPath.resolve("segment-" + segment + ".pb"));
      payloads.add(bytes);
      System.out.printf(
          "metadata segment=%d bytes=%d sha256=%s%n", segment, bytes.length, sha256(bytes));
    }

    List<Index> produced = new ArrayList<>();
    for (int segment = 0; segment < payloads.size(); ++segment) {
      Index index = CrossSdkIndexMetadataAdapter.toJavaIndex(payloads.get(segment));
      produced.add(index);
      require(!index.getFiles().isPresent(), "test adapter must not supply input files");
      require(!index.getSizeBytes().isPresent(), "test adapter must not supply input sizeBytes");
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
        Collections.disjoint(
            coverage(produced.get(0)), coverage(produced.get(1))),
        "C-produced segment coverage overlaps");
    List<Integer> fixtureFields = produced.get(0).fields();
    require(
        produced.get(1).fields().equals(fixtureFields),
        "C-produced segments disagree on indexed fields");

    List<Integer> fixtureFragments = List.of(0, 1);
    Map<UUID, Index> producedByUuid = byUuid(produced);
    long beforeVersion;
    long afterVersion;

    try (RootAllocator allocator = new RootAllocator(Long.MAX_VALUE)) {
      ReadOptions readOptions = new ReadOptions.Builder().build();
      try (Dataset dataset = Dataset.open(allocator, crossSdkPath.toString(), readOptions)) {
        beforeVersion = dataset.version();
        List<Integer> actualFragments =
            dataset.getFragments().stream()
                .map(Fragment::getId)
                .sorted()
                .collect(Collectors.toList());
        require(beforeVersion == 2, "fixture must start at dataset version 2");
        require(
            actualFragments.equals(fixtureFragments),
            "fixture fragment ids differ: " + actualFragments);
        validateSegments(
            produced, actualFragments, fixtureFields, beforeVersion, Map.of(), false);

        List<Index> committed =
            dataset.commitExistingIndexSegments(INDEX_NAME, COLUMN, produced);
        afterVersion = dataset.version();
        require(
            afterVersion == beforeVersion + 1,
            "Java commit must publish exactly one dataset version");
        validateSegments(
            committed, actualFragments, fixtureFields, beforeVersion, producedByUuid, true);
        System.out.printf(
            "commit status=success before_version=%d after_version=%d segments=%d%n",
            beforeVersion, afterVersion, committed.size());
      }

      try (Dataset reopened =
          Dataset.open(allocator, crossSdkPath.toString(), readOptions)) {
        require(
            reopened.version() == afterVersion,
            "reopened dataset version does not match the commit");
        require(reopened.listIndexes().contains(INDEX_NAME), "reopened index name is missing");
        List<Index> reopenedSegments = namedSegments(reopened);
        validateSegments(
            reopenedSegments,
            fixtureFragments,
            fixtureFields,
            beforeVersion,
            producedByUuid,
            true);
        System.out.printf(
            "reopen status=success version=%d segments=%d coverage=%s files=%s%n",
            reopened.version(),
            reopenedSegments.size(),
            reopenedSegments.stream().map(CrossSdkAcceptance::coverage).collect(Collectors.toList()),
            reopenedSegments.stream()
                .map(index -> index.getFiles().orElseThrow().size())
                .collect(Collectors.toList()));

        try (Dataset baseline =
            Dataset.open(allocator, baselinePath.toString(), readOptions)) {
          require(baseline.version() == beforeVersion, "baseline version differs before index build");
          IndexParams params =
              IndexParams.builder()
                  .setScalarIndexParams(ScalarIndexParams.create("btree"))
                  .build();
          IndexOptions options =
              IndexOptions.builder(List.of(COLUMN), IndexType.BTREE, params)
                  .withIndexName(INDEX_NAME)
                  .build();
          Index created = baseline.createIndex(options);
          require(
              baseline.version() == beforeVersion + 1,
              "Java one-shot build must publish exactly one dataset version");
          require(created.indexVersion() == 0, "Java 12 BTree baseline must use version 0");
          require(baseline.listIndexes().contains(INDEX_NAME), "baseline index name is missing");
          System.out.printf(
              "baseline status=success before_version=%d after_version=%d segments=%d%n",
              beforeVersion,
              baseline.version(),
              namedSegments(baseline).size());

          List<QueryCase> cases =
              List.of(
                  new QueryCase("id >= 10 AND id < 20", range(10, 20)),
                  new QueryCase("id >= 1010 AND id < 1020", range(1010, 1020)),
                  new QueryCase("id = 1999", List.of(1999L)));
          runQueries(reopened, baseline, cases);
        }
      }
    }

    System.out.printf(
        "acceptance status=passed producer=native14.0.2@40f15ed "
            + "consumer=org.lance:lance-core:12.0.0 source=cbeec97 "
            + "before_version=%d after_version=%d%n",
        beforeVersion, afterVersion);
  }
}
