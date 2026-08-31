/*
 * Copyright (c) 2020, Citrix Systems, Inc.
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

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>
#include <sys/socket.h>

#include <cmocka-compat.h>

#include "control-wrappers.h"

static int enable_mocks = 0;


FILE *
__real_fdopen(int fd, const char *mode);

FILE *
__wrap_fdopen(int fd, const char *mode)
{
	if (enable_mocks) {
		FILE *file = (FILE*)mock();
		if (file == NULL) {
			errno = ENOENT;
		}

		return file;
	}

	return __real_fdopen(fd, mode);
}

int
__wrap_ioctl(int fd, int request, ...)
{
	int result;

	check_expected_int(fd);
	check_expected_int(request);

	result = (int)mock();

	if (result != 0) {
		errno = result;
		result = -1;
	}

	return result;
}

int
__real_open(const char *pathname, int flags);

int
__wrap_open(const char *pathname, int flags)
{
	int result;

	if (enable_mocks) {
		check_expected_ptr(pathname);
		result = mock();
		if (result == -1)
			errno = ENOENT;
		return result;
	}

	return __real_open(pathname, flags);
}

int
__real_close(int fd);

int
__wrap_close(int fd)
{
	int result;

	check_expected_int(fd);
	result = mock();
	if (result != 0)
	{
		errno = result;
		result = 1;
	}
	return result;
}

int
__real_access(const char *pathname, int mode);

int
__wrap_access(const char *pathname, int mode)
{
	int result;

	if (enable_mocks) {
		check_expected_ptr(pathname);

		result = mock();
		if (result != 0) {
			errno = result;
			result = -1;
		}
		return result;
	}
	return __real_access(pathname, mode);
}

size_t
__wrap_read(int fd, void *buf, size_t count)
{
	int result = -1;
	struct mock_read_params *params;

	check_expected_int(fd);
	params = (struct mock_read_params *)mock();

	if (params->result > 0) {
		memcpy(buf, params->data, params->result);
		result = params->result;
	}
	else if (params->result < 0) {
		errno = -params->result;
		result = -1;
	}

	return result;
}

/*
 * Wrap the write call.
 *
 * Checks the passed FD is expected
 * Mock result is amount of written data to report
 * limited to count, negative values are errnos to
 * set and return -1.
 */
size_t
__wrap_write(int fd, const void *buf, size_t count)
{
	int result;

	check_expected_int(fd);
	check_expected_ptr(buf);
	result = mock();

	if (result > (int)count)
		result = count;
	else if (result < 0) {
		errno = -result;
		result = -1;
	}
	return result;
}

int
__wrap_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	int result;
	check_expected_int(sockfd);
	check_expected_ptr(addr);
	result = mock();
	if (result) {
		errno = result;
		result = -1;
	}
	return result;
}

int
__wrap_select(int nfds, fd_set *readfds, fd_set *writefds,
	      fd_set *exceptfds, struct timeval *timeout)
{
	struct mock_select_params *params;
	check_expected_ptr(timeout);
	params = (struct mock_select_params *)mock();
	if (readfds)
		memcpy(readfds, &params->readfds, sizeof(fd_set));
	if (writefds)
		memcpy(writefds, &params->writefds, sizeof(fd_set));
	if (exceptfds)
		memcpy(exceptfds, &params->exceptfds, sizeof(fd_set));
	return params->result;
}

int
__real_mkdir(const char *pathname, mode_t mode);

int
__wrap_mkdir(const char *pathname, mode_t mode)
{
	int result;
	if (enable_mocks) {
		check_expected_ptr(pathname);
		result = mock();
		if (result != 0){
			errno = result;
			result = -1;
		}
		return result;
	}
	return __real_mkdir(pathname, mode);
}

int
__wrap_flock(int fd, int operation)
{
	int result;
	check_expected_int(fd);
	result = mock();
	if (result != 0) {
		errno = result;
		result = -1;
	}
	return result;
}

/*
 * Prior to glibc 2.33, mknod(3) was an inline wrapper in <sys/stat.h>
 * that forwarded to the exported __xmknod() symbol.
 * Since glibc 2.33, mknod() is exported directly so we have to wrap
 * mknod() instead.
 *
 * Compiler flags (-fno-inline, -Og) and glibc version both influence
 * which symbol the call site actually references, so we define both
 * wrappers unconditionally.
 */
int
__wrap_mknod(const char *pathname, mode_t mode, dev_t dev)
{
	int result;

	(void)mode;
	(void)dev;

	check_expected_ptr(pathname);
	result = mock();
	if (result != 0)
	{
		errno = result;
		result = -1;
	}
	return result;
}

int
__wrap___xmknod(int ver, const char *pathname, mode_t mode, dev_t *dev)
{
	(void)ver;
	(void)mode;
	(void)dev;

	return __wrap_mknod(pathname, mode, (dev_t)(dev ? *dev : 0));
}

int
__wrap_unlink(const char *pathname)
{
	int result;
	check_expected_ptr(pathname);
	result = mock();
	if (result != 0)
	{
		errno = result;
		result = -1;
	}
	return result;
}

int
__wrap_socket(int domain, int type, int protocol)
{
	int result;

	check_expected_int(domain);
	check_expected_int(type);
	check_expected_int(protocol);

	result = mock();

	if (result < 0) {
		errno = -result;
		result = -1;
	}
	return result;
}

int
__wrap_glob(const char *pattern, int flags,
	    int (*errfunc) (const char *epath, int eerrno),
	    glob_t *pglob)
{
	check_expected_ptr(pattern);
	int result = mock();
	if (result == 0)
	{
		pglob->gl_pathc = mock();
		pglob->gl_pathv = (char **)mock();
	}
	return result;
}

void
__wrap_globfree(glob_t *pglob)
{
	if (pglob->gl_pathv) {
		test_free(*(pglob->gl_pathv));
	}
}


void enable_control_mocks()
{
	enable_mocks = true;
}

void disable_control_mocks()
{
	enable_mocks = 0;
}
