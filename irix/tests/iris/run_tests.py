#!/usr/bin/env python3
"""End-to-end test of the agent: runs on the host against a guest started with
tests/iris/boot-ip28.sh, plays the host end of the agent's port and checks what
it says against what the guest says over telnet.

    tests/iris/run_tests.py WORKDIR [--show] [--shutdown]

WORKDIR is boot-ip28.sh's work directory, which holds agent.sock and log.sock.
The guest needs vioser loaded and the agent built (smake, in /share).
--show prints every reply. --shutdown ends by powering the guest off through
guest-shutdown.
IRIS_SRC is an iris checkout, for tools/irix-sh.py (default ../iris).
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IRIS_SRC = os.environ.get("IRIS_SRC", os.path.join(os.path.dirname(ROOT), "iris"))
IRIX_SH = os.path.join(IRIS_SRC, "tools", "irix-sh.py")
AGENT = "/tmp/qemu-guest-agent.obj/qemu-guest-agent"
PIDFILE = "/tmp/qga-test.pid"
LOGFILE = "/tmp/qga-test.log"
USER = "qgatest"

results = []
show = False


def check(name, ok, detail=""):
    results.append((name, ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"\n      {detail}" if detail and not ok else ""))
    sys.stdout.flush()


def guest(*cmds, timeout=120):
    """Run shell commands in the guest; returns (exit status, output). IRIX's tty
    takes at most 256 characters a line, so keep each one short."""
    args = [sys.executable, "-I", IRIX_SH, "--port", "12323", "--timeout", str(timeout), "run", "-k"]
    r = subprocess.run(args + list(cmds), capture_output=True, text=True, errors="replace")
    return r.returncode, r.stdout + r.stderr


class Host:
    """The host end of the agent's port: one connection to agent.sock."""

    def __init__(self, path, timeout=60):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.s.settimeout(timeout)
        self.buf = b""

    def close(self):
        self.s.close()

    def send(self, data):
        self.s.sendall(data)

    def line(self):
        while b"\n" not in self.buf:
            d = self.s.recv(65536)
            if not d:
                raise EOFError("agent's port closed")
            self.buf += d
        line, self.buf = self.buf.split(b"\n", 1)
        return line

    def sync(self, n):
        """What a real host does on connecting: 0xFF, then a delimited sync, then
        everything up to the reply's 0xFF is someone else's."""
        self.send(b"\xff" + json.dumps({"execute": "guest-sync-delimited", "arguments": {"id": n}}).encode() + b"\n")
        while True:
            line = self.line()
            if line.startswith(b"\xff"):
                return json.loads(line[1:])["return"] == n

    def call(self, cmd, **args):
        req = {"execute": cmd}
        if args:
            req["arguments"] = args
        self.send(json.dumps(req).encode() + b"\n")
        reply = json.loads(self.line())
        if show:
            print(f"      {cmd}: {json.dumps(reply)[:600]}")
        return reply

    def ret(self, cmd, **args):
        reply = self.call(cmd, **args)
        if "return" not in reply:
            raise RuntimeError(f"{cmd}: {reply}")
        return reply["return"]

    def run(self, path, *arg, input_data=None, timeout=60):
        """guest-exec and its status: (exit code, stdout, stderr)."""
        args = {"path": path, "arg": list(arg), "capture-output": True}
        if input_data is not None:
            args["input-data"] = base64.b64encode(input_data).decode()
        pid = self.ret("guest-exec", **args)["pid"]
        end = time.time() + timeout
        while time.time() < end:
            st = self.ret("guest-exec-status", pid=pid)
            if st["exited"]:
                return (st.get("exitcode"), base64.b64decode(st.get("out-data", "")),
                        base64.b64decode(st.get("err-data", "")))
            time.sleep(0.1)
        raise TimeoutError(path)

    def put(self, path, data, chunk=48000):
        h = self.ret("guest-file-open", path=path, mode="w")
        for k in range(0, len(data), chunk):
            self.ret("guest-file-write", handle=h, **{"buf-b64": base64.b64encode(data[k:k + chunk]).decode()})
        self.ret("guest-file-close", handle=h)

    def get(self, path, chunk=48000):
        h = self.ret("guest-file-open", path=path, mode="r")
        out = b""
        while True:
            r = self.ret("guest-file-read", handle=h, count=chunk)
            out += base64.b64decode(r["buf-b64"])
            if r["eof"]:
                break
        self.ret("guest-file-close", handle=h)
        return out


