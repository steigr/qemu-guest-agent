# qemu-guest-agent for Mac OS X

Native QEMU Guest Agent (QGA protocol) for Mac OS X 10.0 - 10.5 on PPC, speaking over the
`/dev/cu.virtio-N` node exposed by `drivers/ppc/virtioconsole`, and for the Intel releases 10.4 -
10.15 over the pci-serial port the orchestrator gives them (see "Intel"). Design, command scope, per-OS
matrix and milestones: docs/ppc-macosx-qga-implementation-plan.md (the repository's).

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

## Intel

The Intel releases before Big Sur have no agent of their own, and since 2026-10-08 run this one in
place of mav2287/mac-guest-agent: one universal binary, i386 and x86_64, which the orchestrator
stages as `/usr/local/bin/qemu-guest-agent` and launchd runs as `qemu-guest-agent --path
/dev/cu.pci-serial2` (`--path` is `--tty`, under the name that agent's launcher used).

- **Built on Snow Leopard** with Xcode 3.2.6 from its retail DVD (`make intel`): i386 with GCC 4.0
  and the 10.4u SDK, x86_64 with GCC 4.2 and the 10.5 SDK, both with 10.4 as the minimum, joined
  with `lipo`. i386 runs on 10.4 to 10.14; 10.15 runs only x86_64.
- **Tiger takes the x86_64 slice** on a 64-bit CPU, and has an x86_64 libSystem but no x86_64 IOKit
  or CoreFoundation. That slice links them weakly, so it loads there; `guest-get-diskstats`, the only
  command that needs IOKit, says so instead of answering. The installer stages Tiger's copy thinned
  to i386 (`ditto --arch i386`), which has both, so this matters only for the agent the installer
  runs from its provisioning media.
- **The UART is polled.** A pci-serial port is read every 20 ms whether or not `select()` says so,
  and reopened after ten minutes without a request: mav2287/mac-guest-agent measured that Tiger's
  serial driver can stop waking `select()` with bytes queued, and loses input queued past ~1.3 KB
  before it is read. The virtio console is not polled.
- `build-intel.sh` does all of it against a running `guest` in the cluster: the Xcode
  packages from the DVD image (verified by SHA-256, installed one by one), the source, the build,
  both slices' self-tests, and the binary back with a `BUILD-INFO` in `guest/dist/`.
  `make guest-agent-image` at the repository's root publishes it as the chart's `guestAgent.image`.

Verified: 121 self-test checks on 10.6.8 (both slices, `arch -i386` / `arch -x86_64`) and on
10.4.11 (the x86_64 slice, which is the one Tiger chose). In the extended gauntlet of 2026-10-10
to 2026-10-12 (docs/gauntlet/2026-10-12-extended.md), 0.2.3 was the agent of every release from
10.0 to 10.15, each installed from nothing: on the virtio console on PowerPC, on pci-serial on Intel,
through the install, the update restarts and the checks. Two faults remain open: TODO.md's "An
Intel guest's agent sometimes half-stalls after a boot" and TODO.md's "Leopard's first boot after
10.5.8 hangs every command run through /bin/sh".

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

Besides the serial channel the agent listens on a vsock port, through `drivers/virtiovsock`'s
libvsock (now [`macos/virtio_vsock`](https://git.stei.gr/virtualization/virtio-drivers/src/branch/main/macos/virtio_vsock) of virtio-drivers, laid out there by the build), compiled in from there on every platform (since 0.3.0: on Intel it takes 10.5's
`IOConnectCall*Method` in the x86_64 slice and 10.0's calls in the i386 one; where there is no kext,
every host among them, it says there is no device). On Intel the host dials vsock first and
pci-serial after it, and the kext is put on the installed system by the installer. `--vsock auto`, the default, listens
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
one channel would fight, so remove one. The package's postflight removes it, and the supervisor's
install with the add-ons never puts it there (docs/guest-addons-plan.md in the repository root). The StartupItem script avoids `RunService` (missing in 10.1's
`rc.common`). Not implemented: a config file.

## Releases

[`macos-qemu_guest_agent.yml`](../.forgejo/workflows/macos-qemu_guest_agent.yml) builds the agent
for PowerPC (`gcc3-10.3`, [ci/component.sh](ci/component.sh)) in the Panther guest of the
`macosx-ppc` runner on a push to `main` that touches `macos/`, packages it as `QemuGuestAgent.pkg`,
checks the package in the guest, and publishes package and stage to the `virtualization`
organisation as the generic package
[`macos_qemu_guest_agent`](https://git.stei.gr/virtualization/-/packages/generic/macos_qemu_guest_agent/latest)
(`QGA_VERSION.<run number>` and `latest`), and to `updates.verfüg.bar/macos_qemu_guest_agent/`.
The build lays `macos/` out as `guest/software/qemu-guest-agent` and virtio-drivers'
`macos/virtio_vsock` as `guest/drivers/virtiovsock`, the layout `transport/vsock.c` includes
libvsock from. The Intel binary is still built with `build-intel.sh`, in that layout.

## Source rules

C89 plus `long long`; no external libraries, no `poll()`/`kqueue`, `select()` only; hand-rolled
`cfmakeraw`. Out of memory exits (service wrapper restarts it).
