# tdtest: test-only hooks for one tapdisk VBD

Test-only patch series on top of blktap `87f2722` (v3.55.5). It replaces
the earlier signal-controlled completion-delay patch, and keeps its design:
nothing is enabled by default, and one disk is addressed at a time by a
queued real-time signal sent to one tapdisk PID with the minor as payload.

It exists to reproduce, and to measure fixes for, the hazards described in
win-pvdrivers `docs/tapdisk-dead-ring-late-writes.md` and
`docs/xenvbd-shutdown-ordering.md`. **Do not ship it.** tapback is not
modified.

Code: `drivers/td-test.c` and `drivers/td-test.h`, plus one-line hooks
guarded by `if (unlikely(vbd->tdtest))` in `td-ctx.c`, `td-req.c`,
`td-blkif.c`, `tapdisk-vbd.c` and `tapdisk-server.c`. A VBD that has never
accepted a configuration has `vbd->tdtest == NULL` and takes no other new
path. The helper is `tools/tdtest-ctl.c`.

## Build

`make` builds `drivers/tapdisk` and `tools/tdtest-ctl`; the latter is a
`noinst_PROGRAMS` and is never installed. `configure.ac` and the
`Makefile.am` files change, so regenerate with `./autogen.sh` (or
`autoreconf -fi`) before `configure`.

The banner shows a build identity: `git describe --always --tags --dirty`
of the source tree if it is a git checkout, otherwise `nogit`. Override it
with `make TDTEST_VARIANT=<string>`. The build date and time are always
printed too.

## Control

    tdtest-ctl PID MINOR reload|off

Take `PID` and `MINOR` from the same `tap-ctl list` row, the one whose path
contains the test VDI UUID, immediately before each call. `tdtest-ctl`
queues `SIGRTMIN` (reload) or `SIGRTMIN+1` (off) with `sigqueue()` and the
minor as the value.

- A signal that was not queued (plain `kill`), or whose value is not a
  minor in 0..65535, is ignored: `tdtest: config rejected minor=none ...`
  (or `off rejected`).
- An unknown minor: `tdtest: config rejected minor=N reason=unknown-minor`.
- Never send these signals to an unpatched tapdisk: their default action
  terminates the process.

### reload

Reads `/run/tapdisk-test/<minor>.conf`. Each reload is a complete
specification: keys that are absent take their default.

The file is accepted only if it parses, contains `vdi=<uuid>`, and the
VBD's image name (`vbd->name`, e.g. `vhd:/var/run/sr-mount/<sr>/<vdi>.vhd`)
contains that UUID. This guards against minor reuse. On acceptance:

1. On the first accepted configuration of a VBD, its tdtest state is
   allocated and the banner `tdtest: v1 variant=... built=...` is logged.
2. The configuration is installed and the hold counters are reset.
3. `tdtest: config minor=... vdi=... seq=... <all keys> name=...` is logged.
4. `intake_pause` is applied, the gate is reopened if `fix_gate_new_ring=0`,
   and the dead rings are kicked if `dead_chkrng_kick=1`, in that order.

A rejected file logs `tdtest: config rejected minor=N reason=...`. If the
VBD was already addressed, it is then switched off as for `off`
(`tdtest: off ... reason=config-rejected`). A VBD that was never addressed
stays untouched.

### off

`off` disables new holds, all logging and both fixes for that VBD:

- Requests that are already held keep their timers and are released
  normally when those expire; their `release` lines are still logged.
- Paused rings are unpaused. Live rings get a ring check; dead rings do not
  (see `intake_pause`).
- The R3' gate is reopened, with a ring check on every live ring.
- Events already unregistered by R2 stay unregistered: that ring is dead and
  will only be destroyed.
- The state stays allocated until the VBD is freed, so a later `reload` works.

Logs `tdtest: off minor=N seq=<last seq> reason=off`, or
`tdtest: off minor=N reason=not-addressed` if there was nothing to switch off.

## Configuration file

One `key=value` per line. Blank lines and lines starting with `#` are
ignored; spaces around keys and values are trimmed. Unknown keys, bad values
and lines longer than 254 characters reject the whole file. Numbers are decimal, or
hexadecimal with `0x`. Windows are in 512-byte sectors as in blkif
`sector_number`, half-open `[start, end)`. A request is in a window if any
of its sectors is; a request without data is in it if its `sector_number`
is.