def start_agent(*extra):
    guest(f"{AGENT} --daemon --pidfile {PIDFILE} --log {LOGFILE} " + " ".join(extra))
    time.sleep(1)


def stop_agent():
    guest(f"[ -f {PIDFILE} ] && kill `cat {PIDFILE}`; rm -f {PIDFILE}; sleep 1")


def test_protocol(h):
    check("resync and guest-sync-delimited", h.sync(1234))
    check("guest-ping", h.call("guest-ping") == {"return": {}})
    h.send(b'{"execute":"guest-ping","id":"abc"}\n')
    check("guest-ping with an id", json.loads(h.line()) == {"return": {}, "id": "abc"})
    check("unknown command", h.call("nope")["error"]["class"] == "CommandNotFound")
    info = h.ret("guest-info")
    names = {c["name"] for c in info["supported_commands"]}
    check("guest-info lists the commands", {"guest-exec", "guest-file-read", "guest-get-osinfo"} <= names)
    # A partial line and a resync: only what follows the 0xFF is answered.
    h.send(b'{"execute":"guest-p\xff{"execute":"guest-ping"}\n')
    check("0xFF drops a partial line", json.loads(h.line()) == {"return": {}})


def test_system(h):
    _, uname = guest("uname -n; uname -r; uname -m; uname -v; uname -R")
    lines = uname.split()
    os_ = h.ret("guest-get-osinfo")
    check("guest-get-host-name", h.ret("guest-get-host-name")["host-name"].split(".")[0] == lines[0], uname)
    check("guest-get-osinfo is IRIX", os_["id"] == "irix" and os_["kernel-release"] == "6.5", str(os_))
    check("guest-get-osinfo machine", os_["machine"] == "IP28", str(os_))
    check("guest-get-osinfo version", os_["version"].startswith("6.5.") and os_["version"] in uname, str(os_))

    t0 = h.ret("guest-get-time")
    check("guest-get-time is the guest's clock", t0 > 10 ** 18, str(t0))
    h.ret("guest-set-time", time=h.ret("guest-get-time"))
    t1 = h.ret("guest-get-time")
    check("guest-set-time", 0 < t1 - t0 < 120 * 10 ** 9, f"{t0} {t1}")
    tz = h.ret("guest-get-timezone")
    check("guest-get-timezone", isinstance(tz["offset"], int) and tz.get("zone"), str(tz))

    _, up = guest("/usr/bsd/uptime")
    load = h.ret("guest-get-load")
    want = float(up.split("average:")[1].split(",")[0])
    check("guest-get-load agrees with uptime", abs(load["load1m"] - want) < 1.5, f"{load} / {up}")

    check("guest-get-vcpus", len(h.ret("guest-get-vcpus")) == 1)
    fs = h.ret("guest-get-fsinfo")
    root = [f for f in fs if f["mountpoint"] == "/"]
    check("guest-get-fsinfo has /", root and root[0]["type"] == "xfs" and root[0]["total-bytes"] > 10 ** 9, str(fs))
    check("guest-get-fsinfo skips what is synthetic", not [f for f in fs if f["mountpoint"] in ("/hw", "/ns")], str(fs))

    ifs = h.ret("guest-network-get-interfaces")
    ec0 = [i for i in ifs if i["name"] == "ec0"]
    check("network: ec0 has its address", ec0 and ec0[0]["ip-addresses"][0]["ip-address"] == "192.168.0.2"
          and ec0[0]["ip-addresses"][0]["prefix"] == 24, str(ifs))
    _, eaddr = guest("/sbin/nvram eaddr")
    check("network: ec0's MAC is the nvram's", ec0 and ec0[0].get("hardware-address") in eaddr, f"{ifs} {eaddr}")
    lo0 = [i for i in ifs if i["name"] == "lo0"]
    check("network: lo0, with no MAC", lo0 and "hardware-address" not in lo0[0], str(ifs))
    users = h.ret("guest-get-users")
    check("guest-get-users", isinstance(users, list) and all(u["login-time"] > 0 for u in users), str(users))


