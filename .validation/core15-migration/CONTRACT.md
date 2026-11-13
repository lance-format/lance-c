# Official core 15 migration candidate

The user authorized an isolated candidate and acceptance on 2026-11-13.
This contract does not authorize merging the migration, publishing upstream
Lance, changing downstream Go modules, or lifting the whole-PR #149 hold.
Do not edit, rebase, comment on, merge, or cherry-pick #149. PRs #50/#68
remain author-owned. No native fork or backport is part of this work.

## Inputs and intended result

- Product source: current main `07df7bdf87d633c744ed15b9b62b1abb3a60418f`.
- Existing native: Lance 14.0.2 at
  `40f15ed1b71b66950961b175829fdf17a763f108`; current MSRV 1.94.
- Authorized official candidate: `v15.0.0-beta.19`, immutable commit
  `98f84091db531775ab03fa7edb3d34c86937fbf5`; proposed MSRV 1.95.
- The upstream-declared build toolchain 1.98.1 is distinct from its stated
  minimum 1.95. Passing on 1.98.1 must not be labeled 1.95 acceptance.

Prepare a normal separate lance-c candidate branch from these product inputs.
Update related native dependency revisions coherently, make only necessary
lockfile changes, and adapt the existing thin wrappers to the changed native
APIs, including TransactionV2 and lance-geo where required. Preserve public
C/C++ signatures, ownership, error identities, and existing supported behavior.
Do not add the separately prepared native #35/#55/#108/#113/#71 experiments or
implement RTree bindings here. Those prerequisites have independent ownership.

Treat storage-format defaults, old-reader interoperability, transaction
publication/retry, native validation stages, and Arrow lifetime as explicit
compatibility questions. Document any changed behavior instead of silently
relaxing expectations or building C-only semantics. A native incompatibility
may prevent acceptance even when compilation succeeds.

## Acceptance

Use existing project verification paths on NieR with Git-transferred source,
mbx default build/cache storage, bounded jobs (8) and test threads (4).
Never start or re-enable rust-analyzer. Do not set target/cache overrides.

1. Retain exact source/dependency identities and the complete candidate diff.
   Verify the official target and run format/check/lint appropriate to the
   changed surfaces using the proposed minimum toolchain and locked resolution.
2. Exercise the affected native/Rust/C/C++ boundaries, including existing
   transaction/retry/publication and historical dataset paths. Use existing
   meaningful test cases; add tests only for concrete new risks or confirmed
   migration defects. Run the existing C11/C++17 consumers against the candidate.
3. Check the established semantic ABI guard and installed static/shared
   consumers. Preserve file-format compatibility or report the precise gap.
   A known raw2.2 issue remains reporting/tracking; no workaround is authorized.
4. Separate executed source/toolchain/platform evidence from source-only
   findings. Reuse unaffected earlier evidence. Candidate-wide dependency
   changes justify integration checks but do not require unrelated experiments.

If Rust1.95 fails, diagnose the exact dependency/compiler requirement and
retain the failure; do not substitute another toolchain for the promised
acceptance or silently raise the proposed minimum. Continue independent
source preparation where possible. Keep normal commits and push the candidate
within lance-c; no amend/rebase/reset/force-push. Do not create or merge a PR
for this restricted migration without coordinator acceptance and the separate
adoption decision. Return a concrete diff, actual evidence, compatibility
findings and remaining gates to the coordinator.

## Downstream routing

C owns preparation and acceptance in the existing continuation conversation.
Go owner thread `74cb281f-dd49-45ea-8492-2e438370170f` already knows this limited
authorization. Only accepted integration/adoption outcomes that change
downstream eligibility should be routed there. The glibc2.28 policy is adopted
separately; PR50 integration and any affected package revalidation still remain.
