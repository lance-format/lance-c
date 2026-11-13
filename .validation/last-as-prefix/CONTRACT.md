# Native last-token prefix matching for `MatchQuery`

Status: candidate native contract and implementation against Lance 14.0.2, not
upstreamed. The original eight-case oracle ran with Rust 1.98.1; its separate
Rust 1.94.0 recheck remains in progress. The compatibility revision and additional
public-method regression below have not yet run. No lance-c public C API change
is included. Upstream publication requires separate authorization.

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
This exact revision is the implementation authority for the candidate. A query
representation inspection or a search of open PR titles does not establish
complete absence of an upstream implementation. Applicability to an exact
current upstream revision must be checked before proposing publication.

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
10. **Disabled compatibility.** With `last_as_prefix = false`, ordinary exact
    query planning retains its no-expansion shortcut. The existing public
    `InvertedIndex::expand_fuzzy_tokens` is an explicit vocabulary-selection
    call: even with `fuzziness = Some(0)`, it must discard terms absent from the
    vocabulary, retain positions, and apply `max_expansions`. It calls the same
    selection algorithm directly, without taking the query-planning shortcut.
    Missing `last_as_prefix` in serde input means `false`.
11. **Memory index.** `mem_wal` FTS has its own query expression and does not
    implement prefix expansion. `to_local_expr` rejects the flag with
    `not_supported` instead of silently degrading to fuzzy matching. Native
    parity for `mem_wal` is separate follow-up work.
12. **Errors.** No new input validation errors are introduced. Empty analysis
    and a zero budget are normal no-match cases, consistent with fuzzy behavior.

## Candidate implementation

Patch: `native-last-as-prefix.patch` (11 files, +292/-119 against the pin).
Current SHA-256: `18bad6fe2ae452dc7bd91e5243385eccb9a749413b63f8be8533afab45ae720e`.

- `query.rs`: fields, builders, `uses_vocabulary_expansion`.
- `index/partition.rs`: `PrefixAutomaton` over the whole analyzed token and an
  `ExpansionAutomaton` enum; renamed the generic candidate collector to
  `collect_expansion_candidates_with_automaton`.
- `inverted.rs`: `final_query_tokens` accepts `&[&InvertedIndex]` and retains
  the query-mode shortcut. A private `expand_query_tokens` owns the shared
  vocabulary-selection algorithm: highest-position prefix expansion, fuzzy
  expansion at earlier positions, and the common budget. `prepare_bm25_query`
  uses the same mode predicate.
- `index/search.rs`: public `expand_fuzzy_tokens` calls `expand_query_tokens`
  unconditionally, preserving explicit exact-mode vocabulary selection without
  duplicating its algorithm. Both scoring entry points use the new predicate;
  injected-scorer error text covers both expansion kinds.
- `compound.rs`, `fts.rs`: propagate the flag into effective search params.
- `fts.rs`, `scanner.rs`: exclude prefix leaves from "no expansion" shortcuts
  (compound scorer shortcuts, committed-index stats residual compound,
  post-filter).
- `parser.rs`: JSON key `last_as_prefix`, default `false`.
- `mem_wal/.../builder.rs`: explicit `not_supported` error.

The patch was generated with `git diff` from the checkout root and applies with
`git apply`.

## C surface mapping (proposal only, not part of this task)

The existing `LanceFtsMatchOptions` is a `#[repr(C)]` by-value struct; adding a
field would break its layout. That does not require a second options hierarchy.
The existing `LanceFtsQuery` is an immutable owned typed value. An additive
constructor returning a new query with the native Match flag can preserve both
the old layout and immutable-value semantics, and map directly to the consuming
native builder. A mutating setter would contradict the current query contract.

No C mapping is implemented here. It depends on an accepted native operation in
the selected core revision and needs real C/C++ ownership tests. The candidate's
native JSON parser accepts the flag, but the currently pinned parser does not;
this is not a claim of current lance-c support.

## Validation

Oracle: `fts_last_as_prefix.rs`, a one-off integration test against the patched
checkout (not part of the candidate patch). Corpus is one 10-row dataset with
fixed ids (`alpha alpenglow`, `alphabet soup`, `alpha beta`, ...) and one
list-element dataset. The index uses the simple tokenizer with
`lower_case=true`, stemming/stop-words/folding/max-length disabled so the
analyzer does not hide the token-selection rule.

Covered:

- exact behavior unchanged: `alpha alp` OR = ids [0,2,6,7,8,9] because `alp` is
  a real term in row 8; `alpha alp` AND = [8]; `alpha alpe` AND = empty;
  `alpha beta` AND = [2];
- fuzzy regression (`alpa`, fuzziness 1) = [0,2,6,7,8,9];
- prefix OR and AND; only the last token expands (`alpha alpe` AND = [0,7],
  `alpe alp` AND = empty, `alp alpe` AND = empty because the earlier `alp` is
  exact);
- Boolean must/should composition ([0,7] and [0,2,5,7]);
- shared budget and deterministic truncation (`max_expansions` 0/2/3/5),
  including the earlier exact token consuming budget;
- trailing punctuation/whitespace and empty analysis;
- serde absent/false compatibility and execution of a serde-parsed query;
- boost scaling of the leaf score (2x within 1e-5);
- list-element boundary (`al` + `pha` does not match `alp`; prefix = [0,1,3],
  exact = [3]).

Original command and result on NieR (32-core Linux, mbx-managed cargo shim),
against a disposable source copy at the pinned revision. Its retained
`.rustc_info.json` identifies Rust 1.98.1, so this is not Rust 1.94.0 evidence:

```bash
cargo test -p lance --test fts_last_as_prefix -- --test-threads=4
test result: ok. 8 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out; finished in 0.07s
```

The whole patched `lance` integration-test crate compiled before the run. Two
earlier oracle revisions failed only because their expectations were wrong
(`alpha` itself starts with `alp`; OR unions the leaves), not because of the
implementation; the final assertions above were re-run from scratch. Patch
revision validated: sha256
`3cdaf8373790c5e952585ab85288c9719859cfce2ad06645d3904e0821ab6d4a`.

Limitations: no JSON-column oracle, no default-analyzer (stemming/stop-word)
oracle, no multi-segment partition-split oracle, no full lance-c suite, and no
C surface test. Those are deliberate scope limits for this design step.

## Compatibility regression and remaining delivery work

The ninth oracle calls public `expand_fuzzy_tokens` on the same persisted index
with exact-mode tokens `zzzzmissing alpha beta`. Hardcoded expected terms and
positions verify missing-term filtering and budgets 8, 1 and 0; a missing-only
query must produce no tokens. It checks the returned vocabulary directly,
because a later posting lookup would hide the difference. The previous candidate
would return the original tokens before any dictionary lookup. The added test
is not yet executed and must fail against that candidate before validating the
repair. The original eight cases remain unchanged.

Keep the existing shared per-leaf expansion budget; no independent prefix budget
is required by this proposal. Finish bounded native validation and exact-current-
upstream applicability, then prepare the native change and its durable tests for
review. Upstream publication and a lance-c core-pin migration remain separately
authorized actions. Only after the native prerequisite is available should an
additive immutable C query mapping and C/C++ consumer coverage be delivered.