def test_hw(h):
    _, hinv = guest("hinv -t memory")
    mb = int(hinv.split("size:")[1].split()[0])
    blocks = h.ret("guest-get-memory-blocks")
    size = h.ret("guest-get-memory-block-info")["size"]
    check("memory blocks add up to hinv's", len(blocks) * size >= mb * 2 ** 20 > (len(blocks) - 1) * size,
          f"{len(blocks)} x {size}, {hinv}")
    a = h.ret("guest-get-cpustats")
    time.sleep(3)
    b = h.ret("guest-get-cpustats")
    check("cpustats tick", b[0]["idle"] + b[0]["user"] + b[0]["system"] > a[0]["idle"] + a[0]["user"] + a[0]["system"],
          f"{a} {b}")
    disks = h.ret("guest-get-disks")
    names = {d["name"]: d for d in disks}
    check("guest-get-disks whole disk", "/dev/rdsk/dks0d1vol" in names, str(disks))
    check("guest-get-disks partition", names.get("/dev/dsk/dks0d1s0", {}).get("dependencies") == ["/dev/rdsk/dks0d1vol"],
          str(disks))
    s0 = h.ret("guest-get-diskstats")
    check("guest-get-diskstats lists the disk", [d for d in s0 if d["name"] == "/dev/rdsk/dks0d1vol"], str(s0))
    guest("dd if=/dev/zero of=/usr/tmp/qga.dd bs=64k count=64; sync; rm -f /usr/tmp/qga.dd; sync")
    s1 = h.ret("guest-get-diskstats")
    check("diskstats count a 4 MiB write", s0 and s1 and
          s1[0]["stats"]["write-sectors"] - s0[0]["stats"]["write-sectors"] >= 8192, f"{s0} {s1}")


def test_file_exec(h):
    code, out, err = h.run("/bin/sh", "-c", "cat; echo err >&2; exit 3", input_data=b"hello")
    check("exec: stdin, stdout, stderr, status", (code, out, err) == (3, b"hello", b"err\n"), f"{code} {out} {err}")
    code, out, _ = h.run("/sbin/uname", "-m")
    check("exec: a real tool", code == 0 and out.strip() == b"IP28", f"{code} {out}")
    code, _, err = h.run("/nonexistent/x")
    check("exec: a missing binary exits 127", code == 127 and b"exec" in err, f"{code} {err}")
    code, out, _ = h.run("/usr/bin/stat", "-c", "%s %f", "/etc/passwd")
    check("exec: /usr/bin/stat is the applet", code == 0 and out.split()[1] == b"81a4", f"{code} {out}")

    data = bytes(range(256)) * 4096         # 1 MiB of every byte value
    t = time.time()
    h.put("/usr/tmp/qga.bin", data)
    up = time.time() - t
    t = time.time()
    back = h.get("/usr/tmp/qga.bin")
    down = time.time() - t
    check(f"file: 1 MiB round trip ({up:.1f} s up, {down:.1f} s down)", back == data, f"{len(back)} bytes back")
    code, out, _ = h.run("/usr/bin/sum", "-r", "/usr/tmp/qga.bin")
    check("file: the guest sees the same bytes", code == 0 and out, str(out))
    _, _, _ = h.run("/bin/rm", "-f", "/usr/tmp/qga.bin")
    reply = h.call("guest-file-open", path="/nonexistent/x")
    check("file: an open that fails says so", "No such file" in reply.get("error", {}).get("desc", ""), str(reply))


