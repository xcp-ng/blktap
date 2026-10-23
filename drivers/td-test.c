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
 * Test-only hooks ("tdtest") for one VBD at a time. See TDTEST.md for the
 * configuration keys, the log format and the limitations.
 *
 * Threads: everything runs in the main thread except the completion side
 * (tdtest_read_hold, tdtest_copyback, tdtest_complete,
 * tdtest_dead_ring_destroy[ed]), which runs wherever tapdisk_vbd_kick()
 * runs, i.e. also in a driver thread for TD_DRIVER_THREADED images (qcow2).
 * The configuration, the counters, the hold lists and the gate state are
 * protected by td->lock. Lock order: vbd->mutex, then td->lock, then the
 * scheduler's own mutex. td->lock is never held while calling back into
 * the request path (td_queue_write, td_complete_request, the ring
 * completion code), which may re-enter the hooks.
 */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/time.h>
#include <zlib.h>

#include "debug.h"
#include "tapdisk.h"
#include "tapdisk-server.h"
#include "tapdisk-vbd.h"
#include "td-blkif.h"
#include "td-req.h"
#include "td-test.h"

#ifndef TDTEST_VARIANT
#define TDTEST_VARIANT "unknown"
#endif

#define TDTEST_CONF_DIR         "/run/tapdisk-test"
#define TDTEST_HOLD_MS_MAX      120000U

#define TDTEST_LOG(_f, _a...)   syslog(LOG_WARNING, "tdtest: " _f, ##_a)

struct tdtest_config {
	char            vdi[37];
	char            seq[65];

	bool            log;
	uint64_t        log_lba_start;
	uint64_t        log_lba_end;

	unsigned int    read_hold_ms;
	uint64_t        read_hold_lba_start;
	uint64_t        read_hold_lba_end;
	unsigned int    read_hold_max;

	unsigned int    write_hold_ms;
	uint64_t        write_hold_lba_start;
	uint64_t        write_hold_lba_end;
	unsigned int    write_hold_max;

	bool            intake_pause;
	bool            dead_chkrng_kick;
	bool            fix_dead_intake;
	bool            fix_gate_new_ring;
};

struct td_test {
	td_vbd_t               *vbd;
	pthread_mutex_t         lock;

	bool                    enabled;
	struct tdtest_config    cfg;

	/* holds started since the last accepted configuration */
	unsigned int            read_holds;

	struct list_head        read_held;
};

/* A successful read whose completion (copy-back and response) is held. */
struct tdtest_rhold {
	struct list_head        entry;
	struct td_test         *td;
	struct td_xenblkif     *blkif;
	struct td_xenblkif_req *req;
	event_id_t              event;
	struct timeval          start;
	unsigned int            ms;
};

/*
 * Log lines
 */

struct tdtest_line {
	char    buf[512];
	size_t  len;
};

