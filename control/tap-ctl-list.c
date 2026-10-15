/*
 * Copyright (c) 2016, Citrix Systems, Inc.
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <glob.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "tap-ctl.h"
#include "blktap.h"
#include "list.h"

#define PROC_LOCKS "/proc/locks"

static tap_list_t*
_tap_list_alloc(void)
{
	const size_t sz = sizeof(tap_list_t);
	tap_list_t *tl;

	tl = malloc(sz);
	if (!tl)
		return NULL;

	tl->pid     = -1;
	tl->minor = -1;
	tl->state   = -1;
	tl->type    = NULL;
	tl->path    = NULL;

	INIT_LIST_HEAD(&tl->entry);

	return tl;
}

static void
_tap_list_free(tap_list_t *tl)
{
	list_del_init(&tl->entry);

	if (tl->type) {
		free(tl->type);
		tl->type = NULL;
	}

	if (tl->path) {
		free(tl->path);
		tl->path = NULL;
	}

	free(tl);
}

int
_parse_params(const char *params, char **type, char **path)
{
	char *ptr;
	size_t len;

	ptr = strchr(params, ':');
	if (!ptr)
		return -EINVAL;

	len = ptr - params;

	*type = strndup(params, len);
	*path =  strdup(params + len + 1);

	if (!*type || !*path) {
		free(*type);
		*type = NULL;

		free(*path);
		*path = NULL;

		return -errno;
	}

	return 0;
}

void
tap_ctl_list_free(struct list_head *list)
{
	tap_list_t *tl, *n;

	tap_list_for_each_entry_safe(tl, n, list)
		_tap_list_free(tl);
}

/**
 * Returns a list running tapdisks. tapdisks are searched for by looking for
 * their control socket.
 */
static int
_tap_ctl_find_minors(struct list_head *list)
{
	const char *pattern;
	glob_t glbuf = { 0 };
	tap_list_t *tl;
	int i, err;

	INIT_LIST_HEAD(list);

	pattern = BLKTAP2_NP_RUN_DIR"/tapdisk*";

	err = glob(pattern, 0, NULL, &glbuf);
	switch (err) {
	case GLOB_NOMATCH:
		goto done;

	case GLOB_ABORTED:
	case GLOB_NOSPACE:
		err = -errno;
		EPRINTF("%s: glob failed, err %d", pattern, err);
		goto fail;
	}

	for (i = 0; i < glbuf.gl_pathc; ++i) {
		int n;

		tl = _tap_list_alloc();
		if (!tl) {
			err = -ENOMEM;
			goto fail;
		}

		n = sscanf(glbuf.gl_pathv[i], BLKTAP2_NP_RUN_DIR"/tapdisk-%d", &tl->minor);
		if (n != 1) {
			_tap_list_free(tl);
			continue;
		}

		list_add_tail(&tl->entry, list);
	}

done:
	err = 0;
out:
	if (glbuf.gl_pathv)
		globfree(&glbuf);

	return err;

fail:
	tap_ctl_list_free(list);
	goto out;
}

int
_tap_ctl_find_tapdisks(struct list_head *list)
{
	const char *pattern;
	glob_t glbuf = { 0 };
	int err, i, n_taps = 0;

	pattern = BLKTAP2_CONTROL_DIR"/"BLKTAP2_CONTROL_SOCKET"*";

	INIT_LIST_HEAD(list);

	err = glob(pattern, 0, NULL, &glbuf);
	switch (err) {
	case GLOB_NOMATCH:
		goto done;

	case GLOB_ABORTED:
	case GLOB_NOSPACE:
		err = -errno;
		EPRINTF("%s: glob failed, err %d", pattern, err);
		goto fail;
	}

	for (i = 0; i < glbuf.gl_pathc; ++i) {
		tap_list_t *tl;
		int n;

		tl = _tap_list_alloc();
		if (!tl) {
			err = -ENOMEM;
			goto fail;
		}

		n = sscanf(glbuf.gl_pathv[i],
				BLKTAP2_CONTROL_DIR"/"BLKTAP2_CONTROL_SOCKET"%d",
				&tl->pid);
		if (n != 1)
			goto skip;

		/*
		 * The pid is taken directly from the socket name; liveness is
		 * established later when _tap_ctl_list_tapdisk() connects. This
		 * avoids a redundant connect + PID round-trip per tapdisk.
		 */
		list_add_tail(&tl->entry, list);
		n_taps++;
		continue;

skip:
		_tap_list_free(tl);
	}

done:
	err = 0;
out:
	if (glbuf.gl_pathv)
		globfree(&glbuf);

	return err ? : n_taps;

fail:
	tap_ctl_list_free(list);
	goto out;
}

