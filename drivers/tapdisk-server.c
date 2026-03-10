/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.1 only
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <time.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/signalfd.h>
#ifdef HAVE_EVENTFD
#include <sys/eventfd.h>
#else
#include <sys/syscall.h>
#endif

#include "tapdisk-syslog.h"
#include "tapdisk-server.h"
#include "tapdisk-driver.h"
#include "posixaio-backend.h"
#include "libaio-backend.h"
#include "tapdisk-interface.h"
#include "tapdisk-log.h"
#include "td-blkif.h"
#include "timeout-math.h"
#include "util.h"
#include "td-tracepoints.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include "../cpumond/cpumond.h"

#define DBG(_level, _f, _a...)       tlog_write(_level, _f, ##_a)
#define ERR(_err, _f, _a...)         tlog_error(_err, _f, ##_a)

#define TAPDISK_TIOCBS_PER_QUEUE    (MAX_REQUESTS * BLKIF_MAX_SEGMENTS_PER_REQUEST + 50)

typedef struct tapdisk_server {
	int                          run;
	struct list_head             vbds;
	scheduler_t                  scheduler;
	tqueue                       rw_queue[TAPDISK_MAX_VBD_THREADS];
	tqueue                       ro_queue[TAPDISK_MAX_VBD_THREADS];
	struct backend              *ro_backend;
	struct backend              *rw_backend;
	char                        *name;
	char                        *ident;
	int                          facility;

	/* IO threads states */
	struct {
		pthread_t            tid;
		char                 name[16];
		event_id_t           eventid;
		int                  eventfd;
		scheduler_t          scheduler;
	} io_threads[TAPDISK_MAX_VBD_THREADS];

	/* Memory mode state */
	struct {
		int                          wfd; /* event to watch for */
		int                          efd; /* event fd */
		int                          event_control; /* control fd */
		event_id_t                   mem_evid; /* scheduler handle for
		                                          low mem and backoff
							  events */
		event_id_t                   reset_evid; /* scheduler handle
		                                            for backoff reset
							    event */
		enum memory_mode_t           mode; /* memory mode */
		int                          backoff; /* exponential backoff
							 factor */
	} mem_state;

	/* CPU Utilisation Monitor client state */
	struct {
		int                         fd; /* shm fd */
		cpumond_t                  *cpumon; /* mmap pointer */
	} cpumond_state;

	event_id_t                   tlog_reopen_evid;
	event_id_t                   signal_handler_evid;
	int			     sigfd;
} tapdisk_server_t;

static tapdisk_server_t server;

static __thread td_queue_id_t current_qid = 0;

unsigned int PAGE_SIZE;
unsigned int PAGE_MASK;
unsigned int PAGE_SHIFT;

#define tapdisk_server_for_each_vbd(vbd, tmp)			        \
	list_for_each_entry_safe(vbd, tmp, &server.vbds, next)

td_image_t *
tapdisk_server_get_shared_image(td_image_t *image)
{
	td_vbd_t *vbd, *tmpv;
	td_image_t *img, *tmpi;

	if (!td_flag_test(image->flags, TD_OPEN_SHAREABLE))
		return NULL;

	tapdisk_server_for_each_vbd(vbd, tmpv)
		tapdisk_vbd_for_each_image(vbd, img, tmpi)
			if (img->type == image->type &&
			    !strcmp(img->name, image->name))
				return img;

	return NULL;
}

struct list_head *
tapdisk_server_get_all_vbds(void)
{
	return &server.vbds;
}

td_vbd_t *
tapdisk_server_get_vbd(uint16_t uuid)
{
	td_vbd_t *vbd, *tmp;

	tapdisk_server_for_each_vbd(vbd, tmp)
		if (vbd->uuid == uuid)
			return vbd;

	return NULL;
}

void
tapdisk_server_add_vbd(td_vbd_t *vbd)
{
	list_add_tail(&vbd->next, &server.vbds);
}

void
tapdisk_server_remove_vbd(td_vbd_t *vbd)
{
	list_del(&vbd->next);
	INIT_LIST_HEAD(&vbd->next);
	tapdisk_server_check_state();
}

