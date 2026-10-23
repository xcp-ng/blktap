# Signal-controlled completion delay test

Test-only patch for blktap 87f2722026df184c3ca595b5837ab3faba86f243.
Build/install tapdisk normally, restart the target tapdisk with the patched
binary, and compile tools/tapdisk-delay.c separately:

    cc -Wall -Wextra -o tapdisk-delay tools/tapdisk-delay.c
    tap-ctl list
    ./tapdisk-delay PID MINOR on
    ./tapdisk-delay PID MINOR off

Use PID and MINOR from the SAME tap-ctl list row for the selected disk.
The helper queues SIGRTMIN (on) or SIGRTMIN+1 (off) with the minor as payload.
Plain kill without a queued payload is ignored. Existing USR signals retain
their existing meanings. Do not send these signals to an unpatched tapdisk.

The mode starts disabled. On enables only the chosen VBD, across its rings.
The first successful read completion is held, then a seeded per-VBD PRNG
skips 1..64 eligible reads between holds. Hold times vary from 5..15 seconds.
Only one completion per VBD is held at a time. Re-enabling resets the seed
and countdown; it does not extend a hold already in progress. Concurrent
request ordering still affects which requests are selected.

No disk descriptions or XenStore keys are required. Selection follows the
existing VBD through ring reconnect / image reopen, until off or VBD teardown.
A newly created VBD starts disabled even if its numeric minor is reused.

Watch tapdisk logging for completion-delay: held / release. Issue the guest
reset or snapshot transition while held is logged and before release. The
timer keeps tapdisk's event loop responsive. Off prevents new holds; any
already held completion keeps its original expiry. Normal disconnect retains
the outstanding ring; its eventual completion follows the existing dead-ring
path. Forced VBD teardown cancels the test timer.

This delays completion before read grant-copy and response publication. It
can exercise synthetic responses, poisoned-ring reconnect, and retained-ring
grant revocation. It does not deliberately pin a data grant and does not
guarantee data corruption. Check checksums separately. Pause alone is not
required, and independently timed snapshots do not guarantee overlap: use
the held log as the synchronization point.

This patch has not been built or run against Xen in the Windows workspace.