/**
 * Retrieves all the VBDs a tapdisk is serving.
 *
 * @param pid the process ID of the tapdisk whose VBDs should be retrieved
 * @param list output parameter that receives the list of VBD
 * @returns 0 on success, an error code otherwise
 */
int
_tap_ctl_list_tapdisk(pid_t pid, struct list_head *list)
{
	struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
	tapdisk_message_t message;
	tap_list_t *tl;
	int err, sfd;

	err = tap_ctl_connect_id(pid, &sfd);
	if (err)
		return err;

	memset(&message, 0, sizeof(message));
	message.type   = TAPDISK_MESSAGE_LIST;
	message.cookie = -1;

	err = tap_ctl_write_message(sfd, &message, &timeout);
	if (err)
		goto out;

	INIT_LIST_HEAD(list);

	do {
		err = tap_ctl_read_message(sfd, &message, &timeout);
		if (err) {
			err = -EPROTO;
			goto fail;
		}

		if (message.u.list.count == 0)
			break;

		tl = _tap_list_alloc();
		if (!tl) {
			err = -ENOMEM;
			goto fail;
		}

		tl->pid    = pid;
		tl->minor  = message.u.list.minor;
		tl->state  = message.u.list.state;

		if (message.u.list.path[0] != 0) {
			err = _parse_params(message.u.list.path,
					    &tl->type, &tl->path);
			if (err) {
				_tap_list_free(tl);
				goto fail;
			}
		}

		list_add(&tl->entry, list);
	} while (1);

	err = 0;
out:
	close(sfd);
	return err;

fail:
	tap_ctl_list_free(list);
	goto out;
}

int
tap_ctl_list(struct list_head *list)
{
	struct list_head minors, tapdisks, vbds;
	tap_list_t *t, *next_t, *v, *next_v, *m, *next_m;
	int err;

	/*
	 * Find all minors, find all tapdisks, then list all minors
	 * they attached to. Output is a 3-way outer join.
	 */

	err = _tap_ctl_find_minors(&minors);
	if (err < 0)
		goto fail;

	err = _tap_ctl_find_tapdisks(&tapdisks);
	if (err < 0) {
		EPRINTF("error finding tapdisks: %s\n", strerror(-err));
		goto fail;
	}

	INIT_LIST_HEAD(list);

	tap_list_for_each_entry_safe(t, next_t, &tapdisks) {

		err = _tap_ctl_list_tapdisk(t->pid, &vbds);

		if (err) {
			/*
			 * The tapdisk went away between the glob and the
			 * connect; drop it rather than reporting a stale pid.
			 */
			_tap_list_free(t);
			continue;
		}

		if (list_empty(&vbds)) {
			list_move_tail(&t->entry, list);
			continue;
		}

		tap_list_for_each_entry_safe(v, next_v, &vbds) {

			tap_list_for_each_entry_safe(m, next_m, &minors)
				if (m->minor == v->minor) {
					_tap_list_free(m);
					break;
				}

			list_move_tail(&v->entry, list);
		}

		_tap_list_free(t);
	}

	/* orphaned minors */
	list_splice_tail(&minors, list);

	return 0;

fail:
		tap_ctl_list_free(list);

	tap_ctl_list_free(&vbds);
	tap_ctl_list_free(&tapdisks);
	tap_ctl_list_free(&minors);

	return err;
}

int
tap_ctl_list_pid(pid_t pid, struct list_head *list)
{
	tap_list_t *t;
	int err;

	t = _tap_list_alloc();
	if (!t)
		return -ENOMEM;

	t->pid = tap_ctl_get_pid(pid);
	if (t->pid < 0) {
		_tap_list_free(t);
		return 0;
	}

	err = _tap_ctl_list_tapdisk(t->pid, list);

	if (err || list_empty(list))
		list_add_tail(&t->entry, list);

	return 0;
}

/**
 * Finds the pid of the process holding a BSD (flock) lock on the file
 * identified by (dev, ino), by scanning /proc/locks a line at a time. tapdisk
 * holds such a lock on a minor's marker file for exactly as long as it serves
 * that minor, so this maps a minor to its owning tapdisk without contacting any
 * tapdisk.
 *
 * Note: flock() locks are only visible via /proc/locks (fcntl(F_GETLK) cannot
 * see them, as they are a separate lock space).
 *
 * @returns the holder pid (> 0); 0 if a matching lock exists but its holder is
 * not visible in this pid namespace; -1 if no matching lock is held or
 * /proc/locks could not be read.
 */
