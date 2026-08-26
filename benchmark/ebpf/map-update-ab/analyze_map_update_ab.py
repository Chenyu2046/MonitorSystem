#!/usr/bin/env python3
"""Convert map-update-ab raw snapshots into defensible CSV evidence."""
from __future__ import annotations

import csv
import json
import math
import statistics
import sys
from pathlib import Path


def load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def find_program(obj, name: str):
    if not isinstance(obj, list):
        return None
    candidates = [p for p in obj if isinstance(p, dict) and p.get("name") == name]
    return candidates[-1] if candidates else None


def server_metrics(obj):
    if not isinstance(obj, dict):
        return None
    end = obj.get("end", {})
    summary = end.get("sum_received") or end.get("sum") or {}
    if not summary:
        return None
    bps = float(summary.get("bits_per_second", 0.0))
    pps = summary.get("packets_per_second")
    if pps is None:
        seconds = float(summary.get("seconds", 0.0))
        packets = float(summary.get("packets", 0.0))
        pps = packets / seconds if seconds > 0 else 0.0
    loss = summary.get("lost_percent", summary.get("lost_percent_total", 0.0))
    return float(pps), bps / 1_000_000.0, float(loss)


def cpu_metrics(text: str):
    avg = None
    peaks = []
    for line in text.splitlines():
        cols = line.split()
        if len(cols) < 8 or cols[0] == "Linux":
            continue
        if cols[0] == "Average:" and len(cols) >= 10 and cols[1] == "all":
            try:
                avg = 100.0 - float(cols[-1])
            except ValueError:
                pass
        else:
            cpu_column = 2 if len(cols) >= 3 and cols[1] in ("AM", "PM") else 1
            if len(cols) <= cpu_column or cols[cpu_column] != "all":
                continue
            try:
                peaks.append(100.0 - float(cols[-1]))
            except ValueError:
                pass
    return avg, max(peaks) if peaks else avg


def percentile(values, p):
    values = sorted(values)
    if not values:
        return math.nan
    return values[min(len(values) - 1, int(p * (len(values) - 1)))]


def main(root: Path):
    metadata = root / "rounds.jsonl"
    rows = []
    for line in metadata.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        if not rec.get("valid"):
            continue
        server = load_json(root / "raw" / rec["server_file"])
        before = load_json(root / "raw" / rec["bpf_before"])
        after = load_json(root / "raw" / rec["bpf_after"])
        metrics = server_metrics(server)
        name = "global_update_ingress" if rec["variant"] == "global" else "percpu_update_ingress"
        bp = find_program(before, name)
        ap = find_program(after, name)
        if not metrics or not bp or not ap:
            continue
        count_delta = int(ap.get("run_cnt", 0)) - int(bp.get("run_cnt", 0))
        time_delta = int(ap.get("run_time_ns", 0)) - int(bp.get("run_time_ns", 0))
        if count_delta <= 0 or time_delta < 0:
            continue
        cpu_text = (root / "raw" / rec["mpstat_file"]).read_text(encoding="utf-8", errors="replace")
        cpu_avg, cpu_peak = cpu_metrics(cpu_text)
        pps, mbps, loss = metrics
        rows.append({
            "variant": rec["variant"], "payload": rec["payload"], "streams": rec["streams"],
            "cpu_limit": rec["cpu_limit"], "affinity": rec["affinity"], "round": rec["round"],
            "received_pps": pps, "received_mbps": mbps, "packet_loss_pct": loss,
            "guest_cpu_avg_pct": cpu_avg, "guest_cpu_peak_pct": cpu_peak,
            "bpf_run_cnt_delta": count_delta, "bpf_run_time_ns_delta": time_delta,
            "avg_bpf_cost_ns": time_delta / count_delta,
        })

    fields = list(rows[0]) if rows else ["variant", "payload", "streams", "cpu_limit", "affinity", "round", "received_pps", "received_mbps", "packet_loss_pct", "guest_cpu_avg_pct", "guest_cpu_peak_pct", "bpf_run_cnt_delta", "bpf_run_time_ns_delta", "avg_bpf_cost_ns"]
    for variant in ("global", "percpu"):
        with (root / f"{variant}.csv").open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fields)
            writer.writeheader()
            writer.writerows(r for r in rows if r["variant"] == variant)

    comparisons = []
    keys = sorted({(r["payload"], r["streams"], r["cpu_limit"]) for r in rows})
    for payload, streams, cpu in keys:
        g = [r for r in rows if r["variant"] == "global" and (r["payload"], r["streams"], r["cpu_limit"]) == (payload, streams, cpu)]
        p = [r for r in rows if r["variant"] == "percpu" and (r["payload"], r["streams"], r["cpu_limit"]) == (payload, streams, cpu)]
        if not g or not p:
            continue
        gm = {k: statistics.median(r[k] for r in g) for k in ("received_pps", "received_mbps", "guest_cpu_avg_pct", "avg_bpf_cost_ns")}
        pm = {k: statistics.median(r[k] for r in p) for k in ("received_pps", "received_mbps", "guest_cpu_avg_pct", "avg_bpf_cost_ns")}
        comparisons.append({"payload": payload, "streams": streams, "cpu_limit": cpu,
                           "global_avg_bpf_cost_ns": gm["avg_bpf_cost_ns"], "percpu_avg_bpf_cost_ns": pm["avg_bpf_cost_ns"],
                           "bpf_cost_reduction_pct": (gm["avg_bpf_cost_ns"] - pm["avg_bpf_cost_ns"]) / gm["avg_bpf_cost_ns"] * 100.0,
                           "global_received_pps": gm["received_pps"], "percpu_received_pps": pm["received_pps"],
                           "throughput_gain_pct": (pm["received_pps"] - gm["received_pps"]) / gm["received_pps"] * 100.0 if gm["received_pps"] else math.nan,
                           "global_cpu_avg_pct": gm["guest_cpu_avg_pct"], "percpu_cpu_avg_pct": pm["guest_cpu_avg_pct"],
                           "cpu_difference_pp": gm["guest_cpu_avg_pct"] - pm["guest_cpu_avg_pct"],
                           "global_rounds": len(g), "percpu_rounds": len(p)})
    comp_fields = list(comparisons[0]) if comparisons else ["payload", "streams", "cpu_limit", "global_avg_bpf_cost_ns", "percpu_avg_bpf_cost_ns", "bpf_cost_reduction_pct", "global_received_pps", "percpu_received_pps", "throughput_gain_pct", "global_cpu_avg_pct", "percpu_cpu_avg_pct", "cpu_difference_pp", "global_rounds", "percpu_rounds"]
    with (root / "comparison.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=comp_fields)
        writer.writeheader()
        writer.writerows(comparisons)

    print(f"valid_rows={len(rows)} comparisons={len(comparisons)}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: analyze_map_update_ab.py EVIDENCE_DIR")
    main(Path(sys.argv[1]))
