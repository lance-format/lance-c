# lance-c CMake consumer example

The example builds two executables:

- `consumer` opens the dataset URI passed on the command line.
- `acceptance` creates, closes, reopens and scans a dataset, verifies Arrow
  release callback counts, consumes a thread-local error message, and uses an
  independent cloned dataset handle. Pass a writable `file://` URI.

## Build

After installing lance-c (e.g. via `cmake --install` or `vcpkg install`):

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/lance-c-install
cmake --build build
./build/consumer /path/to/dataset.lance
```

On Windows, the package config reports whether the libraries were built with
the static MSVC CRT. The example selects `MultiThreaded` when
`LanceC_STATIC_CRT` is on, because the installed static archive is a Release
`/MT` library and an imported target cannot propagate the consumer runtime
setting. The shared DLL does not require a redistributable VC Runtime.
