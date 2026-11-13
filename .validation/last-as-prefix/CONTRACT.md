# Native last-token prefix matching for `MatchQuery`

Status: candidate native contract + candidate implementation diff, validated on a
patched Lance 14.0.2 checkout. Not upstreamed. No lance-c public C API change is
included. Upstream publication requires separate authorization.

## Problem

`lance-format/lance-c#35` still lists last-token prefix / typeahead as the one
unimplemented native prerequisite. The C surface can only expand a query with the
existing typed `MatchQuery` fields. At the pinned Lance revision:

- `MatchQuery::prefix_length` is the number of leading characters that fuzzy
  matching must keep exact. It does not turn the last query token into a prefix
  query (`query.rs`, `fuzzy_term_options`).
- `max_expansions` bounds the fuzzy vocabulary returned by `final_query_tokens`;
  it does not create prefix candidates.
- There is no `last_as_prefix` field on `MatchQuery` or `FtsSearchParams`.

Expanding words in C would need a second tokenizer and index vocabulary in the
binding, which issue #35 explicitly rejects. The native query representation
therefore has to own the operation.

## Verified sources at the pin

Pinned revision: `40f15ed1b71b66950961b175829fdf17a763f108` (Lance 14.0.2).
The same gap exists at `15.0.0-beta.16` (`03b7ff329e656636c581f1e3547048101990bc1f`)
and on upstream `main` as of 2026-11-13; no open upstream PR implements it.

Relevant source (`rust/lance-index/src/scalar/inverted/`):

- `query.rs`: `uses_fuzzy_expansion`, `fuzzy_term_options`, `FtsSearchParams`,
  `MatchQuery`.
- `inverted.rs`: `final_query_tokens`, `prepare_bm25_query`,
  `PreparedBm25Query`.
- `index/partition.rs`: `FuzzyAutomaton`,
  `collect_fuzzy_candidates_with_automaton`, posting lookup by query position.
- `index/search.rs`: `expand_fuzzy_tokens`, `bm25_base_scorer`,
  `bm25_search_documents_impl`.
- `compound.rs`: `LeafQuery::effective_params`, `tokenize_leaf`.
- `rust/lance/src/io/exec/fts.rs`: `MatchQueryExec::effective_params`,
  `compound_query_uses_fuzzy_expansion`, post-filter gate.
- `rust/lance/src/dataset/scanner.rs`: committed-index stats residual planning.
- `rust/lance/src/dataset/mem_wal/memtable/scanner/builder.rs`: `to_local_expr`.

## Proposed public Rust contract

Two fields, both defaulting to `false`:

```rust
// MatchQuery
#[serde(default)]
pub last_as_prefix: bool,
// builder
pub fn with_last_as_prefix(self, last_as_prefix: bool) -> Self;

// FtsSearchParams (internal propagation, same meaning)
#[serde(default)]
pub last_as_prefix: bool,
pub fn with_last_as_prefix(self, last_as_prefix: bool) -> Self;
pub fn uses_vocabulary_expansion(&self) -> bool;
```

`uses_vocabulary_expansion()` is `uses_fuzzy_expansion(fuzziness) || last_as_prefix`,
so every token-scoring stage uses one predicate.

### Semantics

1. **Token selection.** The prefix token is the analyzed token with the highest
   position, not the last token emitted by the tokenizer; the jieba tokenizer can
   emit positions out of order. Analysis uses the index tokenizer (lower-casing,
   stemming, stop-word removal, ASCII folding, JSON handling are applied before
   selection).
2. **Trailing content.** Trailing whitespace and punctuation are removed by the
   analyzer. If removal leaves no token, the leaf matches nothing. For `And`,
   the whole leaf fails; sibling `Or` clauses are unaffected.
3. **Stop words.** If the analyzer removes the final token, the new final token
   becomes the prefix token. Nothing is taken from the raw query string.
4. **JSON.** Vocabulary tokens have the form `path,type,value`. Using the whole
   analyzed token as the prefix keeps `path` and `type` exact and only lets
   `value` grow. No special-casing is needed.
5. **One expanded position.** Only the final position is prefix-expanded. Earlier
   positions keep their normal exact/fuzzy behavior, including `fuzziness` and
   `prefix_length`. When `last_as_prefix` is true, `fuzziness` and
   `prefix_length` do not apply to the final position (including `Some(0)` and
   `None`/AUTO).
6. **Candidate set.** A candidate is an indexed vocabulary term that starts with
   the final analyzed token. Candidates come from the per-partition FST
   dictionaries and are merged across all selected segments and partitions
   before truncation, so the surviving vocabulary does not depend on source
   token order, partition layout, or segment order.
7. **Budget.** `max_expansions` is the final vocabulary size of the Match leaf,
   shared by all query positions. This matches the existing fuzzy contract:
   when the expansion path is active, exact or fuzzy terms at earlier positions
   also consume budget once each. Selection is lexicographic (`BTreeSet`) and
   truncated to the remaining budget. `max_expansions = 0` yields no candidates.
   There is no separate per-position or per-partition budget.
