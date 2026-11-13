# Native `last_as_prefix` candidate for upstream main 793a9e5

Source-only candidate that ports the pinned native experiment (Lance 14.0.2,
`40f15ed1b71b66950961b175829fdf17a763f108`) to exact upstream main
`793a9e5156556ef315062f051779ee47de32d5ad`.

This directory is a separate validation artifact. It does not replace the
pinned experiment under `../last-as-prefix/`, and it does not change any native
pin, C API, ABI, or error contract. No C API surface is added or modified.

## Artifacts

| Artifact | SHA256 |
| --- | --- |
| `native-last-as-prefix-main-793a9e5.patch` (this candidate) | `0eb2ee32c25dcff542117dfb911ea4a678ba43b8008a427339ce3ba23641c583` |
| `../last-as-prefix/native-last-as-prefix.patch` (pinned, unchanged) | `18bad6fe2ae452dc7bd91e5243385eccb9a749413b63f8be8533afab45ae720e` |

- Candidate base tree: `b71237fa79dd9c94da47752e026430bd93ce7243`
  (identical to `793a9e5^{tree}`).
- Candidate patch: 16 files, +871/−146.
- Pinned oracle `../last-as-prefix/fts_last_as_prefix.rs` is intentionally not
  copied into the patch; it remains the pinned experiment's test.

## Semantics

`MatchQuery.last_as_prefix` and `FtsSearchParams.last_as_prefix` default to
`false` and round-trip through serde. The builders
`with_last_as_prefix(bool)` exist on both types.

When enabled for a `Match` leaf:

- Only the full analyzed token at the highest analyzed position is
  prefix-expanded. Earlier positions keep exact or fuzzy semantics, including
  `fuzziness` and `prefix_length`.
- The final position ignores `fuzziness`/`prefix_length` for that token and is
  matched by a start-with automaton over the indexed vocabulary.
- The per-leaf `max_expansions` budget is shared. Prefix candidates and earlier
  exact/fuzzy alternatives are selected in position order and then
  lexicographic order; `0` yields no candidates. There is no separate prefix
  budget.
- Analyzer output is used as-is: trailing punctuation/whitespace are removed by
  the analyzer, stop words do not create a final token, and an empty analysis
  matches nothing.
- JSON query tokens are matched verbatim, so only the `value` token can grow.
- `Boolean`, `Boost`, `MultiMatch`, and `CombinedFields` compose the leaf
  unchanged; list-element granularity remains per element.
- `MatchQuery::uses_vocabulary_expansion()` and
  `FtsSearchParams::uses_vocabulary_expansion()` are the single source of truth
  for exact-vs-expansion branches: `uses_fuzzy_expansion(fuzziness) ||
  last_as_prefix`.
- The public `InvertedIndex::expand_fuzzy_tokens` always performs explicit
  vocabulary selection, including when `fuzziness == Some(0)`: absent terms are
  filtered, positions are kept, and `max_expansions` is honored. It uses the
  same private `expand_query_tokens` selector as `final_query_tokens`, so the
  selection algorithm is not duplicated.
- MemWAL rejects the flag with `Error::not_supported` in the memtable scanner
  builder instead of silently ignoring it. Residual mem_wal FTS leaf paths also
  reject any vocabulary-expanding leaf explicitly.

## Pinned to main mapping (no-loss audit)

