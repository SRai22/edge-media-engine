#!/usr/bin/env python3
"""Summarize an EME-016 results JSON into a markdown table: median [min-max] across repetitions."""
import json, statistics, sys
from pathlib import Path


def stat(vals, fmt="{:.0f}"):
    vals = [v for v in vals if v is not None]
    if not vals:
        return "n/a"
    med = statistics.median(vals)
    return f"{fmt.format(med)} [{fmt.format(min(vals))}–{fmt.format(max(vals))}]" if len(vals) > 1 else fmt.format(med)


def tracks(sub, key):
    return statistics.mean(t[key] for t in sub["tracks"]) if sub["tracks"] else None


def main(path):
    d = json.loads(Path(path).read_text())
    modes = [m for m in ("whip", "sdk", "sdk-pts") if any(r["mode"] == m for r in d["runs"])]
    by = {m: [r for r in d["runs"] if r["mode"] == m] for m in modes}
    rows = [
        ("Runs", lambda rs: str(len(rs))),
        ("Publisher setup (ms)", lambda rs: stat([r["publisher"]["setup_ms"] for r in rs])),
        ("Cold start → first decoded frame (ms)", lambda rs: stat([tracks(r["sub_a"], "first_frame_ms") for r in rs])),
        ("Late join → first decoded frame (ms, from process start)", lambda rs: stat([tracks(r["sub_b_late"], "first_frame_ms") for r in rs])),
        ("Late join: room connected → first frame (ms)", lambda rs: stat([tracks(r["sub_b_late"], "first_frame_ms") - r["sub_b_late"]["connected_ms"]
                                                                        for r in rs if r["sub_b_late"].get("connected_ms", -1) >= 0])),
        ("Capture→decode latency p50 (ms)", lambda rs: stat([tracks(r["sub_a"], "latency_p50_ms") for r in rs])),
        ("Capture→decode latency p95 (ms)", lambda rs: stat([tracks(r["sub_a"], "latency_p95_ms") for r in rs])),
        ("Late joiner latency p95 (ms)", lambda rs: stat([tracks(r["sub_b_late"], "latency_p95_ms") for r in rs])),
        ("Frames decoded, sub A (per track, 20 s)", lambda rs: stat([tracks(r["sub_a"], "frames") for r in rs])),
        ("Unreadable stamps (all subs)", lambda rs: stat([sum(t["unreadable"] for s in ("sub_a", "sub_b_late") for t in r[s]["tracks"]) for r in rs])),
        ("Publisher CPU, 1 viewer (cores)", lambda rs: stat([r["steady"]["pub_cpu_s"] for r in rs], "{:.2f}")),
        ("Publisher CPU, 10 load-test viewers (cores)", lambda rs: stat([r["loaded"]["pub_cpu_s"] for r in rs], "{:.2f}")),
        ("Publisher RSS (MB)", lambda rs: stat([r["steady"]["pub_rss_mb"] for r in rs], "{:.1f}")),
        ("Publisher threads", lambda rs: stat([r["steady"]["pub_threads"] for r in rs])),
        ("LiveKit CPU, 1 viewer (cores)", lambda rs: stat([r["steady"].get("livekit_cpu_s") for r in rs], "{:.3f}")),
        ("LiveKit CPU, 10 load-test viewers (cores)", lambda rs: stat([r["loaded"].get("livekit_cpu_s") for r in rs], "{:.3f}")),
        ("Server total CPU, 10 viewers (LiveKit + Ingress, cores)", lambda rs: stat([r["loaded"].get("livekit_cpu_s", 0) + (r["loaded"].get("ingress_cpu_s") or 0) for r in rs], "{:.3f}")),
        ("Server total memory, 10 viewers (MB)", lambda rs: stat([r["loaded"].get("livekit_mem_mb", 0) + (r["loaded"].get("ingress_mem_mb") or 0) for r in rs], "{:.0f}")),
        ("Ingress CPU (cores) / memory (MB)", lambda rs: stat([r["steady"].get("ingress_cpu_s") for r in rs], "{:.3f}") + " / " +
            stat([r["steady"].get("ingress_mem_mb") for r in rs], "{:.0f}") if rs[0]["mode"] == "whip" else "— (not used)"),
        ("Encoded / captured (both cams)", lambda rs: stat([sum(c["encoded"] for c in r["publisher"]["cameras"]) /
                                                             sum(c["captured"] for c in r["publisher"]["cameras"]) for r in rs], "{:.3f}")),
        ("Keyframe requests reaching encoder (per cam)", lambda rs: stat([statistics.mean(c["keyframe_requests"] for c in r["publisher"]["cameras"]) for r in rs])),
        ("Forced keyframes (per cam, 70 s)", lambda rs: stat([statistics.mean(c["forced_keyframes"] for c in r["publisher"]["cameras"]) for r in rs])),
    ]
    sizes = d["binary_bytes_stripped"]
    print(f"Host: {d['host']} · {d['date']} · stripped size: WHIP {sizes['whip'] / 2**20:.1f} MB, "
          f"SDK {sizes['sdk'] / 2**20:.1f} MB (incl. liblivekit + liblivekit_ffi)"
          + (f", EME-008 P2P baseline {sizes['baseline_p2p_webrtc_streamer'] / 2**20:.1f} MB" if 'baseline_p2p_webrtc_streamer' in sizes else "") + "\n")
    print("| Metric | " + " | ".join(m.upper() for m in modes) + " |")
    print("|---|" + "---|" * len(modes))
    for name, fn in rows:
        print(f"| {name} | " + " | ".join(fn(by[m]) for m in modes) + " |")


if __name__ == "__main__":
    main(sys.argv[1])