8. **Row and list-element boundaries.** Expansion uses the vocabulary of the
   index being searched. Tokens are never concatenated across list elements or
   rows. The query and index must still agree on `DocumentGranularity`, as today.
9. **Composition.** AND/OR use the existing per-position alternative lookup:
   several expansions of the final position are alternatives for that position.
   `BooleanQuery`, `BoostQuery`, `MultiMatchQuery`, and `CombinedFieldsQuery`
   compose the leaf unchanged. Boost multiplies the leaf BM25 score.
10. **Disabled compatibility.** With `last_as_prefix = false`, the early return
    in `final_query_tokens` makes the code path identical to the pin. Missing
    `last_as_prefix` in serde input means `false`.
11. **Memory index.** `mem_wal` FTS has its own query expression and does not
    implement prefix expansion. `to_local_expr` rejects the flag with
    `not_supported` instead of silently degrading to fuzzy matching. Native
    parity for `mem_wal` is separate follow-up work.
12. **Errors.** No new input validation errors are introduced. Empty analysis
    and a zero budget are normal no-match cases, consistent with fuzzy behavior.

## Candidate implementation

Patch: `native-last-as-prefix.patch` (11 files, +282/-118 against the pin).

- `query.rs`: fields, builders, `uses_vocabulary_expansion`.
- `index/partition.rs`: `PrefixAutomaton` over the whole analyzed token and an
  `ExpansionAutomaton` enum; renamed the generic candidate collector to
  `collect_expansion_candidates_with_automaton`.
- `inverted.rs`: `final_query_tokens` accepts `&[&InvertedIndex]`, treats prefix
  as vocabulary expansion, selects the maximum position, and chooses the prefix
  automaton there; `prepare_bm25_query` uses the same predicate.
- `index/search.rs`: `expand_fuzzy_tokens` delegates to `final_query_tokens`
  (removes a duplicated implementation); both scoring entry points use the new
  predicate; injected-scorer error text covers both expansion kinds.
- `compound.rs`, `fts.rs`: propagate the flag into effective search params.
- `fts.rs`, `scanner.rs`: exclude prefix leaves from "no expansion" shortcuts
  (compound scorer shortcuts, committed-index stats residual compound,
  post-filter).
- `parser.rs`: JSON key `last_as_prefix`, default `false`.
- `mem_wal/.../builder.rs`: explicit `not_supported` error.

The patch was generated with `git diff` from the checkout root and applies with
`git apply`.

## C surface mapping (proposal only, not part of this task)

The existing `LanceFtsMatchOptions` is a `#[repr(C)]` by-value struct, so adding a
field is an ABI break. Two viable mappings:

1. Versioned struct: add `LanceFtsMatchOptionsV2` (or a `size`/`version` header)
   plus `lance_fts_match_options_v2_set_last_as_prefix`, and keep V1 unchanged.
2. Opaque options handle: introduce an owned options object with a
   `lance_fts_match_options_set_last_as_prefix` setter, then pass the handle to
   the existing constructor path.

Recommendation: option 1 with the smallest new surface; both map to
`MatchQuery::with_last_as_prefix` and require the new field to flow through
`effective_params`. Wait for the parent's decision because this changes the
public C surface. JSON parsing already accepts `last_as_prefix`, so the C JSON
entry point works without new symbols.

## Validation

Oracle: `fts_last_as_prefix.rs`, a one-off integration test against the patched
checkout (not part of the candidate patch). Corpus is four small row datasets and
one list-element dataset with fixed ids; the index uses the simple tokenizer with
`lower_case=true`, stemming/stop-words/folding/max-length disabled so the
analyzer does not hide the token-selection rule.

Covered:

- exact behavior unchanged (`alpha alp` exact = empty; `alpha beta` AND = id 2);
- fuzzy regression (`alpa`, fuzziness 1);
- prefix OR and AND, Boolean must/should composition;
- shared budget and deterministic truncation (`max_expansions` 0/2/3/5),
  including the earlier exact token consuming budget;
- trailing punctuation/whitespace and empty analysis;
- serde absent/false compatibility and execution of a serde-parsed query;
- boost scaling of the leaf score;
- list-element boundary (`al` + `pha` does not match `alp`).

Expected results are asserted as concrete ids; see the test file. Command:

```bash
cargo test -p lance --test fts_last_as_prefix
```

Limitations: no JSON-column oracle, no default-analyzer (stemming/stop-word)
oracle, no multi-segment partition-split oracle, no full lance-c suite, and no
C surface test. Those are deliberate scope limits for this design step.

## Remaining decision

The only material semantic choice is whether `max_expansions` should also count
exact/fuzzy terms at earlier positions when `last_as_prefix` is on. The candidate
keeps the existing fuzzy rule (shared final-vocabulary budget), because
`max_expansions` is already documented as one shared leaf budget and a separate
prefix budget would add a second, query-kind-specific limit.

Before a C feature is deliverable: upstream acceptance and pin upgrade (parent
owned), plus a versioned C options surface and C/C++ tests. Until then, lance-c
cannot expose this via the existing ABI without breaking it.
