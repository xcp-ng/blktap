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

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <alloca.h>

#include "blktap-xenif.h"
#include "config.h"
#include "debug.h"
#include "tapdisk-server.h"
#include "td-ctx.h"
#include "tapdisk-log.h"
#include "timeout-math.h"
#include "util.h"

#include "td-tracepoints.h"

#define ERROR(_f, _a...)           tlog_syslog(TLOG_WARN, "td-ctx: " _f, ##_a)

struct list_head _td_xenio_ctxs = LIST_HEAD_INIT(_td_xenio_ctxs);

/**
 * TODO releases a pool?
 */
static void
tapdisk_xenio_shared_ctx_close(struct td_xenio_shared_ctx * const ctx)
{
    if (!ctx || ctx->count_ref)
        return;

    if (ctx->xcg_handle) {
        xengnttab_close(ctx->xcg_handle);
        ctx->xcg_handle = NULL;
    }

    if (ctx->gntdev_fd != -1) {
        close(ctx->gntdev_fd);
        ctx->gntdev_fd = -1;
    }

    list_del(&ctx->entry);

    free(ctx);
}

#define blkif_get_req(dst, src)                 \
{                                               \
    int i, n = BLKIF_MAX_SEGMENTS_PER_REQUEST;  \
    dst->operation = src->operation;            \
    dst->nr_segments = src->nr_segments;        \
    dst->handle = src->handle;                  \
    dst->id = src->id;                          \
    dst->sector_number = src->sector_number;    \
    xen_rmb();                                  \
    if (n > dst->nr_segments)                   \
        n = dst->nr_segments;                   \
    for (i = 0; i < n; i++)                     \
        dst->seg[i] = src->seg[i];              \
}

/**
 * Utility function that retrieves a request using @idx as the ring index,
 * copying it to the @dst in a H/W independent way.
 *
 * @param blkif the block interface
 * @param dst address that receives the request
 * @param rc the index of the request in the ring
 */
static inline void
xenio_blkif_get_request(struct td_blkif_queue * const queue,
			blkif_request_t *const dst, const RING_IDX idx)
{
    blkif_back_rings_t * rings;

    ASSERT(queue);
    ASSERT(dst);

    rings = &queue->rings;

    switch (queue->blkif->proto) {
        case BLKIF_PROTOCOL_NATIVE:
            {
                blkif_request_t *src;
                src = RING_GET_REQUEST(&rings->native, idx);
                memcpy(dst, src, sizeof(blkif_request_t));
                break;
            }

        case BLKIF_PROTOCOL_X86_32:
            {
                blkif_x86_32_request_t *src;
                src = RING_GET_REQUEST(&rings->x86_32, idx);
                blkif_get_req(dst, src);
                break;
            }

        case BLKIF_PROTOCOL_X86_64:
            {
                blkif_x86_64_request_t *src;
                src = RING_GET_REQUEST(&rings->x86_64, idx);
                blkif_get_req(dst, src);
                break;
            }

        default:
            /*
             * TODO log error
             */
            ASSERT(0);
    }
}

/**
 * Retrieves at most @count request descriptors from the ring, up to the
 * first barrier request, copying them to @reqs.
 *
 * @param blkif the block interface
 * @param reqs array of pointers where each element points to sufficient memory
 * space that receives each request descriptor
 * @param count retrieve at most that many request descriptors
 * @returns the number of retrieved request descriptors
 *
 *  XXX only called by xenio_blkif_get_requests
 */
static inline int
__xenio_blkif_get_requests(struct td_blkif_queue * const queue,
        blkif_request_t *reqs[], const unsigned int count)
{
    blkif_common_back_ring_t * ring;
    RING_IDX rp, rc;
    unsigned int n;
    bool barrier;

    ASSERT(queue);
    ASSERT(reqs);

    if (!count)
        return 0;

    ring = &queue->rings.common;

    rp = ring->sring->req_prod;
    xen_rmb(); /* Ensure we see queued requests up to 'rp'. */

    for (rc = ring->req_cons, n = 0, barrier = false;
         rc != rp && n < count && !barrier;
         rc++, n++)
    {
        blkif_request_t *dst = reqs[n];

        xenio_blkif_get_request(queue, dst, rc);

        tracepoint(tapdisk, request_pull,
            tapdisk_xenblkif_queue_id(queue),
            dst->id, dst->operation, dst->nr_segments,
            dst->sector_number);

        if (unlikely(dst->operation == BLKIF_OP_WRITE_BARRIER))
            barrier = true;
    }

    ring->req_cons = rc;

    return n;
}

