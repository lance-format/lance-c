# Native immutable data-range cache candidate for #71

Prepare native Rust support for [lance-c #71](https://github.com/lance-format/lance-c/issues/71)
and [native #6483](https://github.com/lance-format/lance/issues/6483).
The selected core is `40f15ed1b71b66950961b175829fdf17a763f108` and the
validation toolchain is Rust 1.94.0. This directory holds a native candidate
and its evidence, not a C cache engine or a core dependency update.

## Scope

The requested cache is opt-in, shared through native ownership and supports
bounded memory and disk tiers backed by Foyer. Expose native configuration
for capacities and local storage location; invalid configuration fails
explicitly. Disabled operation preserves existing reads. Retain Session's
metadata and parsed-index caches as separate owners.

Only immutable dataset data-file range bytes are eligible. Manifests,
transactions, deletions, indices and unrelated objects bypass the cache.
Use actual native data-path rules; a filename extension alone is not enough.
Do not cache failed or truncated bodies, and do not turn cache failures into
successful reads of wrong or incomplete data. Document whether local-cache
I/O errors propagate or bypass the cache, using one native policy.

## Integration facts and open design decisions

`WrappingObjectStore` and `ObjectStoreParams::object_store_wrapper` are native
extension points. At the selected core, `CloudObjectReader::get_range` builds
`GetOptions { range: ... }` and calls `get_opts` through the existing retry
path. `get_range_stream` also uses `get_opts`. Implementing only the object's
`get_range` convenience method would miss these real reader paths.

Choose the native owner and insertion point before implementation. Account
for wrapper composition, paginated-list passthrough, read retry/body failure,
and the result metadata expected by existing consumers. Keep the cache
behavior in native modules, with no special lance-c-only query semantics.

A key needs unambiguous store/object identity, byte range and a valid object
generation. Establish the actual strength of available ETag/version metadata;
do not assume equal paths in two stores mean equal objects, or that an object
can never be replaced at the same path. Preserve conditional/versioned reads
and invalid-range errors. Unsupported request shapes may bypass the cache
explicitly instead of weakening their semantics.

Specify disk-cache reuse and invalidation across independently opened readers
and process lifetimes, including stores without a reliable generation token.
Persistent storage must not resurrect stale bytes or reuse another store's
entries. Configuration and cache keys must not expose credentials. Do not
introduce a new consistency promise absent from the native object-store API.

The selected lockfile has Foyer 0.22.6 memory/common components transitively
through GooseFS. This is not an integrated general read cache. A deliberate
native dependency addition can be part of the candidate, but must preserve
Rust 1.94 compatibility, record all lockfile edges and avoid unrelated upgrades.

## Acceptance

Use a real native Dataset/FileReader over an instrumented object store. Count
actual underlying range requests and bytes, separately from metadata reads,
and compare returned values rather than only elapsed time or cache counters.

- Repeated eligible reads return identical values with fewer underlying range
  requests. The disabled control continues to read from the backend.
- Memory and disk are both exercised: force memory eviction, prove a disk
  hit, then force disk eviction and verify the backend fallback. Record
  capacity accounting and the defined capacity/overhead limits.
- Non-data files bypass both tiers. Distinct stores with equal relative
  paths/ranges cannot collide. Replacement/generation change cannot return
  stale bytes, including the documented disk-reopen behavior.
- Failed/truncated range responses are not inserted; subsequent successful
  reads recover. Conditional and invalid-range requests retain native errors.
- Concurrent readers share the configured cache safely; dropping the creator
  leaves live readers valid and the final owner releases local resources.
  Tests use deterministic coordination rather than sleeps.

Retain exact source/patch identities, commands, nonzero executed-test counts
and full logs on NieR. Use Git for source/script transport and mbx default
storage. A memory-only prototype or a wrapper unit test is intermediate
evidence, not completion of the native or C/C++ outcome. If an unresolved
identity or dependency constraint prevents safe implementation, retain the
specific evidence and concrete remaining decision instead of silently
relaxing this contract.

## Delivery boundary

Prepare a native patch, focused native regression tests, a replay command and
an honest source/runtime report here. Do not publish upstream, change the
lance-c native pin/MSRV, or add C symbols. Independent source review follows
native execution. Eventual C/C++ configuration and shared reader/session
lifetime tests remain in #71 after native adoption; this candidate does not
close either tracker.
