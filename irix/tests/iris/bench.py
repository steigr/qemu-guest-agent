#!/usr/bin/env python3
"""Throughput of the agent's channel: runs on the host against a guest started
with tests/iris/boot-ip28.sh and the agent installed and running.

    tests/iris/bench.py WORKDIR [--size MIB] [--reps N] [--json FILE]

Measures file copy in and out through guest-file-write and guest-file-read, at
several chunk sizes, to a real file and past the disk (/dev/null, /dev/zero);
guest-exec's captured output; round trips; the agent's share of the CPU during a
copy; and the bare port with the agent stopped, which is the ceiling for
everything else. Figures are the median of
the runs, in KiB/s of payload (what the base64 on the wire carries).

File copies write to the buffer cache, with no fsync, and read what was just
written, so "file" measures the filesystem's path and not the disk's.
"""
import argparse
import base64
import json
import os
import socket
import statistics
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_tests as rt  # noqa: E402

GUEST_FILE = "/usr/tmp/qga-bench.bin"
PORT = "/hw/virtio-ports/org.qemu.guest_agent.0"
INIT = "/etc/init.d/qemu-guest-agent"
KIB = 1024
MIB = 1024 * KIB
CHUNKS = [4 * KIB, 16 * KIB, 48 * KIB]
PATTERN = bytes(range(256))


def rate(size, seconds):
    return size / KIB / seconds


def med(runs):
    return statistics.median(runs)


def copy_in(h, path, data, chunk):
    t = time.time()
    h.put(path, data, chunk=chunk)
    return time.time() - t


def copy_out_file(h, path, chunk, want):
    t = time.time()
    got = h.get(path, chunk=chunk)
    elapsed = time.time() - t
    if got != want:
        raise RuntimeError(f"{path}: {len(got)} bytes back, not what went in")
    return elapsed


def copy_out_zero(h, size, chunk):
    fd = h.ret("guest-file-open", path="/dev/zero", mode="r")
    t = time.time()
    done = 0
    while done < size:
        r = h.ret("guest-file-read", handle=fd, count=chunk)
        done += r["count"]
    elapsed = time.time() - t
    h.ret("guest-file-close", handle=fd)
    return elapsed


