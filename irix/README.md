# qemu-guest-agent for IRIX

A native QEMU Guest Agent (QGA protocol) for IRIX 6.5, speaking over a port of
[vioser](https://git.stei.gr/irix/virtio), the virtio-serial driver, under the
[IRIS](https://git.stei.gr/irix/iris) emulator. It is a port of
[the Mac OS X agent](https://git.stei.gr/macos/qemu-guest-agent).

## Status

Runs on Indigo2 IMPACT (IP28) under IRIS, on GIO slots `exp0` (interrupts) and
`exp1` (polled): 121 self-test checks and the end-to-end suite pass, and the
installed agent comes up by itself after a cold boot and after a reboot. It has
not run on any other machine; the binary is n32 `-mips3`, which every 6.5 machine
can execute, but vioser itself has only run on IP28.

Commands:

- protocol: `guest-ping`, `guest-sync`, `guest-sync-id`, `guest-sync-delimited`, `guest-info`
- file: `guest-file-open/close/read/write/seek/flush`
- exec: `guest-exec`, `guest-exec-status`
- system: `guest-get-host-name`/`-hostname`, `guest-get-time`, `guest-set-time`,
  `guest-get-timezone`, `guest-get-load`, `guest-get-osinfo`, `guest-get-vcpus`,
  `guest-get-fsinfo`, `guest-get-users`, `guest-network-get-interfaces`, `guest-shutdown`
- hardware: `guest-get-memory-blocks`, `guest-get-memory-block-info` (nominal 128 MiB
  blocks, never offline-able), `guest-get-cpustats`, `guest-get-disks`, `guest-get-diskstats`
- freeze: `guest-fsfreeze-freeze`, `-freeze-list`, `-thaw`, `-status` (see below)
- ssh: `guest-ssh-get/add/remove-authorized-keys`
- user: `guest-set-user-password`

Not registered: `guest-suspend-*`, `guest-fstrim`, `guest-set-vcpus`, `guest-set-memory-blocks`.

### Behaviours worth knowing

- **Freeze is only partly a freeze.** `xfs_freeze` refuses the root filesystem, so
  `/` is only synced. Any other local read-write XFS is really frozen, though no
  test system has had one, so that path has never run. Until `-thaw` the agent
  refuses file, exec, set-time, ssh-key, password and shutdown commands, and does
  not write its log. There is no timeout thaw. `/etc/qemu/fsfreeze-hook freeze|thaw`
  runs if it is executable.
- **`guest-exec` with a missing binary:** the request succeeds and the child exits 127
  with `exec PATH: reason` on stderr. `--strict-exec` gives upstream qemu-ga semantics
  (the request fails). Captured output is capped at 1 MiB per stream.
- **Built-in `/usr/bin/stat`:** IRIX has none (its `/sbin/stat` is another tool). When
  `guest-exec` is asked for it, the agent re-executes itself as `--applet stat`, which
  understands the BSD (`-f "%z %p %m"`) and GNU (`-c "%s %f %Y"`) spellings.
- **`guest-set-user-password`** writes the hash straight into `/etc/shadow` if the user is
  there, otherwise `/etc/passwd`, through `/etc/ptmp`. `crypt()` is DES: only the first 8
  characters of a password count.
- **`guest-get-cpustats`** reports `type: linux`, with IRIX's kernel time as `system`,
  wait as `iowait` and interrupt as `irq`.
- **MAC addresses** come from a snoop socket, which takes root.

## The host's side

In `iris.toml`, with vioser installed in the guest:

```toml
[virtio_serial]
enabled = true
slot    = "exp0"                        # on an Indigo2: exp1 has no interrupt, and is polled

[[virtio_serial.port]]
name   = "org.qemu.guest_agent.0"       # or org.iris.agent.0
socket = "/run/iris/agent.sock"

[[virtio_serial.port]]
name   = "org.iris.log.0"               # optional: the log stream
socket = "/run/iris/log.sock"
```

A host connects to `agent.sock`, sends `0xFF` and a `guest-sync-delimited`, and discards
everything up to the reply's `0xFF`. The socket takes one client at a time.

With no `--tty` the agent looks for `/hw/virtio-ports/org.qemu.guest_agent.0`, then
`org.iris.agent.0`, every 2 s. `--tty PATH` serves another port or a serial line.

## Channel behaviour

- A vioser port reads as end of file while no host is connected. The agent keeps it open and
  tries it every 200 ms; a host that comes back is served at once, with no partial line left
  over from the last connection.
- If the port goes away (a read error) it reopens it every 2 s, and never exits for this.
- After an open, input up to the first newline or `0xFF` is discarded: it may be a stale tail.
  A host that was already connected and does not resync loses its first request.
- `0xFF` on input drops any partial line; `guest-sync-delimited` answers with a leading `0xFF`.
- Writes never block for long: what the host does not drain for 3 s is dropped.
- The driver allows one open of a port, so a second agent logs `cannot open` once and retries.
- A driver with an open port does not unload, so stop the agent before `smake unload`. The rc
  links do this at shutdown.

## Log stream

`--log-stream auto` (the default) follows `/var/adm/SYSLOG` and writes new lines to the
`org.iris.log.0` port if there is one. A host that connects first gets the last 64 KB. After
that, what would block is dropped. `--log-stream off`; `--log-source FILE` for another file.

## Build and test

Inside IRIX, with MIPSpro, from the repository (`/share` in a guest booted by
`tests/iris/boot-ip28.sh`):

```sh
smake            # /tmp/qemu-guest-agent.obj/qemu-guest-agent
smake test       # the self-test and the stdin/stdout framing test
```

[CLAUDE.md](CLAUDE.md) has the end-to-end suite.

## Installing

```sh
smake install                               # or: qemu-guest-agent --install, as root
/etc/init.d/qemu-guest-agent start
chkconfig qemu-guest-agent off              # keep it from starting at boot
qemu-guest-agent --uninstall
```

The binary goes in `/usr/local/libexec`, and the rc links are `S99` and `K01`, either side of
vioser's `S98` and `K02`. The log is `/var/adm/qemu-guest-agent.log`. Nothing restarts an agent
that dies.

## Source rules

C89 plus `long long`, for MIPSpro 7.4. No external libraries. `select()` only.
Out of memory exits.
