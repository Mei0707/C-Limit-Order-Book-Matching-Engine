#!/usr/bin/env python3
"""Create a measured Markdown report and charts from lob_bench CSV files."""
import argparse
import csv
import json
import os
import platform
from pathlib import Path
import statistics
import subprocess


def command(args):
    try:
        return subprocess.check_output(args, text=True, stderr=subprocess.DEVNULL, timeout=10).strip()
    except (OSError, subprocess.SubprocessError):
        return "unavailable"


def environment(build):
    result = {"os": platform.platform(), "architecture": platform.machine(), "logical_cpus": os.cpu_count(),
              "compiler": command(["c++", "--version"]).splitlines()[0], "build_type": "Release"}
    cache = Path(build) / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text().splitlines():
            if line.startswith(("CMAKE_CXX_FLAGS_RELEASE:", "CMAKE_CXX_COMPILER:", "CMAKE_BUILD_TYPE:")):
                key, value = line.split("=", 1)
                result[key.split(":")[0]] = value
    if platform.system() == "Darwin":
        try:
            hardware = json.loads(command(["/usr/sbin/system_profiler", "SPHardwareDataType", "-json"]))
            allowed = {"chip_type", "number_processors", "physical_memory", "machine_model", "machine_name"}
            result["hardware"] = {k: v for k, v in hardware["SPHardwareDataType"][0].items() if k in allowed}
        except (ValueError, KeyError, IndexError):
            result["hardware"] = "unavailable"
    elif Path("/proc/cpuinfo").exists():
        result["cpu"] = next((s.split(":", 1)[1].strip() for s in Path("/proc/cpuinfo").read_text().splitlines()
                              if s.startswith("model name")), platform.processor())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", nargs="+")
    parser.add_argument("--output", default="docs/performance.md")
    parser.add_argument("--build", default="build")
    args = parser.parse_args()
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    groups = {}
    for path in args.csv:
        with open(path, newline="") as stream:
            for row in csv.DictReader(stream):
                key = (row["profile"], row["mode"], int(row["active_initial"]), row["allocator"])
                groups.setdefault(key, []).append(row)
    # This comparison also catches accidental divergence between allocation modes.
    checksums = {}
    for key, rows in groups.items():
        for row in rows:
            identity = (row["profile"], row["mode"], row["active_initial"], row["seed"], row["commands"])
            if identity in checksums and checksums[identity] != row["checksum"]:
                raise ValueError("Nondeterministic final state: " + str(identity))
            checksums[identity] = row["checksum"]
    def median(key, column):
        return statistics.median(float(r[column]) for r in groups[key])

    env = environment(args.build)
    metadata = output.with_suffix(".environment.json")
    metadata.write_text(json.dumps(env, indent=2) + "\n")
    lines = ["# Measured performance", "", "These are local measurements, not the proposal's engineering targets.", "",
             "## Environment", "", "```json", json.dumps(env, indent=2), "```", "",
             "## Results", "", "Throughput and percentiles are medians of the recorded repetitions. Throughput range includes every run; maximum latency is the largest sample across all repetitions.", "",
             "| Profile | Mode | Initial orders | Allocator | Runs | M commands/s (range) | p50 µs | p95 µs | p99 µs | Max µs | System allocations/command | Peak book MiB |",
             "|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for key, rows in sorted(groups.items()):
        throughput = [float(r["commands_per_second"]) / 1e6 for r in rows]
        lat = [median(key, name) / 1000 for name in ["p50_ns", "p95_ns", "p99_ns"]]
        lat.append(max(float(r["max_ns"]) for r in rows) / 1000)
        lines.append(f"| {key[0]} | {key[1]} | {key[2]:,} | {key[3]} | {len(rows)} | "
                     f"{statistics.median(throughput):.3f} ({min(throughput):.3f}–{max(throughput):.3f}) | "
                     + " | ".join(f"{v:.3f}" for v in lat) +
                     f" | {median(key, 'upstream_allocations_per_command'):.5f} | {median(key, 'peak_book_bytes') / 2**20:.2f} |")
    lines += ["", "## Allocation comparison", "",
              "Ratios compare medians on identical generated inputs. Values below 1.0 indicate a regression in throughput.", "",
              "| Profile | Mode | Initial orders | Pooled / baseline throughput | Baseline / pooled system allocations per command |",
              "|---|---|---:|---:|---:|"]
    for key in sorted(groups):
        if key[3] != "baseline":
            continue
        pooled = (*key[:3], "pooled")
        if pooled not in groups:
            continue
        ratio = median(pooled, "commands_per_second") / median(key, "commands_per_second")
        lines.append(f"| {key[0]} | {key[1]} | {key[2]:,} | {ratio:.3f}× | "
                     f"{median(key, 'upstream_allocations_per_command'):.5f} / {median(pooled, 'upstream_allocations_per_command'):.5f} |")
    lines += ["", "### Observations", ""]
    representative = ("mixed", "direct", 10000, "baseline")
    pooled = (*representative[:3], "pooled")
    if representative in groups and pooled in groups:
        speedup = median(pooled, "commands_per_second") / median(representative, "commands_per_second")
        lines.append(f"- For mixed flow with 10,000 initial orders, pooling changed median throughput by {speedup:.3f}× "
                     f"and p99 from {median(representative, 'p99_ns') / 1000:.3f} to {median(pooled, 'p99_ns') / 1000:.3f} µs. "
                     f"Peak tracked book memory changed from {median(representative, 'peak_book_bytes') / 2**20:.2f} "
                     f"to {median(pooled, 'peak_book_bytes') / 2**20:.2f} MiB.")
    for key in sorted(groups):
        pooled = (*key[:3], "pooled")
        if key[3] == "baseline" and pooled in groups and median(pooled, "p99_ns") > 1.05 * median(key, "p99_ns"):
            lines.append(f"- Tail regression: {key[0]} / {key[1]} / {key[2]:,} initial orders had median p99 "
                         f"{median(key, 'p99_ns') / 1000:.3f} µs with baseline allocation and "
                         f"{median(pooled, 'p99_ns') / 1000:.3f} µs with pooling. Chunk refill and scheduling "
                         "are possible contributors; these samples do not isolate their individual effects.")
    rejects = sum(int(r["rejected_commands"]) for rows in groups.values() for r in rows)
    drops = sum(int(r["dropped_commands"]) for rows in groups.values() for r in rows)
    queue_max = max(int(r["queue_max_occupancy"]) for rows in groups.values() for r in rows)
    retries = sum(int(r["queue_full_retries"]) for rows in groups.values() for r in rows)
    lines += ["", "## Method and limits", "",
              "- Release C++20, steady clock, 2,000 add/cancel warm-up pairs, then initial book population. Input generation and population are outside measurement.",
              "- Every command is timed; percentile uses nearest rank. Throughput includes clock calls, event counting and latency collection. Percentiles measure engine processing, including event generation, excluding queue wait.",
              "- Queued throughput includes producer, backpressure retries, worker startup and queue draining. Queue capacity is 4,096. No commands are intentionally dropped.",
              "- Baseline and pooled modes use the same map/list/hash design and pre-reserve the index for the full input size. Repetitions alternate allocator order. This isolates node allocation effects; it is not a comparison against a different matching algorithm.",
              "- PMR uses the standard unsynchronized pool where available. Older Apple libc++ selects the portable 128-node chunk pool. Pooling reduces calls to the system allocator but retains freed chunks and may regress latency or memory usage.",
              "- Allocation counts cover book maps, FIFO lists, and the order index. Event-vector growth, engine metadata, pool metadata, generated inputs and measurement buffers are excluded. Peak book bytes include warm-up and index reservation.",
              "- CSV also records process peak RSS in bytes. It is a process-lifetime high-water mark, so later rows can inherit earlier peaks; use book-resource bytes for per-engine comparisons.",
              "- Queue maximum occupancy is sampled by the producer and may miss a short-lived peak. Full retries count retry attempts, not unique blocked commands.",
              "- No CPU pinning, frequency control or hardware isolation was used. OS scheduling and background work affect tails; these results do not establish hard real-time guarantees.",
              "- Workloads are synthetic. Cancellation and market profiles pair insertion with removal; mixed flow uses an eight-command lifecycle cycle. These are controlled allocation/matching experiments rather than a calibrated exchange feed. Individual 100,000-command runs are short; throughput is not a long-duration soak-test result. Very short latencies are affected by clock quantization and timer overhead.",
              f"- Across these runs: {rejects:,} rejected commands, {drops:,} dropped commands, {retries:,} queue-full retries; largest observed queue occupancy {queue_max:,}.",
              "- All repetitions and both allocation modes produced the same final checksum for each workload configuration.",
              "", "## Reproduction", "", "```sh", "cmake -S . -B build -DCMAKE_BUILD_TYPE=Release",
              "cmake --build build -j 4",
              "./build/lob_bench --commands 100000 --repeats 3 --active-orders 10000 --output benchmarks/results/main.csv",
              "./build/lob_bench --commands 100000 --repeats 3 --active-orders 1000 --profile mixed --output benchmarks/results/mixed-1000.csv",
              "./build/lob_bench --commands 100000 --repeats 3 --active-orders 50000 --profile mixed --output benchmarks/results/mixed-50000.csv",
              "python3 scripts/benchmark_report.py benchmarks/results/*.csv --output docs/performance.md", "```", "",
              "Python chart generation requires Matplotlib (`python3 -m pip install matplotlib`). The C++ project has no Python runtime dependency."]
    try:
        os.environ.setdefault("MPLCONFIGDIR", "/tmp/lob-matplotlib")
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        configurations = sorted({key[:3] for key in groups})
        fig, axes = plt.subplots(2, 1, figsize=(13, 9), constrained_layout=True)
        x = list(range(len(configurations)))
        for allocator, offset, color in [("baseline", -0.19, "#64748b"), ("pooled", 0.19, "#0f766e")]:
            throughput = [median((*k, allocator), "commands_per_second") / 1e6 if (*k, allocator) in groups else 0 for k in configurations]
            p99 = [median((*k, allocator), "p99_ns") / 1000 if (*k, allocator) in groups else 0 for k in configurations]
            axes[0].bar([v + offset for v in x], throughput, width=0.38, label=allocator, color=color)
            axes[1].bar([v + offset for v in x], p99, width=0.38, label=allocator, color=color)
        labels = [f"{p}\n{mode}\n{active:,} initial" for p, mode, active in configurations]
        for ax in axes:
            ax.set_xticks(x, labels, fontsize=8)
            ax.grid(axis="y", alpha=0.2)
            ax.set_axisbelow(True)
            ax.legend()
        axes[0].set_ylabel("Million commands / second")
        axes[0].set_title("Median across repeated local Release runs")
        axes[1].set_ylabel("p99 engine processing latency (µs)")
        chart = output.with_suffix(".svg")
        fig.savefig(chart)
        plt.close(fig)
        lines += ["", f"![Measured throughput and p99 latency]({chart.name})"]
    except ImportError:
        lines += ["", "Chart omitted: install Matplotlib and rerun this script to generate it."]
    output.write_text("\n".join(lines) + "\n")
    print(f"Wrote {output} and {metadata}")


if __name__ == "__main__":
    main()
