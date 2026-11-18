#!/usr/bin/env python3
# [The following text was written autonomously by an AI agent (Claude Code).]
"""Compare library scrolling of Mixxx builds.

Launches each build several times with a fresh copy of a settings profile and
MIXXX_LIBRARY_SCROLL_BENCHMARK set (see src/library/libraryscrollbenchmark.h).
Each launch scrolls the track table in a few scenarios and quits. Prints the
mean of each metric per build and the difference to the first build.

Metrics, per scenario:
  sampled_cells                  Visible cells with an image, counted just
                                 before each scroll step, summed.
  blank_overviews, blank_covers  Visible cells that still show a placeholder
                                 just before each scroll step, summed.
  settle_ms                      Time from the last step until no visible
                                 placeholders remain, -1 after 10 s.
  cpu_s                          Process CPU time (user + system) from the
                                 first step until 3 s after settling.
  memory_mb                      Memory 3 s after settling: footprint on
                                 macOS, resident set size on Linux.
  max_memory_mb                  Highest memory at the scroll steps and
                                 while settling.

The profile should have a library of at least 700 tracks that are analyzed
and have covers, with the track table showing the cover art and overview
columns. Create it by running Mixxx once with --settings-path DIR.

The baseline needs the benchmark too: cherry-pick the commit that adds
libraryscrollbenchmark.cpp onto main and build it.

Example:
  tools/library_scroll_benchmark.py --profile ~/bench-profile --runs 3 \\
      main=../mixxx-main/build/mixxx pr=build/mixxx
"""

import argparse
import os
import shutil
import statistics
import subprocess
import sys
import tempfile

METRICS = [
    "sampled_cells",
    "blank_overviews",
    "blank_covers",
    "settle_ms",
    "cpu_s",
    "memory_mb",
    "max_memory_mb",
]


def run_once(binary, profile, scenarios, timeout):
    with tempfile.TemporaryDirectory(prefix="mixxx-bench-") as tmp:
        settings = os.path.join(tmp, "profile")
        shutil.copytree(
            profile, settings, ignore=shutil.ignore_patterns("mixxx.log*")
        )
        output = os.path.join(tmp, "result.txt")
        env = dict(os.environ)
        env["MIXXX_LIBRARY_SCROLL_BENCHMARK"] = scenarios
        env["MIXXX_LIBRARY_SCROLL_BENCHMARK_OUTPUT"] = output
        with open(os.path.join(tmp, "stdout.txt"), "w") as log:
            process = subprocess.Popen(
                [binary, "--settings-path", settings + os.sep],
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            try:
                process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                print(f"  {binary}: timed out", file=sys.stderr)
        results = {}
        if os.path.exists(output):
            with open(output) as file:
                for line in file:
                    print("  " + line.strip(), file=sys.stderr)
                    fields = dict(
                        item.split("=", 1) for item in line.split() if "=" in item
                    )
                    results[fields["scenario"]] = {
                        metric: float(fields[metric]) for metric in METRICS
                    }
        return results


def mean(values):
    values = [value for value in values if value >= 0]
    return statistics.mean(values) if values else float("nan")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "builds", nargs="+", metavar="LABEL=BINARY", help="Mixxx binaries to compare"
    )
    parser.add_argument(
        "--profile", required=True, help="settings directory, copied for each run"
    )
    parser.add_argument("--runs", type=int, default=3, help="launches per build")
    parser.add_argument(
        "--scenarios",
        default="default",
        help="name:steps:rows:interval_ms,... (default: %(default)s)",
    )
    parser.add_argument(
        "--timeout", type=int, default=300, help="seconds per launch"
    )
    args = parser.parse_args()

    builds = []
    for build in args.builds:
        label, _, binary = build.rpartition("=")
        builds.append((label or binary, os.path.abspath(binary)))

    # label -> scenario -> metric -> values
    results = {label: {} for label, _ in builds}
    scenarios = []
    for run in range(args.runs):
        # Alternate the order, so neither build always runs first
        order = builds if run % 2 == 0 else builds[::-1]
        for label, binary in order:
            print(f"run {run + 1}/{args.runs}: {label}", file=sys.stderr)
            for scenario, metrics in run_once(
                binary, args.profile, args.scenarios, args.timeout
            ).items():
                if scenario not in scenarios:
                    scenarios.append(scenario)
                for metric, value in metrics.items():
                    results[label].setdefault(scenario, {}).setdefault(
                        metric, []
                    ).append(value)

    base = builds[0][0]
    print(f"\nMean of {args.runs} runs, difference to {base}\n")
    for scenario in scenarios:
        print(f"## {scenario}")
        print(f"| metric | " + " | ".join(label for label, _ in builds) + " |")
        print("|---" * (len(builds) + 1) + "|")
        for metric in METRICS:
            row = [metric]
            base_mean = mean(results[base].get(scenario, {}).get(metric, []))
            for label, _ in builds:
                values = results[label].get(scenario, {}).get(metric, [])
                value = mean(values)
                cell = f"{value:.1f}" if values else "n/a"
                if label != base and values:
                    cell += f" ({value - base_mean:+.1f})"
                row.append(cell)
            print("| " + " | ".join(row) + " |")
        print()


if __name__ == "__main__":
    main()

# [End of the text written autonomously by an AI agent (Claude Code).]
