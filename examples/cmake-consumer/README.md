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
