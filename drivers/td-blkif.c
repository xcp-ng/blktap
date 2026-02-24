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
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <libgen.h>
#include <zlib.h>

#include "blktap-xenif.h"
#include "debug.h"
#include "blktap3.h"
#include "tapdisk.h"
#include "tapdisk-log.h"
#include "tapdisk-server.h"
#include "tapdisk-metrics.h"
#include "timeout-math.h"

#include "td-blkif.h"
#include "td-ctx.h"
#include "td-req.h"

#include "util.h"

struct td_xenblkif *
tapdisk_xenblkif_find(const domid_t domid, const int devid)
{
    struct td_xenblkif *blkif = NULL;
    struct td_xenio_shared_ctx *ctx;

    tapdisk_xenio_for_each_ctx(ctx) {
        tapdisk_xenio_ctx_find_blkif(ctx, blkif,
                                     blkif->domid == domid &&
                                     blkif->devid == devid);
        if (blkif)
            return blkif;
    }

    return NULL;
}


/**
 * Returns 0 on success, -errno on failure.
 */
static int
tapdisk_xenblkif_stats_destroy(struct td_xenblkif *blkif)
{
    int err;

    err = shm_destroy(&blkif->xenvbd_stats.io_ring);
    if (unlikely(err))
        goto out;
    free(blkif->xenvbd_stats.io_ring.path);
    blkif->xenvbd_stats.io_ring.path = NULL;

	/*
	 * blkif->stats.xenvbd was initialised to blkif->xenvbd_stats.stats.mem
	 * in tapdisk_xenblkif_stats_create(). That address will be unmapped
	 * by the call to shm_destroy(), and an error return does not mean it
	 * wasn't, so we must unconditionally NULL blkif->stats.xenvbd here.
	 */
	blkif->stats.xenvbd = NULL;
    err = shm_destroy(&blkif->xenvbd_stats.stats);
    if (unlikely(err))
        goto out;
    free(blkif->xenvbd_stats.stats.path);
    blkif->xenvbd_stats.stats.path = NULL;

    if (likely(blkif->xenvbd_stats.root)) {
        err = rmdir(blkif->xenvbd_stats.root);
        if (unlikely(err && errno != ENOENT)) {
            err = errno;
            EPRINTF("failed to remove %s: %s\n",
                    blkif->xenvbd_stats.root, strerror(err));
            goto out;
        }
        err = 0;

        free(blkif->xenvbd_stats.root);
        blkif->xenvbd_stats.root = NULL;
    }
out:
    return -err;
}


/*
 * TODO provide ring stats in raw format (the same way I/O stats are provided).
 * xen-ringwatch will have to be modified accordingly.
 */
static int
tapdisk_xenblkif_stats_create(struct td_xenblkif *blkif)
{
    int err = 0, len;
    char *_path = NULL;

    len = asprintf(&blkif->xenvbd_stats.root, "/dev/shm/vbd3-%d-%d",
            blkif->domid, blkif->devid);
    if (unlikely(len == -1)) {
        err = errno;
        blkif->xenvbd_stats.root = NULL;
        goto out;
    }

	err = mkdir(blkif->xenvbd_stats.root, S_IRUSR | S_IWUSR);
	if (unlikely(err)) {
        err = errno;
        if (err != EEXIST) {
            EPRINTF("failed to create %s: %s\n",
                    blkif->xenvbd_stats.root, strerror(err));
    		goto out;
        }
        err = 0;
    }

    len = asprintf(&blkif->xenvbd_stats.io_ring.path, "%s/io_ring",
            blkif->xenvbd_stats.root);
    if (unlikely(len == -1)) {
        err = errno;
        blkif->xenvbd_stats.io_ring.path = NULL;
        goto out;
    }
    blkif->xenvbd_stats.io_ring.size = PAGE_SIZE;
    err = shm_create(&blkif->xenvbd_stats.io_ring);
    if (unlikely(err)) {
        err = errno;
        EPRINTF("failed to create shm ring stats file: %s\n", strerror(err));
        goto out;
   }

    err = asprintf(&blkif->xenvbd_stats.stats.path, "%s/statistics",
            blkif->xenvbd_stats.root);
    if (unlikely(err == -1)) {
        err = errno;
        blkif->xenvbd_stats.stats.path = NULL;
        goto out;
    }
    blkif->xenvbd_stats.stats.size = PAGE_SIZE;
    err = shm_create(&blkif->xenvbd_stats.stats);
    if (unlikely(err))
        goto out;

    blkif->xenvbd_stats.last = 0;

	blkif->stats.xenvbd = blkif->xenvbd_stats.stats.mem;

    if (tapdisk_server_mem_mode()) {
        td_flag_set(blkif->stats.xenvbd->flags, BT3_LOW_MEMORY_MODE);
        td_flag_set(blkif->vbd_stats.stats->flags, BT3_LOW_MEMORY_MODE);
    }

    err = tapdisk_xenblkif_ring_stats_update(blkif);
    if (unlikely(err)) {
        EPRINTF("failed to generate shared I/O ring stats: %s\n",
                strerror(-err));
        goto out;
    }

    _path = strndup(blkif->xenvbd_stats.io_ring.path, len - 1);
    if (unlikely(!_path)) {
        err = errno;
        goto out;
    }

    err = rename(blkif->xenvbd_stats.io_ring.path, _path);
    if (unlikely(err)) {
        err = errno;
        goto out;
    }

    free(blkif->xenvbd_stats.io_ring.path);
    blkif->xenvbd_stats.io_ring.path = _path;
    _path = NULL;
out:
    free(_path);
    if (err) {
        int err2 = tapdisk_xenblkif_stats_destroy(blkif);
        if (err2)
            EPRINTF("failed to clean up failed stats file: "
                    "%s (error ignored)\n", strerror(-err2));
    }
    return -err;
}