void
tapdisk_server_prep_tiocb(struct tiocb *tiocb, int fd, int rw, char *buf, size_t size,
	long long offset, td_queue_callback_t cb, void *arg)
{
	server.rw_backend->prep(tiocb, fd, rw, buf, size, offset, cb, arg);
}

void
tapdisk_server_queue_tiocb(struct tiocb *tiocb)
{
	server.rw_backend->queue(server.rw_queue[current_qid], tiocb);
}

void
tapdisk_server_prep_tiocb_ro(struct tiocb *tiocb, int fd, int rw, char *buf, size_t size,
	long long offset, td_queue_callback_t cb, void *arg)
{
	server.ro_backend->prep(tiocb, fd, rw, buf, size, offset, cb, arg);
}

void
tapdisk_server_queue_tiocb_ro(struct tiocb *tiocb)
{
	server.ro_backend->queue(server.ro_queue[current_qid], tiocb);
}

void
tapdisk_server_debug(void)
{
	td_vbd_t *vbd, *tmp;

	for (int qid = 0; qid < TAPDISK_MAX_VBD_THREADS; qid++) {
		if (likely(server.rw_queue[qid]))
			server.rw_backend->debug(server.rw_queue[qid]);
		if (likely(server.ro_queue[qid]))
			server.ro_backend->debug(server.ro_queue[qid]);
	}

	tapdisk_server_for_each_vbd(vbd, tmp)
		tapdisk_vbd_debug(vbd);

	DBG(TLOG_INFO, "debug log completed\n");
	tlog_precious(1);
}

void
tapdisk_server_check_state(void)
{
	if (list_empty(&server.vbds))
		server.run = 0;
}

event_id_t
tapdisk_server_register_event(char mode, int fd,
			      struct timeval timeout, event_cb_t cb, void *data)
{
	return scheduler_register_event(&server.scheduler,
					mode, fd, timeout, cb, data);
}

void
tapdisk_server_unregister_event(event_id_t event)
{
	return scheduler_unregister_event(&server.scheduler, event);
}

void
tapdisk_server_mask_event(event_id_t event, int masked)
{
	return scheduler_mask_event(&server.scheduler, event, masked);
}

void
tapdisk_server_set_max_timeout(struct timeval tv)
{
	scheduler_set_max_timeout(&server.scheduler, tv);
}

static
void tapdisk_server_io_thread_wake(td_queue_id_t qid)
{
	uint64_t token = 1;
	int n;

	// XXX: thread_wake might called by IO backend before IO threads are fully
	//      initialized
	if (server.io_threads[qid].eventfd == -1)
		return;

	n = write(server.io_threads[qid].eventfd, &token, sizeof(token));
	ASSERT(n == 8);
}

void tapdisk_server_io_thread_handler(event_id_t id, char mode __attribute__((unused)), void *private)
{
	td_queue_id_t qid = (td_queue_id_t)(long)private;
	uint64_t cnt;
	int n;

	n = read(server.io_threads[qid].eventfd, &cnt, sizeof(cnt));
	ASSERT(n == 8);
}

void
tapdisk_server_io_scheduler_wake(td_queue_id_t qid)
{
	tapdisk_server_io_thread_wake(qid);
}

event_id_t
tapdisk_server_register_io_event(td_queue_id_t qid, char mode, int fd,
				 struct timeval timeout, event_cb_t cb, void *data)
{
	event_id_t eid;

	ASSERT(qid < ARRAY_SIZE(server.io_threads));
	eid =  scheduler_register_event(&server.io_threads[qid].scheduler,
					mode, fd, timeout, cb, data);

	/* Wake thread to use the newly registered event */
	tapdisk_server_io_thread_wake(qid);

	return eid;
}

void
tapdisk_server_unregister_io_event(td_queue_id_t qid, event_id_t event)
{
	ASSERT(qid < ARRAY_SIZE(server.io_threads));
	return scheduler_unregister_event(&server.io_threads[qid].scheduler, event);
}

