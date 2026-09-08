# Scalar index segment scans

An ordinary scanner can use one physical BTree/Bitmap segment to generate
candidates, then read those candidates with the complete scanner filter. This
does not run a global search of the other segments of the logical index. It does
not subdivide a physical segment or make its own index search incremental.

## Configuring a task

Open a fixed dataset version. Select the physical index UUID from that version's
metadata and pass the task's complete fragment domain explicitly:

```c
LanceScanner *scanner = lance_scanner_new(dataset, columns, full_filter_sql);
/* Check every return value in production. */
lance_scanner_set_fragment_ids(scanner, fragment_ids, fragment_count);
lance_scanner_set_scalar_index_segment(scanner, segment_uuid_16_bytes);
lance_scanner_set_limit(scanner, 20000);
/* The scanner-owning thread calls lance_scanner_next as usual. */
```

SQL, Substrait and additional SQL filters keep their existing precedence and AND
composition. The caller does not supply a separate driver predicate: Lance-C
uses the typed filter planner and selects a necessary indexed leaf belonging to
the requested logical index. It only descends through AND, never through OR or
NOT. It then searches the selected UUID and applies the complete filter while
reading candidates with automatic scalar-index planning disabled.

Each task's fragment IDs define its result domain, including on fallback. A
distributed planner must assign disjoint domains whose union covers the intended
scan. Unindexed fragments need their own tasks, or an explicit domain including
them (which causes that task to use fallback). Merely listing indexed segments
does not include appended, unindexed data automatically.

An unknown UUID, absent fragment or invalid option combination is an error. A
known segment with incomplete/unknown coverage, no suitable driver, unsupported
index type, nested key, overlays, fragment reuse, non-exact results or unsupported
row-ID domain falls back to a non-indexed scan of the entire explicit domain.
I/O and corruption errors are propagated, not converted to empty results or
successful fallback.

The first implementation supports live-row ordinary scans and cannot be combined
with vector/FTS queries. Physical row-address
results on stable-row-ID datasets currently fall back; results already expressed
in the correct row-ID domain use the candidate path. Deletes and all remaining
predicates are handled by the ordinary reader. No candidate-count limit is
applied: LIMIT/OFFSET remain after the scanner's complete filter.

If the host has additional predicates outside Lance, do not set a local limit
before those predicates. Never divide the global limit by the number of tasks.
Global OFFSET belongs to the coordinator, not independently to each task.

## Stopping after the host limit

This mode uses the existing scanner/stream lifecycle. Once the host has enough
rows, it stops requesting further batches and closes the scanner after any active
call has returned. Do not call `lance_scanner_close` concurrently with `next`.
Exported Arrow streams remain owned by the caller and must also be released after
their active consumers have finished.

A host stop flag does not interrupt an in-progress `lance_scanner_next`: current
index evaluation or I/O may finish before the host observes stop and closes the
stream. No separate cancellation signal or thread is introduced. The host remains
responsible for enforcing the global LIMIT across concurrent tasks.

## Memory and statistics

Candidate masks stay in Rust, and record batches are streamed. Each active task
can still hold a complete segment's candidate set; scanner I/O buffer size does
not cap that allocation. Control task concurrency and physical segment size.

Successful exhaustion merges segment-search metrics into the existing statistics
callback exactly once. New metrics include `scalar_segments_requested`,
`scalar_segments_searched`, `scalar_segment_candidate_rows`,
`scalar_segment_prepare_time`, `scalar_segment_search_time`, and
`scalar_segment_fallback_*` reasons. `prepare_time` includes search time. Early
release, cancellation and errors retain the existing callback contract: final
statistics are not guaranteed. Metrics do not establish global task concurrency.
