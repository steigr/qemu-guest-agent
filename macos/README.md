# qemu-guest-agent for PowerPC Mac OS X

Native QEMU Guest Agent (QGA protocol) for Mac OS X 10.0 - 10.5 on PPC, speaking over the
`/dev/cu.virtio-N` node exposed by `drivers/ppc/virtioconsole`. Design, command scope, per-OS
matrix and milestones: `doc/plans/ppc-macosx-qga-implementation-plan.md`.

## Status

Implemented (M0-M3 and part of M2/M5), 101 self-test checks:

- protocol: `guest-ping`, `guest-sync`, `guest-sync-id`, `guest-sync-delimited`, `guest-info`
- file: `guest-file-open/close/read/write/seek/flush`
- exec: `guest-exec`, `guest-exec-status`
- system: `guest-get-host-name`/`-hostname`, `guest-get-time`, `guest-set-time`, `guest-get-timezone`,
  `guest-get-load`, `guest-get-osinfo`, `guest-get-vcpus`, `guest-get-fsinfo`,
  `guest-network-get-interfaces` (IPv4 + MAC), `guest-get-users`, `guest-shutdown`

Not yet: memory blocks, cpustats, disks/diskstats, IPv6 addresses, freeze, ssh keys, set-user-password,
installer/daemonizing, config file.

Verified on 10.1.5 (guest, GCC 2.95.2): `make test` passes, and as the live agent the host's
`macos exec` (stdout/stderr/exit code) and `macos cp` (100 KB binary round trip) work. Not yet built
or run on 10.0 or 10.2+.

`guest-exec` quirk: by default a binary that cannot be executed does not fail the request; the child
exits 127 and its stderr carries `exec PATH: reason`, as the Perl agent did. The host's `Stat`
(`macos cp`) relies on this because 10.1 has no `/usr/bin/stat`. `--strict-exec` gives upstream qemu-ga
semantics (the request fails). Captured output is capped at 1 MiB per stream (`*-truncated`).

## Layout

- `core/` JSON parser/serializer, base64, command registry + dispatcher, buffer/log helpers.
- `transport/` serial channel: raw termios open, `/dev/cu.virtio-*` discovery, framed read loop.
- `commands/` one file per command family (`protocol.c`, `file.c`, `exec.c`, `system.c`); each exposes a `qga_*_register()`.
- `selftest.c`, `tests/stdio.sh` host/guest tests.

## Build and test

```sh
make test                          # host or guest: unit tests + stdio framing test
make ARCH_FLAGS="-arch ppc"        # cross-arch flag if needed; on a PPC guest the default is ppc
./qemu-guest-agent --tty auto               # the only /dev/cu.virtio-*; --tty PATH to pin; --tty - for stdio
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

## Source rules

C89 plus `long long`; no external libraries, no `poll()`/`kqueue`, `select()` only; hand-rolled
`cfmakeraw`. Out of memory exits (service wrapper restarts it). Not yet implemented: daemonizing,
config file, installers (M4).