static void tdtest_line_add(struct tdtest_line *l, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void
tdtest_line_add(struct tdtest_line *l, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (l->len >= sizeof(l->buf) - 1)
		return;

	va_start(ap, fmt);
	n = vsnprintf(l->buf + l->len, sizeof(l->buf) - l->len, fmt, ap);
	va_end(ap);

	if (n < 0)
		return;

	l->len += n;
	if (l->len > sizeof(l->buf) - 1)
		l->len = sizeof(l->buf) - 1;
}

static void
tdtest_line_emit(struct tdtest_line *l)
{
	TDTEST_LOG("%s", l->buf);
}

static void
tdtest_line_init(struct tdtest_line *l, const char *ev, td_vbd_t *vbd)
{
	struct timeval now;

	l->len = 0;
	l->buf[0] = '\0';

	gettimeofday(&now, NULL);
	tdtest_line_add(l, "ev=%s t=%ld.%06ld minor=%u",
			ev, (long)now.tv_sec, (long)now.tv_usec, vbd->uuid);
}

static void
tdtest_line_ring(struct tdtest_line *l, const char *ev,
		const struct td_xenblkif *blkif)
{
	tdtest_line_init(l, ev, blkif->vbd);
	tdtest_line_add(l, " ring=%p dead=%d domid=%d devid=%d",
			(const void *)blkif, blkif->dead ? 1 : 0,
			blkif->domid, blkif->devid);
}

static unsigned int
tdtest_msg_nsegs(const blkif_request_t *msg)
{
	unsigned int n = msg->nr_segments;

	/* intake logging runs before the descriptor has been validated */
	if (n > BLKIF_MAX_SEGMENTS_PER_REQUEST)
		n = BLKIF_MAX_SEGMENTS_PER_REQUEST;

	return n;
}

static unsigned int
tdtest_msg_nsect(const blkif_request_t *msg)
{
	unsigned int i, n, nsect = 0;

	n = tdtest_msg_nsegs(msg);
	for (i = 0; i < n; i++)
		if (msg->seg[i].last_sect >= msg->seg[i].first_sect)
			nsect += msg->seg[i].last_sect - msg->seg[i].first_sect + 1;

	return nsect;
}

static const char *
tdtest_op_name(unsigned int op)
{
	switch (op) {
	case BLKIF_OP_READ:
		return "read";
	case BLKIF_OP_WRITE:
		return "write";
	case BLKIF_OP_WRITE_BARRIER:
		return "barrier";
	default:
		return "other";
	}
}

static void
tdtest_line_msg(struct tdtest_line *l, const blkif_request_t *msg)
{
	unsigned int i, n;

	n = tdtest_msg_nsegs(msg);

	tdtest_line_add(l, " id=%"PRIu64" op=%s sector=%"PRIu64
			" nsect=%u nsegs=%u grefs=",
			(uint64_t)msg->id, tdtest_op_name(msg->operation),
			(uint64_t)msg->sector_number, tdtest_msg_nsect(msg),
			(unsigned int)msg->nr_segments);

	if (!n)
		tdtest_line_add(l, "-");

	for (i = 0; i < n; i++)
		tdtest_line_add(l, "%s%u", i ? "," : "",
				(unsigned int)msg->seg[i].gref);
}

/*
 * zlib crc32 of the request's data in req->vma, segment payloads chained in
 * order, i.e. the crc32 of the request's data as one buffer in LBA order.
 * Only valid once the descriptor has passed tapdisk_xenblkif_parse_request.
 */
static uint32_t
tdtest_req_crc(const struct td_xenblkif_req *req)
{
	uLong crc;
	unsigned int i, n;

	crc = crc32(0L, Z_NULL, 0);
	n = tdtest_msg_nsegs(&req->msg);

	for (i = 0; i < n; i++) {
		const struct blkif_request_segment *seg = &req->msg.seg[i];
		const char *p;
		uInt len;

		if (seg->last_sect < seg->first_sect)
			break;

		p = (const char *)req->vma + ((size_t)i << PAGE_SHIFT)
			+ ((size_t)seg->first_sect << SECTOR_SHIFT);
		len = (uInt)(seg->last_sect - seg->first_sect + 1) << SECTOR_SHIFT;
		crc = crc32(crc, (const Bytef *)p, len);
	}

	return (uint32_t)crc;
}

static bool
tdtest_req_has_data(const struct td_xenblkif_req *req, int err)
{
	return !err && req->vma && req->msg.nr_segments &&
		(req->msg.operation == BLKIF_OP_READ ||
		 req->msg.operation == BLKIF_OP_WRITE ||
		 req->msg.operation == BLKIF_OP_WRITE_BARRIER);
}

static void
tdtest_line_crc(struct tdtest_line *l, const struct td_xenblkif_req *req)
{
	tdtest_line_add(l, " crc=%08x", tdtest_req_crc(req));
}

static long
tdtest_elapsed_ms(const struct timeval *start)
{
	struct timeval now, delta;

	gettimeofday(&now, NULL);
	timersub(&now, start, &delta);

	return (long)delta.tv_sec * 1000 + (long)delta.tv_usec / 1000;
}

/*
 * Windows, in 512-byte sectors as in blkif sector_number: [start, end).
 * A request of nsect sectors at sec is in the window if it overlaps it; a
 * request without data is in the window if sec is.
 */
static bool
tdtest_in_window(uint64_t sec, uint64_t nsect, uint64_t start, uint64_t end)
{
	if (sec >= end)
		return false;
	if (sec >= start)
		return true;
	return start - sec < nsect;
}

static void
tdtest_snapshot(struct td_test *td, struct tdtest_config *cfg, bool *enabled)
{
	pthread_mutex_lock(&td->lock);
	*cfg = td->cfg;
	*enabled = td->enabled;
	pthread_mutex_unlock(&td->lock);
}

static bool
tdtest_log_msg_enabled(const struct tdtest_config *cfg,
		const blkif_request_t *msg)
{
	return cfg->log &&
		tdtest_in_window(msg->sector_number, tdtest_msg_nsect(msg),
				cfg->log_lba_start, cfg->log_lba_end);
}

/*
 * Configuration
 */

static char *
tdtest_strip(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t')
		s++;

	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
			   end[-1] == '\r' || end[-1] == '\n'))
		*--end = '\0';

	return s;
}