void
tapdisk_server_mask_io_event(td_queue_id_t qid, event_id_t event, int masked)
{
	ASSERT(qid < ARRAY_SIZE(server.io_threads));
	return scheduler_mask_event(&server.io_threads[qid].scheduler, event, masked);
}

void
tapdisk_server_set_io_max_timeout(td_queue_id_t qid, int seconds)
{
	ASSERT(qid < ARRAY_SIZE(server.io_threads));
	scheduler_set_max_timeout(&server.io_threads[qid].scheduler, TV_SECS(seconds));
}


static void
tapdisk_server_set_retry_timeout(td_queue_id_t qid)
{
	td_vbd_t *vbd, *tmp;

	/* FIXME: VBD list not locked */
	tapdisk_server_for_each_vbd(vbd, tmp) {
		if (tapdisk_vbd_retry_needed(&vbd->queues[qid])) {
			tapdisk_server_set_io_max_timeout(qid, TD_VBD_RETRY_INTERVAL);
			return;
		}
	}
}

static void
tapdisk_server_check_progress(td_queue_id_t qid)
{
	struct timeval now;
	td_vbd_t *vbd, *tmp;

	gettimeofday(&now, NULL);

	tapdisk_server_for_each_vbd(vbd, tmp)
		tapdisk_vbd_check_progress(&vbd->queues[qid]);
}

static void
tapdisk_server_submit_tiocbs(td_queue_id_t qid)
{
	tracepoint(tapdisk, aio_submit, qid, TP_PHASE_BEGIN, 0);
	server.rw_backend->submit_all(server.rw_queue[qid]);
	tracepoint(tapdisk, aio_submit, qid, TP_PHASE_END, 0);

	tracepoint(tapdisk, aio_submit, qid, TP_PHASE_BEGIN, 1);
	server.ro_backend->submit_all(server.ro_queue[qid]);
	tracepoint(tapdisk, aio_submit, qid, TP_PHASE_END, 1);
}

static void
tapdisk_server_kick_responses(td_queue_id_t qid)
{
	td_vbd_t *vbd, *tmp;

	tapdisk_server_for_each_vbd(vbd, tmp)
		tapdisk_vbd_kick(&vbd->queues[qid], false);
}

static void
tapdisk_server_check_vbds(td_queue_id_t qid)
{
	td_vbd_t *vbd, *tmp;

	tapdisk_server_for_each_vbd(vbd, tmp)
		tapdisk_vbd_process_queue(&vbd->queues[qid]);
}

/**
 * Issues new requests. Returns the number of VBDs that contained new requests
 * which have been issued.
 */
static int
tapdisk_server_recheck_vbds(td_queue_id_t qid)
{
	td_vbd_t *vbd, *tmp;
	int rv = 0;

	// TODO: change that, ok for one VBD, but maybe we should have
	//       1:1 thread per queue (for the moment we are sharing a
	//       thread between multiple VBD)
	tapdisk_server_for_each_vbd(vbd, tmp) {
		rv += tapdisk_vbd_recheck_state(&vbd->queues[qid]);
	}

	return rv;
}

static int
tapdisk_server_init_aio(void)
{
	int err;

	for (int qid = 0; qid < TAPDISK_MAX_VBD_THREADS; qid++) {
		err = server.ro_backend->init(&server.ro_queue[qid],
				  TAPDISK_TIOCBS_PER_QUEUE,
				  TIO_DRV_LIO, NULL, qid);
		if (err)
			return err;

		err = server.rw_backend->init(&server.rw_queue[qid],
				  TAPDISK_TIOCBS_PER_QUEUE,
				  TIO_DRV_LIO, NULL, qid);
		if (err)
			return err;
	}

	return 0;
}

static void
tapdisk_server_close_aio(void)
{
	for (int qid = 0; qid < TAPDISK_MAX_VBD_THREADS; qid++) {
		server.rw_backend->free_queue(&server.rw_queue[qid]);
		server.ro_backend->free_queue(&server.ro_queue[qid]);
	}
}