| Key | Default | Meaning |
|---|---|---|
| `vdi` | required | VDI UUID (8-4-4-4-12 hex). Must appear in the VBD's image name. |
| `seq` | `none` | Free token without spaces, up to 64 characters, echoed in the `config` and `off` lines. Bump it on every write so the harness can wait for its own acknowledgement. |
| `log` | `0` | `1` logs the data events (`intake`, `copyin`, `copyback`, `copyback-skipped-dead`, `complete`) for requests in the log window. |
| `log_lba_start`, `log_lba_end` | `0`, 2^64-1 | Log window. |
| `read_hold_ms` | `0` | Hold successful reads in the read window for this long, 0..120000 ms, before their copy-back and response. 0 disables read holds. |
| `read_hold_lba_start`, `read_hold_lba_end` | `0`, 2^64-1 | Read hold window. |
| `read_hold_max` | `1` | Holds to start after this reload. `0` means unlimited. Holds may overlap. |
| `write_hold_ms` | `0` | Hold writes from xenvbd rings in the write window for this long, 0..120000 ms, before they are queued to the image. 0 disables write holds. |
| `write_hold_lba_start`, `write_hold_lba_end` | `0`, 2^64-1 | Write hold window. |
| `write_hold_max` | `1` | As `read_hold_max`, for writes. |
| `intake_pause` | `0` | `current`: the rings live now stop taking requests off their shared ring. `0`: no ring is paused. |
| `dead_chkrng_kick` | `0` | `1`: on this reload, schedule a ring check on every dead ring of this VBD. One-shot per reload. |
| `fix_dead_intake` | `0` | `1`: remediation R2. |
| `fix_gate_new_ring` | `0` | `1`: remediation R3'. |

Notes:

- Hold counters restart at every accepted reload. A reload that rewrites the
  file for another reason re-arms `*_hold_max` more holds; set
  `*_hold_ms=0` when they are no longer wanted.
- `dead_chkrng_kick=1` kicks again at every reload that still contains it.
- `intake_pause=current` takes a snapshot only if no ring of the VBD is
  paused. If rings are already paused by an earlier reload, the set is left
  as it is, so rewriting the file with `intake_pause=current` while changing
  other keys does not pause a ring connected in between. To take a new
  snapshot, reload with `intake_pause=0` first. A paused ring stays paused
  when it becomes dead. A ring connected later is not paused. Paused is a
  flag on the ring itself, not a stored pointer, so a new ring allocated at
  a freed ring's address is never paused by mistake.
- Unpausing schedules a ring check on live rings only. A dead ring that is
  unpaused is checked again by its own chkrng fallback timer (8 s), unless
  R2 unregistered it, or at once with `dead_chkrng_kick=1` in the same
  reload. The unpause happens before the kick.

## What the hooks do

All of them require an accepted, not switched-off configuration, except
`intake_pause` (a ring flag, cleared by `off`) and releases of holds that
already exist.

**Intake** (`td-ctx.c`, `tapdisk_xenio_ctx_process_ring`, before anything is
read from the shared ring): returns 0 for a paused ring, for a dead ring
with R2, and for a live ring while the R3' gate is closed. After requests
are taken off the ring, logs `ev=dead-intake` for each request of a dead
ring (always), or `ev=intake` for each request in the log window.

**Write data** (`td-req.c`, `tapdisk_xenblkif_parse_request`, after
`guest_copy2`): `ev=copyin` with the CRC of the data copied from the guest,
or `ev=copyin-failed err=-errno`. Logged in the log window, and always for a
dead ring.

**Write hold** (`tapdisk-vbd.c`, `tapdisk_vbd_issue_request`, `TD_OP_WRITE`):
only requests named `xenvbd-*` (i.e. from a blkif ring), with at most
`BLKIF_MAX_SEGMENTS_PER_REQUEST` iovs, and not while the VBD mirrors to a
secondary (the mirror write has already been queued at that point). All the
iovs of the request are copied by value into the hold and queued with
`td_queue_write(treq.image, treq)` when the timer expires. The sectors stay
counted in `secs_pending` and the request stays on `pending_requests`, so
pause, close and the implicit disconnect on close wait for it. A write that
failed and is retried goes through `tapdisk_vbd_issue_request` again and can
be held again if the count allows. Logs `ev=hold` and `ev=release`, both
with the ring's `dead` state at that moment and the CRC of the data.

**Read hold** (`td-req.c`, top of `__tapdisk_xenblkif_request_cb`): a
successful read of a live ring in the read window is taken out of the
completion path. On release, it is completed as a final completion
(response published and pushed). Logs `ev=hold` and `ev=release`.