def bench_files(h, size, reps):
    data = (PATTERN * (size // len(PATTERN) + 1))[:size]
    rows = []
    for chunk in CHUNKS:
        cases = {"in, file": [], "in, /dev/null": [], "out, file": [], "out, /dev/zero": []}
        for _ in range(reps):
            cases["in, file"].append(copy_in(h, GUEST_FILE, data, chunk))
            cases["out, file"].append(copy_out_file(h, GUEST_FILE, chunk, data))
            cases["in, /dev/null"].append(copy_in(h, "/dev/null", data, chunk))
            cases["out, /dev/zero"].append(copy_out_zero(h, size, chunk))
        for name, runs in cases.items():
            rows.append({"case": name, "chunk_kib": chunk // KIB, "kib_s": rate(size, med(runs)),
                         "low": rate(size, max(runs)), "high": rate(size, min(runs))})
            print(f"{name:16} {chunk // KIB:3} KiB chunks  {rows[-1]['kib_s']:7.0f} KiB/s", flush=True)
    h.run("/bin/rm", "-f", GUEST_FILE)
    return rows


def run_capture(h, command, poll=0.02):
    """guest-exec with captured output: (seconds until exited, seconds of the reply
    that carries the output, the reply's return value)."""
    t = time.time()
    pid = h.ret("guest-exec", path="/bin/sh", arg=["-c", command], **{"capture-output": True})["pid"]
    while True:
        ask = time.time()
        st = h.ret("guest-exec-status", pid=pid)
        if st["exited"]:
            end = time.time()
            return end - t, end - ask, st
        time.sleep(poll)


def bench_capture(h, reps):
    rows = []
    for kib in (64, 256, 1024, 2048):
        total, wire, got = [], [], None
        for _ in range(reps):
            whole, reply, st = run_capture(h, f"/sbin/dd if=/dev/zero bs=1k count={kib} 2>/dev/null")
            got = st
            total.append(whole)
            wire.append(reply)
        n = len(base64.b64decode(got["out-data"]))
        rows.append({"produced_kib": kib, "delivered_kib": n // KIB, "truncated": got["out-truncated"],
                     "seconds": med(total), "kib_s": rate(n, med(total)), "reply_kib_s": rate(n, med(wire))})
        print(f"capture {kib:5} KiB: {n // KIB:5} KiB delivered in {med(total):6.2f} s "
              f"{rows[-1]['kib_s']:7.0f} KiB/s (the last reply {rows[-1]['reply_kib_s']:7.0f} KiB/s)"
              f"{' truncated' if got['out-truncated'] else ''}", flush=True)
    return rows


def bench_first_byte(h):
    """Whether output arrives while a command runs: it prints at once, then sleeps."""
    pid = h.ret("guest-exec", path="/bin/sh", arg=["-c", "echo first; sleep 5; echo last"],
                **{"capture-output": True})["pid"]
    t = time.time()
    while True:
        st = h.ret("guest-exec-status", pid=pid)
        if st.get("out-data"):
            first = time.time() - t
            print(f"first output of a command that prints at once and then sleeps 5 s: after {first:.2f} s",
                  flush=True)
            return first
        time.sleep(0.05)


def bench_ping(h, n=200):
    runs = []
    for _ in range(n):
        t = time.time()
        h.ret("guest-ping")
        runs.append(time.time() - t)
    ms = med(runs) * 1000
    print(f"guest-ping round trip: median {ms:.1f} ms, worst {max(runs) * 1000:.1f} ms", flush=True)
    return {"median_ms": ms, "worst_ms": max(runs) * 1000}


def bench_raw(sock_path, size, reps):
    """The bare port: dd or cat in the guest, with no agent, base64 or JSON."""
    out_runs, in_runs = [], []
    data = (PATTERN * (size // len(PATTERN) + 1))[:size]
    for _ in range(reps):
        s = socket.socket(socket.AF_UNIX)
        s.connect(sock_path)
        s.settimeout(120)
        time.sleep(1)
        job = threading.Thread(target=rt.guest, args=(f"/sbin/dd if=/dev/zero of={PORT} bs=4k "
                                                      f"count={size // (4 * KIB)} 2>/dev/null",), daemon=True)
        job.start()
        got, t0 = 0, None
        while got < size:
            d = s.recv(1 << 20)
            if not d:
                break
            if t0 is None:
                t0 = time.time()
            got += len(d)
        out_runs.append(time.time() - t0)
        job.join()
        s.close()
        if got != size:
            raise RuntimeError(f"raw port out: {got} of {size} bytes")

        s = socket.socket(socket.AF_UNIX)
        s.connect(sock_path)
        s.settimeout(120)
        time.sleep(1)
        counted = []
        job = threading.Thread(target=lambda: counted.append(rt.guest(f"/sbin/cat {PORT} | wc -c")[1]), daemon=True)
        job.start()
        time.sleep(3)                   # the guest has the port open, and the first bytes wait for it
        t = time.time()
        s.sendall(data)
        in_runs.append(time.time() - t)
        s.close()                       # end of file ends the cat
        job.join()
        if str(size) not in counted[0].split():
            raise RuntimeError(f"raw port in: the guest counted {counted[0]!r}, not {size}")
    print(f"bare port, guest to host {rate(size, med(out_runs)):7.0f} KiB/s, "
          f"host to guest {rate(size, med(in_runs)):7.0f} KiB/s", flush=True)
    return {"out_kib_s": rate(size, med(out_runs)), "in_kib_s": rate(size, med(in_runs))}


def agent_cpu_seconds():
    _, out = rt.guest("ps -o time= -p `cat /var/run/qemu-guest-agent.pid`")
    return rt.secs(out)


def bench_cpu(h, size):
    """How much of a copy's time the guest spends running the agent: near 100% means the agent's
    own work, and not the port, is the limit."""
    data = (PATTERN * (size // len(PATTERN) + 1))[:size]
    rows = {}
    for name, copy in (("in", lambda: copy_in(h, "/dev/null", data, 48 * KIB)),
                       ("out", lambda: copy_out_zero(h, size, 48 * KIB))):
        before = agent_cpu_seconds()
        elapsed = copy()
        used = agent_cpu_seconds() - before
        rows[name] = {"elapsed_s": elapsed, "agent_cpu_s": used}
        print(f"copy {name} of {size // MIB} MiB: {elapsed:.1f} s, agent CPU {used} s", flush=True)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("workdir")
    ap.add_argument("--size", type=int, default=4, help="MiB per file copy")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--json")
    ap.add_argument("--only", help="comma-separated: ping,files,capture,first,cpu,raw")
    a = ap.parse_args()
    out_json = os.path.abspath(a.json) if a.json else None
    os.chdir(a.workdir)                 # a Unix socket's path has to be short
    size = a.size * MIB

    h = rt.Host("agent.sock", timeout=180)
    if not h.sync(1):
        sys.exit("no sync")
    version = h.ret("guest-info")["version"]
    results = {"agent": version, "size_mib": a.size, "reps": a.reps}
    only = a.only.split(",") if a.only else ["ping", "files", "capture", "first", "cpu", "raw"]
    if "ping" in only:
        results["ping"] = bench_ping(h)
    if "files" in only:
        results["files"] = bench_files(h, size, a.reps)
    if "capture" in only:
        results["capture"] = bench_capture(h, a.reps)
    if "first" in only:
        results["first_output_s"] = bench_first_byte(h)
    if "cpu" in only:
        results["cpu"] = bench_cpu(h, 16 * MIB)
    h.close()

    if "raw" in only:
        rt.guest(f"{INIT} stop; sleep 1")
        try:
            results["raw"] = bench_raw("agent.sock", size, a.reps)
        finally:
            rt.guest(f"{INIT} start; sleep 1")
    if out_json:
        with open(out_json, "w") as f:
            json.dump(results, f, indent=1)


if __name__ == "__main__":
    main()
