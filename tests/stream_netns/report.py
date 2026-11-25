#!/usr/bin/env python3
"""Сводка завершённого сетевого прогона; CPU по фазам, сетевые счётчики и задержка пачек."""

import argparse
import csv
import json
from pathlib import Path
import sys


def cpu_by_phase(samples):
    totals = {}
    previous = None
    for sample in samples:
        phase = sample["phase"]
        # Интервал на границе фаз нельзя целиком приписать ни одной из них.
        if previous and previous["phase"] == phase:
            phase_totals = totals.setdefault(phase, {})
            for name, current in sample["nodes"].items():
                old = previous["nodes"].get(name)
                if old is None:
                    continue
                elapsed = current["sample_ns"] - old["sample_ns"]
                cpu = current["cpu_ns"] - old["cpu_ns"]
                if elapsed <= 0 or cpu < 0:
                    raise ValueError(f"{phase}/{name}: non-monotonic CPU/time counter")
                accumulated = phase_totals.setdefault(name, [0, 0])
                accumulated[0] += cpu
                accumulated[1] += elapsed
        previous = sample
    return {phase: {name: {"core_percent": round(100 * cpu / elapsed, 3),
                          "sampled_seconds": round(elapsed / 1e9, 3)}
                    for name, (cpu, elapsed) in nodes.items()} for phase, nodes in totals.items()}


def interface_bytes(snapshot, name):
    # IFB повторяет ingress; учёт только eth0 каждого прикладного узла исключает двойной счёт.
    eth = next(link for link in snapshot[name]["links"] if link["ifname"] == "eth0")
    return {direction: eth["stats64"][direction]["bytes"] for direction in ("tx", "rx")}


def report(directory):
    def read(name):
        return json.loads((directory / name).read_text())

    result = read("result.json")
    if not result.get("pass") or not result.get("host_unchanged"):
        raise ValueError(f"{directory}: run failed or host isolation was not confirmed")
    with (directory / "summary.csv").open(newline="") as source:
        rows = list(csv.DictReader(source))
    with (directory / "samples.jsonl").open() as source:
        cpu = cpu_by_phase(json.loads(line) for line in source)
    initial, final = read("network-initial.json"), read("network-final.json")
    traffic = {}
    for row in rows:
        name = row["node"]
        begin, end = interface_bytes(initial, name), interface_bytes(final, name)
        traffic[name] = {f"{direction}_bytes": end[direction] - begin[direction] for direction in begin}
        if any(value < 0 for value in traffic[name].values()):
            raise ValueError(f"{name}: network counter reset")
    viewers = [row for row in rows if row["node"] != result["publisher"]]
    return {"directory": str(directory), "n": result["n"], "r": result["r"], "rate_bps": result["rate_bps"],
            "publisher": result["publisher"], "generated": result["generated"], "second_stream": result.get("second_stream"),
            "cpu_by_phase": cpu,
            "traffic_whole_run": traffic,
            "max_viewer_quantile_ms": {key: max(int(row[key]) for row in viewers)
                                       for key in ("p50_ms", "p95_ms", "p99_ms", "maximum_ms")},
            # Для сохранённых первых прогонов до расширения гистограммы предел был4095мс.
            "quantile_lower_bounds": {key: [row["node"] for row in viewers
                                            if int(row[key]) == int(row.get("quantile_limit_ms", 4095))]
                                      for key in ("p50_ms", "p95_ms", "p99_ms")},
            "gap_batches": sum(int(row["gap_batches"]) for row in viewers),
            "errors": sum(int(row["errors"]) for row in rows),
            "migrations": sum(int(row["migrations"]) for row in viewers),
            "probes": sum(int(row["probes"]) for row in viewers)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", type=Path, nargs="+")
    args = parser.parse_args()
    try:
        reports = [report(directory) for directory in args.directories]
    except (OSError, ValueError, KeyError, StopIteration) as error:
        print(f"Report failed (incomplete or incompatible artifacts): {error}", file=sys.stderr)
        return 1
    print(json.dumps(reports, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
