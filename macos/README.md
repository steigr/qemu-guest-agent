# qemu-guest-agent for PowerPC Mac OS X

Native QEMU Guest Agent (QGA protocol) for Mac OS X 10.0 - 10.5 on PPC, speaking over the
`/dev/cu.virtio-N` node exposed by `drivers/ppc/virtioconsole`. Design, command scope, per-OS
matrix and milestones: `doc/plans/ppc-macosx-qga-implementation-plan.md`.

## Status

Implemented (all commands of plan 5.1-5.3 that are feasible), 115 self-test checks:

- protocol: `guest-ping`, `guest-sync`, `guest-sync-id`, `guest-sync-delimited`, `guest-info`
- file: `guest-file-open/close/read/write/seek/flush`
- exec: `guest-exec`, `guest-exec-status`
- system: `guest-get-host-name`/`-hostname`, `guest-get-time`, `guest-set-time`, `guest-get-timezone`,
  `guest-get-load`, `guest-get-osinfo`, `guest-get-vcpus`, `guest-get-fsinfo`, `guest-get-users`,
  `guest-network-get-interfaces` (IPv4 + MAC via ioctl, IPv6 by parsing `ifconfig -a`), `guest-shutdown`
- hardware: `guest-get-memory-blocks`, `guest-get-memory-block-info` (nominal 128 MiB blocks, never
  offline-able), `guest-get-cpustats` (Mach, `type: linux` with user/nice/system/idle),
  `guest-get-disks` (`/dev/diskN[sM]`), `guest-get-diskstats` (IOKit `IOBlockStorageDriver`; sectors
  and ios, ticks in ms; other Linux fields omitted)
- freeze: `guest-fsfreeze-freeze`, `-freeze-list`, `-thaw`, `-status`
- ssh: `guest-ssh-get/add/remove-authorized-keys`
- user: `guest-set-user-password` (see below)

Deliberately not registered (plan 5.3): `guest-suspend-*`, `guest-fstrim`, `guest-set-vcpus`,
`guest-set-memory-blocks`.

Verified on 10.0.4 (guest, GCC 2.95.2), 10.1.5 (guest, GCC 2.95.2) and 10.2.8 (guest,
GCC 3.1): self-test (115 checks, no source change needed between releases), the installed StartupItem as live
agent, the host's `macos exec` / `macos cp` (100 KB binary round trip), every read-only command by hand, and a
throwaway NetInfo user for ssh keys (dedupe, remove, reset, owner/mode), passwords (stored hash verified with
`crypt()`) and freeze. The `dscl` (10.3+) and launchd (10.4+) paths are untested. None of 10.0, 10.1 or 10.2 has
`/usr/bin/stat` or `dscl`; 10.0 also has no `/dev/random`, which matters for Dropbear, not for this agent.

### Behaviours worth knowing

- **Freeze is not a freeze.** HFS+ has no kernel freeze. `guest-fsfreeze-freeze` runs
  `/etc/qemu/fsfreeze-hook freeze` if executable, `sync()`s, and `fcntl(F_FULLFSYNC)`s every local
  read-write mount (EINVAL is ignored where unsupported); `-status` then reports `frozen`, and the agent
  refuses file, exec, set-time, ssh-key, password and shutdown commands until `-thaw` (which runs the hook
  with `thaw`). Nothing else on the guest is quiesced, and there is no timeout thaw.
