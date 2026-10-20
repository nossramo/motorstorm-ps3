#!/usr/bin/env python3
"""Summarize sampled 0x8380 call attribution, not a normal-play benchmark."""
import argparse
import collections
import json
import math
import re
import statistics
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("trace", type=Path)
p.add_argument("--out", type=Path, required=True)
a = p.parse_args()
groups = collections.defaultdict(lambda: collections.defaultdict(list))
flips = 0
header = ""
for line in a.trace.read_text().splitlines():
    if line.startswith("#"):
        header = line
        continue
    if line.startswith("T "):
        continue
    kind, who, x, y, start, end = line.split()
    kind = int(kind)
    if kind == 18:
        flips += 1
    if 1000 <= kind <= 1003:
        key = (int(who), start, end)
        groups[key][kind].append((int(x, 16), int(y, 16)))
assert groups and flips, "No culling samples or guest frames in trace"
assert re.search(r"\bdropped=0\b", header), "Dropped trace records invalidate this report"
assert all(set(g) == {1000, 1001, 1002, 1003} for g in groups.values()), "Incomplete sample"
# Several worker contexts share a guest SPU id. The host clock may also
# quantize to a microsecond, so distinct samples can share both timestamps.
# Keep their multiplicities; aggregate attribution needs no arbitrary pairing.
assert all(len({len(v) for v in g.values()}) == 1 for g in groups.values()), "Incomplete sample group"
assert all(sorted(x[1] for x in g[1000]) == sorted(x[1] for x in g[1001]) for g in groups.values())
flat = {kind: [item for g in groups.values() for item in g[kind]] for kind in range(1000, 1004)}
assert all(x <= y for x, y in flat[1001])
cpu = [x for x, _ in flat[1000]]
blocks = sum(y for _, y in flat[1001])
eligible = sum(x for x, _ in flat[1001])
ordered = sorted(cpu)
result = {
    "trace": str(a.trace),
    "header": header,
    "sample_stride_per_thread": 64,
    "sampled_complete_cull_calls": len(cpu),
    "sampled_8480_blocks": blocks,
    "sampled_eligible_blocks": eligible,
    "sampled_eligible_fraction": eligible / blocks if blocks else None,
    "sampled_rejected_input_lanes": {
        "negative_zero": sum(x for x, _ in flat[1002]),
        "subnormal": sum(y for _, y in flat[1002]),
        "small_normal": sum(x for x, _ in flat[1003]),
        "large_or_extended": sum(y for _, y in flat[1003]),
    },
    "sampled_call_inclusive_cpu_ns": {
        "mean": statistics.mean(cpu),
        "median": statistics.median(cpu),
        "p95": ordered[math.ceil(len(ordered) * .95) - 1],
        "max": max(cpu),
    },
    "estimated_inclusive_cull_cpu_ms_per_guest_frame": sum(cpu) * 64 / flips / 1e6,
    "guest_frames_in_window": flips,
    "limitations": [
        "Systematic one-in-64 sampling per thread, not an exact invocation census",
        "CPU includes 0x8380 setup, 0x8480 blocks, dispatch/interrupt work and diagnostic checks",
        "Diagnostic timer and rejection-classification overhead is included",
        "CPU estimate multiplies sampled time by 64; it is not wall-frame latency",
        "Nested, abandoned and trace-boundary-crossing samples are discarded",
        "Host wall timestamps can coincide; all record multiplicities are retained",
        "Eligibility applies to sampled blocks, not every gameplay input",
        "Diagnostic gameplay FPS must not be compared with normal-play FPS",
    ],
}
a.out.write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