static int
tdtest_parse_u64(const char *s, uint64_t *out)
{
	unsigned long long v;
	char *end;
	int base = 10;

	if (!*s || *s == '-' || *s == '+')
		return -EINVAL;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		base = 16;

	errno = 0;
	v = strtoull(s, &end, base);
	if (errno || end == s || *end)
		return -EINVAL;

	*out = v;
	return 0;
}

static int
tdtest_parse_uint(const char *s, unsigned int max, unsigned int *out)
{
	uint64_t v;
	int err;

	err = tdtest_parse_u64(s, &v);
	if (err)
		return err;

	if (v > max)
		return -ERANGE;

	*out = (unsigned int)v;
	return 0;
}

static int
tdtest_parse_bool(const char *s, bool *out)
{
	if (!strcmp(s, "0")) {
		*out = false;
		return 0;
	}

	if (!strcmp(s, "1")) {
		*out = true;
		return 0;
	}

	return -EINVAL;
}

/* A VDI UUID, lower-cased: 8-4-4-4-12 hex digits. */
static int
tdtest_parse_vdi(const char *s, char *out, size_t size)
{
	size_t i, len;

	len = strlen(s);
	if (len != 36 || len >= size)
		return -EINVAL;

	for (i = 0; i < len; i++) {
		char c = s[i];

		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (c != '-')
				return -EINVAL;
		} else if (c >= '0' && c <= '9') {
			;
		} else if (c >= 'a' && c <= 'f') {
			;
		} else if (c >= 'A' && c <= 'F') {
			c = c - 'A' + 'a';
		} else
			return -EINVAL;

		out[i] = c;
	}
	out[len] = '\0';

	return 0;
}

static int
tdtest_parse_seq(const char *s, char *out, size_t size)
{
	size_t len;

	len = strlen(s);
	if (!len || len >= size || strpbrk(s, " \t"))
		return -EINVAL;

	memcpy(out, s, len + 1);
	return 0;
}

static void
tdtest_config_defaults(struct tdtest_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));

	strcpy(cfg->seq, "none");

	cfg->log_lba_end = UINT64_MAX;
	cfg->read_hold_lba_end = UINT64_MAX;
	cfg->read_hold_max = 1;
	cfg->write_hold_lba_end = UINT64_MAX;
	cfg->write_hold_max = 1;
}

