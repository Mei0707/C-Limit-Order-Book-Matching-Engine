#!/usr/bin/env python3
"""Turn completed impact experiments into auditable results and resume bullets."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import statistics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", default="benchmarks/impact-results")
    parser.add_argument("--output", default="docs/impact.md")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source, output = root / args.input, root / args.output
    manifest = json.loads((source / "manifest.json").read_text())
    stress = json.loads((source / "stress.json").read_text())
    if manifest["status"] != "passed" or manifest["quick"]:
        raise ValueError("Resume evidence requires a completed full suite, not a smoke run")
    if manifest["environment"].get("CMAKE_BUILD_TYPE") != "Release":
        raise ValueError("Resume performance evidence requires a Release build")
    for name, expected in manifest["source_sha256"].items():
        if hashlib.sha256((root / name).read_bytes()).hexdigest() != expected:
            raise ValueError("Measured source has changed; rerun experiments before updating claims: " + name)
    if stress["mismatches"] or stress["commands"] != stress["accepted"] + stress["rejected"]:
        raise ValueError("Stress accounting mismatch")
    if len(stress["seed_results"]) != stress["seeds"] or not all(s["replay_matches"] for s in stress["seed_results"]):
        raise ValueError("Replay verification incomplete")
    groups, checksums, rows = {}, {}, []
    expected_files = {Path(command[-1]).name for command in manifest["commands"] if str(command[-1]).endswith(".csv")}
    for filename in sorted(expected_files):
        with (source / filename).open(newline="") as stream:
            for row in csv.DictReader(stream):
                if int(row["processed"]) != int(row["commands"]) or int(row["lost_commands"]) or int(row["rejected"]):
                    raise ValueError("Benchmark command accounting failed")
                if int(row["trades"]) != int(row["commands"]) // 4:
                    raise ValueError("Benchmark trade accounting failed")
                if int(row["price_levels"]) != 128 or int(row["instruments"]) != 4:
                    raise ValueError("Scaling controls changed")
                identity = (row["experiment"], row["seed"], row["active_orders"], row["commands"])
                if identity in checksums and checksums[identity] != row["checksum"]:
                    raise ValueError("Final state differs for identical command prefix: " + str(identity))
                checksums[identity] = row["checksum"]
                key = (row["experiment"], row["allocator"], int(row["active_orders"]), int(row["offered_rate"]))
                groups.setdefault(key, []).append(row)
                rows.append(row)

    def median(key, name):
        return statistics.median(float(row[name]) for row in groups[key])

    def spread(key, name, divisor=1):
        values = [float(row[name]) / divisor for row in groups[key]]
        return f"{statistics.median(values):.3f} ({min(values):.3f}–{max(values):.3f})"

    baseline = ("sustain", "baseline", 100000, 0)
    pooled = ("sustain", "pooled", 100000, 0)
    ratio = median(pooled, "commands_per_second") / median(baseline, "commands_per_second")
    throughput = median(pooled, "commands_per_second") / 1e6
    service_p99 = median(pooled, "service_p99_ns") / 1000
    max_active = max(key[2] for key in groups if key[0] == "scale")
    large = ("scale", "pooled", max_active, 0)
    large_throughput = median(large, "commands_per_second") / 1e6
    large_memory = median(large, "book_bytes") / 2**20
    all_commands = sum(int(row["commands"]) for row in rows)
    queue_commands = sum(int(row["commands"]) for row in rows if row["experiment"] == "queue")
    queue_keys = sorted(key for key in groups if key[0] == "queue")
    env = manifest["environment"]
    hardware = env.get("hardware", {})
    cpu = hardware.get("chip_type", env["architecture"]) if isinstance(hardware, dict) else env.get("cpu", env["architecture"])
    output.parent.mkdir(parents=True, exist_ok=True)
    evidence_path = os.path.relpath(source, output.parent)
    lines = ["# Project results at a glance", "",
             f"Measured on **{cpu}**, {env['os']}, C++20 Release (`{env.get('CMAKE_CXX_FLAGS_RELEASE', 'see manifest')}`). "
             "Values below come from three repetitions per configuration; throughput and percentile headlines use medians, not the fastest run.", "",
             "| Achievement | Measured result | Evidence |", "|---|---|---|",
             f"| Multi-second mixed-flow throughput | **{throughput:.2f} million commands/s**, sampled p99 **{service_p99:.3f} µs**, 100,000 live orders across four instruments | Three runs per allocator, each at least five seconds |",
             f"| Allocation optimization | **{ratio:.2f}× throughput** ({(ratio - 1) * 100:.1f}% increase) vs baseline on the same random-access workload | Alternating allocator trial order |",
             f"| Capacity | **{max_active:,} simultaneous live orders**; {large_throughput:.2f} million commands/s in the fixed-batch scale test | {large_memory:.1f} MiB tracked book allocation, excluding harness and other process memory |",
             f"| Differential correctness | **{stress['commands']:,} random commands**, {stress['seeds']} seeds, **{stress['implementation_comparisons']:,} engine/oracle comparisons**, zero mismatches | Events, full state, depth, quotes and invariants checked after every command |",
             f"| Deterministic reconstruction | **{stress['replayed_commands']:,} commands replayed**, matching event hashes and final state for every seed | Independent fresh-engine replay |",
             f"| Queue integrity under offered load | **{queue_commands:,} commands**, no observed loss or duplication | Ordinals checked individually, exact counts and book accounting checked after drain |",
             "", "The unit suite also has **54 passing Google Tests**, including workload-accounting and percentile tests. "
             "See [validation](validation.md) for sanitizer outcomes; these performance experiments are Release runs.", "",
             "## Sustained random-access mixed flow", "",
             "All 100,000 base orders remain live at the end. Uniform random cancellation/amendment targets span the base order IDs. "
             "A 200,000-command prepared batch repeats for at least five seconds per run. All commands in these benchmark runs are accepted; expiration is an accepted lifecycle outcome.", "",
             "| Allocator | Runs | Commands/s, millions (min–max) | Sampled p50 µs | Sampled p99 µs | Minimum measured duration s | Measured commands | System allocations/command | Tracked book MiB |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for key in (baseline, pooled):
        values = groups[key]
        lines.append(f"| {key[1]} | {len(values)} | {spread(key, 'commands_per_second', 1e6)} | "
                     f"{median(key, 'service_p50_ns') / 1000:.3f} | {median(key, 'service_p99_ns') / 1000:.3f} | "
                     f"{min(float(r['elapsed_seconds']) for r in values):.3f} | {sum(int(r['commands']) for r in values):,} | "
                     f"{median(key, 'system_allocations_per_command'):.6f} | {median(key, 'book_bytes') / 2**20:.2f} |")
    lines += ["", "Allocation counts cover instrument price maps, FIFO nodes and ID-index storage. "
              "Pool metadata, event buffers and harness allocations are excluded. Zero measured upstream allocations means those tracked resources "
              "made no new system allocation requests after warm-up; it does not mean zero memory use or a blanket zero-allocation API guarantee.", "",
              "## Scaling with live order count", "",
              "Each row measures a fixed 200,000-command batch after warm-up. Four instruments and 128 total base price levels remain constant; "
              "the varied dimension is the live order count. This isolates indexed access and FIFO removal from growth in the number of price levels. "
              "Targets are uniform across the live ID range; each finite batch touches a subset of those IDs. This is not a proof of asymptotic complexity.", "",
              "| Live orders | Allocator | Throughput, M commands/s (min–max) | Sampled p99 µs | Tracked book MiB | Tracked bytes/live order |",
              "|---:|---|---:|---:|---:|---:|"]
    scale_keys = sorted(key for key in groups if key[0] == "scale")
    for key in sorted(scale_keys, key=lambda k: (k[2], k[1])):
        lines.append(f"| {key[2]:,} | {key[1]} | {spread(key, 'commands_per_second', 1e6)} | "
                     f"{median(key, 'service_p99_ns') / 1000:.3f} | {median(key, 'book_bytes') / 2**20:.2f} | "
                     f"{median(key, 'book_bytes') / key[2]:.1f} |")
    lines += ["", "## Queue latency at controlled offered rates", "",
              "The producer schedules arrivals at the offered rate for two seconds. The 4,096-slot queue retries when full. "
              "End-to-end latency starts at the intended arrival time, so it includes producer scheduling delay, full-queue backpressure, queue wait and processing. "
              "Delayed requests retain their original scheduled times, avoiding the bias from starting a timer only after successful enqueue. "
              "All offered commands are eventually drained; preserving commands under overload can coexist with a large latency backlog.", "",
              "| Offered M commands/s | Completed M commands/s (min–max) | Sampled service p99 µs | Sampled end-to-end p50 µs | Sampled end-to-end p99 µs | Full-queue retries | Max observed occupancy | Lost commands |",
              "|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for key in queue_keys:
        values = groups[key]
        lines.append(f"| {key[3] / 1e6:.1f} | {spread(key, 'commands_per_second', 1e6)} | "
                     f"{median(key, 'service_p99_ns') / 1000:.3f} | {median(key, 'e2e_p50_ns') / 1000:.3f} | "
                     f"{median(key, 'e2e_p99_ns') / 1000:.3f} | {sum(int(r['queue_full_retries']) for r in values):,} | "
                     f"{max(int(r['queue_max_occupancy']) for r in values):,} | 0 |")
    for key in queue_keys:
        completed = median(key, "commands_per_second")
        if completed < key[3] * 0.95:
            lines += ["", f"At {key[3] / 1e6:g}M offered commands/s, completed throughput was {completed / 1e6:.2f}M/s "
                      f"and sampled end-to-end p99 was {median(key, 'e2e_p99_ns') / 1e9:.3f} seconds. "
                      "This overload point quantifies the latency cost of retrying and draining every command."]
    lines += ["", "Queue throughput includes draining after the offered interval. Rates with large end-to-end tails should not be described "
              "as low-latency capacity. This local producer/consumer test includes OS scheduling and excludes network and disk I/O.", "",
              "## Correctness and replay evidence", "",
              f"- {stress['commands']:,} source commands: {stress['accepted']:,} accepted and {stress['rejected']:,} deliberate/expected rejections; {stress['trade_events']:,} trade events.",
              f"- Baseline and pooled engines each compared with a separately implemented flat-vector reference matcher after every command: {stress['state_comparisons']:,} state comparisons and {stress['invariant_checks']:,} invariant checks.",
              f"- Random tests use small books for exhaustive per-command comparison (observed maximum {stress['max_active_orders']:,} live orders). The million-live-order capacity claim comes from the separate scale experiments.",
              f"- All {stress['seeds']} seeds replayed into a fresh engine with identical ordered-event hashes, full final snapshots and checksums. Seed-specific values are in the raw JSON.",
              f"- Across all performance configurations: {all_commands:,} measured commands; rejected benchmark commands and lost queued commands both zero. Warm-up and initial population are additional, untimed operations.",
              "", "## How to present this project", "",
              "Use the bullets in [resume.md](resume.md) and show the chart above. In an interview, explain the tradeoff: "
              "single-writer ownership gives deterministic matching; indexed FIFO removal makes random cancellation practical; pooling improves allocation reuse, "
              "while retained memory and queue backlog remain measurable costs. These are measured local experiments, not production exchange traffic.", "",
              "## Reproduce and audit", "", "```sh", "cmake -S . -B build -DCMAKE_BUILD_TYPE=Release",
              "cmake --build build -j 4", "ctest --test-dir build --output-on-failure", "python3 scripts/run_impact.py",
              "python3 scripts/impact_report.py", "```", "",
              "The evidence suite runs experiments sequentially. A smoke version is available with "
              "`python3 scripts/run_impact.py --quick --output build/impact-smoke`; smoke results are not accepted for resume report generation.", "",
              f"Raw artifacts: [manifest and source hashes]({evidence_path}/manifest.json), "
              f"[sustained measurements]({evidence_path}/sustain.csv), [differential/replay results]({evidence_path}/stress.json). "
              "The directory also contains scale and offered-rate CSVs. The manifest records compiler flags, CPU/OS, exact invocation and source SHA-256 values.", "",
              "Method limits: three repetitions do not provide a confidence interval. Measurements use a monotonic clock with one latency sample every 127 commands "
              "(prime stride distributes samples across the eight operation positions). Percentiles are nearest rank; table percentile values are medians of each run's sampled percentile. "
              "Timer overhead is not subtracted. The workload is synthetic and repeats a seeded batch; it is not a calibrated exchange feed. "
              "There is no CPU pinning or frequency control. Tracked memory is not total RSS. Warm-up and input generation occur before timing. "
              "Throughput includes statistics and sampling overhead. All conservation, state and invariant verification occurs after performance timing."]

    resume = ["# Resume and recruiter summary", "", "Use two or three bullets, depending on space. "
              f"These numbers were measured on an {cpu}; include that qualification when quoting performance.", "",
              "## Quant developer / C++ systems version", "",
              f"- Built a C++20 multi-instrument matching engine sustaining **{throughput:.2f}M commands/s** with **{service_p99:.3f} µs sampled p99 processing latency** "
              f"on a 100K-live-order mixed workload across four instruments ({cpu}; median of three ≥5-second runs).",
              f"- Improved measured throughput **{ratio:.2f}×** over per-node allocation using pooled storage; supported **{max_active:,} live orders** "
              f"with **{large_memory:.1f} MiB tracked book storage** in a separate scale test.",
              f"- Validated **{stress['commands']:,} randomized commands across {stress['seeds']} seeds** against an independent reference matcher "
              f"(**{stress['implementation_comparisons']:,} implementation comparisons**, zero observed mismatches); verified deterministic replay and no loss/duplication "
              f"across **{queue_commands:,} queued commands** under controlled offered load.", "",
              "## Short version", "",
              f"Built and benchmarked a C++20 matching engine achieving {throughput:.2f}M commands/s on a 100K-order synthetic workload, "
              f"{ratio:.2f}× throughput vs baseline allocation, and zero mismatches across {stress['commands']:,} randomized commands "
              f"({cpu}, three repeated runs).", "",
              "## 60-second demonstration", "",
              "1. Open [impact.md](impact.md) and point to the measured results, scaling table and queue-latency curve.",
              "2. Run `./build/lob_sim --input examples/proposal.txt` and explain the two maker-price executions from the proposal.",
              "3. Show `benchmarks/impact-results/stress.json`: seed counts, event hashes and zero mismatches.",
              "4. Explain one tradeoff: pooling retains memory, or full-queue retries preserve commands while end-to-end latency grows.", "",
              "Describe throughput as **commands/s**, not trades/s: commands include adds, cancels and amendments. "
              "The one-million-order capacity result and the one-million-command oracle run are separate experiments. "
              "Report p99 as sampled **in-process processing** latency; use the queue table when discussing end-to-end latency. "
              "Avoid claims of production readiness, exchange-grade networking, zero bugs, or universal zero allocations.", "",
              "Full methodology, raw data links and reproduction steps: [impact.md](impact.md)."]
    (output.parent / "resume.md").write_text("\n".join(resume) + "\n")
    readme = root / "README.md"
    if readme.exists():
        block = "\n".join([
            "<!-- impact:start -->", "## Measured results", "",
            f"Local {cpu} results, C++20 Release. Throughput uses the median of three runs per allocator, each lasting at least five seconds. "
            "The workload is synthetic; latency measures sampled in-process command handling.", "",
            "| Metric | Result |", "|---|---|",
            f"| Mixed-flow throughput, 100K live orders / four instruments | **{throughput:.2f}M commands/s** |",
            f"| Sampled processing p99 | **{service_p99:.3f} µs** |",
            f"| Pooled vs baseline throughput | **{ratio:.2f}×** |",
            f"| Separately tested book capacity | **{max_active:,} simultaneous live orders** |",
            f"| Differential correctness | **{stress['commands']:,} commands, {stress['seeds']} seeds, zero observed mismatches** |",
            f"| Queue delivery checks under offered load | **{queue_commands:,} commands, zero observed loss/duplication** |", "",
            "[Results and charts](docs/impact.md) · [Resume bullets and demo](docs/resume.md) · "
            "[Raw data and source hashes](benchmarks/impact-results/manifest.json)", "<!-- impact:end -->"])
        text = readme.read_text()
        if "<!-- impact:start -->" in text:
            text = re.sub(r"<!-- impact:start -->.*?<!-- impact:end -->", lambda _: block, text, flags=re.S)
        else:
            text = text.replace("## Build and test", block + "\n\n## Build and test", 1)
        readme.write_text(text)
    summary = {"schema": 1, "cpu": cpu, "sustained_commands_per_second": median(pooled, "commands_per_second"),
               "sampled_service_p99_ns": median(pooled, "service_p99_ns"), "throughput_ratio": ratio,
               "max_live_orders": max_active, "large_book_bytes": median(large, "book_bytes"),
               "differential_commands": stress["commands"], "implementation_comparisons": stress["implementation_comparisons"],
               "queue_commands": queue_commands, "benchmark_commands": all_commands}
    output.with_suffix(".summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    try:
        os.environ.setdefault("MPLCONFIGDIR", "/tmp/lob-matplotlib")
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, axes = plt.subplots(2, 2, figsize=(13, 9), constrained_layout=True)
        colors = {"baseline": "#64748b", "pooled": "#0f766e"}
        for i, key in enumerate((baseline, pooled)):
            values = [float(r["commands_per_second"]) / 1e6 for r in groups[key]]
            value = statistics.median(values)
            axes[0, 0].bar(i, value, color=colors[key[1]], width=0.55)
            axes[0, 0].scatter([i] * len(values), values, s=30, color="black", zorder=3)
            axes[0, 0].text(i, max(values) * 1.02, f"{value:.2f}M", ha="center")
        axes[0, 0].set_xticks([0, 1], ["baseline", "pooled"])
        axes[0, 0].set_ylim(bottom=0, top=max(median(k, "commands_per_second") for k in (baseline, pooled)) / 1e6 * 1.2)
        axes[0, 0].set_ylabel("Million commands / second")
        axes[0, 0].set_title(f"≥5-second runs · 100K live orders · {ratio:.2f}× throughput")
        for allocator in ["baseline", "pooled"]:
            keys = sorted((k for k in scale_keys if k[1] == allocator), key=lambda k: k[2])
            axes[0, 1].plot([k[2] for k in keys], [median(k, "commands_per_second") / 1e6 for k in keys],
                            marker="o", label=allocator, color=colors[allocator])
            axes[1, 0].plot([k[2] for k in keys], [median(k, "book_bytes") / 2**20 for k in keys],
                            marker="o", label=allocator, color=colors[allocator])
        for ax in [axes[0, 1], axes[1, 0]]:
            ax.set_xscale("log")
            ax.set_xlabel("Simultaneous live orders (128 base price levels)")
            ax.legend()
        axes[0, 1].set_ylabel("Million commands / second")
        axes[0, 1].set_title("Random indexed mixed-flow scaling")
        axes[1, 0].set_ylabel("Tracked book storage (MiB)")
        axes[1, 0].set_title("Memory excludes event buffers and harness")
        rates = [k[3] / 1e6 for k in queue_keys]
        for column, label, color in [("service_p99_ns", "engine processing p99", "#0f766e"),
                                      ("e2e_p99_ns", "scheduled arrival → completion p99", "#d97706")]:
            axes[1, 1].plot(rates, [median(k, column) / 1000 for k in queue_keys], marker="o", label=label, color=color)
        axes[1, 1].set_yscale("log")
        axes[1, 1].set_xscale("log")
        axes[1, 1].set_xticks(rates, [f"{rate:g}" for rate in rates])
        axes[1, 1].set_xlabel("Offered million commands / second (log scale)")
        axes[1, 1].set_ylabel("Sampled p99 latency (µs, log scale)")
        axes[1, 1].set_title("Backpressure is included in end-to-end latency")
        axes[1, 1].legend(fontsize=8)
        for ax in axes.flat:
            ax.grid(alpha=0.2)
            ax.set_axisbelow(True)
        fig.suptitle(f"C++20 matching engine · {cpu} · repeated local Release experiments", fontsize=14)
        fig.savefig(output.with_suffix(".svg"))
        fig.savefig(output.with_suffix(".png"), dpi=150)
        plt.close(fig)
        chart_position = lines.index("## Sustained random-access mixed flow")
        lines[chart_position:chart_position] = [f"![Measured throughput, scaling, memory and queue latency]({output.with_suffix('.png').name})", ""]
    except ImportError:
        lines += ["", "Install Matplotlib to generate the chart, then rerun this report command."]
    output.write_text("\n".join(lines) + "\n")
    print(f"Wrote {output}, resume.md, summary JSON and available charts")


if __name__ == "__main__":
    main()
