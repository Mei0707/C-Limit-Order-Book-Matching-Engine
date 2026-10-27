#!/usr/bin/env python3
"""Run the quantitative evidence suite sequentially, with a reproducible manifest."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
from benchmark_report import environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--output", default="benchmarks/impact-results")
    parser.add_argument("--quick", action="store_true", help="Small smoke run; not the resume evidence suite")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = (root / args.build).resolve()
    output = (root / args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    repeats, seconds, count = (1, 1, 8000) if args.quick else (3, 5, 200000)
    manifest = {"schema": 1, "quick": args.quick, "started_utc": datetime.now(timezone.utc).isoformat(),
                "environment": environment(build), "commands": [], "status": "running"}
    sources = sorted([root / "CMakeLists.txt", *root.glob("include/**/*.hpp"), *root.glob("src/*.cpp"),
                      *root.glob("benchmarks/*.cpp"), *root.glob("benchmarks/*.hpp"),
                      *root.glob("tests/*.cpp"), *root.glob("tests/*.hpp")])
    manifest["source_sha256"] = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    manifest_path = output / "manifest.json"

    def run(executable, arguments):
        command = [str(build / executable), *map(str, arguments)]
        manifest["commands"].append(command)
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
        print("Running:", " ".join(command), flush=True)
        subprocess.run(command, cwd=root, check=True)

    try:
        run("lob_stress", ["--commands", 10000 if args.quick else 1000000,
                           "--seeds", 2 if args.quick else 20, "--output", output / "stress.json"])
        run("lob_impact", ["--experiment", "sustain", "--seconds", seconds, "--repeats", repeats,
                           "--commands", count, "--active-orders", 1000 if args.quick else 100000,
                           "--output", output / "sustain.csv"])
        for active in ([1000, 10000] if args.quick else [1000, 10000, 100000, 1000000]):
            run("lob_impact", ["--experiment", "scale", "--commands", count, "--repeats", repeats,
                               "--active-orders", active, "--output", output / f"scale-{active}.csv"])
        for rate in ([100000, 1000000] if args.quick else [100000, 500000, 1000000, 2000000, 4000000, 16000000]):
            run("lob_impact", ["--experiment", "queue", "--seconds", 1 if args.quick else 2,
                               "--commands", count, "--repeats", repeats, "--rate", rate,
                               "--active-orders", 1000 if args.quick else 100000,
                               "--output", output / f"queue-{rate}.csv"])
        manifest["status"] = "passed"
    except (OSError, subprocess.CalledProcessError):
        manifest["status"] = "failed"
        raise
    finally:
        manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Evidence complete: {output}", flush=True)


if __name__ == "__main__":
    main()