/* Returns 0, -EINVAL/-ERANGE for a bad value, or -ENOENT for a bad key. */
static int
tdtest_config_set(struct tdtest_config *cfg, const char *key, const char *val)
{
	if (!strcmp(key, "vdi"))
		return tdtest_parse_vdi(val, cfg->vdi, sizeof(cfg->vdi));
	if (!strcmp(key, "seq"))
		return tdtest_parse_seq(val, cfg->seq, sizeof(cfg->seq));

	if (!strcmp(key, "log"))
		return tdtest_parse_bool(val, &cfg->log);
	if (!strcmp(key, "log_lba_start"))
		return tdtest_parse_u64(val, &cfg->log_lba_start);
	if (!strcmp(key, "log_lba_end"))
		return tdtest_parse_u64(val, &cfg->log_lba_end);

	if (!strcmp(key, "read_hold_ms"))
		return tdtest_parse_uint(val, TDTEST_HOLD_MS_MAX,
				&cfg->read_hold_ms);
	if (!strcmp(key, "read_hold_lba_start"))
		return tdtest_parse_u64(val, &cfg->read_hold_lba_start);
	if (!strcmp(key, "read_hold_lba_end"))
		return tdtest_parse_u64(val, &cfg->read_hold_lba_end);
	if (!strcmp(key, "read_hold_max"))
		return tdtest_parse_uint(val, UINT32_MAX, &cfg->read_hold_max);

	if (!strcmp(key, "write_hold_ms"))
		return tdtest_parse_uint(val, TDTEST_HOLD_MS_MAX,
				&cfg->write_hold_ms);
	if (!strcmp(key, "write_hold_lba_start"))
		return tdtest_parse_u64(val, &cfg->write_hold_lba_start);
	if (!strcmp(key, "write_hold_lba_end"))
		return tdtest_parse_u64(val, &cfg->write_hold_lba_end);
	if (!strcmp(key, "write_hold_max"))
		return tdtest_parse_uint(val, UINT32_MAX, &cfg->write_hold_max);

	if (!strcmp(key, "intake_pause")) {
		if (!strcmp(val, "current")) {
			cfg->intake_pause = true;
			return 0;
		}
		if (!strcmp(val, "0")) {
			cfg->intake_pause = false;
			return 0;
		}
		return -EINVAL;
	}

	if (!strcmp(key, "dead_chkrng_kick"))
		return tdtest_parse_bool(val, &cfg->dead_chkrng_kick);
	if (!strcmp(key, "fix_dead_intake"))
		return tdtest_parse_bool(val, &cfg->fix_dead_intake);
	if (!strcmp(key, "fix_gate_new_ring"))
		return tdtest_parse_bool(val, &cfg->fix_gate_new_ring);

	return -ENOENT;
}

/*
 * Reads TDTEST_CONF_DIR/<minor>.conf. Returns 0 if the file is valid and
 * names the VDI this VBD serves, otherwise -errno with @reason filled in.
 */
