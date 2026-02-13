# Tapdisk LTTng Tracing

LTTng userspace tracepoints let you measure per-request latency at each phase of the I/O path without modifying the running process.

## Build

Install lttng-ust development files, then configure with the flag:

```bash
./configure --enable-lttng && make
```

Without `--enable-lttng`, all `tracepoint()` calls compile to no-ops and no LTTng dependency is needed.

## Tracepoints

Four tracepoints bracket the I/O path. Requests are correlated across all of them via the `req_id` field.

| Tracepoint | Location | Emitted when |
|---|---|---|
| `tapdisk:request_pull` | `td-ctx.c` | Request parsed from blkif ring (+ grant copy for writes) |
| `tapdisk:driver_queue` | `tapdisk-interface.c` | Request handed to storage driver |
| `tapdisk:driver_complete` | `tapdisk-interface.c` | Storage driver signals completion |
| `tapdisk:response_push` | `td-req.c` | Response written to ring (+ grant copy for reads) |

### Fields

All four tracepoints carry:

| Field | Type | Notes |
|---|---|---|
| `req_id` | `uint64_t` | Correlate across tracepoints |
| `operation` / `op` | `uint8_t` / `int` | 0=read, 1=write |
| `sector` | `uint64_t` | Starting sector |
| `nr_segments` / `secs` | `uint8_t` / `int` | Size of the request |

### Latency phases

```
request_pull    : ring parse + grant copy (writes only)
driver_queue    : VBD queuing + driver dispatch
driver_complete : storage backend I/O (qcow2, AIO, ...)
response_push   : grant copy (reads only) + ring response
```

## Capture a trace

```bash
# Install: lttng-tools, lttng-ust (runtime)

# 1. Create session
lttng create tapdisk-trace --output=/tmp/tapdisk-trace

# 2. Enable UST channel and all tapdisk events
lttng enable-channel -u tapdisk-chan
lttng enable-event --userspace 'tapdisk:*' --channel=tapdisk-chan

# 3. Start tracing
lttng start

# 4. Run workload
fio --filename=/dev/xvdb --rw=randread --bs=4k --numjobs=1 --runtime=10 --name=test

# 5. Stop and destroy
lttng stop
lttng destroy tapdisk-trace
```

Trace data is in `/tmp/tapdisk-trace` (CTF format).

## Convert to text

```bash
# babeltrace (v1)
babeltrace /tmp/tapdisk-trace > tapdisk-trace.txt

# or babeltrace2
babeltrace2 /tmp/tapdisk-trace > tapdisk-trace.txt
```

Output lines look like:

```
[13:42:01.123456789] (+0.000012345) hostname tapdisk:request_pull: { req_id = 42, operation = 0, nr_segments = 1, sector = 8192 }
[13:42:01.123461234] (+0.000004445) hostname tapdisk:driver_queue: { req_id = 42, op = 0, sector = 8192, secs = 8 }
[13:42:01.124100000] (+0.000638766) hostname tapdisk:driver_complete: { req_id = 42, op = 0, sector = 8192, secs = 8 }
[13:42:01.124105678] (+0.000005678) hostname tapdisk:response_push: { req_id = 42, operation = 0, sector = 8192 }
```

## Filter specific events

```bash
# Enable only request boundaries
lttng enable-event --userspace 'tapdisk:request_pull' --channel=tapdisk-chan
lttng enable-event --userspace 'tapdisk:response_push' --channel=tapdisk-chan

# Enable all tapdisk events (wildcard)
lttng enable-event --userspace 'tapdisk:*' --channel=tapdisk-chan
```

## Troubleshooting

**No events captured** — tapdisk must be built with `--enable-lttng` and the lttng session must be started *before* tapdisk processes the first request (UST registers on library load; events fired before the session attaches are lost).

**`lttng enable-event` fails with "UST channel not found"** — create the channel explicitly before enabling events (`lttng enable-channel -u tapdisk-chan`). Required on lttng-tools < 2.5.

**tapdisk crashes on start** — lttng-ust runtime library not installed; install `lttng-ust` (not just `lttng-tools`).
