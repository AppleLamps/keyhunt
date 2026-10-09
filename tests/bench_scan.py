#!/usr/bin/env python3
"""Compare Linux/WSL binaries on sequential compressed single-target scanning.

Example: python3 tests/bench_scan.py before=/tmp/before/keyhunt.bin after=./keyhunt
Runs each binary with field none/avx2 and 6/12 threads, in alternating order.
Uses a synthetic absent target in the puzzle-71 range. No wallet data is needed.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import random
import re
import statistics
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binaries", nargs="+", help="label=path to a Linux executable")
    parser.add_argument("--seconds", type=int, default=20, help="seconds per run, including warmup")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--output", type=Path, default=Path("build/bench-scan.json"))
    args = parser.parse_args()
    if args.seconds < 8 or args.repeats < 1:
        parser.error("use at least 8 seconds and one repeat")
    binaries = {}
    for spec in args.binaries:
        label, sep, path = spec.partition("=")
        if not sep or not label or label in binaries:
            parser.error("binaries must have unique label=path arguments")
        binary = Path(path).resolve()
        if not binary.is_file():
            parser.error(f"missing binary: {binary}")
        binaries[label] = str(binary)

    report = {
        "platform": platform.platform(),
        "cpu": subprocess.check_output(["lscpu"], text=True),
        "seconds": args.seconds,
        "repeats": args.repeats,
        "hash_simd": "avx2",
        "measurement": "counter delta between first report at >=3s and last report",
        "runs": [],
    }
    jobs = [(label, field, threads) for field in ("none", "avx2")
            for threads in (6, 12) for label in binaries]
    # Keep before/after adjacent; reverse each repetition to reduce order bias.
    pairs = [jobs[i:i + len(binaries)] for i in range(0, len(jobs), len(binaries))]
    random.Random(71).shuffle(pairs)
    jobs = [job for pair in pairs for job in pair]
    with tempfile.TemporaryDirectory(prefix="keyhunt-bench-") as work:
        target = Path(work) / "target.rmd"
        target.write_text("00" * 20 + "\n")
        for repeat in range(args.repeats):
            for label, field, threads in jobs if repeat % 2 == 0 else reversed(jobs):
                command = [binaries[label], "-m", "rmd160", "-f", str(target),
                           "-r", "400000000000000000:7fffffffffffffffff",
                           "-n", "0x100000", "-l", "compress", "-t", str(threads),
                           "-q", "-s", "1", "-L"]
                env = dict(os.environ, KEYHUNT_SIMD="avx2", KEYHUNT_FIELD_SIMD=field)
                with subprocess.Popen(command, cwd=work, env=env, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT) as proc:
                    try:
                        output, _ = proc.communicate(timeout=args.seconds)
                        raise RuntimeError(f"scanner exited early ({proc.returncode}): {output.decode(errors='replace')}")
                    except subprocess.TimeoutExpired:
                        proc.terminate()
                        try:
                            output, _ = proc.communicate(timeout=5)
                        except subprocess.TimeoutExpired:
                            proc.kill()
                            output, _ = proc.communicate()
                samples = [(int(keys), int(seconds)) for keys, seconds in
                           re.findall(rb"Total (\d+) keys in (\d+) seconds", output)
                           if int(seconds) >= 3]
                if len(samples) < 2 or samples[-1][1] <= samples[0][1]:
                    raise RuntimeError(f"insufficient stats: {output.decode(errors='replace')}")
                keys = samples[-1][0] - samples[0][0]
                seconds = samples[-1][1] - samples[0][1]
                rate = keys / seconds / 1e6
                report["runs"].append(dict(binary=label, field=field, threads=threads,
                                           repeat=repeat + 1, mkeys_s=rate, samples=samples,
                                           command=command, output=output.decode(errors="replace")))
                print(f"{label:12} field={field:4} threads={threads:2} repeat={repeat + 1}: {rate:.3f} Mkeys/s", flush=True)
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(json.dumps(report, indent=2) + "\n")
    print("\nMedian Mkeys/s (min..max):")
    for label, field, threads in sorted(jobs):
        rates = [run["mkeys_s"] for run in report["runs"]
                 if (run["binary"], run["field"], run["threads"]) == (label, field, threads)]
        print(f"{label:12} field={field:4} threads={threads:2}: "
              f"{statistics.median(rates):.3f} ({min(rates):.3f}..{max(rates):.3f})")
    print(f"Raw results: {args.output}")


if __name__ == "__main__":
    main()
