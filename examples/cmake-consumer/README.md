# lance-c CMake consumer example

Minimal C++ program that opens a Lance dataset via `find_package(LanceC)`.

## Build

After installing lance-c (e.g. via `cmake --install` or `vcpkg install`):

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/lance-c-install
cmake --build build
./build/consumer /path/to/dataset.lance
```
