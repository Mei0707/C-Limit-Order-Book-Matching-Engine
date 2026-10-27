# Validation results

Validated on the local Apple M2 Pro / macOS 26.5.2 host using Apple Clang 14.0.3, C++20 and Google Test v1.15.2. The Google Test source archive is SHA-256 pinned in CMake. The host's older preinstalled shared Google Test 1.11 package was incompatible with this toolchain; builds use v1.15.2 compiled from source.

| Check | Outcome |
|---|---|
| Release build (`-O3 -DNDEBUG`) | Passed, library, simulator, tests and benchmark |
| Google Test / CTest | 51/51 passed |
| Debug + UndefinedBehaviorSanitizer, non-recovering | 51/51 passed |
| Randomized differential matching | 50,000 commands; events, full order states, depth, quotes, volume and invariants compared after every operation |
| Same-price stress | 20,000 initial orders, 10,000 cancellations, then a 10,000-maker sweep, under both allocation modes |
| Concurrent ingestion | 100,000 commands in exact order through a two-slot queue; no loss/duplication; empty input and consumer exceptions terminate correctly |
| Direct vs queued engine | Identical events, snapshots and checksums for 12,000 lifecycle commands |
| Proposal CLI replay | Direct and queued journals identical; JSON parsed successfully |
| CLI failure paths | Detects replay mismatches before writing output, rejects malformed trailing input, and prevents output/input path collisions |
| Stateful benchmarks | Six profiles, three repetitions, both allocators; burst also queued; additional 1,000/50,000-order mixed profiles |
| Benchmark correctness | No rejected or dropped commands; each configuration's repeated and allocator-comparison checksums match |

The proposal CLI example processes fourteen commands: thirteen accepted, one invalid-tick rejection, three live orders, traded volume 220, final checksum `6521155281173364438`. The first incoming crossing limit executes exactly 80 at 10000 and 70 at 10050.

## Sanitizer runtime limitation

ASan and TSan could not execute on this host. The limitation was isolated using an independent `int main() { return 0; }` program compiled with each sanitizer, without this project's library or Google Test:

| Sanitizer | Empty-program outcome |
|---|---|
| AddressSanitizer | Aborted during runtime initialization: `sanitizer_malloc_mac.inc:189`, `!asan_init_is_running` check failed |
| ThreadSanitizer | Segmentation fault before executing application work |
| UndefinedBehaviorSanitizer | Exited successfully |

Accordingly, this report does **not** claim ASan or TSan passes. The separate build configurations are provided for a host with functioning sanitizer runtimes. UBSan executed the entire suite with undefined-behavior recovery disabled. Release concurrency tests verify observed ordering and drain behavior; they do not replace dynamic race detection.

## Reproduce correctness checks

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure

cmake -S . -B build-ubsan -DCMAKE_BUILD_TYPE=Debug -DLOB_UBSAN=ON
cmake --build build-ubsan -j 4
ctest --test-dir build-ubsan --output-on-failure

./build/lob_sim --input examples/proposal.txt \
  --events build/events.log --snapshot build/state.json
./build/lob_sim --input examples/proposal.txt --queued \
  --verify build/events.log --events build/queued-events.log
python3 -m json.tool build/state.json
```

For a restricted/offline build, extract the pinned Google Test archive locally and supply CMake's standard `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/absolute/path/to/googletest-1.15.2` override. The initial build here required network access only to download that source archive. No user files or global Python/compiler installations were modified.