int
tapdisk_xenblkif_destroy(struct td_xenblkif * blkif)
{
    int err = 0;

    for (int qid = 0; qid < blkif->nr_queues; qid++) {
        struct td_blkif_queue* queue = &blkif->queues[qid];

        if (tapdisk_xenblkif_chkrng_event_id(queue) >= 0) {
            tapdisk_server_unregister_io_event(qid,
                tapdisk_xenblkif_chkrng_event_id(queue));
            queue->chkrng_event = -1;
        }

        if (tapdisk_xenblkif_stoppolling_event_id(queue) >= 0) {
            tapdisk_server_unregister_io_event(qid,
                tapdisk_xenblkif_stoppolling_event_id(queue));
            queue->stoppolling_event = -1;
        }

        tapdisk_xenblkif_reqs_free(queue);

        if (queue->ctx) {
            if (queue->ctx->port >= 0) {
                /* TODO: to study, this should be done in tapdisk_xenio_ctx_unbind_queue() */
                xenevtchn_unbind(queue->ctx->xce_handle, queue->ctx->port);
            }

            if (queue->rings.common.sring) {
                err = xengnttab_unmap(queue->ctx->shared->xcg_handle,
                                      queue->rings.common.sring, blkif->ring_n_pages);
                if (unlikely(err)) {
                    err = errno;
                    EPRINTF("failed to unmap ring page %p (%d pages): %s "
                            "(error ignored)\n",
                            queue->rings.common.sring, blkif->ring_n_pages,
                            strerror(err));
                    err = 0;
                }
            }

            /* Save shared context, before the queue context */
            struct td_xenio_shared_ctx* shared_ctx = queue->ctx->shared;
            tapdisk_xenio_ctx_unbind_queue(queue->ctx, queue);

            if (shared_ctx->count_ref == 0) {
                tapdisk_xenio_shared_ctx_put(shared_ctx);
            }
        }
    }

    err = td_metrics_vbd_stop(&blkif->vbd_stats);
    if (unlikely(err))
        EPRINTF("failed to destroy blkfront stats file: %s\n", strerror(-err));

    err = tapdisk_xenblkif_stats_destroy(blkif);
    if (unlikely(err)) {
        EPRINTF("failed to clean up ring stats file: %s (error ignored)\n",
                strerror(-err));
        err = 0;
    }

    /* Remove blkif from the lists currently holding it */
    list_del(&blkif->entry_ctx);
    list_del(&blkif->entry);

    free(blkif);

    return err;
}


int
tapdisk_xenblkif_reqs_pending(const struct td_blkif_queue * const queue)
{
	ASSERT(queue);

	return queue->ring_size - queue->n_reqs_free;
}


