# Changelog

## [0.2.0](https://github.com/lance-format/lance-c/compare/v0.1.0...v0.2.0) (2026-04-27)


### Features

* add fragment enumeration and fragment-scoped scanning APIs ([#3](https://github.com/lance-format/lance-c/issues/3)) ([5edc555](https://github.com/lance-format/lance-c/commit/5edc55515dbf4e1435f05c108146156c0da6aff3))
* add lance_dataset_restore for rolling back to a prior version ([#18](https://github.com/lance-format/lance-c/issues/18)) ([6f0663e](https://github.com/lance-format/lance-c/commit/6f0663e84d3992a32d0d4b0dbf0139cd0ce7482b))
* add lance_dataset_versions for listing dataset version history ([#17](https://github.com/lance-format/lance-c/issues/17)) ([5e201c7](https://github.com/lance-format/lance-c/commit/5e201c7976d3e0dcc63f53626cb33f04c0ee599d))
* add lance_dataset_write for create/append/overwrite from ArrowArrayStream ([#16](https://github.com/lance-format/lance-c/issues/16)) ([07dbdb4](https://github.com/lance-format/lance-c/commit/07dbdb4e73546e143f956afd41c7962e9d62be30))
* add lance_write_fragments for local fragment creation ([#5](https://github.com/lance-format/lance-c/issues/5)) ([5771d6f](https://github.com/lance-format/lance-c/commit/5771d6f7c674fa8254ed5fbd072277c0ebf318fd))
* **dist:** Phase 4 package distribution — CMake, vcpkg, Conan ([#26](https://github.com/lance-format/lance-c/issues/26)) ([08f14ef](https://github.com/lance-format/lance-c/commit/08f14ef25d7788caaaa37d30ddd09ba12eb1df24))
* **index:** vector & scalar index lifecycle (Phase 2 PR 1/3) ([#21](https://github.com/lance-format/lance-c/issues/21)) ([7cd4ab6](https://github.com/lance-format/lance-c/commit/7cd4ab68792b080e81c8cea9de2b867002f152d3))
* **scanner:** add lance_scanner_set_substrait_filter for Substrait filter pushdown ([#25](https://github.com/lance-format/lance-c/issues/25)) ([67c5f21](https://github.com/lance-format/lance-c/commit/67c5f21aceaa8d81db69b0a379dfbffcdfc7f6cb))
* **scanner:** full-text search via scanner builder (Phase 2 PR 3/3) ([#23](https://github.com/lance-format/lance-c/issues/23)) ([a663d53](https://github.com/lance-format/lance-c/commit/a663d5371c7f9b82e347bfa91aa7da54b828fe5b))
* **scanner:** k-NN vector search via scanner builder (Phase 2 PR 2/3) ([#22](https://github.com/lance-format/lance-c/issues/22)) ([c6b2daf](https://github.com/lance-format/lance-c/commit/c6b2daf57cfeb0fbca9a0460465e5c88da0287fd))
* Setup lance-c crate  ([#1](https://github.com/lance-format/lance-c/issues/1)) ([c2768f7](https://github.com/lance-format/lance-c/commit/c2768f7c4742dbb9b4ff8e693249f8f8cddde307))


### Bug Fixes

* **ci:** fix ci by install protobuf-compiler ([#2](https://github.com/lance-format/lance-c/issues/2)) ([8e7811b](https://github.com/lance-format/lance-c/commit/8e7811b5ad7a86d9511e986b52efa27c57e79ff7))