static pid_t
_tap_ctl_marker_holder(dev_t dev, ino_t ino)
{
	FILE *fp;
	char line[256];
	unsigned int target_major = major(dev);
	unsigned int target_minor = minor(dev);
	pid_t pid = -1;

	fp = fopen(PROC_LOCKS, "r");
	if (!fp)
		return -1;

	/*
	 * Each line looks like:
	 *   2: FLOCK  ADVISORY  WRITE 18210 00:45:230 0 EOF
	 * i.e. "<id>: <type> <mode> <rw> <pid> <major>:<minor>:<inode> ...",
	 * with major/minor in hex and inode in decimal. Blocked waiters are
	 * printed as "<id>: -> <type> ..." and are skipped by the FLOCK check.
	 * Lines are short and bounded, so a fixed line buffer is sufficient.
	 */
	while (fgets(line, sizeof(line), fp)) {
		char *tok, *savetok, *pidtok, *devtok;
		unsigned int maj, min;
		unsigned long lino;

		tok = strtok_r(line, " \t", &savetok);		/* "<id>:" */
		if (!tok)
			continue;

		tok = strtok_r(NULL, " \t", &savetok);		/* type / "->" */
		if (!tok || strcmp(tok, "FLOCK"))
			continue;

		strtok_r(NULL, " \t", &savetok);		/* ADVISORY/... */
		strtok_r(NULL, " \t", &savetok);		/* READ/WRITE */
		pidtok = strtok_r(NULL, " \t", &savetok);	/* pid */
		devtok = strtok_r(NULL, " \t", &savetok);	/* maj:min:ino */
		if (!pidtok || !devtok)
			continue;

		if (sscanf(devtok, "%x:%x:%lu", &maj, &min, &lino) != 3)
			continue;

		if (maj == target_major && min == target_minor &&
		    lino == (unsigned long)ino) {
			pid = (pid_t)atoi(pidtok);
			break;
		}
	}

	fclose(fp);

	return pid;
}

/**
 * Lists the single VBD attached to a given minor, without enumerating every
 * tapdisk. The owning tapdisk is located directly from the flock it holds on
 * the minor's marker file (see _tap_ctl_marker_holder), so the cost is one stat
 * + one read of /proc/locks + one control-socket round-trip, independent of the
 * number of running tapdisks.
 *
 * The result is identical to tap_ctl_list() filtered to \p minor. On any
 * unexpected condition (marker present but holder unreachable, stale marker,
 * /proc/locks unreadable) it falls back to the full tap_ctl_list() scan so the
 * outcome is always the same as the slow path.
 */
int
tap_ctl_list_minor(int minor, struct list_head *list)
{
	struct list_head vbds = LIST_HEAD_INIT(vbds);
	tap_list_t *v, *next_v, *found = NULL;
	struct stat st;
	char *path = NULL;
	pid_t pid;
	int err;

	INIT_LIST_HEAD(list);

	err = asprintf(&path, "%s/tapdisk-%d", BLKTAP2_NP_RUN_DIR, minor);
	if (err == -1)
		return -errno;

	err = stat(path, &st);
	free(path);
	if (err) {
		if (errno == ENOENT)
			return 0;	/* minor not allocated: empty list */
		goto fallback;
	}

	pid = _tap_ctl_marker_holder(st.st_dev, st.st_ino);
	if (pid < 0) {
		/* Marker exists but nobody serves it: orphan minor. */
		tap_list_t *tl = _tap_list_alloc();
		if (!tl)
			return -ENOMEM;
		tl->minor = minor;
		list_add_tail(&tl->entry, list);
		return 0;
	}
	if (pid == 0)
		goto fallback;		/* served, but holder not visible to us */

	err = _tap_ctl_list_tapdisk(pid, &vbds);
	if (err)
		goto fallback;		/* holder went away / IPC error */

	tap_list_for_each_entry_safe(v, next_v, &vbds) {
		if (!found && v->minor == minor) {
			list_move_tail(&v->entry, list);
			found = v;
		} else
			_tap_list_free(v);
	}

	if (!found)
		goto fallback;		/* stale marker: minor not served here */

	return 0;

fallback:
	tap_ctl_list_free(&vbds);
	tap_ctl_list_free(list);
	return tap_ctl_list(list);
}

int
tap_ctl_find_minor(const char *type, const char *path)
{
	struct list_head list = LIST_HEAD_INIT(list);
	tap_list_t *entry;
	int minor, err;

	err = tap_ctl_list(&list);
	if (err)
		return err;

	minor = -1;

	tap_list_for_each_entry(entry, &list) {

		if (type && (!entry->type || strcmp(entry->type, type)))
			continue;

		if (path && (!entry->path || strcmp(entry->path, path)))
			continue;

		minor = entry->minor;
		break;
	}

	tap_ctl_list_free(&list);

	return minor >= 0 ? minor : -ENOENT;
}