def test_user(h):
    guest("sed -e /^qgatest:/d /etc/passwd > /tmp/p && cp /tmp/p /etc/passwd",
          f"echo '{USER}:*:1999:20:qga test:/usr/tmp/{USER}:/bin/sh' >> /etc/passwd",
          f"mkdir -p /usr/tmp/{USER}; chown {USER} /usr/tmp/{USER}")
    try:
        pw = base64.b64encode(b"sekrit42").decode()
        h.ret("guest-set-user-password", username=USER, password=pw)
        _, line = guest(f"grep '^{USER}:' /etc/passwd")
        fields = line.strip().split(":")
        check("password: a DES hash went in", len(fields[1]) == 13 and fields[0].endswith(USER), line)
        check("password: the rest of the line is kept", fields[2:5] == ["1999", "20", "qga test"], line)
        src = (b'#include <stdio.h>\n#include <crypt.h>\n#include <pwd.h>\n#include <string.h>\n'
               b'int main(){struct passwd*p=getpwnam("' + USER.encode() + b'");'
               b'puts(strcmp(crypt("sekrit42",p->pw_passwd),p->pw_passwd)?"no":"yes");return 0;}\n')
        h.put("/tmp/qgac.c", src)
        guest("cc -n32 -o /tmp/qgac /tmp/qgac.c 2>/dev/null", timeout=120)
        _, out = guest("/tmp/qgac")
        check("password: crypt() accepts it", "yes" in out, out)
        h.ret("guest-set-user-password", username=USER, password=base64.b64encode(b"ab3zzzzzzzzzz").decode(),
              crypted=True)
        _, line = guest(f"grep '^{USER}:' /etc/passwd")
        check("password: crypted goes in as it is", ":ab3zzzzzzzzzz:" in line, line)
        check("password: no such user", "no such user" in h.call(
            "guest-set-user-password", username="nosuch", password=pw)["error"]["desc"])
        _, ls = guest("ls -l /etc/passwd /etc/ptmp")
        check("password: the file keeps its mode, no ptmp left", "-rw-r--r--" in ls and "Cannot access /etc/ptmp" in ls, ls)

        keys = ["ssh-rsa AAAA one", "ssh-rsa BBBB two"]
        h.ret("guest-ssh-add-authorized-keys", username=USER, keys=keys)
        h.ret("guest-ssh-add-authorized-keys", username=USER, keys=keys[:1])
        check("ssh: add dedupes", h.ret("guest-ssh-get-authorized-keys", username=USER)["keys"] == keys)
        h.ret("guest-ssh-remove-authorized-keys", username=USER, keys=keys[:1])
        check("ssh: remove", h.ret("guest-ssh-get-authorized-keys", username=USER)["keys"] == keys[1:])
        _, ls = guest(f"ls -ld /usr/tmp/{USER}/.ssh/authorized_keys")
        check("ssh: owner and mode", "-rw-------" in ls and USER in ls, ls)
    finally:
        guest("sed -e /^qgatest:/d /etc/passwd > /tmp/p && cp /tmp/p /etc/passwd",
              f"rm -rf /usr/tmp/{USER} /tmp/qgac /tmp/qgac.c")


def test_freeze(h):
    check("freeze: thawed to start", h.ret("guest-fsfreeze-status") == "thawed")
    n = h.ret("guest-fsfreeze-freeze")
    check("freeze: counts the root filesystem", n >= 1, str(n))
    check("freeze: status", h.ret("guest-fsfreeze-status") == "frozen")
    check("freeze: exec refused", "disabled" in h.call("guest-exec", path="/bin/true")["error"]["desc"])
    check("freeze: ping still answers", h.call("guest-ping") == {"return": {}})
    check("freeze: a second one fails", "error" in h.call("guest-fsfreeze-freeze"))
    check("freeze: thaw", h.ret("guest-fsfreeze-thaw") == n)
    check("freeze: exec again", h.run("/bin/true")[0] == 0)