int
tapdisk_xenblkif_disconnect(const domid_t domid, const int devid)
{
    int err;
    bool pending = false;
    struct td_xenblkif *blkif;

    blkif = tapdisk_xenblkif_find(domid, devid);
    if (!blkif)
        return -ENODEV;

    for (int qid = 0; qid < blkif->nr_queues; qid++) {
        struct td_blkif_queue* queue = &blkif->queues[qid];

        if (tapdisk_xenblkif_reqs_pending(queue)) {
            RING_DEBUG(blkif, "disconnect from ring with %d pending requests\n",
                       queue->ring_size - queue->n_reqs_free);
            if (td_atomic_flag_test(queue->blkif->vbd->state, TD_VBD_PAUSED))
                RING_ERR(blkif, "disconnect from ring with %d pending requests "
                         "and the VBD paused\n",
                         queue->ring_size - queue->n_reqs_free);

            if (queue->ctx) {
                tapdisk_xenio_ctx_unbind_queue(queue->ctx, queue);
            }

            if (!blkif->dead) {
                list_move(&blkif->entry, &blkif->vbd->dead_rings);
                blkif->dead = true;

                err = td_metrics_vbd_stop(&blkif->vbd_stats);
                if (unlikely(err))
                    EPRINTF("failed to destroy blkfront stats file: %s\n", strerror(-err));

                err = tapdisk_xenblkif_stats_destroy(blkif);
                if (unlikely(err))
                    EPRINTF("failed to clean up ring stats file: %s (error ignored)\n",
                            strerror(-err));
            }

            pending = true;
        }
    }

    if (pending)
        /*
         * FIXME shall we unmap the ring or will that lead to some fatal error
         * in tapdisk? IIUC if we don't unmap it we'll get errors during grant
         * copy.
         */
        return 0;
    else
        return tapdisk_xenblkif_destroy(blkif);
}


void
tapdisk_xenblkif_sched_stoppolling(const struct td_blkif_queue *queue)
{
	int err;

	ASSERT(queue);

	err = tapdisk_server_io_event_set_timeout(tapdisk_xenblkif_queue_id(queue),
		tapdisk_xenblkif_stoppolling_event_id(queue), TV_USECS(queue->blkif->poll_duration));
	ASSERT(!err);
}

void
tapdisk_xenblkif_unsched_stoppolling(const struct td_blkif_queue *queue)
{
	int err;

	ASSERT(queue);

	err = tapdisk_server_io_event_set_timeout(tapdisk_xenblkif_queue_id(queue),
		tapdisk_xenblkif_stoppolling_event_id(queue), TV_INF);
	ASSERT(!err);
}


void
tapdisk_start_polling(struct td_blkif_queue *queue)
{
    ASSERT(queue);

    /* Only enter polling if the CPU utilisation is not too high */
    if (tapdisk_server_system_idle_cpu() > (float)queue->blkif->poll_idle_threshold) {
        queue->in_polling = true;

        /* Start checking the ring immediately */
        tapdisk_xenblkif_sched_chkrng(queue);

        /* Schedule the future 'stop polling' event */
        tapdisk_xenblkif_sched_stoppolling(queue);

	tapdisk_server_mask_io_event(tapdisk_xenblkif_queue_id(queue),
            tapdisk_xenblkif_evtchn_event_id(queue), 1);
    }
}

void
tapdisk_start_polling_all_queues(struct td_xenblkif *blkif)
{
    int i;

    ASSERT(blkif);

    for (i = 0; i < blkif->nr_queues; i++) {
	tapdisk_start_polling(&blkif->queues[i]);
    }
}

static inline void
tapdisk_xenblkif_cb_stoppolling(event_id_t id __attribute__((unused)),
        char mode __attribute__((unused)), void *private)
{
    struct td_blkif_queue *queue = private;

    ASSERT(queue);

    /* Process the ring one final time, setting the event counter */
    if (!tapdisk_xenio_ctx_process_ring(queue, true)) {
        /* If there were no new requests this time, then stop polling */
        queue->in_polling = false;

        /* Stop obsessively checking the ring */
        tapdisk_xenblkif_unsched_chkrng(queue);

        /* Make the 'stop polling' event not fire again */
        tapdisk_xenblkif_unsched_stoppolling(queue);

	tapdisk_server_mask_io_event(tapdisk_xenblkif_queue_id(queue),
            tapdisk_xenblkif_evtchn_event_id(queue), 0);
    }
}

