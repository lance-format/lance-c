# Native cooperative index cancellation candidate

This is validation-only source preparation for lance-c issue #55 (E4). It is
not a delivered C API, a dependency migration, or permission to publish upstream.

## Authority and starting evidence

Use immutable Lance 14.0.2 source at
`40f15ed1b71b66950961b175829fdf17a763f108`. Keep the lance-c dependency and public
ABI unchanged. The independent current-upstream prefix candidate has a different
base and toolchain; do not mix its source or evidence into this experiment.

The accepted six-case progress-error probe at lance-c `f8a7ddf` established that
native fallible progress callbacks preserve an `Error::External` source and
prevent manifest publication in the exercised scalar/vector paths. Its final
observer can request cancellation while returning `Ok`, after which today's
builder still publishes. BTREE also ignores progress during its long build.
These are missing capabilities of the proposed cancellation feature, rather than
violations of today's observer contract.

## Observable contract

- Cancellation is a cooperative request. A request observed at a native
  checkpoint returns a typed native cancellation error and does not publish
  an index manifest. An unobserved request arriving after the final checkpoint
  may lose to successful publication; do not claim atomic cancellation/commit.
- Check cancellation before build planning and I/O, inside the affected BTREE
  and vector partition/training work, before returning uncommitted metadata,
  and immediately before every CreateIndex publication route. The ordinary and
  both multi-segment FM branches must retain the final publication check.
- Do not cancel by dropping the outer runtime future or reconstructing a second
  transaction/commit algorithm. Reuse native build and commit entry points.
- Completed but unreferenced segment files may remain for normal native cleanup,
  as the existing E4 proposal allows. Do not promise synchronous deletion on all
  storage backends. Preserve writer/shuffle temporary-file cleanup.
- Cancellation is distinct from observer errors, I/O errors, and generic Stop.
  A non-cancellation observer failure retains its original identity.
- Preserve normal successful scalar/vector builds, progress observer semantics,
  fragment scoping, and the default no-cancellation path.

## Mechanism to prototype

Keep `IndexBuildProgress` as the single fallible native checkpoint seam. Add an
async default `check_cancelled() -> Result<()>` returning success, so existing
implementations remain source compatible and no fake progress stage is emitted.

Provide a native cancellable progress adapter around an existing observer and an
owned/clonable token. Prefer the workspace's existing `tokio_util` cancellation
token; an intentional direct dependency edge is acceptable if required, with no
version changes. Combine observer and token once per build. Do not introduce an
independent callback protocol, global cancellation registry, or token whose
lifetime depends on a borrowed foreign context. Using the existing `.progress`
builder argument is sufficient for the candidate; a new builder setter is not a
prerequisite and must not introduce setter-order bugs.

Define a public native typed cancellation source in the index progress module,
carried by the existing `Error::External` representation. This avoids changing
the exhaustive public core error enum. Verify recovery by typed downcast through
the public error API, rather than parsing text. Keep arbitrary observer errors
unchanged. Do not rename `Error::Stop`, allocate a C error number, or change C
error mapping in this experiment.

The existing C bridge retires raw callbacks with `ProgressCallbackGate`; retain
that safety boundary. For native workers, inspect actual error propagation and
task ownership, and drain/cancel owned work where necessary. Do not infer that
all backend I/O is quiescent from a progress error or an empty local directory.
Record any remaining worker-lifecycle gap explicitly; do not add a generic task
framework merely to make a stronger, previously unpromised guarantee.

## Evidence and completion

Produce a replayable native patch plus focused tests using real CreateIndexBuilder
entry points and multi-fragment fixtures. Reuse the accepted probe's fixture and
controls where suitable. Cover startup cancellation, cancellation during a long
stage, the last-completion-callback window, committed/uncommitted results,
typed identity versus an unrelated observer error, and a successful control.
Verify the separate FM publication routes with execution where practical, and
state a source-only boundary otherwise. A returned cancellation must leave the
reopened version/index set unchanged. Inspect residual files without requiring
universal orphan removal. Use deterministic coordination, not sleep-based races.

Run focused native checks on NieR with explicit/nested Rust 1.94.0, jobs 8,
test threads 4, and normal mbx storage. Do not run lance-c full/native/CMake suites
for a validation-only core patch. Record actual cases, commands, exit states,
source and patch identity. Keep failures and prior evidence. Source preparation,
bounded execution acceptance, upstream publication, release availability, and C
delivery remain separate states.