| Pinned hunk | Main destination | Notes |
| --- | --- | --- |
| `inverted/query.rs` fields, builders, predicate | same file | Added `last_as_prefix` to both params and query; kept main's term-group model and serde defaults. |
| `inverted.rs` `final_query_tokens` | same file | Kept the main `&[Arc<InvertedIndex>]` signature, so `cross_column.rs` needs no edit (the pinned adaptation there was signature-only). Moved selection into one private `expand_query_tokens(&[&InvertedIndex], ...)` shared with the public API. |
| Pinned `PrefixAutomaton` / `ExpansionAutomaton` | `inverted.rs` | Replaced the bespoke two-enum automaton with `fst::automaton::Str::new(token).starts_with()`. Same language: terms whose UTF-8 bytes begin with the analyzed token; empty prefix matches all. Removes impossible fuzzy/prefix state combinations. |
| `inverted/index/partition.rs` | same file | Partition-level `expand_fuzzy` applies prefix only at the highest position; collector renamed `collect_expansion_candidates_with_automaton<A: fst::Automaton>`. |
| `inverted/index/search.rs` | same file | `expand_fuzzy_tokens` delegates to the shared selector; both scoring guards use the predicate; injected-scorer error mentions fuzzy or prefix. |
| `inverted/parser.rs` | same file | JSON key `last_as_prefix`: omission defaults to `false`; provided non-boolean values, including `null`, return `InvalidInput` with the field and value, matching serde's boolean field contract. |
| `inverted/compound.rs` `effective_params` | same file | Propagates `last_as_prefix` into each leaf's effective params. |
| `dataset/scanner.rs` residual/postfilter gates | `dataset/scanner/fts.rs` | Main split scanner FTS planning into a module; residual compound gating uses `!uses_vocabulary_expansion()` for `Match` and `MultiMatch`. |
| `io/exec/fts.rs` gates | `io/exec/fts/{matches,match_query,compound,flat_match,corpus}.rs` | Main split the old file; predicate renamed `compound_query_uses_vocabulary_expansion`, scorer-only guard, post-filter rejection, and Match exec effective params all use the predicate. |
| (not in pinned patch) `io/exec/fts/corpus.rs` | same file | Added the shared-scorer pair guard for vocabulary-expanding producers. Hardening required because the split moved that check into its own module. |
| `mem_wal/.../scanner/builder.rs` | same file | Explicit `not_supported` rejection plus a unit test. |
| (not in pinned patch) `mem_wal/index/fts.rs` | same file | Residual exact-leaf guards now reject any vocabulary-expanding leaf, including `last_as_prefix`, instead of only fuzzy leaves. |

`git diff --check` is clean. All 16 changed native files pass direct rustfmt
from the upstream-declared Rust 1.98.1 toolchain, with edition 2024 and
`skip_children=true`; this does not claim a whole-workspace Cargo fmt run.
The candidate has not yet been compiled or executed.

## Tests added

Index-level (`rust/lance-index/src/scalar/inverted/index/tests/query.rs`):

- `test_last_as_prefix_shares_one_budget_across_positions` (caps 0/1/3/8,
  last-position-only expansion, earlier exact term competes).
- `test_last_as_prefix_disabled_keeps_exact_tokens`.
- `test_last_as_prefix_keeps_earlier_fuzzy_positions`.
- `test_public_expand_fuzzy_tokens_selects_exact_vocabulary` (absent-term
  filtering, positions, caps 8/1/0, and `final_query_tokens` passthrough).

Dataset-level (`rust/lance/tests/query/inverted.rs`):

- `test_inverted_last_as_prefix_expands_only_highest_position` (exact
  unchanged, last-token-only, argument order, Boolean must/should, shared
  budget 1/2, trailing punctuation, empty analysis, boost score).
- `test_inverted_last_as_prefix_respects_list_element_boundaries`.
- `test_inverted_last_as_prefix_merges_fragments_before_truncation`
  (reject all-unindexed and partially-indexed prefix queries, retain the flat
  exact-match control, then optimize/append and check merged-vocabulary caps).

Serde/parser:

- `test_match_query_serde` and `test_fts_search_params_last_as_prefix_serde_default`.
- `test_from_json_match` updated plus `test_from_json_match_last_as_prefix_defaults_false`
  and `test_from_json_match_last_as_prefix_rejects_non_boolean`, including direct
  serde rejection of the same malformed values.

MemWAL:

- `local_fts_query_maps_leaf_shapes_and_rejects_the_rest` covers `not_supported`
  for `last_as_prefix`.

