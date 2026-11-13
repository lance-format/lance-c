# Native strict-before checkout candidate (#108)

This is a validation-only Rust prerequisite for lance-c #108, not an approved
upstream API, a dependency migration, or a delivered C operation. Prepare it
against immutable Lance 14.0.2 `40f15ed1b71b66950961b175829fdf17a763f108`.
Keep all lance-c product APIs, its native pin, and Rust 1.94 requirement unchanged.

## Existing behavior and proposed boundary

The pinned Python entry point (`python/python/lance/__init__.py:316-341`)
selects the greatest **numeric version** among retained versions with a
timestamp strictly less than the cutoff. It does not select the greatest
timestamp. An explicit `version` bypasses Python's `asof` logic. Its old
"on or earlier" documentation does not match the strict comparison.

The Rust `Dataset::versions` method (`rust/lance/src/dataset.rs:2715`) reads
retained, attached manifests in the current branch and sorts numeric versions.
`Dataset::checkout_version` delegates to the established branch-aware checkout
path, preserving session, object stores, and read options. No native method
combines timestamp selection with checkout.

Prototype one additive native method next to the existing checkout methods:

```rust
pub async fn checkout_before(&self, cutoff: DateTime<Utc>) -> Result<Self>
```

The explicit name communicates the strict boundary. Reuse `versions()` and
`checkout_version()`; do not introduce a second manifest reader, reference enum
variant, options type, index, cache, or generic resolver framework. Select the
maximum numeric version satisfying `version.timestamp < cutoff`. A helper is
only justified by a concrete independent responsibility, not test access.

No match returns native `Error::InvalidInput` with the cutoff, current branch,
and a precise explanation that no retained version is strictly before it.
Do not claim that the cutoff predates every historical version: cleanup may
have removed earlier versions. Preserve listing, decoding, and checkout errors
unchanged. Do not silently skip unreadable manifests or retry with an older
version when the selected version becomes unavailable.

## Precision, history, and ownership

- Compare native `DateTime<Utc>` values at their existing precision. Do not
  convert through `i64` nanoseconds, floating point, Python datetime, or local
  time. This also avoids adding an artificial 1677-2262 timestamp range.
- Python's `_convert_version_timestamp` (`dataset.py:6184`) converts to a naive
  local datetime with microsecond precision; `sanitize_ts` can also lose
  sub-microsecond precision. Those binding limitations are not the proposed
  native contract. Do not claim exact Python equivalence for timezone-aware or
  sub-microsecond inputs. No Python behavior changes are part of this candidate.
- Search only the current branch's retained attached history. A handle checked
  out at an old version does not cap the history search at that version.
  Detached commits and other branches do not enter the selection set. Existing
  checkout enforces the selected branch; do not reconstruct the dataset by URI.
- Return an independently owned dataset and leave the receiver unchanged.
  Follow the existing checkout path's metadata and storage option behavior.
- The list-and-checkout operation is not a transaction with concurrent cleanup
  or new commits. Resolve the versions observed by the existing enumeration;
  propagate a subsequent checkout failure. Do not promise a globally atomic
  snapshot or add retry/fallback semantics.
- Reusing `versions()` reads the retained manifests and computes their summaries.
  This bounded candidate keeps that existing cost; no performance claim or
  separate pagination/indexing project is required.

## Observable examples

With retained versions `(1, 10s)`, `(2, 30s)`, `(3, 20s)`:

| Cutoff | Result | Reason |
| --- | --- | --- |
| 10s | InvalidInput | Equality is excluded and nothing is earlier. |
| 20s | Version 1 | Version 3 is equal to the cutoff. |
| 25s | Version 3 | It is the largest eligible version number. |
| 31s | Version 3 | Numeric order wins even though version 2 has a later timestamp. |

Equal timestamps on versions 2 and 3 choose version 3 when both are eligible.
After removing version 3, cutoff 25s chooses retained version 1, not a missing
historical state. If no eligible retained version remains, report InvalidInput.
At nanosecond boundaries, a timestamp of `10s + 1ns` is excluded at that same
cutoff and included at `10s + 2ns`.

## Focused acceptance

Use native dataset entry points with controlled manifest timestamps and real
multi-version, multi-fragment data. Prefer an existing native commit-time seam;
if no public seam exists, place focused tests beside the native implementation
where existing test facilities can set timestamps. Do not export a test-only
production method or sleep to manufacture distinct timestamps.

Cover exact equality, no match, nonmonotonic/equal timestamps, sub-microsecond
boundaries, retained-history gaps, old-handle selection, current-branch isolation,
and unchanged receiver/returned data. Assert selected version and actual rows
through the returned dataset, not only an internal selection helper. Cover a
retained-manifest read failure if the existing store/test seam makes it practical;
otherwise record that propagation as source-only. Distinguish newly added
behavioral execution from the already accepted C reference/tag/branch tests.

Deliver a replayable native patch, focused tests, and a small runner under this
validation directory on a task-specific lance-c branch. Use normal incremental
commits and pushes for Git transport; never amend, reset, rebase, or force-push.
All compilation and tests run on NieR with explicit Rust 1.94.0, jobs 8, test
threads 4, and mbx default storage. Keep raw logs/exits, exact source/patch identity,
and actual test names/counts. Do not rerun full C/C++ suites for this native-only
candidate. Independent source review and upstream adoption remain separate gates.
