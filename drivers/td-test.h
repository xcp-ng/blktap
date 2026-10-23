/*
 * Copyright (c) 2026, Vates.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Test-only hooks ("tdtest"), see TDTEST.md.
 *
 * Nothing here is active unless one VBD has been addressed with a queued
 * SIGRTMIN carrying its minor, and the configuration file it then reads
 * names the VDI that VBD serves. A VBD that has never been addressed has
 * vbd->tdtest == NULL, and every hook in the existing code is guarded by
 * "if (unlikely(vbd->tdtest))", so such a VBD takes no other new path.
 *
 * All tdtest_* functions below except tdtest_signal() must only be called
 * when vbd->tdtest is non-NULL.
 */

#ifndef __TD_TEST_H__
#define __TD_TEST_H__

#include <stdbool.h>
#include <stdint.h>

#include "tapdisk.h"
#include "td-blkif.h"
#include "td-req.h"

struct td_test;
struct tdtest_whold;

/* td_xenblkif.tdtest_flags */
#define TDTEST_RING_PAUSED           0x1 /* intake_pause=current */
#define TDTEST_RING_BLOCK_LOGGED     0x2 /* R2 block logged once */

/* tdtest_complete() @rsp for a dead ring: no response is published */
#define TDTEST_RSP_NONE              1

/*
 * Control, from the signalfd handler: @reload is true for SIGRTMIN, false
 * for SIGRTMIN+1. @code and @value are ssi_code and ssi_int.
 */
void tdtest_signal(bool reload, int code, int32_t value);

/* VBD teardown */
void tdtest_vbd_close_vdi(td_vbd_t *vbd);
void tdtest_vbd_free(td_vbd_t *vbd);

/* td-blkif.c */
void tdtest_ring_connect(struct td_xenblkif *blkif);
void tdtest_ring_disconnect(struct td_xenblkif *blkif, int pending);

/* td-ctx.c */
bool tdtest_ring_blocked(struct td_xenblkif *blkif);
void tdtest_ring_intake(struct td_xenblkif *blkif,
		blkif_request_t *reqs[], int n_reqs);

/* td-req.c */
void tdtest_copyin(struct td_xenblkif *blkif,
		struct td_xenblkif_req *req, int err);
bool tdtest_read_hold(struct td_xenblkif *blkif,
		struct td_xenblkif_req *req, int error);
void tdtest_copyback(struct td_xenblkif *blkif,
		struct td_xenblkif_req *req, int err);
void tdtest_complete(struct td_xenblkif *blkif,
		struct td_xenblkif_req *req, int err, int rsp, int final);
void tdtest_dead_ring_destroy(struct td_xenblkif *blkif);
void tdtest_dead_ring_destroyed(td_vbd_t *vbd);

/* tapdisk-vbd.c */
struct tdtest_whold *tdtest_write_hold_begin(td_vbd_t *vbd,
		td_vbd_request_t *vreq);
void tdtest_write_hold_add(struct tdtest_whold *hold, td_request_t treq);

#endif /* __TD_TEST_H__ */
