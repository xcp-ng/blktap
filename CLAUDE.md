# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

blktap is the user-level disk I/O layer for XenServer. It presents virtual block
devices to VMs that are implemented in userspace and backed by raw partitions,
files, or the network. Working with the kernel blktap driver, VM disk I/O is
handed to the `tapdisk` userspace daemon over a shared-memory/character-device
interface; per-disk image drivers (VHD, raw AIO, QCOW, NBD, RAM, ...) service
requests using asynchronous Linux libaio + O_DIRECT.

This is a C autotools project. It is normally built as an RPM for XenServer dom0
(see `mk/blktap.spec.in`) and depends heavily on Xen headers/libraries
(`xen/xen.h`, xenstore, libxenctrl), libaio, libuuid, and openssl.

## Build

```sh
sh autogen.sh            # regenerate configure via autoreconf (needs autoconf/automake/libtool)
./configure              # add --enable-tests to build the unit test programs
make                     # build tapdisk, vhd-util, tap-ctl, tapback, cbt-util, etc.
make install             # DESTDIR=... to stage into a buildroot
```

The RPM build sequence mirrors this: `sh autogen.sh` → `%configure` →
`make` → `make check` → `make install`. Coverage builds pass `GCOV=true`.

## Tests

There are two test systems:

- **cmocka unit tests** (`mockatests/`) — the primary in-tree tests, run via
  automake. These require `--enable-tests` at configure time.
  ```sh
  ./configure --enable-tests && make && make check
  ```
  `make check` recurses through `mockatests/{wrappers,drivers,cbt,control,vhd}`.
  On failure the spec dumps the logs: `find mockatests -name '*.log' -exec cat {} \;`.
  Coverage is collected with `./collect-test-results.sh <output-dir>` (lcov/genhtml).

  To run a single suite, build and run its `check_PROGRAM` directly, e.g.:
  ```sh
  make -C mockatests/drivers check          # one suite
  ./mockatests/drivers/test-drivers         # run the built binary directly
  ```
  Each `mockatests/*/Makefile.am` defines the suite's `check_PROGRAMS`, `TESTS`,
  and — importantly — the set of `-Wl,--wrap=<symbol>` linker flags used to
  intercept libc/internal calls (see `mockatests/wrappers/` and the per-suite
  `*-wrappers.c`). When adding a test that stubs a new function, add a matching
  `--wrap` flag in the suite Makefile.

- **ceedling tests** (`tests/`) — legacy; driven by an external repo
  (`storage-ci`), not part of the normal in-tree flow. See `tests/README.md`.

Note: `AM_CFLAGS` in test dirs uses `-Wall -Werror`, so warnings break the test build.

## Architecture

The system is a set of cooperating processes and libraries:

- **`drivers/` — `tapdisk` daemon and image drivers.** The core.
  `libtapdisk.la` is the internal library linked into `tapdisk`, `td-util`,
  `tapdisk-stream`, etc. Request flow: `tapdisk-server.c` (event loop via
  `scheduler.c`) → `tapdisk-vbd.c` (per-VBD queueing) → an image driver.
  - Image drivers are the pluggable backends (`block-vhd.c`, `block-aio.c`,
    `block-qcow.c`, `block-ram.c`, `block-nbd.c`, `block-cache.c`,
    `block-crypto.c`, `block-log.c`, `block-valve.c`, ...). They are registered
    by `DISK_TYPE_*` index in the dispatch tables in `drivers/tapdisk-disktype.c`
    — this file is the map from a disk-type name (e.g. `vhd`, `aio`) to its
    driver struct. Drivers can be **filters** stacked on top of other drivers.
  - AIO is abstracted behind an io-backend interface (`io-backend.h`,
    `libaio-backend.c`, `posixaio-backend.c`) with `io-optimize.c` for request
    merging.
  - `tapdisk-control.c` handles the control-socket protocol
    (`tapdisk-message.h`) used by `tap-ctl`.

- **`control/` — `tap-ctl`.** CLI + library to spawn/manage tapdisk instances
  (allocate, attach, open, pause, close, list, stats). Each verb is its own
  `tap-ctl-*.c`; they talk to a running tapdisk over its control socket.

- **`tapback/` — xenstore bridge daemon.** Watches xenstore for VBD
  frontend/backend state (`backend.c`, `frontend.c`, `xenstore.c`) and drives
  tap-ctl operations to connect guests. Installed as a systemd service
  (`tapback.service`). The `-b` option disables write I/O barriers globally.

- **`vhd/` — VHD tooling.** `vhd-util` (multiplexes subcommands like
  `create`, `check`, `coalesce`, `snapshot`, `resize`, each a
  `vhd/lib/vhd-util-*.c`) and `vhd-index`. `vhd/lib/libvhd*.c` is the VHD
  format library (`libvhd.c`, `libvhd-index.c`, `libvhd-journal.c`) used both by
  the tools and the `block-vhd` driver.

- **`cbt/` — `cbt-util`.** Changed Block Tracking metadata tool.

- **`lvm/` — `lvm-util`.** LVM metadata scanning helpers.

- **`cpumond/` — `cpumond`.** CPU monitor daemon (systemd service).

- **`drivers/crypto/`, `drivers/blktap/`** — XTS-AES crypto for encrypted disks
  and the kernel-interface glue.

- **`include/`** — shared public headers (`libvhd.h`, `tapdisk-message.h`,
  `blktap.h`, `list.h`, etc.).

## Conventions

- New/modified files should build cleanly under `-Wall` (test dirs add
  `-Werror`). Recent history shows careful handling of `strncpy`/`safe_strncpy`
  edge cases — prefer the existing safe helpers in `include/util.h`.
- Commit messages in this repo are typically prefixed with a XenServer ticket
  reference (e.g. `CA-395093: ...`).
- When adding an image driver, register it in `drivers/tapdisk-disktype.c` and
  wire its sources into `drivers/Makefile.am`.
- `cscope.out`/`cscope.files` are checked in — `cscope -d` works out of the box
  for symbol navigation.
