# Official core 15 migration and beta20 adoption

The user initially authorized an isolated candidate and acceptance on
2026-11-13. A later explicit instruction authorizes updating that accepted
candidate to official beta20 and proceeding through the existing lance-c
ready-PR and merge workflow after acceptance. This supersedes the earlier
preparation-only restriction and pending beta19 adoption discussion.
It does not authorize publishing upstream Lance or lance-c packages,
changing downstream Go modules, or lifting the whole-PR #149 hold.
Do not edit, rebase, comment on, merge, or cherry-pick #149. PRs #50/#68
remain author-owned. No native fork or backport is part of this work.

## Inputs and intended result

- Product source: current main `07df7bdf87d633c744ed15b9b62b1abb3a60418f`.
- Existing native: Lance 14.0.2 at
  `40f15ed1b71b66950961b175829fdf17a763f108`; current MSRV 1.94.
- Accepted starting candidate: `ec058b4817c043c9d5d65f2d73baa506438e4688`,
  with beta19 runtime artifacts retained at C source `c8e07c0`.
- Authorized official target: `v15.0.0-beta.20`, immutable commit
  `86fff5f40f299a2ff6abb0c10210451f27e605eb`; MSRV 1.95.
- Previous candidate pin: `98f84091db531775ab03fa7edb3d34c86937fbf5`.
  Preserve the accepted beta19 evidence under its original identities.
- The upstream-declared build toolchain 1.98.1 is distinct from its stated
  minimum 1.95. Passing on 1.98.1 must not be labeled 1.95 acceptance.

Continue the existing isolated lance-c candidate from these product inputs.
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

For beta20, inspect the actual beta19-to-beta20 dependency changes rather
than assuming that the release contains only the two blocker fixes. Reuse
unchanged evidence and fixtures; justify any broader integration selection
by the changed reader, encoding, dataset, index or dependency surfaces.

Required incremental behavior:

- Exercise the released 14.0.2 Blob fixture through native raw-reader and
  existing public C/C++ raw-reader entry points. Check default and explicit
  projection, descriptors, null and empty values, and ownership across
  multiple Blob columns. Do not substitute a dataset scanner for raw-reader
  acceptance or infer a language binding pass from a native-only result.
- Exercise shallow clone with separate source and target stores, reading
  shared data through the returned handle, publication at the target,
  reopening with explicit source parameters, and chained-clone lifetimes.
  Use only capabilities actually exposed by the current binding. If a
  required storage option is not exposed, record that boundary precisely;
  do not add a C-only mechanism or claim an unexecuted public path passed.
- Preserve the real core14 attached/detached transaction fixture and public
  C readback. No arbitrary cross-core transaction wire guarantee is added.
- Namespace refresh has only offline real-I/O evidence upstream. Do not
  claim real cloud credentials, Python/Java publication, or platform-wide
  acceptance. A fresh process still needs explicit source parameters.

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
within lance-c; no amend/rebase/reset/force-push. The coordinator owns ready-PR
creation and merge after independent source review, execution acceptance and
normal hosted CI. No further adoption permission is needed for this beta20
update. Return concrete source and artifact identities, actual evidence,
compatibility findings and actionable remaining gaps to the coordinator.

## Downstream routing

C owns preparation and acceptance in the existing continuation conversation.
Go owner thread `74cb281f-dd49-45ea-8492-2e438370170f` already knows this limited
authorization. Only accepted integration/adoption outcomes that change
downstream eligibility should be routed there. The glibc2.28 policy is adopted
separately; PR50 integration and any affected package revalidation still remain.