void
tapdisk_xenblkif_sched_chkrng(const struct td_blkif_queue *queue)
{
	int err;

	ASSERT(queue);

	err = tapdisk_server_io_event_set_timeout(tapdisk_xenblkif_queue_id(queue),
			tapdisk_xenblkif_chkrng_event_id(queue), TV_ZERO);
	ASSERT(!err);
}

void
tapdisk_xenblkif_unsched_chkrng(const struct td_blkif_queue *queue)
{
	int err;

	ASSERT(queue);

	err = tapdisk_server_io_event_set_timeout(tapdisk_xenblkif_queue_id(queue),
			tapdisk_xenblkif_chkrng_event_id(queue), TV_INF);
	ASSERT(!err);
}

static inline void
tapdisk_xenblkif_cb_chkrng(event_id_t id __attribute__((unused)),
        char mode __attribute__((unused)), void *private)
{
    struct td_blkif_queue *queue = private;

    ASSERT(queue);

    /*
     * If we are polling, process the ring without setting the event counter.
     * If we are not polling, unschedule this event, process the ring and set
     * the event counter.
     */

    if (!queue->in_polling)
        tapdisk_xenblkif_unsched_chkrng(queue);

    tapdisk_xenio_ctx_process_ring(queue, !queue->in_polling);
}


