# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

The QEMU guest agent for IRIX 6.5, over a port of the vioser virtio-serial driver (sibling checkout `../virtio`) under the IRIS emulator (`../iris`, branch `feature/virtio-serial-close-drain`, which is `feature/virtio-serial-guest-device-412469` plus the fix `--close-only` tests). Ported from `https://git.stei.gr/macos/qemu-guest-agent`, whose `core/` is nearly unchanged. `README.md` covers status, behaviour and the host's configuration.

## Commands

This builds only inside IRIX: there is no cross compiler, and `commands/` uses IRIX-only interfaces. Inside the guest, from `/share` (this repository when booted as below):

```sh
smake                    # /tmp/qemu-guest-agent.obj/qemu-guest-agent; stock make cannot read the Makefile
smake test               # --self-test, then tests/stdio.sh
smake install            # the binary, rc script and chkconfig flag; the guest's disk keeps them
```

End to end, from the host, against the installed system in `$IRIX_INSTANCE`. Build `iris` as `../virtio/CLAUDE.md` says.

```bash
IRIS="$TGT/release/iris" VIRTIO_SLOT=exp0 tests/iris/boot-ip28.sh "$WORK"   # blocks; run in the background
python3 ../iris/tools/irix-sh.py --port 12323 wait --timeout 480
python3 ../iris/tools/irix-sh.py --port 12323 --timeout 300 run -k 'mount /share; cd /share && smake'
tests/iris/run_tests.py "$WORK"                                             # --show prints every reply
tests/iris/run_tests.py "$WORK" --shutdown                                  # ends with guest-shutdown
```

`tests/iris/bench.py "$WORK"` measures throughput against the installed agent (`--only ping,files,capture,first,cpu,raw` to pick parts); `README.md` has the results. It takes about 10 minutes, and host load moves the figures.

`run_tests.py --close-only` runs just the tests of a host that sends and hangs up at once. Otherwise it has no way to run a single test: call its `test_*` functions, or send requests by hand to `$WORK/agent.sock`. It stops an installed agent and does not restart it, so run `/etc/init.d/qemu-guest-agent start` afterwards. Two `request ... failed` lines in `--self-test` are expected: they are negative tests.

A change to how the agent waits on the port needs a run on both `VIRTIO_SLOT=exp0` and `exp1`, which reach `select()` by different paths (interrupt and poll timer).

Writes go to `irix65.raw.overlay` beside the instance's image, never to the image. vioser and the agent are installed in that overlay, so deleting it after a crash removes them too: reinstall with `smake install` in `../virtio/driver` and here. Power off with `guest-shutdown` or `shutdown -p -g0 -y`; a killed guest leaves XFS dirty.

**Never run two emulators on one overlay.** Doing so on 2026-11-01 ruined the root filesystem (`xlog_bread`, then `vfs_mountroot: no root found`). iris locks an overlay only with `feature/overlay-lock`, which the branch above does not include, so both repositories' `boot-ip28.sh` also refuse when `lsof` shows the overlay open. Do not test whether a guest has gone with `pgrep -x iris`, since the binary can have another name: `lsof` on the overlay is what counts.

## Architecture

- `core/`: JSON, base64, the command table, and `qga_handle`, which turns a request line into a reply. Portable.
- `commands/`: one file per command family, each with a `qga_*_register()`. `system.c`, `hw.c`, `freeze.c` and `user.c` are where IRIX lives; the others are plain POSIX.
- `transport/serve.c`: the one loop. It frames requests by line and `0xFF`, and `select()`s on the port and on the children of `guest-exec` (`qga_poller`), so requests never wait on a child's pipes. The log stream is served from the same loop.
- `install.c`: `--daemon`, `--install`, and the rc script as a string.

A command runs to completion inside the loop, so one that blocks stops the agent answering. Anything slow has to go through a poller, as `guest-exec` does.

Differences from the blueprint that are on purpose: no vsock (IRIX has no driver for it), no UART polling (that was for a Tiger bug), and no CI or packaging yet.

## Non-obvious constraints

- **A port with no host looks broken and is not.** vioser's `read` returns 0 and its `poll` reports `POLLHUP` while no host is connected, so `select()` returns at once. `serve.c` therefore takes the port out of the read set and tries it on a 200 ms tick instead of closing it. Treating that 0 as a lost channel, as the blueprint did, makes a host that connects in the gap lose its first request to the stale-tail discard.
- **The freeze must not write.** Anything written to a frozen XFS blocks, and a blocked agent cannot thaw. So `qga_log_pause` stops the log during a freeze, and commands that touch the disk carry `QGA_BLOCK_FROZEN`. A new command that writes needs that flag.
- **Kernel interfaces here were found by reading `/usr/include/sys` and trying them**, not from documentation: load is `sysget(SGT_KSYM, "avenrun")` scaled by 1024, disk counters are the `_dkiotime` attribute on `/hw/rdisk/*vol`, and a MAC is `SIOCGIFADDR` on a bound snoop socket. Check the header rather than trusting memory.
- **IRIX's tools are old.** `/bin/sh` is Bourne, with no `$(...)`. `grep` has no `\|` or `-A`/`-B` (use `egrep`), and there is no `sed -i` or bash.
- **`tools/irix-sh.py` hangs or garbles a command line over 256 characters** (IRIX's tty limit). Push a script with its `push` action and run that. Its `wait` returns before rc2 finishes.
- **`/tmp` is emptied at boot**, so the build goes with it. **`/share` mounts late** after boot and is `noexec` for practical purposes, which is why objects go to `/tmp`: `mount /share` if it is empty.
- **`smake` runs jobs in parallel and outlives a closed pipe.** `smake | head` leaves compilers running, and a second build then links half-written objects (`ld32: file is empty`). Send build output to a file.

## What has run

IP28 under IRIS only, on `exp0` and `exp1`. Never run: other machines, and `xfs_freeze` on a non-root XFS (the test system has only `/`). Say so rather than implying otherwise.
