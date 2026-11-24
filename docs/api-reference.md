# Generated C/C++ API reference

The public headers carry Doxygen documentation, and [`Doxyfile`](../Doxyfile) renders it as searchable HTML. The configuration reads the public C headers under `include/lance/` (the exact list is the `INPUT` entries in `Doxyfile`), the C++ umbrella `include/lance/lance.hpp`, and the shared C++ declaration header `include/lance/detail/declarations.hpp`; it does not scan the Rust sources or any other directory.

## Render locally

Use the pinned Doxygen release so the diagnostics match CI:

```bash
bash .github/scripts/install-doxygen.sh
# Add the bin directory printed by the installer to PATH, then:
doxygen Doxyfile
```

Open `build/doxygen/html/index.html`. The output is ignored by Git.

If Doxygen is already installed, `doxygen Doxyfile` from the repository root is enough. A different Doxygen version can report a different warning set. To run the full CI check with a specific binary, set `DOXYGEN_BIN`:

```bash
DOXYGEN_BIN=/path/to/doxygen bash .github/scripts/check-doxygen.sh
```

## What CI validates

The `API docs (Doxygen)` CI job runs `.github/scripts/check-doxygen.sh` on every pull request and push to `main`. The check:

- Runs a header-only self-test that must fail on a wrong `@param` name, a documented parameter without a description, malformed markup such as an unescaped `<uri>`, and an unresolved `@ref`. A correctly documented fixture must pass, so the check cannot pass by rejecting everything.
- Renders the public headers with `WARN_AS_ERROR = FAIL_ON_WARNINGS`, so any documentation warning fails the job. Findings are written to `build/doxygen/warnings.log`.
- Asserts that the index page, the public header pages, the `lance::Dataset` and `lance::Scanner` class pages, the search index, and representative C and C++ symbols are present. A configuration change that silently dropped a header or stopped extracting entities fails here.

The job does not require a Rust toolchain and takes seconds after checkout.

## Publication

The rendered HTML is a build artifact. CI validates it but does not publish or upload it. Hosting the reference is a separate decision.