Flat execution:

- `test_flat_match_query_rejects_last_as_prefix` calls the public physical
  execution boundary with the flag supplied through either the query or the
  search params, and asserts the native `NotSupported` source.
- The common scanner flat-planning path rejects the flag before reading data;
  its callers include unindexed/partial scans and forced flat paths. The direct
  execution guard also protects callers that bypass that planner. Separate
  overlay/vector-rerank runtime cases are not claimed.

## Verification performed

- `git apply --check` on a pristine `793a9e5` working tree: passes.
- Applying the patch to that pristine tree produces tree
  `96dfc52af9955f4d27d4f916dc283b1bcbb51c36`, identical to the local staged
  tree of the candidate.
- `git diff --check`: clean.
- Static review of every changed hunk, remaining `fuzziness == Some(0)` /
  `uses_fuzzy_expansion` call sites, match struct literals, and test helpers.

Not run, per the source-only constraint: `cargo check`, `cargo test`, Clippy,
CMake, benchmarks, and any runtime execution. The separate pinned Rust 1.94
MSRV job was not touched.

## Unresolved concerns

1. **Indexed execution boundary**: last-token prefix requires an indexed FTS
   execution path. Flat Match planning/execution returns `NotSupported` instead
   of silently substituting exact matching, including no-index and partial
   coverage. Forced flat paths such as overlay scans or vector reranking can
   also be unsupported even when some or all rows have index coverage. Full
   flat prefix scoring is not implemented by this candidate.
2. **Explicit fuzziness uses the simple analyzer**: with `fuzziness > 0`,
   `tokenizer_for_match_query` keeps the existing legacy behavior of using a
   simple analyzer instead of the index analyzer (upstream issue #9656). Prefix
   with `fuzziness == Some(0)` uses the index analyzer as intended; prefix with
   positive fuzziness inherits that existing limitation.
3. **ngram token types**: ngram tokens all share position `0` with independent
   term groups, so the max-position rule expands every ngram group. This
   follows the documented position rule but is not separately designed for
   ngram.
4. **Shared budget ownership**: upstream issue #9659 questions how fuzzy AND
   terms consume the shared `max_expansions` budget. This patch keeps the
   existing shared-budget contract and documents it; it does not change that
   design.
5. **Planner refactor risk**: upstream PR #11018 (row-address domain for the
   inverted index) touches the same area and will require a rebase.
6. **JSON types**: omission defaults to false; true/false are accepted.
   Every provided non-boolean value, including null, is rejected. This aligns
   the new custom-parser field with its serde boolean contract; it does not
   change parsing of existing fields.
7. **Certification**: the old eight-case pinned oracle does not certify JSON
   query execution, default-analyzer edge cases, or multi-segment behavior by
   itself. New tests cover cross-fragment merging and serde/parser defaults,
   but none of them were executed in this source-only task.

## Validation toolchain boundary

Upstream declares Rust 1.98.1 in `rust-toolchain.toml` and a minimum of 1.95.0.
Any execution of this upstream candidate must identify that toolchain and must
not be called lance-c Rust 1.94 acceptance. The selected lance-c core pin and
its Rust 1.94 contract remain unchanged. Native-current execution is pending;
the previously accepted pinned-native nine-case oracle is separate evidence.

## Dataset budget correction (2026-11-13)

The first NieR dataset run executed three cases on Rust 1.98.1: two passed,
one exposed a mistaken test expectation. The fixture has no literal `alp` in
its vocabulary. Shared expansion selects actual vocabulary keys, so `alpha`
uses the first slot and `alpenglow` uses the second. The corrected test checks
that cap 1 cannot satisfy both AND positions and cap 2 returns row 0. No
production selector changed. The prior 14 index and two lance-lib results
remain applicable; the corrected dataset case awaits its focused recheck.
Earlier source-only statements describe the original preparation stage.