static int
tdtest_config_load(td_vbd_t *vbd, struct tdtest_config *cfg,
		char *reason, size_t rsize)
{
	char path[64], line[256];
	FILE *f;
	int lineno = 0, err = 0;

	tdtest_config_defaults(cfg);

	snprintf(path, sizeof(path), TDTEST_CONF_DIR "/%u.conf", vbd->uuid);

	f = fopen(path, "r");
	if (!f) {
		err = -errno;
		snprintf(reason, rsize, "open-%s:%s", path, strerror(-err));
		return err;
	}

	while (fgets(line, sizeof(line), f)) {
		char *key, *val, *eq;
		size_t len;

		lineno++;

		len = strlen(line);
		if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
			snprintf(reason, rsize, "line-%d-too-long", lineno);
			err = -EINVAL;
			goto out;
		}

		key = tdtest_strip(line);
		if (!*key || *key == '#')
			continue;

		eq = strchr(key, '=');
		if (!eq) {
			snprintf(reason, rsize, "line-%d-no-equals", lineno);
			err = -EINVAL;
			goto out;
		}

		*eq = '\0';
		key = tdtest_strip(key);
		val = tdtest_strip(eq + 1);

		err = tdtest_config_set(cfg, key, val);
		if (err) {
			snprintf(reason, rsize, "line-%d-%s-key=%s", lineno,
					err == -ENOENT ? "unknown" : "bad-value-for",
					key);
			goto out;
		}
	}

	if (ferror(f)) {
		snprintf(reason, rsize, "read-error");
		err = -EIO;
		goto out;
	}

	if (!cfg->vdi[0]) {
		snprintf(reason, rsize, "missing-vdi");
		err = -EINVAL;
		goto out;
	}

	if (!vbd->name) {
		snprintf(reason, rsize, "vbd-not-open");
		err = -ENODEV;
		goto out;
	}

	if (!strstr(vbd->name, cfg->vdi)) {
		snprintf(reason, rsize, "vdi-mismatch:vdi=%s", cfg->vdi);
		err = -EINVAL;
		goto out;
	}

	if (cfg->log_lba_start >= cfg->log_lba_end ||
	    cfg->read_hold_lba_start >= cfg->read_hold_lba_end ||
	    cfg->write_hold_lba_start >= cfg->write_hold_lba_end) {
		snprintf(reason, rsize, "empty-lba-window");
		err = -EINVAL;
		goto out;
	}

out:
	fclose(f);
	return err;
}

/*
 * Ring control
 */

static void
tdtest_log_ring(const char *ev, const struct td_xenblkif *blkif,
		const char *extra)
{
	struct tdtest_line l;

	tdtest_line_ring(&l, ev, blkif);
	if (extra)
		tdtest_line_add(&l, " %s", extra);
	tdtest_line_emit(&l);
}

void
tdtest_ring_intake(struct td_xenblkif *blkif, blkif_request_t *reqs[],
		int n_reqs)
{
	struct tdtest_config cfg;
	struct tdtest_line l;
	bool enabled;
	int i;

	tdtest_snapshot(blkif->vbd->tdtest, &cfg, &enabled);
	if (!enabled)
		return;

	for (i = 0; i < n_reqs; i++) {
		if (blkif->dead)
			tdtest_line_ring(&l, "dead-intake", blkif);
		else if (tdtest_log_msg_enabled(&cfg, reqs[i]))
			tdtest_line_ring(&l, "intake", blkif);
		else
			continue;

		tdtest_line_msg(&l, reqs[i]);
		tdtest_line_emit(&l);
	}
}

void
tdtest_ring_connect(struct td_xenblkif *blkif)
{
	struct tdtest_config cfg;
	bool enabled;

	tdtest_snapshot(blkif->vbd->tdtest, &cfg, &enabled);
	if (!enabled)
		return;

	tdtest_log_ring("connect", blkif, NULL);
}

/*
 * Called by tapdisk_xenblkif_disconnect, after the ring has been made dead
 * if @pending, or before it is destroyed otherwise.
 */
void
tdtest_ring_disconnect(struct td_xenblkif *blkif, int pending)
{
	struct tdtest_config cfg;
	struct tdtest_line l;
	bool enabled;

	tdtest_snapshot(blkif->vbd->tdtest, &cfg, &enabled);
	if (!enabled)
		return;

	tdtest_line_ring(&l, "disconnect", blkif);
	tdtest_line_add(&l, " pending=%d", pending);
	tdtest_line_emit(&l);
}

void
tdtest_copyin(struct td_xenblkif *blkif, struct td_xenblkif_req *req, int err)
{
	struct tdtest_config cfg;
	struct tdtest_line l;
	bool enabled;

	tdtest_snapshot(blkif->vbd->tdtest, &cfg, &enabled);
	if (!enabled)
		return;

	if (!blkif->dead && !tdtest_log_msg_enabled(&cfg, &req->msg))
		return;

	tdtest_line_ring(&l, err ? "copyin-failed" : "copyin", blkif);
	tdtest_line_msg(&l, &req->msg);
	if (err)
		tdtest_line_add(&l, " err=%d", err);
	else
		tdtest_line_crc(&l, req);
	tdtest_line_emit(&l);
}