int
tapdisk_xenblkif_connect(domid_t domid, int devid, const grant_ref_t grefs[][MAX_RING_PAGES],
                         int order, int nr_queues, evtchn_port_t* ports, bool persistent_grant,
                         unsigned int indirect_max_segments,
                         int proto, int poll_duration, int poll_idle_threshold,
                         const char *pool, td_vbd_t * vbd)
{
    struct td_xenblkif *td_blkif = NULL; /* TODO rename to blkif */
    int err;
    unsigned int i;
    void *sring;
    size_t sz;

    ASSERT(grefs);
    ASSERT(vbd);
    ASSERT(nr_queues > 0);

    /*
     * Already connected?
     */
    if (tapdisk_xenblkif_find(domid, devid)) {
        /* TODO log error */
        return -EALREADY;
    }

    td_blkif = calloc(1, sizeof(*td_blkif));
    if (!td_blkif) {
        /* TODO log error */
        err = -errno;
        goto fail;
    }

    td_blkif->domid = domid;
    td_blkif->devid = devid;
    td_blkif->vbd = vbd;
    td_blkif->proto = proto;
    td_blkif->dead = false;
    td_blkif->poll_duration = poll_duration;
    td_blkif->poll_idle_threshold = poll_idle_threshold;
    td_blkif->nr_queues = nr_queues;

    td_blkif->xenvbd_stats.root = NULL;
    shm_init(&td_blkif->xenvbd_stats.io_ring);
    shm_init(&td_blkif->xenvbd_stats.stats);

    memset(&td_blkif->stats, 0, sizeof(td_blkif->stats));

    INIT_LIST_HEAD(&td_blkif->entry_ctx);
    INIT_LIST_HEAD(&td_blkif->entry);

    /*
     * Create the shared ring.
     */
    td_blkif->ring_n_pages = 1 << order;
    if (td_blkif->ring_n_pages > ARRAY_SIZE(td_blkif->queues[0].ring_ref)) {
        RING_ERR(td_blkif, "too many pages (%u), max %zu\n",
                td_blkif->ring_n_pages, ARRAY_SIZE(td_blkif->queues[0].ring_ref));
        err = -EINVAL;
        goto fail;
    }

    /*
     * Size of the ring, in bytes.
     */
    sz = (size_t)PAGE_SIZE << order;

    if (td_blkif->nr_queues > ARRAY_SIZE(td_blkif->queues)) {
        RING_ERR(td_blkif, "too many queues (%u), max %zu\n",
                td_blkif->nr_queues, ARRAY_SIZE(td_blkif->queues));
        err = -EINVAL;
        goto fail;
    }

    for (int qid = 0; qid < td_blkif->nr_queues; qid++) {
        struct td_blkif_queue* queue = &td_blkif->queues[qid];
        struct td_xenio_shared_ctx *shared_ctx;

        pthread_mutex_init(&queue->mutex, NULL);

        queue->blkif = td_blkif;
        queue->chkrng_event = -1;
        queue->stoppolling_event = -1;
        queue->in_polling = false;

        queue->barrier.msg = NULL;
        queue->barrier.io_done = false;
        queue->barrier.io_err = 0;

        err = tapdisk_xenio_shared_ctx_get(pool, &shared_ctx);
        if (err) {
            /* TODO log error */
            goto fail;
        }

        /*
         * TODO Why don't we just keep a copy of the array's address? There should
         * be a reason for copying the addresses of the pages, figure out why.
         *
         * TODO Why do we even store it in the td_blkif in the first place?
         */
        for (i = 0; i < td_blkif->ring_n_pages; i++) {
            queue->ring_ref[i] = grefs[qid][i];
        }

        /*
         * Map the grant references that will be holding the request descriptors.
         */
        sring = xengnttab_map_domain_grant_refs(shared_ctx->xcg_handle,
                        td_blkif->ring_n_pages, td_blkif->domid, queue->ring_ref,
                        PROT_READ | PROT_WRITE);
        if (!sring) {
            err = -errno;
            RING_ERR(td_blkif, "failed to map domain's grant references: %s\n",
                    strerror(-err));
            goto fail;
        }

        /*
         * Initialize the mapped address into the shared ring.
         *
         * TODO Check for protocol support in the beginning of this function.
         */
        switch (td_blkif->proto) {
            case BLKIF_PROTOCOL_NATIVE:
                {
                    blkif_sring_t *__sring = sring;
                    BACK_RING_INIT(&queue->rings.native, __sring, sz);
                    break;
                }
            case BLKIF_PROTOCOL_X86_32:
                {
                    blkif_x86_32_sring_t *__sring = sring;
                    BACK_RING_INIT(&queue->rings.x86_32, __sring, sz);
                    break;
                }
            case BLKIF_PROTOCOL_X86_64:
                {
                    blkif_x86_64_sring_t *__sring = sring;
                    BACK_RING_INIT(&queue->rings.x86_64, __sring, sz);
                    break;
                }
            default:
                RING_ERR(td_blkif, "unsupported protocol 0x%x\n", td_blkif->proto);
                err = -EPROTONOSUPPORT;
                goto fail;
        }

        /*
         * Bind to the remote port.
         */
        queue->ctx = tapdisk_xenio_ctx_bind_queue(shared_ctx, ports[qid], queue);
        if (!queue->ctx) {
            err = -errno;
            RING_ERR(td_blkif, "failed to bind to event channel port %d: %s\n",
                    ports[qid], strerror(-err));
            goto fail;
        }

        err = tapdisk_xenblkif_reqs_init(queue);
        if (err) {
            /* TODO log error */
            goto fail;
        }

        queue->chkrng_event = tapdisk_server_register_io_event(
            tapdisk_xenblkif_queue_id(queue),
            SCHEDULER_POLL_TIMEOUT,         -1, TV_INF,
            tapdisk_xenblkif_cb_chkrng, queue);
        if (unlikely(queue->chkrng_event < 0)) {
            err = queue->chkrng_event;
            RING_ERR(td_blkif, "failed to register event: %s\n", strerror(-err));
            goto fail;
        }

        queue->stoppolling_event = tapdisk_server_register_io_event(
            tapdisk_xenblkif_queue_id(queue),
            SCHEDULER_POLL_TIMEOUT,         -1, TV_INF,
            tapdisk_xenblkif_cb_stoppolling, queue);
        if (unlikely(queue->stoppolling_event < 0)) {
            err = queue->stoppolling_event;
            RING_ERR(td_blkif, "failed to register event: %s\n", strerror(-err));
            goto fail;
        }
    }

    err = td_metrics_vbd_start(td_blkif->domid, td_blkif->devid, &td_blkif->vbd_stats);
    if (unlikely(err))
        goto fail;

    err = tapdisk_xenblkif_stats_create(td_blkif);
    if (unlikely(err))
        goto fail;

    list_add_tail(&td_blkif->entry, &vbd->rings);
    if (td_blkif->queues[0].ctx) {
        /* TODO: not very satisfied by this */
        struct td_xenio_shared_ctx* ctx = td_blkif->queues[0].ctx->shared;
        list_add_tail(&td_blkif->entry_ctx, &ctx->blkifs);
    }

    DPRINTF("ring %p connected\n", td_blkif);

    return 0;

fail:
    if (td_blkif) {
        int err2 = tapdisk_xenblkif_destroy(td_blkif);
        if (err2)
            EPRINTF("failed to destroy the block interface: %s "
                    "(error ignored)\n", strerror(-err2));
    }

    return err;
}