int
tapdisk_server_openlog(const char *name, int options, int facility)
{
	server.facility = facility;
	server.name     = strdup(name);
	server.ident    = tapdisk_syslog_ident(name);

	if (!server.name || !server.ident)
		return -errno;

	openlog(server.ident, options, facility);

	return 0;
}

void
tapdisk_server_closelog(void)
{
	closelog();

	free(server.name);
	server.name = NULL;

	free(server.ident);
	server.ident = NULL;
}

static int
tapdisk_server_open_tlog(void)
{
	int err = 0;

	if (server.name)
		err = tlog_open(server.name, server.facility, TLOG_WARN);

	return err;
}

static void
tapdisk_server_close_tlog(void)
{
	tlog_close();
}

static void
tapdisk_server_close(void)
{
	if (likely(server.tlog_reopen_evid >= 0))
		tapdisk_server_unregister_event(server.tlog_reopen_evid);

	if (likely(server.signal_handler_evid >= 0))
		tapdisk_server_unregister_event(server.signal_handler_evid);

	if (likely(server.sigfd > 0))
		close(server.sigfd);

	tapdisk_server_close_tlog();
	tapdisk_server_close_aio();
}

void
tapdisk_io_iterate(td_queue_id_t qid)
{
	int ret;

	current_qid = qid;

	tapdisk_server_set_retry_timeout(qid);
	tapdisk_server_check_progress(qid);

	ret = scheduler_wait_for_events(&server.io_threads[qid].scheduler);
	if (ret < 0)
		DBG(TLOG_WARN, "server wait returned %s\n", strerror(-ret));

	tapdisk_server_check_vbds(qid);
	do {
		tapdisk_server_submit_tiocbs(qid);
		tapdisk_server_kick_responses(qid);

		ret = tapdisk_server_recheck_vbds(qid);
	} while (ret); /* repeat until there are no new requests to issue */
}

static void*
__tapdisk_io_thread_run(void* arg)
{
	td_queue_id_t  qid = (unsigned long)arg;

	/* TODO: maybe use something else ?
	         threads should be stopped before main loop */
	for(;;)
		tapdisk_io_iterate(qid);

	return NULL;
}

void
tapdisk_server_iterate(void)
{
	td_vbd_t *vbd, *tmp;
	int ret;

	ret = scheduler_wait_for_events(&server.scheduler);
	if (ret < 0)
		DBG(TLOG_WARN, "server wait returned %s\n", strerror(-ret));

	tapdisk_server_for_each_vbd(vbd, tmp)
		tapdisk_vbd_check_state(vbd);
}

static void
__tapdisk_server_run(void)
{
	while (server.run) {
		tapdisk_server_iterate();
	}
}

