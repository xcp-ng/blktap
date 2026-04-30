/*
 * Copyright (c) 2026, Vates SAS
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

#ifndef _TAPDISK_ERR_H_
#define _TAPDISK_ERR_H_

#define td_err_init_errno(e) _td_err_init_errno((e), __func__)
#define td_err_generate_success() _td_err_generate_success(__func__)
#define td_err_generate_errno_error(e) _td_err_generate_errno_error((e), __func__)

#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "tapdisk-message.h"

typedef enum _td_err_code_e {
    TD_SUCCESS,
    TD_USE_ERRNO,
    TD_UNKNOWN
} td_err_code;

typedef struct _td_err_s {
    char				    cmd[TAPDISK_MESSAGE_STRING_LENGTH];
    char				    reason[TAPDISK_MESSAGE_STRING_LENGTH];
    td_err_code             code;
    int					    saved_errno;
} td_err;

static inline const char *
td_err_code_to_string(const td_err_code code)
{
    switch (code) {
        case TD_SUCCESS:
            return "success";
        case TD_USE_ERRNO:
        case TD_UNKNOWN:
            return "invalid error code";
        default:
            ASSERT(0);
            return "unhandled code";
    }
}

static inline const char *
td_err_get_cmd(const td_err *err)
{
    ASSERT(err)
    return err->cmd;
}

static inline const char *
td_err_get_reason(const td_err *err)
{
    ASSERT(err)
    return err->reason;
}

static inline td_err_code
td_err_get_code(const td_err *err)
{
    ASSERT(err)
    return err->code;
}

static inline int
td_err_get_errno(const td_err *err)
{
    ASSERT(err)
    return err->saved_errno;
}

static inline int
td_err_set_cmd(td_err *err, const char *cmd)
{
    ASSERT(err)
    if (!cmd) {
        err->cmd[0] = '\0';
        return 0;
    }
    return snprintf(err->cmd, TAPDISK_MESSAGE_STRING_LENGTH, "%s", cmd);
}

static inline int
td_err_set_reason(td_err *err, const char *reason)
{
    ASSERT(err)
    if (!reason) {
        err->reason[0] = '\0';
        return 0;
    }
    return snprintf(err->reason, TAPDISK_MESSAGE_STRING_LENGTH, "%s", reason);
}

static inline td_err_code
td_err_set_code(td_err *err, const td_err_code code)
{
    ASSERT(err)
    return err->code = code;
}

static inline int
td_err_set_errno(td_err *err, const int saved_errno)
{
    ASSERT(err)
    return err->saved_errno = saved_errno;
}

static inline void
td_err_reset(td_err *err)
{
    ASSERT(err)
    memset(err, 0, sizeof(*err));
}

static inline int
td_err_set_success(td_err *err)
{
    td_err_set_code(err, TD_SUCCESS);
    return td_err_set_errno(err, 0);
}

static inline void
_td_err_init_errno(td_err *err, const char *cmd)
{
    td_err_set_cmd(err, cmd);
    td_err_set_code(err, TD_USE_ERRNO);
}

static inline void
td_err_init(td_err *err, const int saved_errno, const td_err_code code, const char *cmd)
{
    td_err_reset(err);
    td_err_set_code(err, code);
    td_err_set_errno(err, saved_errno);
    td_err_set_cmd(err, cmd);
}

static inline td_err
_td_err_generate_success(const char *cmd)
{
    td_err ret;
    td_err_init(&ret, 0, TD_SUCCESS, cmd);
    return ret;
}

static inline td_err
_td_err_generate_errno_error(const int err, const char *cmd)
{
    td_err ret;
    td_err_init(&ret, err, TD_USE_ERRNO, cmd);
    return ret;
}

#endif //_TAPDISK_ERR_H_