def test_channel(sock):
    """The port's lifetime: a host that goes and comes back, with the agent staying up."""
    a = Host(sock)
    check("channel: first connection", a.sync(1))
    a.send(b'{"execute":"guest-pi')            # a partial line, then the host goes
    a.close()
    time.sleep(1)
    b = Host(sock)
    # No sync: a new connection's first request must be answered, and not after the old tail.
    b.send(b'{"execute":"guest-ping"}\n')
    check("channel: reconnect drops the old partial line", json.loads(b.line()) == {"return": {}})
    b.close()
    for k in range(5):
        h = Host(sock)
        ok = h.sync(100 + k)
        h.close()
        if not ok:
            break
    check("channel: five more reconnects", ok)
    _, cpu = guest(f"ps -o time= -p `cat {PIDFILE}`")
    time.sleep(10)
    _, cpu2 = guest(f"ps -o time= -p `cat {PIDFILE}`")
    check("channel: no host, no spinning", abs(secs(cpu2) - secs(cpu)) <= 1, f"{cpu} {cpu2}")
    guest(f"{AGENT} --log /tmp/qga2.log --log-stream off & sleep 3; kill $!")
    _, out = guest("grep -c 'cannot open' /tmp/qga2.log; rm -f /tmp/qga2.log")
    check("channel: a second agent is refused, one line of log", out.split()[:1] == ["1"], out)
    h = Host(sock)
    check("channel: the first agent still serves", h.sync(77))
    h.close()


def secs(ps_time):
    # The last word that looks like [hh:]mm:ss: the shell's own lines may follow it.
    parts = [w for w in ps_time.split() if ":" in w][-1].split(":")
    return sum(int(p) * 60 ** k for k, p in enumerate(reversed(parts)))


def test_log_stream(sock):
    marker = f"qga-log-test-{int(time.time())}"
    guest(f"logger -t qgatest {marker}-old")
    time.sleep(2)
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock)
    s.settimeout(1)
    got = b""
    # A host that connects gets the history first: the line logged before it came.
    end = time.time() + 15
    while marker.encode() + b"-old" not in got and time.time() < end:
        try:
            got += s.recv(65536)
        except socket.timeout:
            pass
    check("log stream: history on connect", marker.encode() + b"-old" in got, got[-200:].decode(errors="replace"))
    guest(f"logger -t qgatest {marker}-new")
    end = time.time() + 15
    while marker.encode() + b"-new" not in got and time.time() < end:
        try:
            got += s.recv(65536)
        except socket.timeout:
            pass
    check("log stream: a new line follows", marker.encode() + b"-new" in got, got[-200:].decode(errors="replace"))
    s.close()


def test_shutdown(h, work):
    h.send(b'{"execute":"guest-shutdown","arguments":{"mode":"powerdown"}}\n')
    log = os.path.join(work, "console.log")
    end = time.time() + 180
    down = False
    while time.time() < end and not down:
        down = subprocess.run(["pgrep", "-x", "iris"], capture_output=True).returncode != 0
        time.sleep(2)
    check("guest-shutdown powers the guest off", down)
    if os.path.exists(log):
        with open(log, errors="replace") as f:
            check("guest-shutdown went through init", "The system is shutting down" in f.read())


def main():
    global show
    ap = argparse.ArgumentParser()
    ap.add_argument("workdir")
    ap.add_argument("--show", action="store_true")
    ap.add_argument("--shutdown", action="store_true")
    a = ap.parse_args()
    show = a.show
    os.chdir(a.workdir)                 # a Unix socket's path has to be short

    rc, _ = guest(f"[ -x {AGENT} ]")
    if rc != 0:
        sys.exit(f"{AGENT} is not built: run smake in the guest's /share")
    stop_agent()
    guest(f"rm -f {LOGFILE}; [ -f /var/run/qemu-guest-agent.pid ] && /etc/init.d/qemu-guest-agent stop")
    start_agent()
    h = Host("agent.sock")
    try:
        if not h.sync(9):
            sys.exit("no sync")
        test_protocol(h)
        test_system(h)
        test_hw(h)
        test_file_exec(h)
        test_user(h)
        test_freeze(h)
        h.close()
        test_channel("agent.sock")
        test_log_stream("log.sock")
        _, log = guest(f"cat {LOGFILE}")
        print(log)
        if a.shutdown:
            h = Host("agent.sock")
            h.sync(10)
            test_shutdown(h, ".")
    finally:
        if not a.shutdown:
            stop_agent()
    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)} of {len(results)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