static void
tapdisk_server_signal_handler(event_id_t id, char mode __attribute__((unused)), void *private)
{
	int signal;
	ssize_t size;
	struct signalfd_siginfo fdsi;
	td_vbd_t *vbd, *tmp;
	struct td_xenblkif *blkif;
	static int xfsz_error_sent = 0;

	while ((size = read(server.sigfd, &fdsi, sizeof(fdsi))) > 0) {
		if (size != sizeof(fdsi)) {
			ERR(EFBIG, "failed to read signals");
			continue;
		}

		signal = fdsi.ssi_signo;

		switch (signal) {
		case SIGBUS:
			tapdisk_server_for_each_vbd(vbd, tmp)
				tapdisk_vbd_close(vbd);
			break;

		case SIGXFSZ:
			ERR(EFBIG, "received SIGXFSZ");

			tapdisk_server_for_each_vbd(vbd, tmp)
				tapdisk_vbd_kill_queue(vbd);

			if (xfsz_error_sent)
				break;

			xfsz_error_sent = 1;
			break;

		case SIGUSR1:
			DBG(TLOG_INFO, "debugging on signal %d\n", signal);
			tapdisk_server_debug();
			break;

		case SIGUSR2:
			DBG(TLOG_INFO, "triggering polling on signal %d\n", signal);
			tapdisk_server_for_each_vbd(vbd, tmp)
				list_for_each_entry(blkif, &vbd->rings, entry)
					tapdisk_start_polling_all_queues(blkif);
			break;

		case SIGHUP:
			tapdisk_server_event_set_timeout(server.tlog_reopen_evid, TV_ZERO);
			break;
		}
	}

	if (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
		ERR(errno, "failed to read signals: %s", strerror(errno));
}


static void
tlog_reopen_cb(event_id_t id, char mode __attribute__((unused)), void *private)
{
	tlog_reopen();
	tapdisk_server_event_set_timeout(id, TV_INF);
}


/* Low memory algorithm:
 * Register for low memory notifications from the kernel.
 * If a low memory notification is received, deregister for the notification
 * and go into low memory mode for an exponential backoff amount of time.  When
 * this time period has finished, return to normal memory mode and reregister
 * for the low memory notification.  Also set a timer to reset the backoff
 * value after a certain amount of time in normal mode.  If a low memory
 * notification is received while the reset timer is running, cancel the reset
 * timer.
 *
 * While perhaps a better alternative would be to remain subscribed to the
 * notifications while in low memory mode and have an exponential backoff based
 * on the notifications, these notifications can be frequent and bursty which
 * is bad if there are a couple of thousand instances of tapdisk running.
 */

#ifndef HAVE_EVENTFD
int eventfd(unsigned int initval, int flags)
{
	return syscall(SYS_eventfd2, initval, flags);
}
#endif

#define MIN_BACKOFF 8
#define MAX_BACKOFF 512
#define RESET_BACKOFF 512

#define MEMORY_PRESSURE_LEVEL "critical"
#define MEMORY_PRESSURE_PATH "/sys/fs/cgroup/memory/memory.pressure_level"
#define EVENT_CONTROL_PATH "/sys/fs/cgroup/memory/cgroup.event_control"

static int tapdisk_server_reset_lowmem_mode(void);

enum memory_mode_t
tapdisk_server_mem_mode(void)
{
	return server.mem_state.mode;
}

static void lowmem_state_init(void)
{
	server.mem_state.wfd = -1;
	server.mem_state.efd = -1;
	server.mem_state.event_control = -1;
	server.mem_state.mem_evid = -1;
	server.mem_state.reset_evid = -1;
	server.mem_state.backoff = MIN_BACKOFF;
	server.mem_state.mode = NORMAL_MEMORY_MODE;
}

static void lowmem_cleanup(void)
{
	if (server.mem_state.wfd >= 0)
		close(server.mem_state.wfd);
	if (server.mem_state.efd >= 0)
		close(server.mem_state.efd);
	if (server.mem_state.event_control >= 0)
		close(server.mem_state.event_control);
	if (server.mem_state.mem_evid >= 0)
		tapdisk_server_unregister_event(server.mem_state.mem_evid);
	if (server.mem_state.reset_evid >= 0)
		tapdisk_server_unregister_event(server.mem_state.reset_evid);

	lowmem_state_init();
}

/* Called when backoff period finishes */
static void lowmem_timeout(event_id_t id, char mode, void *data)
{
	int ret;
	td_vbd_t           *vbd,   *tmpv;
	struct td_xenblkif *blkif, *tmpb;

	server.mem_state.mode = NORMAL_MEMORY_MODE;
	tapdisk_server_unregister_event(server.mem_state.mem_evid);
	server.mem_state.mem_evid = -1;

	tapdisk_server_for_each_vbd(vbd, tmpv)
		tapdisk_vbd_for_each_blkif(vbd, blkif, tmpb) {
		if (likely(blkif->stats.xenvbd))
			td_flag_clear(blkif->stats.xenvbd->flags, BT3_LOW_MEMORY_MODE);
		if (likely(blkif->vbd_stats.stats))
			td_flag_clear(blkif->vbd_stats.stats->flags, BT3_LOW_MEMORY_MODE);
	}

	if ((ret = tapdisk_server_reset_lowmem_mode()) < 0) {
		ERR(-ret, "Failed to re-init low memory handler: %s\n",
		    strerror(-ret));
		lowmem_cleanup();
	}
}

/* We received a low memory event.  Switch into low memory mode. */
static void lowmem_event(event_id_t id, char mode, void *data)
{
	uint64_t result;
	ssize_t n;
	int backoff;

	td_vbd_t           *vbd,   *tmpv;
	struct td_xenblkif *blkif, *tmpb;

	n = read(server.mem_state.efd, &result, sizeof(result));
	if (n < 0) {
		ERR(-errno, "Failed to read from eventfd: %s\n",
		    strerror(-errno));
		lowmem_cleanup();
		return;
	}
	if (n != sizeof(result)) {
		ERR(n,
		    "Failed to read from eventfd: short read\n");
		lowmem_cleanup();
		return;
	}

	close(server.mem_state.efd);
	server.mem_state.efd = -1;
	tapdisk_server_unregister_event(server.mem_state.mem_evid);
	server.mem_state.mem_evid = -1;

	if (server.mem_state.reset_evid != -1) {
		tapdisk_server_unregister_event(server.mem_state.reset_evid);
		server.mem_state.reset_evid = -1;
	}

	/* Back off for a duration in the range n..(2n-1) */
	backoff = rand() % server.mem_state.backoff + server.mem_state.backoff;

	server.mem_state.mem_evid =
		tapdisk_server_register_event(SCHEDULER_POLL_TIMEOUT,
                                      -1,
                                      TV_SECS(backoff),
                                      lowmem_timeout,
                                      NULL);
	if (server.mem_state.mem_evid < 0) {
		ERR(-server.mem_state.mem_evid,
		    "Failed to initialize backoff: %s\n",
		    strerror(-server.mem_state.mem_evid));
		lowmem_cleanup();
		return;
	}
	server.mem_state.mode = LOW_MEMORY_MODE;

	tapdisk_server_for_each_vbd(vbd, tmpv)
		tapdisk_vbd_for_each_blkif(vbd, blkif, tmpb) {
		if (likely(blkif->stats.xenvbd))
			td_flag_set(blkif->stats.xenvbd->flags, BT3_LOW_MEMORY_MODE);
		if (likely(blkif->vbd_stats.stats))
			td_flag_set(blkif->vbd_stats.stats->flags, BT3_LOW_MEMORY_MODE);
	}

	/* Increment backoff up to a limit */
	if (server.mem_state.backoff < MAX_BACKOFF)
		server.mem_state.backoff *= 2;
}

/* If a low memory event isn't received for RESET_BACKOFF seconds, reset the
 * backoff
 */
static void reset_timeout(event_id_t id, char mode, void *data)
{
	server.mem_state.backoff = MIN_BACKOFF;
	tapdisk_server_unregister_event(server.mem_state.reset_evid);
	server.mem_state.reset_evid = -1;
}

/* Register for low memory notifications.  Register a timer to reset the
 * exponential backoff if necessary.
 */
static int
tapdisk_server_reset_lowmem_mode(void)
{
	int ret;
	char line[LINE_MAX];

	server.mem_state.efd = eventfd(0, 0);
	if (server.mem_state.efd == -1)
		return -errno;

	ret = snprintf(line, LINE_MAX, "%d %d " MEMORY_PRESSURE_LEVEL,
		       server.mem_state.efd, server.mem_state.wfd);
	if (ret >= LINE_MAX || ret < 0)
		return -EINVAL;

	if (write(server.mem_state.event_control, line, ret) == -1)
		return -errno;

	server.mem_state.mem_evid =
		tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
                                      server.mem_state.efd,
                                      TV_ZERO,
                                      lowmem_event,
                                      NULL);
	if (server.mem_state.mem_evid < 0)
		return server.mem_state.mem_evid;

	if (server.mem_state.backoff != MIN_BACKOFF) {
		/* Start a timeout to reset the exponential backoff */
		server.mem_state.reset_evid =
			tapdisk_server_register_event(SCHEDULER_POLL_TIMEOUT,
					-1,
					TV_SECS(RESET_BACKOFF),
					reset_timeout,
					NULL);
		if (server.mem_state.reset_evid < 0)
			return server.mem_state.reset_evid;
	}

	return 0;
}