**Read copy-back** (`tapdisk_xenblkif_complete_request`): `ev=copyback`
with the CRC of the data copied to the guest, or `ev=copyback-failed
err=-errno`; on a dead ring, where the copy-back is skipped,
`ev=copyback-skipped-dead`. Then `ev=complete` for every request, with
`rsp=none` on a dead ring. Log window only.

**Rings** (`td-blkif.c`): `ev=connect`, `ev=disconnect pending=N` (the ring
is dead afterwards if N > 0), and `ev=dead-destroy` when the last request of
a dead ring completes. Always logged.

**`dead_chkrng_kick`**: `tapdisk_xenblkif_sched_chkrng` on every ring in the
VBD's `dead_rings`, skipping rings whose chkrng event R2 unregistered.

### R2, `fix_dead_intake=1`

Stops intake on dead rings, two ways:

- `tapdisk_xenio_ctx_process_ring` returns 0 for a dead ring before reading
  the shared ring (logged once per ring as `ev=dead-intake-blocked`). This
  also covers rings that died before R2 was switched on.
- In `tapdisk_xenblkif_disconnect`, when the ring is made dead with requests
  pending, its `chkrng_event` and `stoppolling_event` are unregistered and
  set to -1 (`ev=r2-unregister`), so that `tapdisk_xenblkif_destroy` does
  not unregister them a second time. If the ring was polling, polling is
  stopped and the context's event-channel event, which polling masks and
  which the ring's stoppolling event would otherwise have unmasked, is
  unmasked; it is shared with the other rings of the context, including a
  reconnected ring.

Nothing else touches those events on a dead ring: completions schedule a
ring check only for live rings, suspend and resume only walk live rings, and
`dead_chkrng_kick` skips a ring whose event is -1.

### R3', `fix_gate_new_ring=1`

A tapdisk-only stand-in for blkback's refusal to reconnect while old I/O is
pending. While a dead ring of the VBD still has a write anywhere in the VBD
queues (new, pending including held, failed or completed-not-yet-returned),
the live rings of the VBD take no requests: `process_ring` returns 0. In
practice the live ring is the newly connected one, since tapdisk only
accepts one live ring per domid/devid.

- The gate state is logged on transitions: `ev=gate-close` with the ring
  that found it closed, and `ev=gate-open reason=...`.
- While closed, the gated ring re-checks every 100 ms by rearming its chkrng
  event, so the gate also reopens when the dead rings only have reads left.
  If the gated ring was polling, the end of polling resets that event to the
  usual 8 s fallback once, so one re-check can take up to 8 s.
- When the last dead ring is destroyed (end of
  `tapdisk_xenblkif_complete_request`), the gate is reopened and every live
  ring gets `tapdisk_xenblkif_sched_chkrng`.
- `off`, a rejected reload, or a reload with `fix_gate_new_ring=0` also
  reopen it.

The upstream form of this fix is blkback's: refuse the connect while dead
rings have pending writes, with tapback retrying it. tapback serves every
VM on the host, so it is out of scope here.

## Lifetime and teardown

- A read hold points at its ring and its ring request. The request is still
  outstanding, so the ring is not destroyed, including across a normal
  disconnect, until the hold is released.
- A write hold points at its `td_vbd_request_t` (inside the ring request)
  and at the images of its iovs. The request stays on `pending_requests`
  with sectors pending, so `tapdisk_vbd_pause`, `tapdisk_vbd_close`, the
  shutdown path and the close path's wait for dead rings all wait for it,
  and the images stay open. Only a forced shutdown closes the images with
  requests pending: `tapdisk_vbd_close_vdi` therefore first fails any held
  write with `-EIO` through `td_complete_request`, while the images and the
  stats are still valid (`ev=release-cancelled reason=close-vdi`).
- `tapdisk_vbd_free` unregisters the timers of anything still held and
  frees the state (`ev=release-cancelled reason=vbd-free`). A read held at
  that point is dropped with its ring, as a forced shutdown already does
  with dead rings.
- Timers unregister themselves when they fire. A timer cancelled while its
  event is already pending in the same scheduler pass is skipped, because
  the scheduler does not run events marked dead.
- Release callbacks run in the main thread. A read release takes the VBD
  mutex, as `tapdisk_vbd_kick` does around request callbacks. A write
  release holds no lock while it queues the iovs, as
  `tapdisk_vbd_issue_request` does, and does not touch the request after
  the last iov is queued.

## Log format

All lines go through `syslog(LOG_WARNING, "tdtest: ...")`. tapdisk opens
syslog with ident `tapdisk[<pid>]` and facility `daemon`, so on XCP-ng they
appear in `/var/log/daemon.log` as

    <date> <host> tapdisk[<pid>]: tdtest: ev=...