/**
 * Retrieves at most @count request descriptors.
 *
 * @param blkif the block interface
 * @param reqs array of pointers where each pointer points to sufficient
 * memory to hold a request descriptor
 * @count maximum number of request descriptors to retrieve
 * @param final re-enable notifications before it stops reading
 * @returns the number of request descriptors retrieved
 *
 * TODO change name
 */
static inline int
xenio_blkif_get_requests(struct td_blkif_queue * const queue,
        blkif_request_t *reqs[], const int count, const bool final)
{
    blkif_common_back_ring_t * ring;
    int n = 0;
    bool work = false;

    ASSERT(queue);
    ASSERT(reqs);

    ring = &queue->rings.common;

    do {
        if (final)
            RING_FINAL_CHECK_FOR_REQUESTS(ring, work);
        else
            work = RING_HAS_UNCONSUMED_REQUESTS(ring);

        if (!work)
            break;

        if (n >= count)
            break;

        n += __xenio_blkif_get_requests(queue, reqs + n, count - n);

        if (unlikely(n && reqs[(n - 1)]->operation == BLKIF_OP_WRITE_BARRIER))
            break;

    } while (1);

    return n;
}

int
tapdisk_xenio_ctx_process_ring(struct td_blkif_queue *queue, bool final)
{
    struct td_xenblkif *blkif = queue->blkif;
    int n_reqs;
    int start;
    blkif_request_t **reqs;
    int limit;

    pthread_mutex_lock(&queue->mutex);
    start = queue->n_reqs_free;

    if (unlikely(queue->barrier.msg)) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }

    /*
     * In each iteration, copy as many request descriptors from the shared ring
     * that can fit within the constraints.
     * If there's memory available, use as many requests as available.
     * If in low memory mode, don't copy any if there's some in flight.
     * Otherwise, only copy one.
     */
    if (tapdisk_server_mem_mode() == LOW_MEMORY_MODE)
        limit = queue->ring_size != queue->n_reqs_free ? 0 : 1;
    else
        limit = queue->n_reqs_free;

    do {
        reqs = &queue->reqs_free[queue->ring_size - queue->n_reqs_free];

        ASSERT(reqs);

        n_reqs = xenio_blkif_get_requests(queue, reqs, limit, final);
        ASSERT(n_reqs >= 0);
        if (!n_reqs)
            break;

        queue->n_reqs_free -= n_reqs;
        ASSERT(queue->n_reqs_free <= queue->ring_size);
        limit -= n_reqs;
        final = 1;

        if (unlikely(reqs[(n_reqs - 1)]->operation ==
                     BLKIF_OP_WRITE_BARRIER)) {
            ASSERT(!queue->barrier.msg);
            queue->barrier.msg = reqs[(n_reqs - 1)];
            queue->barrier.io_done = false;
            queue->barrier.io_err = 0;
            break;
        }
    } while (1);

    n_reqs = start - queue->n_reqs_free;

    if (!n_reqs) {
        /*
         * We got a notification but the ring is empty. This is because we had
         * previously suspended the operation of the ring because of a
         * VBD.pause but when we completed a request prior to the pause we
         * checked the ring for new requests. If there were request in the ring
         * at that time, we consumed them but we did not consume the
         * notification. This notification is the one we should have consumed,
         * and can be ignored.
         */
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }

    if (queue->in_polling)
        /* We found at least one request, so keep polling some more */
        tapdisk_xenblkif_sched_stoppolling(queue);
    else if (blkif->poll_duration)
        /* We weren't polling, but polling is enabled, so let's start now */
        tapdisk_start_polling(queue);

    blkif->stats.reqs.in += n_reqs;

    reqs = alloca(sizeof(blkif_request_t*) * n_reqs);
    memcpy(reqs, &queue->reqs_free[queue->ring_size - start],
           sizeof(blkif_request_t*) * n_reqs);
    pthread_mutex_unlock(&queue->mutex);

    tapdisk_xenblkif_queue_requests(queue, reqs, n_reqs);

    return n_reqs;
}

