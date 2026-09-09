/*
 * Copyright (c) 2026, Vates SAS.
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

#ifndef __CMOCKA_COMPAT_H__
#define __CMOCKA_COMPAT_H__

#include <cmocka.h>

/*
 * Newer cmocka releases deprecate some macros, and -Werror=deprecated-declarations
 * turns the warning into a build failure.
 * Fall back to the old macros when the new ones aren't available.
 */
#ifndef expect_int_value
# define expect_int_value(function, parameter, value) \
	expect_value(function, parameter, value)
#endif

#ifndef expect_uint_value
# define expect_uint_value(function, parameter, value) \
	expect_value(function, parameter, value)
#endif

#ifndef expect_ptr_value
# define expect_ptr_value(function, parameter, value) \
	expect_uint_value(function, parameter, (uintptr_t)value)
#endif

#ifndef expect_uint_value_count
# define expect_uint_value_count(function, parameter, value, count) \
	expect_value_count(function, parameter, value, count)
#endif

#ifndef check_expected_int
# define check_expected_int(parameter) check_expected(parameter)
#endif
#ifndef check_expected_uint
# define check_expected_uint(parameter) check_expected(parameter)
#endif
#ifndef check_expected_ptr
# define check_expected_ptr(parameter) check_expected(parameter)
#endif

#ifndef assert_int_in_range
# define assert_int_in_range(value, minimum, maximum) \
	assert_in_range(value, minimum, maximum)
#endif
#ifndef will_return_int_always
# define will_return_int_always(function, value) \
	will_return_always(function, value)
#endif
#ifndef will_return_ptr_always
# define will_return_ptr_always(function, value) \
	will_return_always(function, value)
#endif

#endif /* __CMOCKA_COMPAT_H__ */
