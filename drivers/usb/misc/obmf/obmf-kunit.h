/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * What the OBMF-ICP unit tests share; not part of the driver.
 */

#ifndef _OBMF_KUNIT_H
#define _OBMF_KUNIT_H

#include <kunit/test.h>

/*
 * obmf_test_warns(test) { ... }: a block whose WARN the case causes on
 * purpose, to check a driver bug's guard. KUnit hides its backtrace and
 * panic_on_warn ignores it; a KUnit older than the suppression API runs
 * the block as it is, and the WARN prints.
 */
#ifdef KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT
#define obmf_test_warns(test) kunit_warning_suppress(test)
#else
#define obmf_test_warns(test) \
	for (int __obmf_w = ((void)(test), 1); __obmf_w; __obmf_w = 0)
#endif

#endif /* _OBMF_KUNIT_H */