/**
 * Callback executed when there is a request descriptor in the ring. Copies as
 * many request descriptors as possible (limited by local buffer space) to the
 * td_blkif's local request buffer and queues them to the tapdisk queue.
 */
static inline void
tapdisk_xenio_ctx_ring_event(event_id_t id __attribute__((unused)),
        char mode __attribute__((unused)), void *private)
{
    struct td_blkif_queue *queue = private;
    xenevtchn_port_or_error_t port;
    int err;

    /* Ignore initial spurious event */
    /* XXX: should we have a mecanism to postpone event ?
            What will happen if it is never triggered again ?
            This is might be safe because keeps triggering until
            xenevtchn_pending() is called */
    if (queue->ctx == NULL)
        return;

    ASSERT(queue);


    /*
     * Get the local port for which there is a pending event.
     */
    port = xenevtchn_pending(queue->ctx->xce_handle);
    if (port == -1) {
        /* TODO log error */
        return;
    }

    err = xenevtchn_unmask(queue->ctx->xce_handle, port);
    if (err) {
        /* TODO log error */
        return;
    }

    queue->stats.kicks.in++;
    queue->blkif->stats.kicks.in++;

    tapdisk_xenio_ctx_process_ring(queue, false);
}

/* NB. may be NULL, but then the image must be bouncing I/O */
#define TD_XENBLKIF_DEFAULT_POOL "td-xenio-default"

/**
 * Opens a context on the specified pool.
 *
 * @param pool_name the pool, it can either be NULL or a non-zero length string
 * @returns 0 in success, -errno on error
 *
 * TODO The pool is ignored, we always open the default pool.
 */
static inline int
tapdisk_xenio_shared_ctx_open(const char *pool_name)
{
    struct td_xenio_shared_ctx *ctx;
    int err;

    /* zero-length pool names are not allowed */
    if (pool_name && !strlen(pool_name))
        return -EINVAL;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        err = -errno;
        ERROR("cannot allocate memory");
        goto fail;
    }

    ctx->count_ref = 0;
    ctx->gntdev_fd = -1;
    ctx->pool_name = TD_XENBLKIF_DEFAULT_POOL;
    INIT_LIST_HEAD(&ctx->blkifs);
    list_add(&ctx->entry, &_td_xenio_ctxs);

    ctx->gntdev_fd = open("/dev/xen/gntdev", O_NONBLOCK);
    if (ctx->gntdev_fd == -1) {
        err = -errno;
        ERROR("failed to open the grant device: %s\n", strerror(-err));
        goto fail;
    }

    ctx->xcg_handle = xengnttab_open(NULL, 0);
    if (!ctx->xcg_handle) {
        err = -errno;
        ERROR("failed to open the grant table driver: %s\n",
                strerror(-err));
        goto fail;
    }

    return 0;

fail:
    tapdisk_xenio_shared_ctx_close(ctx);
    return err;
}


/**
 * Tells whether @ctx belongs to @pool.
 *
 * If no @pool is not specified and a default pool is set, @ctx is compared
 * against the default pool. Note that NULL is valid pool name value.
 */
static inline int
__td_xenio_ctx_match(struct td_xenio_shared_ctx * ctx, const char *pool_name)
{
        if (unlikely(!pool_name)) {
                assert(TD_XENBLKIF_DEFAULT_POOL);
                return !strcmp(ctx->pool_name, TD_XENBLKIF_DEFAULT_POOL);
        }

        return !strcmp(ctx->pool_name, pool_name);
}

