/*
 * Tapdisk tracepoint definitions.
 *
 * When built with --enable-lttng, provides LTTng-UST tracepoints.
 * Otherwise, tracepoint() calls compile to nothing.
 *
 * Usage:
 *   Build with: ./configure --enable-lttng && make
 *   Trace with: tools/lttng-tapdisk.sh start
 */

#include "config.h"

#ifdef HAVE_LTTNG
#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER tapdisk
#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "td-tracepoints.h"
#endif

#if !defined(_TD_TRACEPOINTS_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _TD_TRACEPOINTS_H

#ifdef HAVE_LTTNG

#include <lttng/tracepoint.h>
#include <stdint.h>

#define TAPDISK_TP_PROVIDER	tapdisk

/*
 * Emitted when a request is pulled from the shared ring.
 * Correlate with response_push using (queue, req_id).
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	request_pull,	/* name */
	TP_ARGS(
		uint16_t, queue,
		uint64_t, req_id,
		uint8_t, operation,
		uint8_t, nr_segments,
		uint64_t, sector
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(uint8_t, operation, operation)
		ctf_integer(uint8_t, nr_segments, nr_segments)
		ctf_integer(uint64_t, sector, sector)
	)
)

/*
 * Emitted when a response is written to the shared ring.
 * Correlate with request_pull using (queue, req_id).
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	response_push,
	TP_ARGS(
		uint16_t, queue,
		uint64_t, req_id,
		uint8_t, operation,
		uint64_t, sector
		//int, status
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(uint8_t, operation, operation)
		ctf_integer(uint64_t, sector, sector)
		//ctf_integer(int, status, status)
	)
)

/*
 * Emitted when a request is dispatched to the storage driver (read or write).
 * Correlate with request_pull/response_push using req_id.
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	driver_queue,
	TP_ARGS(
		uint16_t, queue,
		uint64_t, req_id,
		int, op,
		uint64_t, sector,
		int, secs
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(int, op, op)
		ctf_integer(uint64_t, sector, sector)
		ctf_integer(int, secs, secs)
	)
)

/*
 * Emitted when the storage driver completes a request.
 * Correlate with driver_queue/request_pull/response_push using req_id.
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	driver_complete,
	TP_ARGS(
		uint16_t, queue,
		uint64_t, req_id,
		int, op,
		uint64_t, sector,
		int, secs
		//int, res
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(int, op, op)
		ctf_integer(uint64_t, sector, sector)
		ctf_integer(int, secs, secs)
		//ctf_integer(int, res, res)
	)
)

/*
 * Emitted around the event channel notification to the guest.
 * phase: 0 = begin (before xenevtchn_notify), 1 = end (after).
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	evtchn_notify,
	TP_ARGS(
		uint16_t, queue,
		int, phase
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(int, phase, phase)
	)
)

/*
 * Emitted when a VBD request is issued from the new_requests queue to the
 * driver chain (moved to pending_requests).
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	vbd_issue,
	TP_ARGS(
		uint16_t, queue,
		uint64_t, req_id,
		int, op,
		uint64_t, sector,
		int, nr_iov
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(int, op, op)
		ctf_integer(uint64_t, sector, sector)
		ctf_integer(int, nr_iov, nr_iov)
	)
)

/*
 * Emitted when a request is enqueued to the qcow2 inflight list
 * (cross-thread handoff to QEMU IOThread).
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	qcow2_enqueue,
	TP_ARGS(
		uint16_t, queue,
		uint64_t, req_id,
		int, op,
		uint64_t, sector,
		int, secs
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(int, op, op)
		ctf_integer(uint64_t, sector, sector)
		ctf_integer(int, secs, secs)
	)
)

/*
 * Emitted when the QEMU IOThread actually submits AIO to the block layer
 * (blk_aio_preadv / blk_aio_pwritev).
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	qcow2_submit,
	TP_ARGS(
		uint16_t, queue,
		uint64_t, req_id,
		int, op,
		uint64_t, sector,
		int, secs
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(int, op, op)
		ctf_integer(uint64_t, sector, sector)
		ctf_integer(int, secs, secs)
	)
)

/*
 * Emitted around the full guest_copy2() call (grant copy + segment handling).
 * phase: 0 = begin, 1 = end.
 * direction: 0 = from guest (write path), 1 = to guest (read completion).
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	guest_copy,
	TP_ARGS(
		uint64_t, req_id,
		int, phase,
		int, direction,
		int, nr_segments
	),
	TP_FIELDS(
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(int, phase, phase)
		ctf_integer(int, direction, direction)
		ctf_integer(int, nr_segments, nr_segments)
	)
)

/*
 * Emitted around the grant-copy ioctl (Xen hypercall).
 * phase: 0 = begin (before ioctl), 1 = end (after ioctl).
 * direction: 0 = from guest (write path), 1 = to guest (read completion).
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	grant_copy,
	TP_ARGS(
		//uint16_t, queue,
		uint64_t, req_id,
		int, phase,
		int, direction,
		int, nr_segments
	),
	TP_FIELDS(
		//ctf_integer(uint16_t, queue, queue)
		ctf_integer(uint64_t, req_id, req_id)
		ctf_integer(int, phase, phase)
		ctf_integer(int, direction, direction)
		ctf_integer(int, nr_segments, nr_segments)
	)
)

/*
 * Emitted around backend submit_all calls (wraps io_submit / lio_listio).
 * phase: 0 = begin, 1 = end.
 * rw: 0 = read-write backend, 1 = read-only backend.
 */
TRACEPOINT_EVENT(
	TAPDISK_TP_PROVIDER,
	aio_submit,
	TP_ARGS(
		uint16_t, queue,
		int, phase,
		int, rw
	),
	TP_FIELDS(
		ctf_integer(uint16_t, queue, queue)
		ctf_integer(int, phase, phase)
		ctf_integer(int, rw, rw)
	)
)

#else /* !HAVE_LTTNG */

#define tracepoint(...)

#endif /* HAVE_LTTNG */

#endif /* _TD_TRACEPOINTS_H */

#ifdef HAVE_LTTNG
#include <lttng/tracepoint-event.h>
#endif
