"""Offline camera/input-idle evidence; never infer AFK success from traffic alone.

Usage: analyze_afk_activity.py protocol-report.jsonl output.json
Generate the input with protocol_report.exe capture.a2session report.jsonl.
"""
import collections
import json
import pathlib
import statistics
import sys


def analyze(path):
    rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    groups = collections.defaultdict(list)
    for row in rows:
        if row.get("kind") == "message":
            groups[row.get("proxy_connection", 0)].append(row)
    result = {"source": str(path), "capture": rows[0], "connections": []}
    for connection, messages in groups.items():
        messages.sort(key=lambda row: (row["time_us"], row["packet_ids"][0]))
        origin = messages[0]["time_us"]
        cameras, movements, generated, echo_times = [], [], [], []
        for row in messages:
            if row.get("inbound_by_port"):
                continue
            name = row.get("name", "")
            if not name.startswith("0x"):
                continue
            opcode = int(name.split()[0], 16)
            base = {"elapsed_ms": (row["time_us"] - origin) / 1000,
                    "packet_ids": row["packet_ids"], "opcode": hex(opcode)}
            if row.get("tool_generated"):
                generated.append(base)
                continue
            if 0x3700 <= opcode <= 0x3718:
                movements.append(base)
            if opcode == 0x3601:
                echo_times.append(row["time_us"])
            if opcode != 0xFFA1 or not row.get("structure_complete"):
                continue
            values = {field["name"]: field["value"] for field in row["fields"]}
            idle = int(values["距上次鼠标输入时间（毫秒）"])
            angles = [float(values[key]) for key in (
                "相机欧拉角 C（度；输入分量 2）", "相机欧拉角 A（度；输入分量 0）",
                "相机欧拉角 B（度；输入分量 1）")]
            item = {**base, "mouse_idle_ms": idle, "angles_degrees": angles,
                    "zoom": float(values["相机缩放 (_zoom；物理单位未确认)"]),
                    "estimated_last_input_elapsed_ms": base["elapsed_ms"] - idle}
            if cameras:
                prev = cameras[-1]
                advance = item["estimated_last_input_elapsed_ms"] - prev["estimated_last_input_elapsed_ms"]
                item.update({"interval_ms": base["elapsed_ms"] - prev["elapsed_ms"],
                             "input_clock_advance_ms": advance,
                             "input_reset_candidate": advance > 2000,
                             "angles_changed": any(abs(a-b) > .001 for a, b in zip(angles, prev["angles_degrees"]))})
            cameras.append(item)
        if not cameras and not echo_times:
            continue
        result["connections"].append({
            "connection": connection,
            "capture_span_ms": (messages[-1]["time_us"] - origin) / 1000,
            "cameras": cameras, "native_movements": movements,
            "generated_messages": generated, "time_echo_count": len(echo_times),
            "time_echo_median_ms": statistics.median((b-a)/1000 for a, b in zip(echo_times, echo_times[1:])) if len(echo_times)>1 else None,
            "camera_only_experiment_uncontaminated": not movements and not generated,
        })
    result["stream_gaps"] = [r.get("stream") for r in rows if r.get("kind") == "stream" and r.get("gap")]
    result["limitations"] = [
        "A reset candidate is a change in reported mouse-input age, not AFK acceptance.",
        "Input-time estimates use capture arrival times; jitter and clock differences remain.",
        "Operator timing and an idle-disconnect baseline are required to assess AFK effects.",
        "A short or truncated capture cannot establish that a connection avoids AFK.",
    ]
    return result


if __name__ == "__main__":
    source, destination = map(pathlib.Path, sys.argv[1:3])
    result = analyze(source)
    destination.write_text(json.dumps(result, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    print(json.dumps({"connections": [{"id": c["connection"], "camera_samples": len(c["cameras"]),
        "input_reset_candidates": sum(x.get("input_reset_candidate", False) for x in c["cameras"]),
        "native_movements": len(c["native_movements"]), "generated_messages": len(c["generated_messages"])}
        for c in result["connections"]]}, ensure_ascii=False))
