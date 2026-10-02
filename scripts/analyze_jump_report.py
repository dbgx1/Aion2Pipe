"""Summarize historical decoded jump traffic; never connects to the game."""
import collections
import hashlib
import json
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
destination = pathlib.Path(sys.argv[2])
rows = [json.loads(line) for line in source.read_text(encoding="utf-8").splitlines()]
tx = [r for r in rows if r.get("kind") == "message" and not r["inbound_by_port"]]


def fields(row):
    return {f["name"]: f["value"] for f in row["fields"]}


def compact(row, baseline=0):
    f = fields(row)
    return {
        "opcode": row["name"].split()[0],
        "stream": row["stream"],
        "offset": row["offset"],
        "packet_ids": row["packet_ids"],
        "time_us": row["time_us"],
        "relative_ms": (row["time_us"] - baseline) / 1000,
        "length": row["length"],
        "complete": row["structure_complete"],
        "flags": f.get("可选字段位图"),
        "gravity_mode": f.get("重力模式（枚举待确认）"),
        "jump_up": f.get("是否向上跳跃", "absent"),
        "position": [f.get(k) for k in ("X", "Y", "Z")],
        "velocity": [f.get("移动速度 " + k) for k in ("X", "Y", "Z")],
        "client_time": f.get("客户端时间（时基待确认）"),
    }


starts = [r for r in tx if r["name"].startswith("0x3702")]
jumps = [r for r in starts if fields(r).get("是否向上跳跃") == "true"]
assert jumps, "No explicit jump-up samples"
first = jumps[0]
following = [r for r in tx if r["stream"] == first["stream"] and r["offset"] >= first["offset"]]
timeline = []
for row in following:
    if row["name"].split()[0] not in ("0x3700", "0x3701", "0x3702", "0x3703", "0x3718"):
        continue
    timeline.append(compact(row, first["time_us"]))
    if row["name"].startswith(("0x3700", "0x3701", "0x3718")):
        break
result = {
    "source": str(source),
    "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    "outbound_counts": dict(collections.Counter(r["name"].split()[0] for r in tx)),
    "gravity_start_count": len(starts),
    "jump_up_counts": dict(collections.Counter(fields(r).get("是否向上跳跃", "absent") for r in starts)),
    "start_flags": dict(collections.Counter(fields(r).get("可选字段位图") for r in starts)),
    "explicit_jump_initial_vz": dict(collections.Counter(fields(r).get("移动速度 Z") for r in jumps)),
    "all_starts_complete": all(r["structure_complete"] for r in starts),
    "first_jump_fields": first["fields"],
    "first_jump_timeline": timeline,
    "all_gravity_starts": [compact(r, first["time_us"]) for r in starts],
    "limits": ["Historical capture, not a newly controlled jump experiment", "No claim of AFK reset", "Gravity enum names, clock origin and stationary landing remain unverified"],
}
destination.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(json.dumps({k: v for k, v in result.items() if k not in ("first_jump_fields", "first_jump_timeline", "all_gravity_starts")}, ensure_ascii=True))
