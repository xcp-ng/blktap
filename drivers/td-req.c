/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <syslog.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifdef __linux__
#include <linux/version.h>
#endif

#include "blktap-xenif.h"
#include "config.h"
#include "debug.h"
#include "td-req.h"
#include "td-blkif.h"
#include "td-ctx.h"
#include "tapdisk-server.h"
#include "tapdisk-metrics.h"
#include "tapdisk-vbd.h"
#include "tapdisk-log.h"
#include "tapdisk.h"
#include "timeout-math.h"
#include "util.h"

#include "td-tracepoints.h"

#ifdef DEBUG
#define BLKIF_MSG_POISON 0xdeadbeef
#endif

#define ERR(blkif, fmt, args...) \
    EPRINTF("%d/%d: "fmt, (blkif)->domid, (blkif)->devid, ##args);

#define TD_REQS_BUFCACHE_EXPIRE 3 // time in seconds
#define TD_REQS_BUFCACHE_MIN    1 // buffers to always keep in the cache

static void
td_xenblkif_bufcache_free(struct td_blkif_queue * const queue);
static inline void
td_xenblkif_bufcache_evt_unreg(struct td_blkif_queue * const queue);

static void
td_xenblkif_bufcache_event(event_id_t id, char mode, void *private)
{
    struct td_blkif_queue *queue = private;

    pthread_mutex_lock(&queue->mutex);
    td_xenblkif_bufcache_free(queue);

    td_xenblkif_bufcache_evt_unreg(queue);
    pthread_mutex_unlock(&queue->mutex);
}

/**
 * Unregister the event to expire the request buffer cache.
 *
 * @param blkif the block interface
 */
static inline void
td_xenblkif_bufcache_evt_unreg(struct td_blkif_queue * const queue)
{
    if (queue->reqs_bufcache_evtid > 0){
        tapdisk_server_unregister_io_event(tapdisk_xenblkif_queue_id(queue),
                                           queue->reqs_bufcache_evtid);
    }
    queue->reqs_bufcache_evtid = 0;
}

/**
 * Register the event to expire the request buffer cache.
 *
 * @param blkif the block interface
 */
static inline void
td_xenblkif_bufcache_evt_reg(struct td_blkif_queue* queue)
{
    queue->reqs_bufcache_evtid =
        tapdisk_server_register_io_event(tapdisk_xenblkif_queue_id(queue),
                                      SCHEDULER_POLL_TIMEOUT,
                                      -1, /* dummy fd */
                                      TV_SECS(TD_REQS_BUFCACHE_EXPIRE),
                                      td_xenblkif_bufcache_event,
                                      queue);
}

/**
 * Free request buffer cache.
 *
 * @param queue  a queue of the block interface
 */
static void
td_xenblkif_bufcache_free(struct td_blkif_queue* queue)
{
    ASSERT(queue);

    while (queue->n_reqs_bufcache_free > TD_REQS_BUFCACHE_MIN){
        munmap(queue->reqs_bufcache[--queue->n_reqs_bufcache_free],
               (size_t)BLKIF_MAX_BUFFER_SEGMENTS_PER_REQUEST << PAGE_SHIFT);
    }
}

/**
 * Get buffer for a request. From cache if available or newly allocated.
 *
 * @param blkif the block interface
 */
static void *
td_xenblkif_bufcache_get(struct td_blkif_queue * const queue)
{
    void *buf;

    ASSERT(queue);

    if (!queue->n_reqs_bufcache_free) {
	    buf = mmap(NULL, (size_t)TD_REQ_BUFFER_SIZE,
                   PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (unlikely(buf == MAP_FAILED))
            buf = NULL;
    } else
        buf = queue->reqs_bufcache[--queue->n_reqs_bufcache_free];

    // If we just got a request, we cancel the cache expire timer
    td_xenblkif_bufcache_evt_unreg(queue);

    return buf;
}

static void
td_xenblkif_bufcache_put(struct td_blkif_queue * const queue, void *buf)
{
    ASSERT(queue);

    if (unlikely(!buf))
        return;

#ifdef DEBUG
    {
	int i;

	for (i = 0; i < queue->n_reqs_bufcache_free; i++)
	    ASSERT(queue->reqs_bufcache[i] != buf);
    }
#endif

    queue->reqs_bufcache[queue->n_reqs_bufcache_free++] = buf;

    /* If we're in low memory mode, prune the bufcache immediately. */
    if (tapdisk_server_mem_mode() == LOW_MEMORY_MODE) {
        td_xenblkif_bufcache_free(queue);
    } else {
        // We only set the expire event when no requests are inflight
        if (queue->n_reqs_free == queue->ring_size)
            td_xenblkif_bufcache_evt_reg(queue);
    }
}

/**
 * Puts the request back to the free list of this block interface.
 *
 * @param blkif the block interface
 * @param req the request to give back
 */
static void
tapdisk_xenblkif_free_request(struct td_blkif_queue * const queue,
			      struct td_xenblkif_req * const req)
{
    bool put_bufcache;

    ASSERT(queue);
    ASSERT(req);
    ASSERT(queue->n_reqs_free < queue->ring_size);

    put_bufcache = req->msg.nr_segments != 0;

#ifdef DEBUG
    memset(&req->msg, BLKIF_MSG_POISON, sizeof(req->msg));
#endif

    queue->reqs_free[queue->ring_size - (++queue->n_reqs_free)] = &req->msg;

    if (likely(put_bufcache))
        td_xenblkif_bufcache_put(queue, req->vma);
}

/**
 * Returns the size, in request descriptors, of the shared ring
 *
 * @param blkif the block interface
 * @returns the size, in request descriptors, of the shared ring
 */
static int
td_blkif_ring_size(const struct td_blkif_queue * const queue)
{
    ASSERT(queue);

    switch (queue->blkif->proto) {
        case BLKIF_PROTOCOL_NATIVE:
            return RING_SIZE(&queue->rings.native);

        case BLKIF_PROTOCOL_X86_32:
            return RING_SIZE(&queue->rings.x86_32);

        case BLKIF_PROTOCOL_X86_64:
            return RING_SIZE(&queue->rings.x86_64);

        default:
            return -EPROTONOSUPPORT;
    }
}

/**
 * Get the response that corresponds to the specified ring index in a H/W
 * independent way.
 *
 * @returns a pointer to the response, NULL on error, sets errno
 *
 * TODO use function pointers instead of switch
 * XXX only called by xenio_blkif_put_response
 */
static inline blkif_response_t *
xenio_blkif_get_response(struct td_blkif_queue* const queue,
			 const RING_IDX rp)
{
    blkif_back_rings_t * const rings = &queue->rings;
    blkif_response_t * p = NULL;

    switch (queue->blkif->proto) {
        case BLKIF_PROTOCOL_NATIVE:
            p = (blkif_response_t *) RING_GET_RESPONSE(&rings->native, rp);
            break;
        case BLKIF_PROTOCOL_X86_32:
            p = (blkif_response_t *) RING_GET_RESPONSE(&rings->x86_32, rp);
            break;
        case BLKIF_PROTOCOL_X86_64:
            p = (blkif_response_t *) RING_GET_RESPONSE(&rings->x86_64, rp);
            break;
        default:
            errno = EPROTONOSUPPORT;
            return NULL;
    }

    return p;
}

/**
 * Puts a response in the ring.
 *
 * @param blkif the VBD
 * @param req the request for which the response should be put
 * @param status the status of the response (success or an error code)
 * @param final controls whether the front-end will be notified, if necessary
 *
 * TODO @req can be NULL so the function will only notify the other end. This
 * is used in the error path of tapdisk_xenblkif_queue_requests. The point is
 * that the other will just be notified, does this make sense?
 */
static int
xenio_blkif_put_response(struct td_blkif_queue * const queue,
			 struct td_xenblkif_req *req, int const status, bool const final)
{
    blkif_common_back_ring_t * const ring = &queue->rings.common;

    if (req) {
        blkif_response_t * msg = xenio_blkif_get_response(queue,
                                                          ring->rsp_prod_pvt);
        if (!msg)
            return -errno;

        ASSERT(status == BLKIF_RSP_EOPNOTSUPP || status == BLKIF_RSP_ERROR
                || status == BLKIF_RSP_OKAY);

        msg->id = req->msg.id;

        msg->operation = req->msg.operation;

        msg->status = status;

        ring->rsp_prod_pvt++;

        tracepoint(tapdisk, response_push,
                   tapdisk_xenblkif_queue_id(queue),
                   req->msg.id, req->msg.operation,
                   req->msg.sector_number/*, status*/);
    }

    if (final) {
        int notify;
        RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(ring, notify);
        if (notify) {
            struct td_xenblkif* const blkif = queue->blkif;
            uint16_t __unused qid = tapdisk_xenblkif_queue_id(queue);

            tracepoint(tapdisk, evtchn_notify, qid, TP_PHASE_BEGIN);
            int err = xenevtchn_notify(queue->ctx->xce_handle, queue->ctx->port);
            tracepoint(tapdisk, evtchn_notify, qid, TP_PHASE_END);

            if (err < 0) {
                err = -errno;
                if (req) {
                    RING_ERR(blkif, "req %lu: failed to notify event channel: "
                            "%s\n", req->msg.id, strerror(-err));
                } else {
                    RING_ERR(blkif, "failed to notify event channel: %s\n",
                            strerror(-err));
                }
                return err;
            }
        }
    }

    return 0;
}


/**
 * Tells whether the request requires data to be read.
 */
static inline bool
blkif_rq_rd(blkif_request_t const * const msg)
{
	return BLKIF_OP_READ == msg->operation;
}


/**
 * Tells whether the request requires data to be written.
 */
static inline bool
blkif_rq_wr(blkif_request_t const * const msg)
{
	return BLKIF_OP_WRITE == msg->operation ||
		(BLKIF_OP_WRITE_BARRIER == msg->operation && msg->nr_segments);
}


/**
 * Tells whether the request requires data to transferred.
 */
static inline bool
blkif_rq_data(blkif_request_t const * const msg)
{
	return blkif_rq_rd(msg) || blkif_rq_wr(msg);
}


static int
guest_copy2(struct td_xenblkif * const blkif, const struct td_xenio_shared_ctx* ctx,
        struct td_xenblkif_req * const req)
{
    int i = 0;
    long err = 0;
    struct ioctl_gntdev_grant_copy gcopy;

    ASSERT(blkif);
    ASSERT(ctx);
    ASSERT(req);
    ASSERT(blkif_rq_data(&req->msg));
    ASSERT(req->msg.nr_segments > 0);
    ASSERT(req->msg.nr_segments <= ARRAY_SIZE(req->gcopy_segs));

    int dir __unused = blkif_rq_wr(&req->msg) ? 0 : 1;
    tracepoint(tapdisk, guest_copy,
               req->msg.id, TP_PHASE_BEGIN, dir, req->msg.nr_segments);

    for (i = 0; i < req->msg.nr_segments; i++) {
        struct blkif_request_segment *blkif_seg = &req->msg.seg[i];
        struct gntdev_grant_copy_segment *gcopy_seg = &req->gcopy_segs[i];

        if (blkif_rq_wr(&req->msg)) {
            /* copy from guest */
            gcopy_seg->dest.virt = req->vma + (i << PAGE_SHIFT)
                + (blkif_seg->first_sect << SECTOR_SHIFT);
            gcopy_seg->source.foreign.ref = blkif_seg->gref;
            gcopy_seg->source.foreign.offset = blkif_seg->first_sect << SECTOR_SHIFT;
            gcopy_seg->source.foreign.domid = blkif->domid;
            gcopy_seg->flags = GNTCOPY_source_gref;
        } else {
            /* copy to guest */
            gcopy_seg->source.virt = req->vma + (i << PAGE_SHIFT)
                + (blkif_seg->first_sect << SECTOR_SHIFT);
            gcopy_seg->dest.foreign.ref = blkif_seg->gref;
            gcopy_seg->dest.foreign.offset = blkif_seg->first_sect << SECTOR_SHIFT;
            gcopy_seg->dest.foreign.domid = blkif->domid;
            gcopy_seg->flags = GNTCOPY_dest_gref;
        }

        gcopy_seg->len = (blkif_seg->last_sect
                - blkif_seg->first_sect
                + 1)
            << SECTOR_SHIFT;
    }
    gcopy.count = req->msg.nr_segments;
    gcopy.segments = req->gcopy_segs;

    tracepoint(tapdisk, grant_copy,
	       req->msg.id, TP_PHASE_BEGIN, dir, req->msg.nr_segments);

    err = -ioctl(ctx->gntdev_fd, IOCTL_GNTDEV_GRANT_COPY, &gcopy);

    tracepoint(tapdisk, grant_copy,
	       req->msg.id, TP_PHASE_END, dir, req->msg.nr_segments);

    if (err) {
        err = -errno;
        RING_ERR(blkif, "failed to grant-copy request %"PRIu64" "
                "(%d segments): %s\n", req->msg.id,
                req->msg.nr_segments, strerror(-err));
        goto out;
    }

	for (i = 0; i < req->msg.nr_segments; i++) {
		struct gntdev_grant_copy_segment *gcopy_seg = &req->gcopy_segs[i];
		if (gcopy_seg->status != GNTST_okay) {
			/*
			 * TODO use gnttabop_error for reporting errors, defined in
			 * xen/extras/mini-os/include/gnttab.h (header not available to
			 * user space)
			 */
			RING_ERR(blkif, "req %lu: failed to grant-copy segment %d: %d\n",
				req->msg.id, i, gcopy_seg->status);
			err = -EIO;
			goto out;
		}
	}

out:
    tracepoint(tapdisk, guest_copy,
               req->msg.id, TP_PHASE_END, dir, req->msg.nr_segments);
    return err;
}


/**
 * Completes a request. If this is the last pending request of a dead block
 * interface, the block interface is destroyed, the caller must not access it
 * any more.
 *
 * @blkif the VBD the request belongs belongs to
 * @req the request to complete
 * @error completion status of the request
 * @final controls whether the other end should be notified
 * @lock must always be true except in this function to control recursion
 */
static bool
tapdisk_xenblkif_complete_request(struct td_blkif_queue * const queue,
		struct td_xenblkif_req* req, int err, const bool final,
		bool lock)
{
	int _err;
	long long *max = NULL, *sum = NULL, *cnt = NULL;
	static _Atomic int depth = 0;
	bool processing_barrier_message;
	bool blkif_destroyed = false;
	uint64_t *ticks = NULL;

	ASSERT(queue);
	ASSERT(req);
	ASSERT(depth >= 0);

	struct td_xenblkif * const blkif = queue->blkif;
	ASSERT(blkif);

	if (lock)
		pthread_mutex_lock(&queue->mutex);
	depth++;

	processing_barrier_message =
		req->msg.operation == BLKIF_OP_WRITE_BARRIER;

	/*
	 * If a barrier request completes, check whether it's an I/O completion
	 * (the barrier carries write I/O data), or a completion because the last
	 * pending non-barrier request completed. If the former case is true, we
	 * need to check again whether the latter is true and proceed with the
	 * completion, otherwise simply note the fact that I/O is done and when
	 * the last pending non-barrier requests completes, this function will be
	 * called again passing the barrier request.
	 */
	if (unlikely(processing_barrier_message)) {
		ASSERT(queue->barrier.msg == &req->msg);
		if (req->msg.nr_segments && !queue->barrier.io_done) {
			queue->barrier.io_err = err;
			queue->barrier.io_done = true;
		}
		if (!tapdisk_xenblkif_barrier_should_complete(queue))
			goto out;
	}

	if (likely(!blkif->dead)) {
		if (blkif_rq_rd(&req->msg)) {
			/*
			 * TODO stats should be collected after grant-copy for better
			 * accuracy
			 */
			if (likely(blkif->stats.xenvbd)) {
				cnt = &blkif->stats.xenvbd->st_rd_cnt;
				sum = &blkif->stats.xenvbd->st_rd_sum_usecs;
				max = &blkif->stats.xenvbd->st_rd_max_usecs;
			}
			blkif->vbd_stats.stats->read_reqs_completed++;
			ticks = &blkif->vbd_stats.stats->read_total_ticks;
			if (likely(!err)) {
				_err = guest_copy2(blkif, queue->ctx->shared, req);
				if (unlikely(_err)) {
					err = _err;
					RING_ERR(blkif, "req %lu: failed to copy from/to guest: "
							"%s\n", req->msg.id, strerror(-err));
				}
			}
		} else if (blkif_rq_wr(&req->msg)) {
			if (likely(blkif->stats.xenvbd)) {
				cnt = &blkif->stats.xenvbd->st_wr_cnt;
				sum = &blkif->stats.xenvbd->st_wr_sum_usecs;
				max = &blkif->stats.xenvbd->st_wr_max_usecs;
			}
			blkif->vbd_stats.stats->write_reqs_completed++;
			ticks = &blkif->vbd_stats.stats->write_total_ticks;
		}

		if (likely(cnt)) {
			struct timeval now;
			long long interval;
			gettimeofday(&now, NULL);
			interval = timeval_to_us(&now) - timeval_to_us(&req->ts);
			*ticks += interval;
			if (interval > *max)
				*max = interval;

			*sum += interval;
			*cnt += 1;
		}

		if (likely(err == 0))
			_err = BLKIF_RSP_OKAY;
		else
			_err = BLKIF_RSP_ERROR;

		xenio_blkif_put_response(queue, req, _err, final);
	}

	tapdisk_xenblkif_free_request(queue, req);

	queue->stats.reqs.out++;
	blkif->stats.reqs.out++;
	if (final) {
		queue->stats.kicks.out++;
		blkif->stats.kicks.out++;
	}

	if (unlikely(processing_barrier_message))
		queue->barrier.msg = NULL;

	/*
	 * Schedule a ring check in case we left requests in it due to lack of
	 * memory or in case we stopped processing it because of a barrier.
	 *
	 * FIXME we should decide whether a ring check is necessary more
	 * intelligently.
	*/
	if (!queue->barrier.msg) {
		if (likely(!blkif->dead))
			tapdisk_xenblkif_sched_chkrng(queue);
	} else {
		/*
		 * If this is the last request, complete the barrier request.
		 */
		if (tapdisk_xenblkif_barrier_should_complete(queue)) {
			blkif_destroyed = tapdisk_xenblkif_complete_request(queue,
					msg_to_tapreq(queue->barrier.msg), 0, 1, false);
			/*
			 * We assert here on "blkif_destroyed == true" because as
			 * "depth > 1" in the recursive call above, the branch to
			 * destroy the ring shouldn't be taken.
			 */
			ASSERT(!blkif_destroyed);
		}
	}

	/*
	 * Last request of a dead ring completes, destroy the ring.
	 */
	if (unlikely(1 == depth
		&& blkif->dead
		&& !tapdisk_xenblkif_reqs_pending(queue))) {

		RING_DEBUG(blkif, "destroying dead ring\n");
		pthread_mutex_unlock(&queue->mutex);
		tapdisk_xenblkif_destroy(blkif);
		blkif_destroyed = true;
		lock = false; /* blkif with its mutex were destroyed above so don't try to unlock it */
	}

out:
	depth--;
	if (lock)
		pthread_mutex_unlock(&queue->mutex);
	return blkif_destroyed;
}

/**
 * Request completion callback, executed when the tapdisk has finished
 * processing the request.
 *
 * @param vreq the completed request
 * @param error status of the request
 * @param token token previously associated with this request
 * @param final controls whether the other end should be notified
 */
static inline void
__tapdisk_xenblkif_request_cb(struct td_vbd_request * const vreq,
        const int error, void * const token, const int final)
{
    struct td_xenblkif_req *req;
    struct td_blkif_queue* queue = token;
    struct td_xenblkif * const blkif = queue->blkif;

    ASSERT(vreq);
    ASSERT(blkif);

    req = container_of(vreq, struct td_xenblkif_req, vreq);

    if (error) {
        pthread_mutex_lock(&queue->mutex);
        if (likely(!blkif->dead)) {
            queue->stats.errors.img++;
            blkif->stats.errors.img++;
            blkif->vbd_stats.stats->io_errors++;
        }
        pthread_mutex_unlock(&queue->mutex);
    }

    tapdisk_xenblkif_complete_request(queue, req, error, final, true);
}


static inline int
tapdisk_xenblkif_parse_request_locked(struct td_blkif_queue* const queue,
        struct td_xenblkif_req * const req)
{
    struct td_xenblkif * const blkif = queue->blkif;
    td_vbd_request_t *vreq;
    int i;
    struct td_iovec *iov;
    void *page, *next, *last;
    int err = 0;
    unsigned nr_sect = 0;

    ASSERT(blkif);
    ASSERT(queue);
    ASSERT(req);

    vreq = &req->vreq;
    ASSERT(vreq);

    req->vma = td_xenblkif_bufcache_get(queue);
    if (unlikely(!req->vma)) {
        err = errno;
        RING_ERR(blkif, "errno %d: invalid vma\n", err);
        goto out;
    }

    for (i = 0; i < req->msg.nr_segments; i++) {
        struct blkif_request_segment *seg = &req->msg.seg[i];

        /*
         * Note that first and last may be equal, which means only one sector
         * must be transferred.
         */
        if (seg->last_sect < seg->first_sect) {
            RING_ERR(blkif, "req %lu: invalid sectors %d-%d\n",
                    req->msg.id, seg->first_sect, seg->last_sect);
            err = EINVAL;
            goto out;
        }
    }

    /*
     * Vectorises the request: creates the struct iovec (in req->iov) that
     * describes each segment to be transferred. Also, merges consecutive
     * segments.
     *
     * In each loop, iov points to the previous scatter/gather element in
     * order to reuse it if the current and previous segments are
     * consecutive.
     */
    iov = req->iov - 1;
    last = NULL;
    page = req->vma;

    for (i = 0; i < req->msg.nr_segments; i++) { /* for each segment */
        struct blkif_request_segment *seg = &req->msg.seg[i];
        size_t size;

        /* TODO check that first_sect/last_sect are within page */

        next = page + (seg->first_sect << SECTOR_SHIFT);
        size = seg->last_sect - seg->first_sect + 1;

        if (next != last) {
            iov++;
            iov->base = next;
            iov->secs = size;
        } else /* The "else" is true if first_sect is 0. */
            iov->secs += size;

        last = iov->base + (iov->secs << SECTOR_SHIFT);
        page += PAGE_SIZE;
        nr_sect += size;
    }

    vreq->iov = req->iov;
    vreq->iovcnt = iov - req->iov + 1;
    vreq->sec = req->msg.sector_number;
#ifdef HAVE_LTTNG
    vreq->req_id = req->msg.id;
#endif

    if (blkif_rq_wr(&req->msg)) {
        err = guest_copy2(blkif, queue->ctx->shared, req);
        if (err) {
            RING_ERR(blkif, "req %lu: failed to copy from guest: %s\n",
                    req->msg.id, strerror(-err));
            goto out;
        }
		if (likely(blkif->stats.xenvbd))
			blkif->stats.xenvbd->st_wr_sect += nr_sect;
		if (likely(blkif->vbd_stats.stats))
			blkif->vbd_stats.stats->write_sectors += nr_sect;
    } else {
		if (likely(blkif->stats.xenvbd))
			blkif->stats.xenvbd->st_rd_sect += nr_sect;
		if (likely(blkif->vbd_stats.stats))
			blkif->vbd_stats.stats->read_sectors += nr_sect;
    }

    vreq->token = queue;
    vreq->cb = __tapdisk_xenblkif_request_cb;

out:
    return err;
}


/**
 * Initialises the standard tapdisk request (td_vbd_request_t) from the
 * intermediate ring request (td_xenblkif_req) in order to prepare it
 * processing.
 *
 * @param blkif the block interface
 * @param req the request to prepare
 * @returns 0 on success
 *
 * XXX only called by tapdisk_xenblkif_queue_request
 */
static inline int
tapdisk_xenblkif_make_vbd_request(struct td_blkif_queue* queue,
        struct td_xenblkif_req * const req)
{
    int err = 0;
    td_vbd_request_t *vreq;
    struct td_xenblkif * const blkif = queue->blkif;
    bool blkif_freed = false;

    ASSERT(queue);
    ASSERT(req);

    vreq = &req->vreq;
    ASSERT(vreq);
    memset(vreq, 0, sizeof(*vreq));

    req->vma = NULL;
    switch (req->msg.operation) {
    case BLKIF_OP_READ:
        if (likely(queue->stats.xenvbd))
                blkif->stats.xenvbd->st_rd_req++;
        if (likely(blkif->vbd_stats.stats))
                blkif->vbd_stats.stats->read_reqs_submitted++;
        req->prot = PROT_WRITE;
        vreq->op = TD_OP_READ;
        break;
    case BLKIF_OP_WRITE:
    case BLKIF_OP_WRITE_BARRIER:
        if (likely(blkif->stats.xenvbd) && req->msg.nr_segments)
		blkif->stats.xenvbd->st_wr_req++;
	if (likely(blkif->vbd_stats.stats) && req->msg.nr_segments)
		blkif->vbd_stats.stats->write_reqs_submitted++;
        req->prot = PROT_READ;
        vreq->op = TD_OP_WRITE;
        break;
    default:
        RING_ERR(blkif, "req %lu: invalid request type %d\n",
                req->msg.id, req->msg.operation);
        err = EOPNOTSUPP;
        goto out;
    }
    /* Timestamp before the requests leave the blkif layer */
    gettimeofday(&req->ts, NULL);

    /*
     * Check that the number of segments is sane.
     */
    if (unlikely((req->msg.nr_segments == 0 &&
                req->msg.operation != BLKIF_OP_WRITE_BARRIER) ||
            req->msg.nr_segments > BLKIF_MAX_BUFFER_SEGMENTS_PER_REQUEST)) {
        RING_ERR(blkif, "req %lu: bad number of segments in request (%d)\n",
                req->msg.id, req->msg.nr_segments);
        err = EINVAL;
        goto out;
    }

    if (likely(req->msg.nr_segments)) {
        pthread_mutex_lock(&queue->mutex);   // XXX: why locking blkif ?
        err = tapdisk_xenblkif_parse_request_locked(queue, req);
        pthread_mutex_unlock(&queue->mutex);
    /*
     * If we only got one request from the ring and that was a barrier one,
     * check whether the barrier requests completion conditions are satisfied
	 * and if they are, complete the barrier request.
     *
     * It could be that there are more requests in the ring after the barrier
     * request, tapdisk_xenblkif_complete_request() will schedule a ring check.
     */
    } else {
        pthread_mutex_lock(&queue->mutex);
        if (tapdisk_xenblkif_barrier_should_complete(queue)) {
            blkif_freed = tapdisk_xenblkif_complete_request(queue,
                    msg_to_tapreq(queue->barrier.msg), 0, 1, false);
            err = 0;
        }
        if (!blkif_freed)
            pthread_mutex_unlock(&queue->mutex);
    }
out:
    return err;
}


/**
 * Queues a ring request, after it prepares it, to the standard tapdisk queue
 * for processing.
 *
 * @param blkif the block interface
 * @param msg the ring request
 * @param req the intermediate request
 *
 * TODO don't really need to supply the ring request since it's either way
 * contained in the req
 *
 * XXX only called by tapdisk_xenblkif_queue_requests
 */
static inline int
tapdisk_xenblkif_queue_request(struct td_blkif_queue * const queue,
        blkif_request_t *msg, struct td_xenblkif_req *req, bool final)
{
    int err;
    bool queue_request;

    ASSERT(queue);
    ASSERT(msg);
    ASSERT(req);

    queue_request = req->msg.nr_segments != 0;

    /*
     * Do not use req after tapdisk_xenblkif_make_vbd_request
     * because this function can release req->msg and reinsert it
     * in the reqs_free array.
     */
    err = tapdisk_xenblkif_make_vbd_request(queue, req);
    if (unlikely(err)) {
        /* TODO log error */
        queue->blkif->stats.errors.map++;
        return err;
    }

	if (likely(queue_request)) {
		td_queue_id_t qid = tapdisk_xenblkif_queue_id(queue);

		// XXX: be careful to matching of queue IDs between blkif and VBD
		err = tapdisk_vbd_queue_request(queue->blkif->vbd, &req->vreq, qid, final);
		if (unlikely(err)) {
			/* TODO log error */
			queue->blkif->stats.errors.vbd++;
			return err;
		}
	}

    return 0;
}


void
tapdisk_xenblkif_queue_requests(struct td_blkif_queue * const queue,
        blkif_request_t *reqs[], const int nr_reqs)
{
    int i;
    int err;
    int nr_errors = 0;
    bool blkif_freed = false;

    ASSERT(reqs);
    ASSERT(nr_reqs >= 0);

    for (i = 0; i < nr_reqs && !blkif_freed; i++) { /* for each request in the ring... */
        blkif_request_t *msg = reqs[i];
        struct td_xenblkif_req *req;

        ASSERT(msg);

        req = msg_to_tapreq(msg);

        ASSERT(req);

        err = tapdisk_xenblkif_queue_request(queue, msg, req, (i == nr_reqs-1));
        if (err) {
            /* TODO log error */
            nr_errors++;
            blkif_freed = tapdisk_xenblkif_complete_request(queue, req, err, 1, true);
        }
    }

    /* there is a possibility of blkif getting freed if ring is 
       dead and current request is the last one, hence adding 
       this check to avoid seg fault */
    if (nr_errors && !blkif_freed) {
	struct td_xenblkif * const blkif = queue->blkif;

	ASSERT(blkif);

        pthread_mutex_lock(&queue->mutex);
        xenio_blkif_put_response(queue, NULL, 0, true);
        pthread_mutex_unlock(&queue->mutex);
    }
}

void
tapdisk_xenblkif_reqs_free(struct td_blkif_queue* queue)
{
    ASSERT(queue);

    td_xenblkif_bufcache_free(queue);
    td_xenblkif_bufcache_evt_unreg(queue);

    free(queue->reqs_bufcache);
    queue->reqs_bufcache = NULL;

    free(queue->reqs);
    queue->reqs = NULL;

    free(queue->reqs_free);
    queue->reqs_free = NULL;

    /* TODO: to be moved */
    pthread_mutex_destroy(&queue->mutex);
}

int
tapdisk_xenblkif_reqs_init(struct td_blkif_queue* queue)
{
    void *buf;
    int i = 0;
    int err = 0;

    ASSERT(queue);

    queue->ring_size = td_blkif_ring_size(queue);
    ASSERT(queue->ring_size > 0);

    queue->reqs =
        calloc(queue->ring_size, sizeof(struct td_xenblkif_req));
    if (!queue->reqs) {
        err = -errno;
        goto fail;
    }

    queue->reqs_free =
        malloc(queue->ring_size * sizeof(blkif_request_t *));
    if (!queue->reqs_free) {
        err = -errno;
        goto fail;
    }

    queue->n_reqs_free = 0;
    for (i = 0; i < queue->ring_size; i++)
        tapdisk_xenblkif_free_request(queue, &queue->reqs[i]);

    // Allocate the buffer cache
    queue->reqs_bufcache = malloc(sizeof(void*) * queue->ring_size);
    if (!queue->reqs_bufcache) {
        err = -errno;
        goto fail;
    }
    queue->n_reqs_bufcache_free = 0;
    queue->reqs_bufcache_evtid = 0;

    // Populate cache with one buffer
    buf = td_xenblkif_bufcache_get(queue);
    td_xenblkif_bufcache_put(queue, buf);
    td_xenblkif_bufcache_evt_unreg(queue);

    return 0;

fail:
    tapdisk_xenblkif_reqs_free(queue);
    return err;
}