event_id_t
tapdisk_xenblkif_evtchn_event_id(const struct td_blkif_queue *queue)
{
    return queue->ctx->ring_event;
}


event_id_t
tapdisk_xenblkif_chkrng_event_id(const struct td_blkif_queue *queue)
{
	return queue->chkrng_event;
}


event_id_t
tapdisk_xenblkif_stoppolling_event_id(const struct td_blkif_queue *queue)
{
	return queue->stoppolling_event;
}


int
tapdisk_xenblkif_ring_stats_update(struct td_xenblkif *blkif)
{
    time_t t;
    int err = 0, len;
	struct blkif_common_back_ring *ring = NULL;
	uLong *chksum = NULL;

    if (!blkif)
        return 0;

    if (unlikely(blkif->dead))
        return 0;

    ring = &blkif->queues[0].rings.common;
    if (!ring->sring)
        return 0;

    ASSERT(blkif->xenvbd_stats.io_ring.mem);

    /*
     * Update the ring stats once every five seconds.
     */
    t = time(NULL);
	if (t - blkif->xenvbd_stats.last < 5)
		return 0;
	blkif->xenvbd_stats.last = t;

    len = snprintf(blkif->xenvbd_stats.io_ring.mem + sizeof(*chksum),
            blkif->xenvbd_stats.io_ring.size,
            "nr_ents %u\n"
            "req prod %u cons %d event %u\n"
            "rsp prod %u pvt %d event %u\n",
            ring->nr_ents,
            ring->sring->req_prod, ring->req_cons, ring->sring->req_event,
            ring->sring->rsp_prod, ring->rsp_prod_pvt, ring->sring->rsp_event);
    if (unlikely(len < 0))
        err = errno;
    else if (unlikely(len + sizeof(uLong) >= blkif->xenvbd_stats.io_ring.size))
        err = ENOBUFS;
    else {
        err = ftruncate(blkif->xenvbd_stats.io_ring.fd, len + sizeof(*chksum));
        if (unlikely(err)) {
            err = errno;
            EPRINTF("failed to truncate %s to %lu: %s\n",
                    blkif->xenvbd_stats.io_ring.path, len + sizeof(*chksum),
					strerror(err));
        }
    }

	chksum = blkif->xenvbd_stats.io_ring.mem;
	*chksum = crc32(0L, blkif->xenvbd_stats.io_ring.mem + sizeof(*chksum),
			len);

    return -err;
}


void
tapdisk_xenblkif_suspend(struct td_xenblkif * const blkif)
{
	int i;

	ASSERT(blkif);

	for (i = 0; i < blkif->nr_queues; i++) {
		struct td_blkif_queue* queue = &blkif->queues[i];
		tapdisk_server_mask_io_event(i, tapdisk_xenblkif_evtchn_event_id(queue), 1);
		tapdisk_server_mask_io_event(i, tapdisk_xenblkif_chkrng_event_id(queue), 1);
        }
}


void
tapdisk_xenblkif_resume(struct td_xenblkif * const blkif)
{
	int i;

	ASSERT(blkif);

	for (i = 0; i < blkif->nr_queues; i++) {
		struct td_blkif_queue* queue = &blkif->queues[i];
		tapdisk_server_mask_io_event(i, tapdisk_xenblkif_evtchn_event_id(queue), 0);
		tapdisk_server_mask_io_event(i, tapdisk_xenblkif_chkrng_event_id(queue), 0);
        }
}


bool
tapdisk_xenblkif_barrier_should_complete(const struct td_blkif_queue * const queue)
{
	ASSERT(queue);

	return queue->barrier.msg && 1 == tapdisk_xenblkif_reqs_pending(queue) &&
		(0 == queue->barrier.msg->nr_segments || queue->barrier.io_done);
}
