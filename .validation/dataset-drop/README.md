# Native dataset deletion candidate

This patch adds `lance::dataset::drop_dataset(&ObjectStore, &Path, bool)` on
Lance 14.0.2 (`40f15ed1b71b66950961b175829fdf17a763f108`). It makes the
existing Python deletion sequence available in Rust: validate the root,
delete everything under that root, and optionally suppress a deletion-stage
native `NotFound`. The caller supplies the already resolved store and path.
Both stages use that same pair. Native URI resolution, storage credentials,
validation, and error classification remain authoritative.

The validator accepts empty or missing roots, readable dataset manifests,
and the existing namespace markers. It rejects nonempty roots without those
proofs, including warehouses containing child datasets. Files unrelated to
Lance inside an accepted root are deleted too. Similar sibling prefixes and
external base paths are outside the deletion scope.

This is not a transactional deletion protocol. Concurrent writes can race
with validation and deletion; partial deletion can precede an error. The
operation does not protect source data referenced by shallow clones, revoke
open handles, or deregister namespace entries. Missing-prefix behavior stays
backend-specific: local deletion can return `NotFound`, while deletion of
an absent object-store prefix can succeed without `ignore_not_found`.

## Artifacts and execution

`identity.json` records the immutable base, patched tree, and patch SHA256.
`native-dataset-drop.patch` changes only the native dataset module and adds
five native integration tests. There are no C/C++ API, native pin, dependency,
or MSRV changes. This is an unpublished upstream candidate, not a delivered
C dataset-deletion API.

On NieR, prepare a clean native checkout at the recorded base using Git.
Transfer this artifact commit through Git too, then run:

```sh
bash .validation/dataset-drop/run.sh /path/to/native-checkout /path/to/evidence
```

The runner applies the patch to the native index, verifies the resulting
tree, runs the five tests using Rust 1.94.0 and `--locked`, and checks for
source drift. It uses the existing mbx storage with eight build jobs and
four test threads. It retains the command, raw log, exit code, and source
identity. Execution results must be read before accepting this candidate.

## Coverage

- Real two-fragment, two-version dataset deletion, including an unmanaged
  file; a similarly named sibling survives and reopening the dataset fails.
- Data-only, corrupt-manifest, and warehouse roots are refused without
  changing stored objects, even with `ignore_not_found`.
- Both existing namespace reservation markers permit deletion.
- Local missing and empty roots, and an absent in-memory object-store prefix,
  retain native backend semantics.
- An injected object-store failure after one successful deletion propagates
  despite `ignore_not_found`; remaining objects and an external sibling are
  checked directly.

These tests exercise native local and in-memory backends. They do not certify
cloud-provider IAM, eventual consistency, concurrent writers, shallow clone
reference protection, or namespace deregistration. Existing validator
manifest-format coverage remains in the native module.