/* Once-off initialization */
static int
tapdisk_server_initialize_lowmem_mode(void)
{
	lowmem_state_init();

	srand(time(NULL));

	server.mem_state.wfd =
		open(MEMORY_PRESSURE_PATH, O_RDONLY);
	if (server.mem_state.wfd == -1)
		return -errno;

	server.mem_state.event_control = open(EVENT_CONTROL_PATH, O_WRONLY);
	if (server.mem_state.event_control == -1)
		return -errno;

	return tapdisk_server_reset_lowmem_mode();
}

static void cpumond_state_init(void)
{
	server.cpumond_state.fd = -1;
	server.cpumond_state.cpumon = (cpumond_t *) 0;
}

static void cpumond_cleanup(void)
{
	if (server.cpumond_state.cpumon)
		munmap(server.cpumond_state.cpumon, sizeof(cpumond_t));
	if (server.cpumond_state.fd >= 0)
		close(server.cpumond_state.fd);

	cpumond_state_init();
}

float
tapdisk_server_system_idle_cpu(void)
{
	if (server.cpumond_state.cpumon > 0)
		return server.cpumond_state.cpumon->idle;
	else
		return 0.0;
}

/* Create the CPU Utilisation Monitor client. */
static int
tapdisk_server_initialize_cpumond_client(void)
{
	server.cpumond_state.fd = shm_open(CPUMOND_PATH, O_RDONLY, 0);
	if (server.cpumond_state.fd == -1)
		return -errno;

	server.cpumond_state.cpumon = mmap(NULL, sizeof(cpumond_t), PROT_READ, MAP_PRIVATE, server.cpumond_state.fd, 0);
	if (server.cpumond_state.cpumon == (cpumond_t *) -1) {
		server.cpumond_state.cpumon = 0;
		return -errno;
	}

	return 0;
}

