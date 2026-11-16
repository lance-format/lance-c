// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

package org.lance.crosssdk;

import com.google.protobuf.InvalidProtocolBufferException;
import org.lance.crosssdk.proto.IndexMetadataProto;
import org.lance.index.Index;
import org.roaringbitmap.RoaringBitmap;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

/**
 * Decodes the pinned Lance v14 {@code IndexMetadata} wire envelope into the
 * public Java 12 {@link Index.Builder} surface.
 *
 * <p>This is interoperability test tooling, not a Java SDK API proposal. It
 * deliberately omits input files and sizeBytes because the Java 12 commit
 * re-lists {@code _indices/<uuid>/} before publishing the new manifest.
 */
public final class CrossSdkIndexMetadataAdapter {
  private CrossSdkIndexMetadataAdapter() {}

  public static Index toJavaIndex(byte[] encoded) {
    if (encoded == null) {
      throw new IllegalArgumentException("encoded IndexMetadata must not be null");
    }

    IndexMetadataProto.IndexMetadata metadata;
    try {
      metadata = IndexMetadataProto.IndexMetadata.parseFrom(encoded);
    } catch (InvalidProtocolBufferException e) {
      throw new IllegalArgumentException("invalid v14 IndexMetadata protobuf", e);
    }

    if (!metadata.hasUuid() || metadata.getUuid().getUuid().size() != 16) {
      throw new IllegalArgumentException("IndexMetadata.uuid must contain exactly 16 bytes");
    }
    if (metadata.getFieldsCount() == 0) {
      throw new IllegalArgumentException("IndexMetadata.fields must not be empty");
    }
    if (metadata.getName().isEmpty()) {
      throw new IllegalArgumentException("IndexMetadata.name must not be empty");
    }
    if (metadata.getDatasetVersion() < 0) {
      throw new IllegalArgumentException(
          "IndexMetadata.dataset_version exceeds the Java long range: "
              + Long.toUnsignedString(metadata.getDatasetVersion()));
    }
    if (metadata.getFragmentBitmap().isEmpty()) {
      throw new IllegalArgumentException("IndexMetadata.fragment_bitmap must be present and non-empty");
    }
    if (!metadata.hasIndexDetails()) {
      throw new IllegalArgumentException("IndexMetadata.index_details must be present");
    }
    if (metadata.hasClusteringId()) {
      throw new IllegalArgumentException(
          "v14 clustering_id cannot be represented by the Java 12 Index API");
    }
    if (!metadata.getMetadataMap().isEmpty()) {
      throw new IllegalArgumentException(
          "non-empty v14 index metadata cannot be represented by the Java 12 Index API");
    }

    RoaringBitmap bitmap = new RoaringBitmap();
    try {
      bitmap.deserialize(ByteBuffer.wrap(metadata.getFragmentBitmap().toByteArray()));
    } catch (IOException | RuntimeException e) {
      throw new IllegalArgumentException("invalid portable Roaring fragment bitmap", e);
    }
    if (bitmap.isEmpty()) {
      throw new IllegalArgumentException("IndexMetadata fragment coverage must not be empty");
    }

    List<Integer> fragments = new ArrayList<>(bitmap.getCardinality());
    for (int fragmentId : bitmap) {
      if (fragmentId < 0) {
        throw new IllegalArgumentException(
            "fragment id exceeds the Java Integer range: "
                + Integer.toUnsignedString(fragmentId));
      }
      fragments.add(fragmentId);
    }

    byte[] uuidBytes = metadata.getUuid().getUuid().toByteArray();
    ByteBuffer uuidBuffer = ByteBuffer.wrap(uuidBytes);
    UUID uuid = new UUID(uuidBuffer.getLong(), uuidBuffer.getLong());

    Index.Builder builder =
        Index.builder()
            .uuid(uuid)
            .fields(new ArrayList<>(metadata.getFieldsList()))
            .coveringFields(new ArrayList<>(metadata.getCoveringFieldsList()))
            .name(metadata.getName())
            .datasetVersion(metadata.getDatasetVersion())
            .fragments(fragments)
            .indexDetails(metadata.getIndexDetails().toByteArray())
            .indexVersion(metadata.hasIndexVersion() ? metadata.getIndexVersion() : 0);

    if (metadata.hasCreatedAt()) {
      if (metadata.getCreatedAt() < 0) {
        throw new IllegalArgumentException(
            "IndexMetadata.created_at exceeds the Java long range: "
                + Long.toUnsignedString(metadata.getCreatedAt()));
      }
      builder.createdAt(Instant.ofEpochMilli(metadata.getCreatedAt()));
    }
    if (metadata.hasBaseId()) {
      if (metadata.getBaseId() < 0) {
        throw new IllegalArgumentException(
            "IndexMetadata.base_id exceeds the Java Integer range: "
                + Integer.toUnsignedString(metadata.getBaseId()));
      }
      builder.baseId(metadata.getBaseId());
    }

    return builder.build();
  }
}
