"""Correlate generated jumps with same-connection server evidence, offline only."""
import collections
import hashlib
import json
import pathlib
import sys

source, output = map(pathlib.Path, sys.argv[1:3])
rows = [json.loads(line) for line in source.read_text(encoding="utf-8").splitlines()]
events = []


def walk(row, parent=None):
    if parent:
        row = dict(row)
        for key in ("time_us", "proxy_connection", "inbound_by_port", "packet_ids", "tool_generated", "mixed_source"):
            row[key] = parent[key]
    if row.get("name", "").startswith("0x"):
        row["opcode"] = int(row["name"].split()[0], 16)
        row["values"] = {f["name"]: f["value"] for f in row["fields"]}
        events.append(row)
    for child in row.get("children", []):
        walk(child, row)


for row in rows:
    if row.get("kind") == "message":
        walk(row)
events.sort(key=lambda e: (e["time_us"], e["packet_ids"][0]))
attempts = []
for request in events:
    if request["opcode"] != 0x3702 or request["values"].get("是否向上跳跃") != "true" or not request.get("tool_generated") or request.get("mixed_source"):
        continue
    connection, start = request["proxy_connection"], request["time_us"]
    own = None
    for event in events:
        if event["time_us"] > start:
            break
        if event["proxy_connection"] == connection and event["inbound_by_port"]:
            if event["opcode"] == 0x3621:
                own = None
            elif event["opcode"] == 0x3633 and event["structure_complete"]:
                own = event["values"].get("实体编号")
    window = [e for e in events if e["proxy_connection"] == connection and start < e["time_us"] <= start + 15000000]
    server = [e for e in window if e["inbound_by_port"]]
    matching = [e for e in server if own is not None and e["values"].get("实体编号") == own]
    movement = [e for e in window if not e["inbound_by_port"] and not e["tool_generated"] and 0x3700 <= e["opcode"] <= 0x3718]
    def evidence(event):
        return {"relative_ms": (event["time_us"] - start) / 1000, "opcode": hex(event["opcode"]),
                "complete": event["structure_complete"], "packet_ids": event["packet_ids"],
                "fields": {k: v for k, v in event["values"].items() if k not in ("编码长度", "消息编号")}}
    end = max(e["time_us"] for e in events if e["proxy_connection"] == connection)
    attempts.append({"connection": connection, "time_us": start, "self_entity": own,
        "request": evidence(request), "time_coverage_15s": end >= start + 15000000,
        "self_server_messages": [evidence(e) for e in matching],
        "first_native_movement_ms": None if not movement else (movement[0]["time_us"] - start) / 1000,
        "native_movement_count": len(movement), "server_time_echo_count": sum(e["opcode"] == 0x3600 for e in server),
        "unknown_server_message_count": sum(not e["structure_complete"] for e in server),
        "self_movement_penalties": [evidence(e) for e in matching if e["structure_complete"] and e["values"].get("移动惩罚启用") == "true"],
        "self_move_rejections": [evidence(e) for e in matching if e["opcode"] in (0x373e, 0x373f, 0x3740)],
        "server_opcode_counts": dict(collections.Counter(hex(e["opcode"]) for e in server))})
result = {"source": str(source), "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    "capture": rows[0], "attempts": attempts,
    "limitations": ["Timing correlation is not an explicit server acknowledgement", "No AFK reset claim", "Unknown server layouts remain", "Capture may stop at retention limit"]}
output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"attempts": len(attempts), "penalty_counts": [len(a["self_movement_penalties"]) for a in attempts],
                  "first_native_movement_ms": [a["first_native_movement_ms"] for a in attempts]}))
