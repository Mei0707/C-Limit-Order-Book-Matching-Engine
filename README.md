# C++20 Limit Order Book & Matching Engine

An in-memory, multi-instrument exchange simulator implementing the supplied [Project Proposal.pdf](Project%20Proposal.pdf). Matching uses price-time priority, integer prices and one writer per engine. The repository includes a command-line simulator, deterministic replay, Google Test correctness tests, an independent reference matcher, a bounded SPSC queue and reproducible allocation/latency benchmarks.

## Build and test

Requires CMake 3.20+, a C++20 compiler, and threads. Google Test is used only for tests. CMake uses an installed Google Test 1.15+ package or downloads pinned release v1.15.2 when `LOB_FETCH_GTEST=ON` (default). To build completely offline, provide `GTest_DIR`, or disable tests with `-DLOB_BUILD_TESTS=OFF`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

The core has no third-party runtime dependencies. Modern standard libraries use `std::pmr`; older Apple libc++ builds use the included resource allocator and chunk pool.

## Run the proposal's example

```sh
./build/lob_sim --input examples/proposal.txt \
  --events build/events.log --snapshot build/state.json

# Reconstruct from commands and verify every event and intermediate checksum.
./build/lob_sim --input examples/proposal.txt --queued \
  --verify build/events.log --events build/queued-events.log
```

The example buys 150 shares at a limit of 10050 ticks: 80 execute at 10000 and 70 at 10050, leaving 50 shares at 10050. It then demonstrates amendments, failed FOK, market orders, cancellation, a second instrument, and tick validation. Journals go to stdout unless `--events` is supplied; the summary goes to stderr. `--baseline` disables pooling. A successful run exits 0 even if the engine rejects individual commands; malformed files and verification failures exit 1. `--help` lists options.

### Command format

```text
LOB_COMMANDS 1
INSTRUMENT 1 1
INSTRUMENT 2 5
ADD 1 1 BUY LIMIT GTC 10000 80
ADD 2 1 SELL MARKET IOC 0 20
MODIFY 1 1 9990 50
CANCEL 1 1
```

Fields:

- `INSTRUMENT instrument_id tick_size`: definitions precede commands; default is instrument 1, tick size 1.
- `ADD order_id instrument_id BUY|SELL LIMIT|MARKET GTC|IOC|FOK price quantity`.
- `MODIFY order_id instrument_id new_price new_remaining_quantity`.
- `CANCEL order_id instrument_id`.

All numbers are decimal integers. Prices are positive integer units aligned to the instrument's tick size; market prices must be zero. If one unit is $0.01, 12537 means $125.37. `#` starts a comment. The parser rejects malformed or out-of-range values with line numbers; valid syntax with invalid order semantics generates a rejection event.

## Matching and lifecycle rules

- Best opposite price first, then FIFO. Trades execute at the resting order's price. Partial fills retain the maker's priority.
- GTC limit residuals rest. IOC residuals cancel. Market orders support IOC or FOK and never rest; market GTC is rejected.
- FOK checks executable liquidity without changing the book. Insufficient liquidity produces an acceptance followed by a cancellation, with no trades.
- Amendments specify remaining quantity. A same-price decrease or no-op preserves priority. An increase or price change replaces the order with the same ID and a new sequence, and may immediately match.
- Zero-quantity amendments are rejected; use `CANCEL`. Invalid amendments leave the original unchanged.
- IDs are unique across live orders in all instruments. IDs may be reused after cancellation, expiration or full execution. Instrument books do not match each other.
- Accepted commands, including cancellations, amendments and unsuccessful FOKs, receive increasing sequences. Rejections have sequence 0 and do not advance engine state. The order's sequence records when it acquired its current priority.
- Quantity aggregation detects overflow and rejects before mutation. Cumulative traded volume uses two 64-bit words.

See [architecture and tradeoffs](docs/design.md), [proposal coverage](docs/requirements.md), [validation results](docs/validation.md), and [measured performance](docs/performance.md).

## Library API

```cpp
#include "lob/engine.hpp"

lob::Engine engine({{1, 1}, {2, 5}});
const auto& events = engine.process(
    lob::Command::add(42, 1, lob::Side::Buy, 12537, 100));
auto bid = engine.best_bid(1);          // O(1), optional DepthLevel
auto depth = engine.depth(1, 10);       // Up to ten levels on each side
auto order = engine.find(42);          // Expected O(1)
auto snapshot = engine.snapshot();
auto checksum = engine.checksum();
```

Returned event references last until the next `process()` call; copy events you need to retain. Queries for unknown instruments throw `std::out_of_range`. `process()` represents validation errors as events. Construction rejects invalid instrument definitions with `std::invalid_argument`. The engine has one owner thread; queries and mutations must not run concurrently. `run_queued()` creates and joins a matching thread, retries full queues and drains before returning. It transfers exclusive engine access to that worker for the duration of the call.

## Sanitizers

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLOB_ASAN_UBSAN=ON
cmake --build build-asan -j 4
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLOB_TSAN=ON
cmake --build build-tsan -j 4
ctest --test-dir build-tsan --output-on-failure
```

Address/undefined sanitizers and thread sanitizer require separate builds. Runtime support depends on the host platform.

An independent UBSan build is available with `-DLOB_UBSAN=ON` when the host cannot run ASan. Local validation outcomes, including runtime limitations, are recorded in [validation](docs/validation.md).

## Benchmark

```sh
mkdir -p benchmarks/results
./build/lob_bench --commands 100000 --repeats 3 --active-orders 10000 \
  --output benchmarks/results/main.csv
python3 scripts/benchmark_report.py benchmarks/results/*.csv
```

Profiles cover resting insertion, cancellation churn, aggressive markets, mixed lifecycle operations, deep books, and burst ingestion. Every profile compares baseline allocation and pooling on identical generated commands; burst also runs through the queue. CSV records throughput, p50/p95/p99/max latency, tracked memory and allocation counts, process peak RSS, queue occupancy, retries, dropped commands, rejections, trades and final checksums. Generate data before timing; warm up before collecting samples. Report generation optionally uses Matplotlib; the benchmark executable does not.

The proposal's 100K/1M commands per second and latency budgets are engineering targets. [The report](docs/performance.md) contains measured local results, repeated-run ranges and measurement limits.

## Layout

```text
include/lob/      Public types, engine, queue, runner, replay, memory resources
src/             Matching engine and structured-text serialization
apps/            Command-line simulator
tests/           Google Test units, randomized differential oracle, queue and replay tests
benchmarks/      Stateful workload harness and raw measured CSVs
scripts/         Report and chart generation
examples/        Proposal example and lifecycle commands
docs/            Design, requirement mapping, validation and performance report
```

Resume summary: Built a C++20 multi-instrument matching engine with price-time priority, IOC/FOK, indexed cancellation, deterministic replay, pooled allocation and bounded SPSC ingestion; validated against an independent reference matcher and measured throughput and latency across six reproducible workload profiles. Add hardware-specific numbers from the measured report when using this summary.