int
tapdisk_server_init(void)
{
	int ret;
	unsigned int i = 0;

	PAGE_SIZE = sysconf(_SC_PAGESIZE);
	PAGE_MASK = ~(PAGE_SIZE - 1);

	for (i = PAGE_SIZE, PAGE_SHIFT = 0; i > 1; i >>= 1, PAGE_SHIFT++);

	memset(&server, 0, sizeof(server));
	INIT_LIST_HEAD(&server.vbds);

	server.tlog_reopen_evid = -1;
	server.signal_handler_evid = -1;

	scheduler_initialize(&server.scheduler);

	for (int qid = 0; qid < ARRAY_SIZE(server.io_threads); qid++) {
		server.io_threads[qid].tid = 0;   /* XXX: not portable; but practical... */
		server.io_threads[qid].eventfd = -1;
		scheduler_initialize(&server.io_threads[qid].scheduler);
	}

	if ((ret = tapdisk_server_initialize_lowmem_mode()) < 0) {
		EPRINTF("Failed to initialize low memory handler: %s\n",
		        strerror(-ret));
		lowmem_cleanup();
		goto out;
	}

	if ((ret = tapdisk_server_initialize_cpumond_client()) < 0) {
		EPRINTF("Failed to connect to cpumond: %s (continuing without it)\n",
			strerror(-ret));
		cpumond_cleanup();
		ret = 0;  /* non-fatal: runnning without cpumond is ok */
	}

out:
	return ret;
}

