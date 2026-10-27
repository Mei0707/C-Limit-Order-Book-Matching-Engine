# Proposal coverage

The supplied PDF has eleven pages. Its repository-structure section ends after the first README entry; the layout here fills in that incomplete outline. The document is treated as the project specification, while its performance numbers remain targets rather than asserted outcomes.

| Proposal requirement | Implementation | Verification |
|---|---|---|
| C++20, deterministic single writer | `Engine`, fixed integer types, accepted-command sequences | Unit and replay tests |
| Buy/sell limit and market | Symmetric maker-price matching | Both-side units, randomized oracle |
| Partial and multiple fills, FIFO | Ordered price maps and FIFO lists | Proposal example, priority and 10,000-maker sweep tests |
| GTC, IOC, FOK | Rest, cancel residual, non-mutating FOK liquidity check | FOK success/failure, market FOK, empty book tests |
| Quantity/price amendments | Decrease preserves priority; increases and price changes replace | Amendment and immediate matching tests |
| Cancellation | Global ID index to stable order iterator and level | Middle/last cancellation, churn tests |
| Validation | Instruments, live ID uniqueness, tick alignment, supported combinations, quantities and aggregate overflow | Invalid-input, amendment, large-quantity and differential tests |
| Multiple independent instruments | Per-instrument books, globally unique live IDs | Isolation and wrong-instrument lifecycle tests |
| Reports and market data | Ordered events, best quotes, aggregated depth | Unit assertions and every-operation differential comparison |
| Replay logs, snapshots, checksums | Versioned command/journal text, JSON state, portable checksum | Roundtrip and direct/queued replay verification |
| Invariants | Positivity, aggregates, IDs/indexes, sequence/FIFO, uncrossed books | Checker after every differential command |
| Quantity conservation | Trade maker/taker deltas, cumulative 128-bit volume | Exact reference events/state and explicit partial-fill/large-volume tests |
| Independent reference matcher | Flat-vector, linear best-order selection | 50,000 seeded randomized operations across both allocators |
| Stress workloads | Same-price churn, large sweeps, dense/sparse/deep books, large integers, queue bursts | Google Test stress cases and six benchmark profiles |
| Allocation optimization | Configurable baseline/pool, reserves and resource counters | Same-input before/after benchmark tables and charts |
| Bounded SPSC and backpressure | One producer, one worker; retry on full | Tiny-queue wrap/order tests and queued benchmarks |
| Shutdown and drain | Completion signal, acquire/recheck, join; exception propagation | Empty input, full-queue drain, worker failure tests |
| Sanitizers | Separate ASan+UBSan and TSan CMake options | Actual execution recorded in validation report |
| Reproducible performance evaluation | Seeded inputs, warm-up, repeated Release runs, latency distributions, memory, queue metrics | Raw CSVs and generated local performance report |
| Documentation and resume summary | README, design, coverage, validation, performance | Reproduction commands included |

Google Test is used for correctness; the standalone benchmark harness handles stateful workflows and queue ingestion, as permitted by the proposal's Google Benchmark guidance. No Google Benchmark dependency is required. The allocation comparison is an implemented experiment; intrusive lists, flat price arrays and fixed-capacity order arenas remain possible future optimizations. No future work is substituted for a functional requirement above.
