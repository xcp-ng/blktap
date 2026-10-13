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

#ifndef __TD_REQ_H__
#define __TD_REQ_H__

#include "tapdisk.h"
#include "blktap-xenif.h"
#include <sys/types.h>
#include <xen/io/blkif.h>
#include <xen/gntdev.h>
#include "td-blkif.h"

/*
 * A direct request (blkif_request_t) carries at most
 * BLKIF_MAX_SEGMENTS_PER_REQUEST segments, each mapping a single page. An
 * indirect request (BLKIF_OP_INDIRECT) carries the grant references of pages
 * holding up to the number of segments the back-end announced in
 * feature-max-indirect-segments, at most TD_MAX_INDIRECT_SEGMENTS.
 */
#define TD_MAX_INDIRECT_SEGMENTS 256
#define TD_INDIRECT_SEGMENTS_PER_PAGE (PAGE_SIZE / sizeof(struct blkif_request_segment))
#define TD_MAX_SEGMENTS_PER_REQUEST \
	(TD_MAX_INDIRECT_SEGMENTS > BLKIF_MAX_SEGMENTS_PER_REQUEST ? \
	 TD_MAX_INDIRECT_SEGMENTS : BLKIF_MAX_SEGMENTS_PER_REQUEST)

/* The data buffer of a direct request */
#define TD_REQ_BUFFER_SIZE (BLKIF_MAX_SEGMENTS_PER_REQUEST << PAGE_SHIFT)

/**
 * Representation of the intermediate request used to retrieve a request from
 * the shared ring and handle it over to the main tapdisk request processing
 * routine.  We could merge it into td_vbd_request_t or define it inside
 * td_vbd_request_t, but keeping it separate simplifies keeping Xen stuff
 * outside tapdisk.
 *
 * TODO rename to something better, e.g. ring_req?
 */
struct td_xenblkif_req {
    /**
     * A request descriptor in the ring. We need to copy the descriptors
     * because the guest may modify it while we're using it. Note that we
     * only copy the descriptor and not the actual data, the guest is free
     * to modify the data and corrupt itself if it wants to.
     */
    union {
        blkif_request_t msg;
        blkif_request_indirect_t ind;
    };

    /*
     * The operation and the segments of the request, direct or indirect: the
     * data operation of an indirect request, and its segments, copied from
     * the indirect pages to indirect_segs. The other fields (id,
     * sector_number) are at the same place in both kinds of request.
     */
    uint8_t op;
    unsigned int nr_segs;
    struct blkif_request_segment *segs;

    /*
     * Queue number the request is originating from.
     */
    td_queue_id_t queue_idx;

    /**
     * tapdisk's representation of the request.
     */
    td_vbd_request_t vreq;

    /**
     * Pointer to memory-mapped grant refs. We keep this around because we need
     * to pass it to xengnttab_munmap when the requests is completed.
     */
    void *vma;

    struct timeval ts;

    /**
     * The scatter/gather list td_vbd_request_t.iov points to.
     */
    struct td_iovec iov[TD_MAX_SEGMENTS_PER_REQUEST];

    int prot;

	struct gntdev_grant_copy_segment
		gcopy_segs[TD_MAX_SEGMENTS_PER_REQUEST];

    struct blkif_request_segment indirect_segs[TD_MAX_INDIRECT_SEGMENTS];
};

struct td_blkif_queue;

/**
 * Queues the requests to the standard tapdisk queue.
 *
 * @param td_blkif the block interface corresponding to the VBD
 * @param reqs array holding the request rescriptors
 * @param nr_reqs number of requests in the array
 */
void
tapdisk_xenblkif_queue_requests(struct td_blkif_queue * const queue,
        blkif_request_t *reqs[], const int nr_reqs);

/**
 * Initilises the intermediate requests of this block queue.
 *
 * @params queue the queue whose requests must be initialised
 * @returns 0 on success
 */
int
tapdisk_xenblkif_reqs_init(struct td_blkif_queue* queue);

/**
 * Releases all the requests of the block interface.
 *
 * @param blkif the block interface whose requests should be freed
 */
void
tapdisk_xenblkif_reqs_free(struct td_blkif_queue * const queue);

#define msg_to_tapreq(_req) \
	container_of(_req, struct td_xenblkif_req, msg)

#endif /* __TD_REQ_H__ */
