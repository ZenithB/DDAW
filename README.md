# DDAW

C++20 / JUCE 8 audio workstation built around DDSP synthesis. macOS, Apple Silicon, AGPL-3.0.
Start with `docs/PLAN.md`, then `docs/ARCH.md`.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Audio check on the default output device (plays a quiet tone, needs a Release build for meaningful timing):

```sh
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel
./build-rel/src/app/ddaw_audiotest_artefacts/Release/ddaw_audiotest 4 64   # seconds, buffer frames
```

Options: `-DDDAW_BUILD_APP=OFF` (skip JUCE, fast core/test builds), `-DDDAW_BUILD_TESTS=OFF`.
JUCE 8.0.15 and Catch2 3.16.0 are fetched at configure time (`cmake/Dependencies.cmake`).

### Broken Command Line Tools libc++

If `#include <vector>` fails, `/Library/Developer/CommandLineTools/usr/include/c++/v1`
is a partial stub that shadows the SDK's libc++. Proper fix: reinstall the CLT
(`sudo rm -rf /Library/Developer/CommandLineTools && xcode-select --install`).
Workaround (needs env vars, not `-D` flags, because JUCE's `juceaide` configures separately):

```sh
SDK=$(xcrun --show-sdk-path)
export CXXFLAGS="-nostdinc++ -isystem $SDK/usr/include/c++/v1" OBJCXXFLAGS="$CXXFLAGS"
```
