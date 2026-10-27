# Resume and recruiter summary

Use two or three bullets, depending on space. These numbers were measured on an Apple M2 Pro; include that qualification when quoting performance.

## Quant developer / C++ systems version

- Built a C++20 multi-instrument matching engine sustaining **19.26M commands/s** with **0.166 µs sampled p99 processing latency** on a 100K-live-order mixed workload across four instruments (Apple M2 Pro; median of three ≥5-second runs).
- Improved measured throughput **2.49×** over per-node allocation using pooled storage; supported **1,000,000 live orders** with **129.7 MiB tracked book storage** in a separate scale test.
- Validated **1,000,000 randomized commands across 20 seeds** against an independent reference matcher (**2,000,000 implementation comparisons**, zero observed mismatches); verified deterministic replay and no loss/duplication across **141,600,000 queued commands** under controlled offered load.

## Short version

Built and benchmarked a C++20 matching engine achieving 19.26M commands/s on a 100K-order synthetic workload, 2.49× throughput vs baseline allocation, and zero mismatches across 1,000,000 randomized commands (Apple M2 Pro, three repeated runs).

## 60-second demonstration

1. Open [impact.md](impact.md) and point to the measured results, scaling table and queue-latency curve.
2. Run `./build/lob_sim --input examples/proposal.txt` and explain the two maker-price executions from the proposal.
3. Show `benchmarks/impact-results/stress.json`: seed counts, event hashes and zero mismatches.
4. Explain one tradeoff: pooling retains memory, or full-queue retries preserve commands while end-to-end latency grows.

Describe throughput as **commands/s**, not trades/s: commands include adds, cancels and amendments. The one-million-order capacity result and the one-million-command oracle run are separate experiments. Report p99 as sampled **in-process processing** latency; use the queue table when discussing end-to-end latency. Avoid claims of production readiness, exchange-grade networking, zero bugs, or universal zero allocations.

Full methodology, raw data links and reproduction steps: [impact.md](impact.md).