int
tapdisk_server_complete(void)
{
	int err;
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGBUS);
	sigaddset(&set, SIGXFSZ);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGUSR2);
	sigaddset(&set, SIGHUP);
	pthread_sigmask(SIG_BLOCK, &set, NULL);

	server.rw_backend = get_libaio_backend();
	server.ro_backend = get_libaio_backend();

	err = tapdisk_server_init_aio();
	if (err)
		goto fail;

	err = tapdisk_server_open_tlog();
	if (err)
		goto fail;

	for (int qid = 0; qid < ARRAY_SIZE(server.io_threads); qid++) {
		pthread_t tid;
		int fd;

		/* Wake IO thread mecanism */
		fd = eventfd(0, 0);
		if (fd < 0) {
			EPRINTF("Failed to create eventfd: %s\n", strerror(errno));
			goto fail;
		}
		server.io_threads[qid].eventfd = fd;

		server.io_threads[qid].eventid =
			tapdisk_server_register_io_event(qid,
							 SCHEDULER_POLL_READ_FD,
							 server.io_threads[qid].eventfd,
							 TV_INF,
							 tapdisk_server_io_thread_handler,
							 (void*)(long)qid);

		/* Create thread */
		err = pthread_create(&tid , NULL, __tapdisk_io_thread_run, (void*)(unsigned long)qid);
		if (err) {
			EPRINTF("Failed to create IO thread #%d\n", qid);
			goto fail;
		}
		server.io_threads[qid].tid = tid;

		snprintf(server.io_threads[qid].name,
			 sizeof(server.io_threads[qid].name), "td-queue-%d", qid);
		pthread_setname_np(tid, server.io_threads[qid].name);
	}

	server.run = 1;

	return 0;

fail:
	for (int qid = 0; qid < ARRAY_SIZE(server.io_threads); qid++) {
		if (server.io_threads[qid].tid)
			pthread_cancel(server.io_threads[qid].tid);

		if (server.io_threads[qid].eventfd != -1)
			close(server.io_threads[qid].eventfd);

		tapdisk_server_unregister_io_event(qid, server.io_threads[qid].eventid);
	}

	tapdisk_server_close_tlog();
	tapdisk_server_close_aio();
	return err;
}

int
tapdisk_server_initialize(const char *read, const char *write)
{
	int err;


	err = tapdisk_server_init();
	if (err)
		goto fail;

	err = tapdisk_server_complete();
	if (err)
		goto fail;

	return 0;

fail:
	tapdisk_server_close();
	return err;
}

int
tapdisk_server_run()
{
	int err;
	sigset_t set;

	err = tapdisk_set_resource_limits();
	if (err)
		return err;

	sigemptyset(&set);
	sigaddset(&set, SIGBUS);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGUSR2);
	sigaddset(&set, SIGHUP);
	sigaddset(&set, SIGXFSZ);
	server.sigfd = signalfd(-1, &set, SFD_NONBLOCK);
	if (server.sigfd == -1) {
		err = errno;
		EPRINTF("failed to create a new signalfd: %s\n",
			strerror(err));
		goto out;
	}

	if (sigprocmask(SIG_BLOCK, &set, NULL) == -1) {
		err = errno;
		EPRINTF("failed to block signals we'd like to handle with signalfd: %s\n",
			strerror(err));
		goto out;
	}

	err = tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
			server.sigfd, TV_ZERO,
			tapdisk_server_signal_handler, &server);
	if (unlikely(err < 0)) {
		EPRINTF("failed to register signal handler event: %s\n", strerror(-err));
		goto out;
	}

	server.signal_handler_evid = err;

	err = tapdisk_server_register_event(SCHEDULER_POLL_TIMEOUT, -1,	TV_INF,
			tlog_reopen_cb,	NULL);
	if (unlikely(err < 0)) {
		EPRINTF("failed to register reopen log event: %s\n", strerror(-err));
		goto out;
	}

	server.tlog_reopen_evid = err;

	err = 0;

	__tapdisk_server_run();

out:
	tapdisk_server_close();

	return err;
}

int
tapdisk_server_event_set_timeout(event_id_t event_id, struct timeval timeo) {
	return scheduler_event_set_timeout(&server.scheduler, event_id, timeo);
}

int
tapdisk_server_io_event_set_timeout(td_queue_id_t qid, event_id_t event_id, struct timeval timeo) {
	ASSERT(qid < ARRAY_SIZE(server.io_threads));
	return scheduler_event_set_timeout(&server.io_threads[qid].scheduler,
					   event_id, timeo);
}