void
tdtest_copyback(struct td_xenblkif *blkif, struct td_xenblkif_req *req,
		int err)
{
	struct tdtest_config cfg;
	struct tdtest_line l;
	bool enabled;

	tdtest_snapshot(blkif->vbd->tdtest, &cfg, &enabled);
	if (!enabled || !tdtest_log_msg_enabled(&cfg, &req->msg))
		return;

	tdtest_line_ring(&l, err ? "copyback-failed" : "copyback", blkif);
	tdtest_line_msg(&l, &req->msg);
	if (err)
		tdtest_line_add(&l, " err=%d", err);
	else
		tdtest_line_crc(&l, req);
	tdtest_line_emit(&l);
}

static const char *
tdtest_rsp_name(int rsp)
{
	switch (rsp) {
	case BLKIF_RSP_OKAY:
		return "okay";
	case BLKIF_RSP_ERROR:
		return "error";
	case BLKIF_RSP_EOPNOTSUPP:
		return "eopnotsupp";
	default:
		return "none";
	}
}

void
tdtest_complete(struct td_xenblkif *blkif, struct td_xenblkif_req *req,
		int err, int rsp, int final)
{
	struct tdtest_config cfg;
	struct tdtest_line l;
	bool enabled;

	tdtest_snapshot(blkif->vbd->tdtest, &cfg, &enabled);
	if (!enabled || !tdtest_log_msg_enabled(&cfg, &req->msg))
		return;

	if (rsp == TDTEST_RSP_NONE && req->msg.operation == BLKIF_OP_READ) {
		tdtest_line_ring(&l, "copyback-skipped-dead", blkif);
		tdtest_line_msg(&l, &req->msg);
		tdtest_line_add(&l, " err=%d", err);
		if (tdtest_req_has_data(req, err))
			tdtest_line_crc(&l, req);
		tdtest_line_emit(&l);
	}

	tdtest_line_ring(&l, "complete", blkif);
	tdtest_line_msg(&l, &req->msg);
	tdtest_line_add(&l, " err=%d rsp=%s final=%d", err,
			tdtest_rsp_name(rsp), final);
	if (tdtest_req_has_data(req, err))
		tdtest_line_crc(&l, req);
	tdtest_line_emit(&l);
}

void
tdtest_dead_ring_destroy(struct td_xenblkif *blkif)
{
	struct tdtest_config cfg;
	bool enabled;

	tdtest_snapshot(blkif->vbd->tdtest, &cfg, &enabled);
	if (!enabled)
		return;

	tdtest_log_ring("dead-destroy", blkif, NULL);
}

/*
 * Read holds
 */

static void
tdtest_log_rhold(const char *ev, struct tdtest_rhold *h, const char *extra)
{
	struct tdtest_line l;

	tdtest_line_ring(&l, ev, h->blkif);
	tdtest_line_msg(&l, &h->req->msg);
	tdtest_line_add(&l, " kind=read hold_ms=%u held_ms=%ld",
			h->ms, tdtest_elapsed_ms(&h->start));
	tdtest_line_crc(&l, h->req);
	if (extra)
		tdtest_line_add(&l, " %s", extra);
	tdtest_line_emit(&l);
}

/*
 * Runs in the main thread. Takes vbd->mutex, as tapdisk_vbd_kick() holds it
 * around request callbacks. Publishes the response even if the request was
 * not the last one of its batch; on a dead ring the response and the
 * copy-back are skipped and the ring may be destroyed.
 */