These are synchronous `syslog(3)` calls, like tapdisk's own `DPRINTF` and
`EPRINTF` lines (e.g. `destroying dead ring`, `failed to grant-copy`), so
they are ordered with them. Lines logged through tapdisk's buffered
`tlog_syslog` may appear out of order with respect to them. Each event line
carries `t=<seconds>.<microseconds>` (gettimeofday) for finer ordering.

Event lines start with

    ev=<ev> t=<s.us> minor=<minor>

Ring events and request events add

    ring=<%p> dead=<0|1> domid=<d> devid=<d>

and request events add

    id=<u64> op=<read|write|barrier|other> sector=<u64> nsect=<u> nsegs=<u> grefs=<g,g,...|->

`id` is the guest's request id, in decimal. `grefs` are decimal, one per
segment. `nsegs` is the descriptor's raw `nr_segments`; `grefs` and `nsect`
cover at most 11 segments. `crc=<%08x>` is zlib `crc32` over the request's
data as one buffer in LBA order (each segment's payload in `req->vma`,
chained).

| `ev=` | Extra fields | When |
|---|---|---|
| `intake` | | request taken off a live ring, in the log window |
| `dead-intake` | | request taken off a dead ring, always |
| `copyin` | `crc` | write data copied from the guest |
| `copyin-failed` | `err` | grant copy from the guest failed |
| `hold` | `kind=read\|write hold_ms held_ms=0 crc` | hold started |
| `release` | `kind hold_ms held_ms crc` | hold expired, `dead` as of now |
| `release-cancelled` | `kind ... reason=close-vdi\|vbd-free` | hold cancelled by teardown (the `vbd-free` form only has `ring=<%p> kind reason`) |
| `copyback` | `crc` | read data copied to the guest |
| `copyback-failed` | `err` | grant copy to the guest failed |
| `copyback-skipped-dead` | `err [crc]` | read completed on a dead ring |
| `complete` | `err rsp=okay\|error\|eopnotsupp\|none final [crc]` | request completed; `rsp=none` on a dead ring |
| `connect` | | ring connected |
| `disconnect` | `pending=N` | ring disconnected |
| `dead-destroy` | | last request of a dead ring completed, ring destroyed next |
| `dead-kick` | `[skipped=chkrng-unregistered]` | `dead_chkrng_kick` |
| `dead-intake-blocked` | | R2 refused intake on a dead ring (once per ring) |
| `r2-unregister` | `chkrng=<id> stoppolling=<id> polling=<0\|1>` | R2 at disconnect |
| `intake-pause`, `intake-resume` | | ring paused or unpaused |
| `gate-close` | `reason=dead-ring-writes` | R3' gate closed |
| `gate-open` | `reason=...` (no ring fields) | R3' gate reopened |

`err` is the error passed to the completion: 0, a negative errno from the
image or from a grant copy, or a positive errno (`EINVAL`, `EOPNOTSUPP`) for
a request rejected when it was taken off the ring.

Control lines:

    tdtest: v1 variant=<describe> built=<date>_<time>
    tdtest: config minor=<m> vdi=<uuid> seq=<seq> log=... name=<image>
    tdtest: config rejected minor=<m|none> reason=<reason>
    tdtest: off minor=<m> seq=<seq> reason=<off|config-rejected>
    tdtest: off minor=<m> reason=not-addressed
    tdtest: off rejected minor=<m|none> reason=<reason>

## Limitations

- A read hold takes the request out of a `tapdisk_vbd_kick` batch. If the
  held read was the batch's final completion, the responses of the earlier
  requests of that batch (completed with `final=0`) are written to the ring
  but not pushed, and the frontend is not notified, until something else
  pushes on that ring: the release, or any later completion. The `complete`
  lines show `final=0` for them.
- Holds are limited to 120 s, tapdisk's default request timeout.
- tapdisk's watchdog may log `watchdog timeout: pending requests idle` while
  a write is held for 10 s or more. That is expected.
- Completions of TD_DRIVER_THREADED images (qcow2) run in a driver thread.
  The hold lists, counters and configuration are locked, but the walk of
  the live rings when the last dead ring is destroyed is not, like the
  existing ring list handling in that path. Test with VHD images.
- The configuration file is read in the main loop when the signal is
  handled. Write it atomically (temporary file and `rename`) before
  signalling.
- `release` of a write logs before the iovs are queued. The write's own
  completion is logged by `complete` if it is in the log window.
- Only blkif rings are covered; NBD clients of the same VBD are never held
  and never logged.