- **`guest-exec` with a missing binary:** by default the request succeeds and the child exits 127 with
  `exec PATH: reason` on stderr, as the Perl agent did (the host's `Stat` relies on it). `--strict-exec`
  gives upstream qemu-ga semantics (the request fails). Captured output is capped at 1 MiB per stream.
- **Built-in `/usr/bin/stat`:** 10.0/10.1 ship none. When `guest-exec` is asked for `/usr/bin/stat` and it is
  not executable, the agent re-executes itself as `--applet stat`, which understands the BSD
  (`-f "%z %p %m"`) and GNU (`-c "%s %f %Y"`) spellings the host uses.
- **`guest-set-user-password`:** `dscl . -passwd` where `/usr/bin/dscl` exists (plain text only);
  otherwise NetInfo: `niutil -createprop . /users/NAME passwd HASH` with a `crypt()` hash (DES: only the
  first 8 characters count). The password is passed to the tool as an argument, so it is briefly visible in `ps`.

## Layout

- `core/` JSON parser/serializer, base64, command registry + dispatcher, buffer/log helpers.
- `transport/` channels: `tty.c` raw termios open and `/dev/cu.virtio-*` discovery, `vsock.c` libvsock
  (PowerPC) or stubs, `serve.c` the one loop serving both with the line framing.
- `commands/` one file per command family (`protocol`, `file`, `exec`, `system`, `hw`, `freeze`, `ssh`, `user`, `stat` applet); each exposes a `qga_*_register()`.
- `selftest.c`, `tests/stdio.sh` host/guest tests.

## Build and test

```sh
make test                          # host or guest: unit tests + stdio framing test
make ARCH_FLAGS="-arch ppc"        # cross-arch flag if needed; on a PPC guest the default is ppc
./qemu-guest-agent --tty auto               # the only /dev/cu.virtio-*; --tty PATH to pin; --tty - for stdio
./qemu-guest-agent --vsock 7700              # vsock as well (default: auto, port 7700 when there is a device)
printf '{"execute":"guest-ping"}\n' | ./qemu-guest-agent --tty -
```

The Makefile flags (`-std=gnu89 -Wextra`) are untested on GCC 2.95 (10.0/10.1); loosen them there
and record the evidence here when that milestone is reached.

## Channel behaviour (plan section 4.1)

- Never exits when the channel disappears: rediscovers and reopens every 2 s.
- After (re)open, input up to the first newline or `0xFF` is discarded (may be a stale tail).
- `0xFF` on input drops any partial line (host resync); `guest-sync-delimited` answers with a
  leading `0xFF`.
- Writes never block indefinitely; data is dropped if the host does not drain it.
- Several `cu.virtio-*` nodes and no `--tty`: refuses to guess, logs the candidates, retries.

## vsock

Besides the serial channel the agent listens on a vsock port, through `drivers/ppc/virtiovsock`'s
libvsock (compiled in from there, on PowerPC only: its IOKit call is missing from current host SDKs,
where the vsock calls are stubs that say there is no device). `--vsock auto`, the default, listens
on port 7700 when the guest has a vsock device (`VirtioVsock.kext` loaded; the device is
`vhost-vsock-pci`, so the host is Linux) and stays quiet when it has none, retrying every 10 s in
case the kext loads later; `--vsock PORT` asks for a port and logs every failure; `--vsock off`;
`--tty off` serves vsock alone. Both channels are served from one loop and share the commands'
state (open files, exec'd children, the freeze); vsock handles are not file descriptors, so while
vsock is up `select()` waits 20 ms and the handles are polled after it. One connection at a time: a
new one replaces the old, which is what a host that reconnects needs. The host connects to the
guest's CID, port 7700, with `AF_VSOCK`.

Proven on all four PowerPC releases (2026-10-04, the cluster pods' vhost-vsock, each agent built by
the ladder with that release's tools, VirtioVsock loaded from `/tmp`, the agent run with `--tty off`
beside the pod's own): 10.0.4 (`guest`, CID 303), 10.1.5 (`guest`, 301), 10.2.8 (`guest`,
302) and 10.3.9 (`guest`, 304) each answered sync, osinfo and exec on port 7700, read 1 MiB of
`/mach_kernel` at 948-1119 KB/s, and on port 7701 gave the log's history on connect and then a line
`logger` wrote 10 s before. Earlier, on 10.2.8 (`guest`, CID 302), the agent built there with GCC 3.1: from the
pod, `guest-sync-delimited`, `guest-ping`, `guest-get-osinfo`, `guest-exec` with captured output,
1 MiB of `/mach_kernel` through `guest-file-read` at 940 KB/s (the Perl agent over its channel: 25
KB/s), a reconnect, and a second connection replacing the first. With no vsock device (the local
ladder, 10.2.8) the agent serves the virtio console as before: `ladder.sh 10.2 virtio-test` passed.

## Log stream

`--log-stream auto` (the default) follows `/var/log/system.log` -- where syslogd writes what the
system and the kernel log on these releases; the unified log is 10.12's -- and writes every new line
to vsock port 7701 and to the second virtio console (`/dev/cu.virtio-1`: with two, the lowest is the
agent's, so the host gives the agent's console the lower PCI slot). A host that connects over vsock
first gets the last 64 KB, then live lines; the log channels never wait (a console nobody reads on
the host would stall the agent): what would block is dropped. The file is polled once a second and
followed across newsyslog's rotation. `--log-stream off`; `--log-source FILE` for another file.
Proven over vsock on 10.0.4 to 10.3.9 (above); over the second console by the ladder's virtio-test.

## Running and installing

```sh
./qemu-guest-agent --install      # as root: /usr/local/libexec + StartupItem (<10.4) or LaunchDaemon (10.4+)
./qemu-guest-agent --upgrade      # same, overwriting; --uninstall removes it
./qemu-guest-agent --daemon       # detach; log /var/log/qemu-guest-agent.log, pid /var/run/qemu-guest-agent.pid
```

The installer warns when the Perl agent's StartupItem (`MacGuestAgent`) is present: two agents on the
one channel would fight, so remove one. The StartupItem script avoids `RunService` (missing in 10.1's
`rc.common`). Not implemented: a config file.

## Source rules

C89 plus `long long`; no external libraries, no `poll()`/`kqueue`, `select()` only; hand-rolled
`cfmakeraw`. Out of memory exits (service wrapper restarts it).
