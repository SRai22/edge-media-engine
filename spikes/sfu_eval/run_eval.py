#!/usr/bin/env python3
"""EME-016: compare the two SFU publisher paths (EME-014 SDK, EME-015 WHIP) on an identical workload.

Timeline per run (publisher GOP = 600 frames = 40 s, so recovery inside the run depends on keyframe requests):
  t=0    fresh LiveKit stack already up; publisher starts; subscriber A starts (cold start -> first frame)
  t=25   subscriber B joins late (time to first decoded frame = keyframe request path)
  t=45   `lk load-test` adds 10 subscribers for 15 s (scaling: encode count must not change)
  t=70   publisher stops and prints its JSON
Sampling at 1 Hz: publisher RSS/CPU/threads (/proc) and LiveKit (+Ingress) container CPU/memory (cgroup v2).
Usage: run_eval.py [--reps 3] [--modes whip,sdk]
"""
import argparse, json, os, shutil, statistics, subprocess, tempfile, threading, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUN = ROOT / "spikes/livekit/.run"
BUILD = ROOT / "build-spike"
LK = Path.home() / "deps/lk/lk"
SDK_LIB = Path.home() / "deps/livekit-sdk/livekit-sdk-ubuntu-24.04-x64-1.13.1/lib"
GOP, PUB_SECONDS = 600, 70
CLK = os.sysconf("SC_CLK_TCK")


def envfile(p):
    return dict(line.strip().split("=", 1) for line in p.read_text().splitlines() if "=" in line)


def token(env, identity):
    out = subprocess.run([LK, "token", "create", "--room", "eme-spike", "--identity", identity, "--join",
                          "--valid-for", "10m"], env=env, capture_output=True, text=True, check=True).stdout
    return next(w for w in out.split() if w.startswith("eyJ"))


def cgroup(container):
    cid = subprocess.run(["docker", "inspect", "--format", "{{.Id}}", container], capture_output=True, text=True).stdout.strip()
    p = Path(f"/sys/fs/cgroup/system.slice/docker-{cid}.scope")
    return p if p.exists() else None


class Sampler(threading.Thread):
    def __init__(self, pid, cgroups):
        super().__init__(daemon=True)
        self.pid, self.cgroups, self.samples, self.stop = pid, cgroups, [], threading.Event()

    def run(self):
        t0 = time.monotonic()
        while not self.stop.is_set():
            s = {"t": time.monotonic() - t0}
            try:
                st = Path(f"/proc/{self.pid}/stat").read_text().rsplit(")", 1)[1].split()
                s["pub_cpu_s"] = (int(st[11]) + int(st[12])) / CLK
                s["pub_threads"] = int(st[17])
                status = Path(f"/proc/{self.pid}/status").read_text()
                s["pub_rss_mb"] = int(next(l for l in status.splitlines() if l.startswith("VmRSS")).split()[1]) / 1024
            except (FileNotFoundError, StopIteration, ProcessLookupError):
                break
            for name, cg in self.cgroups.items():
                s[f"{name}_cpu_s"] = int((cg / "cpu.stat").read_text().split()[1]) / 1e6
                s[f"{name}_mem_mb"] = int((cg / "memory.current").read_text()) / 2**20
            self.samples.append(s)
            self.stop.wait(1.0)


def window(samples, key, t_from, t_to, rate=False):
    pts = [s for s in samples if t_from <= s["t"] <= t_to and key in s]
    if len(pts) < 2:
        return None
    if rate:  # average CPU cores used over the window
        return (pts[-1][key] - pts[0][key]) / (pts[-1]["t"] - pts[0]["t"])
    return statistics.mean(p[key] for p in pts)


def run_once(mode):
    subprocess.run([ROOT / "spikes/livekit/up.sh"], check=True, capture_output=True)
    time.sleep(3)
    env = {**os.environ, **envfile(RUN / "env")}
    pub_env = dict(env)
    if mode == "whip":
        pub_env.update(envfile(RUN / "whip.env"))
        cmd = [BUILD / "spike_whip_publisher", str(PUB_SECONDS), str(GOP)]
    else:
        pub_env["LIVEKIT_TOKEN"] = token(env, "sdk-publisher")
        if mode == "sdk-pts":
            pub_env["SPIKE_SDK_TS"] = "pts"
        cmd = [BUILD / "spike_livekit_sdk_publisher", str(PUB_SECONDS), str(GOP)]
    cgroups = {k: cg for k, c in [("livekit", "livekit-livekit-1"), ("ingress", "livekit-ingress-1")] if (cg := cgroup(c))}
    sub = lambda ident, secs: subprocess.Popen([BUILD / "spike_lk_subscriber", str(secs)], stdout=subprocess.PIPE,
                                               stderr=subprocess.DEVNULL, text=True,
                                               env={**env, "LIVEKIT_TOKEN": token(env, ident)})
    pub = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, env=pub_env)
    sampler = Sampler(pub.pid, cgroups)
    sampler.start()
    sub_a = sub("sub-a", 20)
    time.sleep(25)
    sub_b = sub("sub-b", 15)
    time.sleep(20)
    load = subprocess.Popen([LK, "load-test", "--room", "eme-spike", "--subscribers", "10", "--duration", "15s"],
                            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pub_out, _ = pub.communicate(timeout=PUB_SECONDS + 30)
    sampler.stop.set()
    load.wait(timeout=30)
    a, b = (json.loads(p.communicate(timeout=30)[0].strip().splitlines()[-1]) for p in (sub_a, sub_b))
    smp = sampler.samples
    result = {
        "mode": mode, "publisher": json.loads(pub_out.strip().splitlines()[-1]), "sub_a": a, "sub_b_late": b,
        # sub A alone: 5-18 s; 10 load-test viewers after their ramp (A and B have left): 52-60 s
        "steady": {k: window(smp, k, 5, 18, rate=k.endswith("cpu_s")) for k in smp[0] if k != "t"},
        "loaded": {k: window(smp, k, 52, 60, rate=k.endswith("cpu_s")) for k in smp[0] if k != "t"},
        "samples": smp,
    }
    return result


def stripped_size(paths):
    total = 0
    with tempfile.TemporaryDirectory() as d:
        for p in paths:
            dst = Path(d) / p.name
            shutil.copy(p, dst)
            subprocess.run(["strip", dst], check=True)
            total += dst.stat().st_size
    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--modes", default="whip,sdk,sdk-pts")
    args = ap.parse_args()
    out = {"host": subprocess.run(["lscpu"], capture_output=True, text=True).stdout.split("Model name:")[1].splitlines()[0].strip(),
           "date": time.strftime("%Y-%m-%d %H:%M"),
           "binary_bytes_stripped": {
               "whip": stripped_size([BUILD / "spike_whip_publisher"]),
               "sdk": stripped_size([BUILD / "spike_livekit_sdk_publisher", SDK_LIB / "liblivekit.so", SDK_LIB / "liblivekit_ffi.so"]),
               "baseline_p2p_webrtc_streamer": stripped_size([BUILD / "webrtc_streamer"])},
           "runs": []}
    for rep in range(args.reps):
        for mode in args.modes.split(","):  # interleave modes so drift affects both equally
            print(f"rep {rep + 1}/{args.reps}: {mode}", flush=True)
            out["runs"].append(run_once(mode))
    res_dir = Path(__file__).parent / "results"
    res_dir.mkdir(exist_ok=True)
    path = res_dir / f"eval-{time.strftime('%Y%m%d-%H%M%S')}.json"
    path.write_text(json.dumps(out, indent=1))
    print(path)


if __name__ == "__main__":
    main()
