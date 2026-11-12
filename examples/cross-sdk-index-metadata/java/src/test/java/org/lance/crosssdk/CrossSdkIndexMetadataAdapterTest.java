// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

package org.lance.crosssdk;

import com.google.protobuf.Any;
import org.junit.jupiter.api.Test;
import org.lance.crosssdk.proto.IndexMetadataProto;
import org.lance.index.Index;
import org.roaringbitmap.RoaringBitmap;

import java.nio.ByteBuffer;
import java.time.Instant;
import java.util.List;
import java.util.UUID;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class CrossSdkIndexMetadataAdapterTest {
  private static byte[] roaring(int... values) {
    RoaringBitmap bitmap = new RoaringBitmap();
    for (int value : values) {
      bitmap.add(value);
    }
    ByteBuffer buffer = ByteBuffer.allocate(bitmap.serializedSizeInBytes());
    bitmap.serialize(buffer);
    return buffer.array();
  }

  private static Any details(byte[] value) {
    return Any.newBuilder().setTypeUrl("/lance.table.BTreeIndexDetails").setValue(
        com.google.protobuf.ByteString.copyFrom(value)).build();
  }

  private static IndexMetadataProto.IndexMetadata validMetadata() {
    return IndexMetadataProto.IndexMetadata.newBuilder()
        .setUuid(
            IndexMetadataProto.UUID.newBuilder()
                .setUuid(
                    com.google.protobuf.ByteString.copyFrom(
                        new byte[] {
                          0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                          0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
                        })))
        .addFields(3)
        .setName("btree_cross_sdk")
        .setDatasetVersion(2)
        .setFragmentBitmap(
            com.google.protobuf.ByteString.copyFrom(roaring(0)))
        .setIndexDetails(details(new byte[] {1, 2, 3}))
        .setIndexVersion(0)
        .setCreatedAt(1_700_000_000_123L)
        .build();
  }

  @Test
  void mapsRepresentableV14FieldsWithoutInputFiles() {
    IndexMetadataProto.IndexMetadata metadata =
        validMetadata().toBuilder()
            .addFiles(
                IndexMetadataProto.IndexFile.newBuilder()
                    .setPath("page_data.lance")
                    .setSizeBytes(123)
                    .build())
            .build();

    Index index = CrossSdkIndexMetadataAdapter.toJavaIndex(metadata.toByteArray());

    assertEquals(
        UUID.fromString("00010203-0405-0607-0809-0a0b0c0d0e0f"), index.uuid());
    assertEquals(List.of(3), index.fields());
    assertEquals(List.of(), index.coveringFields());
    assertEquals("btree_cross_sdk", index.name());
    assertEquals(2, index.datasetVersion());
    assertEquals(List.of(0), index.fragments().orElseThrow());
    assertArrayEquals(metadata.getIndexDetails().toByteArray(), index.indexDetails().orElseThrow());
    assertEquals(0, index.indexVersion());
    assertEquals(Instant.ofEpochMilli(1_700_000_000_123L), index.createdAt().orElseThrow());
    assertFalse(index.getFiles().isPresent());
    assertFalse(index.getSizeBytes().isPresent());
  }

  @Test
  void preservesVectorDetailsAsOpaqueAnyBytes() throws Exception {
    byte[] details =
        Any.newBuilder()
            .setTypeUrl("type.googleapis.com/lance.index.pb.VectorIndexDetails")
            .setValue(com.google.protobuf.ByteString.copyFrom(new byte[] {8, 0, 32, 4}))
            .build()
            .toByteArray();
    IndexMetadataProto.IndexMetadata metadata =
        validMetadata().toBuilder()
            .setName("ivfpq_cross_sdk")
            .setIndexDetails(Any.parseFrom(details))
            .setIndexVersion(1)
            .build();

    Index index = CrossSdkIndexMetadataAdapter.toJavaIndex(metadata.toByteArray());

    assertEquals("ivfpq_cross_sdk", index.name());
    assertEquals(1, index.indexVersion());
    assertArrayEquals(details, index.indexDetails().orElseThrow());
  }

  @Test
  void preservesInvertedDetailsAsOpaqueAnyBytes() throws Exception {
    byte[] details =
        Any.newBuilder()
            .setTypeUrl("type.googleapis.com/lance.table.InvertedIndexDetails")
            .setValue(
                com.google.protobuf.ByteString.copyFrom(
                    new byte[] {0x0a, 0x06, 0x73, 0x69, 0x6d, 0x70, 0x6c, 0x65}))
            .build()
            .toByteArray();
    IndexMetadataProto.IndexMetadata metadata =
        validMetadata().toBuilder()
            .setName("fts_cross_sdk")
            .setIndexDetails(Any.parseFrom(details))
            .setIndexVersion(2)
            .build();

    Index index = CrossSdkIndexMetadataAdapter.toJavaIndex(metadata.toByteArray());

    assertEquals("fts_cross_sdk", index.name());
    assertEquals(2, index.indexVersion());
    assertArrayEquals(details, index.indexDetails().orElseThrow());
  }

  @Test
  void rejectsV14ClusteringAndSegmentMetadata() {
    byte[] clustering = validMetadata().toBuilder().setClusteringId(7).build().toByteArray();
    IllegalArgumentException clusteringError =
        assertThrows(
            IllegalArgumentException.class,
            () -> CrossSdkIndexMetadataAdapter.toJavaIndex(clustering));
    assertTrue(clusteringError.getMessage().contains("clustering_id"));

    byte[] indexMetadata =
        validMetadata().toBuilder().putMetadata("owner", "doris").build().toByteArray();
    IllegalArgumentException metadataError =
        assertThrows(
            IllegalArgumentException.class,
            () -> CrossSdkIndexMetadataAdapter.toJavaIndex(indexMetadata));
    assertTrue(metadataError.getMessage().contains("metadata"));
  }

  @Test
  void rejectsValuesOutsideJava12Ranges() {
    assertThrows(
        IllegalArgumentException.class,
        () ->
            CrossSdkIndexMetadataAdapter.toJavaIndex(
                validMetadata().toBuilder().setDatasetVersion(-1L).build().toByteArray()));
    assertThrows(
        IllegalArgumentException.class,
        () ->
            CrossSdkIndexMetadataAdapter.toJavaIndex(
                validMetadata().toBuilder().setBaseId((int) 0x80000000L).build().toByteArray()));
    assertThrows(
        IllegalArgumentException.class,
        () ->
            CrossSdkIndexMetadataAdapter.toJavaIndex(
                validMetadata().toBuilder().setCreatedAt(-1L).build().toByteArray()));
    assertThrows(
        IllegalArgumentException.class,
        () ->
            CrossSdkIndexMetadataAdapter.toJavaIndex(
                validMetadata()
                    .toBuilder()
                    .setFragmentBitmap(
                        com.google.protobuf.ByteString.copyFrom(
                            roaring(Integer.MIN_VALUE)))
                    .build()
                    .toByteArray()));
  }

  @Test
  void rejectsMissingCommitInputs() {
    assertThrows(
        IllegalArgumentException.class,
        () ->
            CrossSdkIndexMetadataAdapter.toJavaIndex(
                validMetadata().toBuilder().clearIndexDetails().build().toByteArray()));
    assertThrows(
        IllegalArgumentException.class,
        () ->
            CrossSdkIndexMetadataAdapter.toJavaIndex(
                validMetadata()
                    .toBuilder()
                    .setFragmentBitmap(com.google.protobuf.ByteString.EMPTY)
                    .build()
                    .toByteArray()));
  }
}
