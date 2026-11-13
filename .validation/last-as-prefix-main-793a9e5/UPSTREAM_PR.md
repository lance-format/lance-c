# Proposed title

feat: support last-token prefix expansion in FTS match queries

# Proposed body

Match queries currently expose fuzzy matching with an exact-prefix length,
but cannot treat the last analyzed token as a vocabulary prefix. Add an
opt-in `last_as_prefix` flag to native match queries and search parameters.
For `alpha alp`, only the highest analyzed position expands to terms beginning
with `alp`; earlier positions retain their exact/fuzzy rules. Omitted flags
preserve exact behavior.

Expansion uses the indexed vocabulary and existing per-leaf shared
`max_expansions` budget, in position then lexicographic order. Absent literal
terms consume no budget. Compound queries propagate the option to their leaves;
JSON parsing and serde retain the boolean/default contract. Unsupported flat
and MemWAL execution explicitly rejects expansion, including direct and
reconstructed flat filters with a params-only flag.

The public explicit vocabulary-expansion API preserves its existing absent-term
filtering and budget semantics. This change does not alter the shared-budget
design, implement flat prefix scoring, or add Python/C APIs.

Validation: focused native execution on NieR with Rust1.98.1 and `--locked`
covered14 index cases, two lance-lib cases, three dataset cases and16 direct
filter-binding cases. Evidence combines applicable earlier runs and targeted
corrections rather than claiming a repeated whole suite. A regression first
proved the direct params-only fallback (2 passed/1 failed), then all16 filter
cases passed after the guard fix. Dataset coverage includes analyzed-position
selection, per-list-element boundaries and merged-fragment vocabulary budgets.
No whole-workspace, benchmark or binding-level acceptance is claimed.

# Submission preparation (not part of the PR body)

No upstream branch or PR has been published. Publication requires explicit
authorization separate from lance-c repository maintenance.

- Accepted artifact: `9eb383b11eff3607527b7058fcefb13c34f1d83e`.
- Patch SHA256: `077703e17d694d6efa0ffe67922653ee2784629004f2f31f13db524cdd9d4470`.
- Executed native base: `793a9e5156556ef315062f051779ee47de32d5ad`.
- Executed post-tree: `27c52e47c9cc559b6af1dcbadba8fa20f00ca8a9`.
- Current-main applicability checked at
  `3e4279e4e656ac029ec3b82947795282f6c6e8ff`; identical patch applies cleanly,
  producing tree `7a6eaa408e42a9bc5f4c1a3f3800a30bd43905cd`.

The intervening upstream diff touches five files: FTS format documentation,
fuzzy-automaton construction errors/tests, HNSW builder, KNN execution and the
resource-test allocation tracker. The FTS change classifies excessive
automaton construction as `InvalidInput` with guidance; the candidate preserves
it. The other deltas do not alter the focused prefix paths. Accepted execution
at the old base is retained as such; current-main source applicability is not
relabeled as a second execution. Focused execution of the newly added upstream
error cases is recorded separately when complete.

A bounded open-PR search for `last_as_prefix` returned no matches on2026-11-13.
This is not a claim that no unlinked or differently named work exists.
