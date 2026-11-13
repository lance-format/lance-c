# Native host memory accounting acceptance for #34

This is an integration contract for the remaining native prerequisite of
[lance-c #34](https://github.com/lance-format/lance-c/issues/34), not an
implemented API or executed test report. The selected core remains Lance
`40f15ed1b71b66950961b175829fdf17a763f108`. Native
[PR #10802](https://github.com/lance-format/lance/pull/10802) is compared at
`95d90ae0968c155936d6b3a6b231bbc9b654b70a`; its author's implementation
ownership is retained. These requirements extend beyond that PR's stated
read-admission scope and are not defects against its own contract.

## Required outcome

A host query engine can supply a native per-scan accounting owner, reject
new reservations, and keep admitted memory charged for its actual lifetime,
including output retained after the scanner closes. This includes scan
read-ahead, decoded pages, take/coalescing buffers, vector-search scratch and
filter evaluation, as well as returned Arrow buffers. A read-ahead limit or
charging only exported batches does not complete this outcome.

Accounting does not replace the backing allocator, guarantee process RSS,
or provide cooperative spill. Allocation capacities and any explicitly
untracked overhead must be distinguished from logical row bytes. Existing
session caches need explicit ownership attribution when shared across scans;
a per-scan total must not silently claim all session or process memory.

## Candidate integration boundaries

| Existing native seam at the inspected head | What can be reused | What still needs a native contract |
| --- | --- | --- |
| `Session::with_read_memory_budget` and `read_budget` | Shared read scheduling and query identities | Select a caller-owned accounting context per scan without changing other scans sharing the session. |
| `BudgetReader::admission`, `Admission::reserve` (`lance-io/src/read_budget.rs:328,350`) | Ordered admission, cancellation of queued requests, wakeups | Fallible host admission returns a native error before the covered work allocates; internal overdraft must not override host rejection. |
| `BudgetReader::charge`, `Reservation::{split_off,resize}` (`read_budget.rs:300,493,510`) | Internal reservation accounting and ownership transfer | Growing a host charge must be fallible; charging already allocated memory cannot prove pre-allocation admission. Failed growth preserves the existing charge. |
| `ReadChunk` and decoded-size estimation (`lance/src/io/exec/read_budget.rs:1`) | Chunk planning and estimates for scheduling | Separate estimated admission from actual retained capacities; a learned expansion ratio alone cannot enforce host rejection for larger decoded allocations. |
| `MaterializedBlobBatch` (`dataset/blob/materialize.rs:218`) | Shared reservations while batches remain wrapped, including split chunks | `into_batch` drops the separate reservations. Plain Arrow output and every surviving buffer owner must retain the accounting lease beyond stream emission. |
| Filtered read conversion to plain batches (`io/exec/filtered_read.rs`) | Existing output path | Verify the lease survives this conversion and later export; retaining it only on a stream or temporary batch wrapper is insufficient. |

The product's locked Arrow 58.3.0 `MemoryPool::reserve` is infallible. Merely
adapting it to a callback with a return code cannot implement refusal. Native
fallible allocation/admission paths must propagate the refusal first; the C
binding can subsequently translate the resulting native error.

## Reservation and ownership invariants

- A denied request acquires no host charge and schedules no covered allocation.
  The original native failure remains observable. No retry loop, silent
  fallback, or uncharged overdraft converts denial into success.
- A successful charge has one release obligation. Partial construction,
  downstream errors, cancellation and ordinary drops release their acquired
  charges. An allocation that is still used by another owner remains charged.
- Increasing capacity obtains the additional charge before the covered growth.
  Rejection leaves the old allocation and its old charge valid. Shrinkage or
  ownership transfer releases only bytes no longer retained.
- Shared Arrow buffers, clones and slices keep a shared lease for the retained
  allocation. Slicing a small view does not reduce its charge while the full
  backing allocation remains live. Distinct newly allocated copies acquire
  their own charge while both allocations coexist.
- Scanner and stream closure release their own work. Output consumers can
  outlive both; the accounting owner therefore outlives every outstanding
  output lease. A future C context destructor follows that same lifetime.
- Host callbacks must not run while holding internal admission locks. The
  native interface must document concurrent invocation and reentrancy rules.
  Failure/drop paths cannot depend on a callback acquiring the same lock.

This specifies the observable lifetime, not a required wrapper type. A
RecordBatch wrapper or an ArrowArray release callback alone cannot establish
ownership for native array clones that survive independently. The eventual
implementation must attach the lease at a boundary that survives those
operations or explicitly prevent unsupported escape paths in its native API.

## Acceptance matrix

Use the real native scanner with a deterministic recording accounting owner.
The owner records admissions, denials, retained bytes and releases and can
reject a selected request. Use barriers or explicit hooks for pending work,
not timing sleeps. These are proposed checks; none is claimed executed here.

| Scenario | Required observation |
| --- | --- |
| Two scans share one Session but have different host owners | Each scan charges the selected owner; closing or denying one does not transfer its charges to the other. |
| Deny first read/decode admission | Native scan returns the refusal; the covered read/allocation does not proceed; accepted reservations balance on exit. |
| Deny growth after one successful unit | The failed growth does not change the old charge; error cleanup releases completed work that has no surviving output owner. |
| Compressed variable-width data exceeds the initial estimate | The decode allocation obtains required capacity admission before growth or fails; retrospective `resize` is not counted as prevention. |
| Retain an output batch, then drop scanner and stream | Its charge remains until the batch and all shared buffer owners are dropped. Unrelated internal work is released. |
| Clone/slice nested, dictionary and variable-width output | Dropping the original batch retains the backing charge; dropping the final owner releases it exactly once. No charge multiplication for one shared allocation. |
| Take/coalesce or filter creates fresh buffers | Simultaneous input and output allocations are accounted independently, then released with their own lifetimes. |
| IVF/HNSW query with scratch allocations | Host refusal reaches the real vector path; scratch charges end on success, error and dropped execution. Cache attribution is reported separately. |
| Drop while an ordered admission is pending | Cancelled work leaves no ticket or reservation leak and does not prevent a surviving reader from advancing. |
| Export output through Arrow C Data Interface | Dropping the scanner/stream before foreign release preserves the lease and callback context; final release balances admitted output memory. This is a later binding gate. |

For each required allocation family, retain the real call path, the admitted
capacity, the native error/ownership result and the test identity. A helper
unit test only proves the helper. Tracking a subset is useful incremental
progress, but all families above remain in #34 until independently covered.

## Integration sequence and evidence boundary

Agree the native fallible reservation and buffer ownership seams with the
existing native work, then validate them through actual scan paths. Preserve
its scheduling behavior where compatible; do not emulate it in lance-c.
Only an adopted native interface can support the thin C/C++ bridge and its
foreign-context lifetime tests. Native publication, core adoption and an
MSRV change are separate authorization decisions.

The source excerpts and exact PR metadata used here are retained under
`/home/xuanwo/.local/state/codex/lance-c-parity-20261110/`, in
`native-memory34-pr10802-source/` and
`native-memory34-pr10802-meta-20261113.json`. This contract adds no allocator,
no build result and no memory benchmark. The earlier source mapping remains
valid only for the inspected native head.