#define tapdisk_xenio_find_ctx(_ctx, _cond)     \
    do {                                        \
        bool found = false;                     \
        tapdisk_xenio_for_each_ctx(_ctx) {      \
            if (_cond) {                        \
                found = true;                   \
                break;                          \
            }                                   \
        }                                       \
        if (!found)                             \
            _ctx = NULL;                        \
    } while (0)

int
tapdisk_xenio_shared_ctx_get(const char *pool_name, struct td_xenio_shared_ctx ** _ctx)
{
    struct td_xenio_shared_ctx *ctx;
    int err = 0;

    do {
        tapdisk_xenio_find_ctx(ctx, __td_xenio_ctx_match(ctx, pool_name));
        if (ctx) {
            *_ctx = ctx;
            return 0;
        }

        err = tapdisk_xenio_shared_ctx_open(pool_name);
    } while (!err);

    return err;
}

void
tapdisk_xenio_shared_ctx_put(struct td_xenio_shared_ctx * ctx)
{
    if (list_empty(&ctx->blkifs))
        tapdisk_xenio_shared_ctx_close(ctx);
}

struct td_xenio_ctx*
tapdisk_xenio_ctx_bind_queue(struct td_xenio_shared_ctx * shared_ctx,
                             evtchn_port_t port,
                             struct td_blkif_queue* queue)
{
    int fd = -1;
    int err = 0;
    int ret = -1;
    xenevtchn_handle* evthdl = NULL;
    event_id_t ring_event = -1;
    td_queue_id_t qid = tapdisk_xenblkif_queue_id(queue);

    evthdl = xenevtchn_open(NULL, 0);
    if (!evthdl) {
        err = -errno;
        ERROR("failed to open the event channel driver: %s\n",
                strerror(-err));
        goto fail;
    }

    fd = xenevtchn_fd(evthdl);
    if (fd < 0) {
        err = -errno;
        ERROR("failed to get the event channel file descriptor: %s\n",
                strerror(-err));
        goto fail;
    }

    ring_event = tapdisk_server_register_io_event(qid,
                                                  SCHEDULER_POLL_READ_FD,
                                                  fd, TV_ZERO, tapdisk_xenio_ctx_ring_event, queue);
    if (ring_event < 0) {
        err = ring_event;
        ERROR("failed to register event: %s\n", strerror(-err));
        goto fail;
    }

    ret = xenevtchn_bind_interdomain(evthdl, queue->blkif->domid, port);
    if (ret == -1) {
            err = -errno;
            RING_ERR(queue->blkif, "failed to bind to event channel port %d: %s\n",
                     port, strerror(-err));
            goto fail;
    }

    struct td_xenio_ctx* ctx = calloc(1, sizeof(struct td_xenio_ctx));
    if (!ctx) {
        goto fail;
    }

    ctx->shared = shared_ctx;
    shared_ctx->count_ref++;

    ctx->xce_handle = evthdl;
    ctx->ring_event = ring_event;
    ctx->fd = fd;
    ctx->port = ret;   // XXX: we do not store the port in argument ?

    return ctx;

fail:
    if (ring_event >= 0)
        tapdisk_server_unregister_io_event(qid, ring_event);
    if (ret != -1)
        xenevtchn_unbind(evthdl, ret);
    if (evthdl)
        xenevtchn_close(evthdl); // also close owned 'fd'

    return NULL;
}

void
tapdisk_xenio_ctx_unbind_queue(struct td_xenio_ctx* ctx, struct td_blkif_queue* queue)
{
    if (ctx->ring_event >= 0) {
        tapdisk_server_unregister_io_event(tapdisk_xenblkif_queue_id(queue),
                                           ctx->ring_event);
        ctx->ring_event = -1;
        ctx->fd = -1;
    }

    if (ctx->xce_handle) {
        xenevtchn_close(ctx->xce_handle);
        ctx->xce_handle = NULL;
    }

    ctx->port = -1;

    ASSERT(ctx->shared->count_ref > 0);
    ctx->shared->count_ref--;

    free(ctx);
}
