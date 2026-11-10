# qemu-guest-agent

A native QEMU Guest Agent (QGA protocol) for systems QEMU's own agent never ran on: IRIX 6.5 under
the [IRIS](https://git.stei.gr/irix/iris) emulator, and Mac OS X 10.0 to 10.15 under QEMU. Both
come from one C89 source that has been ported, not shared: the IRIX agent started as a copy of the
Mac OS X one, and each now has its own folder, build file, README and release workflow. Their
drivers are in [virtio-drivers](https://git.stei.gr/virtualization/virtio-drivers).

## Agents

| Agent | Platform | Channel | Version | Package |
| --- | --- | --- | --- | --- |
| [irix](irix) | IRIX 6.5 | virtio-serial ([virtio_serial](https://git.stei.gr/virtualization/virtio-drivers/src/branch/main/irix/virtio_serial)) | 0.1.0 | `irix_qemu_guest_agent` (inst product `qemu_guest_agent`) |
| [macos](macos) | Mac OS X 10.0 to 10.5 PowerPC, 10.4 to 10.15 Intel | virtio console or pci-serial, and vsock ([virtio_vsock](https://git.stei.gr/virtualization/virtio-drivers/src/branch/main/macos/virtio_vsock)) | 0.3.0 | `macos_qemu_guest_agent` |

The version is `QGA_VERSION` in each agent's `core/dispatch.h`.

## Layout

```
irix/                sources, Makefile (smake), tests, README.md, CLAUDE.md (the end-to-end suite)
irix/ci/             builds, installs and describes the inst package in the IRIX guest
macos/               sources, Makefile, build-intel.sh (the Intel binary), tests, README.md
macos/ci/            builds and packages the agent in the Panther guest of the macosx-ppc runner
ci/                  runner side, both platforms: package registry and verfuegbar publisher uploads
.forgejo/workflows/  one workflow per agent: <platform>-qemu_guest_agent.yml
```

## Releases

Each agent's workflow runs on a push to `main` that touches it, and on demand. It builds the agent
in its platform's guest, checks the package there, and publishes it twice:

- to this organisation's generic packages, as `<platform>_qemu_guest_agent`
  ([list](https://git.stei.gr/virtualization/-/packages)), under its version and `latest`;
- to the verfuegbar publisher in the home cluster (namespace `verfuegbar`), which serves it at
  `https://updates.xn--verfg-nva.bar/<platform>_qemu_guest_agent/`; the IRIX package is listed on
  [irix.verfüg.bar](https://irix.xn--verfg-nva.bar).

All tokens are the job's ID token, so the workflows need no secret. The registry accepts it through
an Authorized Integration whose audience is the repository variable `AUD`. The macOS build also
reads `macos/virtio_vsock` of virtio-drivers, whose libvsock it compiles in, through a second
Authorized Integration with the audience `AUD_SOURCES`. The publisher accepts it through its policy
for this repository and each workflow.

## History

The agents were separate repositories until 2026-11-10, imported here with their history:
[irix/qemu-guest-agent](https://git.stei.gr/irix/qemu-guest-agent) and
[macos/qemu-guest-agent](https://git.stei.gr/macos/qemu-guest-agent). They published as the `irix`
organisation's `qemu_guest_agent` and the `macos` organisation's `qemu-guest-agent`. Those packages
stay where they are, and new builds publish under the names above.