static void
tdtest_read_release(event_id_t id, char mode __attribute__((unused)),
		void *private)
{
	struct tdtest_rhold *h = private;
	struct td_test *td = h->td;
	td_vbd_t *vbd = td->vbd;
	struct td_xenblkif *blkif = h->blkif;
	struct td_xenblkif_req *req = h->req;

	tapdisk_server_unregister_event(id);

	pthread_mutex_lock(&vbd->mutex);

	pthread_mutex_lock(&td->lock);
	list_del(&h->entry);
	pthread_mutex_unlock(&td->lock);

	tdtest_log_rhold("release", h, NULL);
	free(h);

	tapdisk_xenblkif_complete_held_read(blkif, req);

	pthread_mutex_unlock(&vbd->mutex);
}

/*
 * At the top of the ring request callback, with vbd->mutex held. The
 * outstanding request keeps its ring alive, including across a normal
 * disconnect, until it is completed by tdtest_read_release.
 */
bool
tdtest_read_hold(struct td_xenblkif *blkif, struct td_xenblkif_req *req,
		int error)
{
	struct td_test *td = blkif->vbd->tdtest;
	struct tdtest_rhold *h;
	struct timeval timeout;
	event_id_t event;
	unsigned int ms;

	if (error || blkif->dead || req->msg.operation != BLKIF_OP_READ)
		return false;

	pthread_mutex_lock(&td->lock);

	ms = td->cfg.read_hold_ms;
	if (!td->enabled || !ms ||
	    (td->cfg.read_hold_max && td->read_holds >= td->cfg.read_hold_max) ||
	    !tdtest_in_window(req->msg.sector_number,
			      tdtest_msg_nsect(&req->msg),
			      td->cfg.read_hold_lba_start,
			      td->cfg.read_hold_lba_end)) {
		pthread_mutex_unlock(&td->lock);
		return false;
	}

	h = calloc(1, sizeof(*h));
	if (!h) {
		pthread_mutex_unlock(&td->lock);
		return false;
	}

	h->td = td;
	h->blkif = blkif;
	h->req = req;
	h->ms = ms;
	gettimeofday(&h->start, NULL);

	timeout.tv_sec = ms / 1000;
	timeout.tv_usec = (ms % 1000) * 1000;
	event = tapdisk_server_register_event(SCHEDULER_POLL_TIMEOUT, -1,
			timeout, tdtest_read_release, h);
	if (event < 0) {
		pthread_mutex_unlock(&td->lock);
		free(h);
		return false;
	}

	h->event = event;
	td->read_holds++;
	list_add_tail(&h->entry, &td->read_held);

	tdtest_log_rhold("hold", h, NULL);

	pthread_mutex_unlock(&td->lock);

	return true;
}

/*
 * VBD teardown
 */

/*
 * From tapdisk_vbd_free(). Cancels the timers of held requests so that they
 * cannot fire on freed memory. Held reads are dropped with their ring, as
 * for the dead rings a forced shutdown leaves behind.
 */
void
tdtest_vbd_free(td_vbd_t *vbd)
{
	struct td_test *td = vbd->tdtest;
	struct tdtest_rhold *rh, *rnext;
	struct tdtest_line l;

	list_for_each_entry_safe(rh, rnext, &td->read_held, entry) {
		tapdisk_server_unregister_event(rh->event);
		list_del(&rh->entry);
		tdtest_line_init(&l, "release-cancelled", vbd);
		tdtest_line_add(&l, " ring=%p kind=read reason=vbd-free",
				(void *)rh->blkif);
		tdtest_line_emit(&l);
		free(rh);
	}

	pthread_mutex_destroy(&td->lock);
	free(td);
	vbd->tdtest = NULL;
}

/*
 * Control
 */

static void
tdtest_disable(td_vbd_t *vbd, const char *reason)
{
	struct td_test *td = vbd->tdtest;
	char seq[sizeof(td->cfg.seq)];

	pthread_mutex_lock(&td->lock);
	td->enabled = false;
	memcpy(seq, td->cfg.seq, sizeof(seq));
	pthread_mutex_unlock(&td->lock);

	TDTEST_LOG("off minor=%u seq=%s reason=%s", vbd->uuid, seq, reason);
}

static struct td_test *
tdtest_alloc(td_vbd_t *vbd)
{
	struct td_test *td;

	td = calloc(1, sizeof(*td));
	if (!td)
		return NULL;

	td->vbd = vbd;
	pthread_mutex_init(&td->lock, NULL);
	INIT_LIST_HEAD(&td->read_held);
	tdtest_config_defaults(&td->cfg);

	return td;
}

static void
tdtest_reload(td_vbd_t *vbd)
{
	struct tdtest_config cfg;
	struct td_test *td;
	char reason[160];
	int err;

	err = tdtest_config_load(vbd, &cfg, reason, sizeof(reason));
	if (err) {
		TDTEST_LOG("config rejected minor=%u reason=%s",
				vbd->uuid, reason);
		if (vbd->tdtest)
			tdtest_disable(vbd, "config-rejected");
		return;
	}

	td = vbd->tdtest;
	if (!td) {
		td = tdtest_alloc(vbd);
		if (!td) {
			TDTEST_LOG("config rejected minor=%u reason=enomem",
					vbd->uuid);
			return;
		}
		vbd->tdtest = td;
		TDTEST_LOG("v1 variant=%s built=%s_%s", TDTEST_VARIANT,
				__DATE__, __TIME__);
	}

	pthread_mutex_lock(&td->lock);
	td->cfg = cfg;
	td->enabled = true;
	td->read_holds = 0;
	pthread_mutex_unlock(&td->lock);

	TDTEST_LOG("config minor=%u vdi=%s seq=%s"
			" log=%d log_lba=%"PRIu64":%"PRIu64
			" read_hold_ms=%u read_hold_lba=%"PRIu64":%"PRIu64
			" read_hold_max=%u"
			" write_hold_ms=%u write_hold_lba=%"PRIu64":%"PRIu64
			" write_hold_max=%u"
			" intake_pause=%s dead_chkrng_kick=%d"
			" fix_dead_intake=%d fix_gate_new_ring=%d name=%s",
			vbd->uuid, cfg.vdi, cfg.seq,
			cfg.log ? 1 : 0, cfg.log_lba_start, cfg.log_lba_end,
			cfg.read_hold_ms, cfg.read_hold_lba_start,
			cfg.read_hold_lba_end, cfg.read_hold_max,
			cfg.write_hold_ms, cfg.write_hold_lba_start,
			cfg.write_hold_lba_end, cfg.write_hold_max,
			cfg.intake_pause ? "current" : "0",
			cfg.dead_chkrng_kick ? 1 : 0,
			cfg.fix_dead_intake ? 1 : 0,
			cfg.fix_gate_new_ring ? 1 : 0,
			vbd->name);
}

void
tdtest_signal(bool reload, int code, int32_t value)
{
	td_vbd_t *vbd;

	/* Plain kill carries no minor: ignore it. */
	if (code != SI_QUEUE || value < 0 || value > UINT16_MAX) {
		TDTEST_LOG("%s rejected minor=none reason=not-sigqueue-or-bad-minor"
				" code=%d value=%d", reload ? "config" : "off",
				code, (int)value);
		return;
	}

	vbd = tapdisk_server_get_vbd((td_uuid_t)value);
	if (!vbd) {
		TDTEST_LOG("%s rejected minor=%d reason=unknown-minor",
				reload ? "config" : "off", (int)value);
		return;
	}

	if (reload) {
		tdtest_reload(vbd);
		return;
	}

	if (!vbd->tdtest) {
		TDTEST_LOG("off minor=%u reason=not-addressed", vbd->uuid);
		return;
	}

	tdtest_disable(vbd, "off");
}
