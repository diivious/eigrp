#ifndef EIGRP_TEST_PRINTFRR_H
#define EIGRP_TEST_PRINTFRR_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

static inline ssize_t snprintfrr(char *buf, size_t len, const char *fmt, ...)
{
	va_list ap;
	int rc;

	va_start(ap, fmt);
	rc = vsnprintf(buf, len, fmt, ap);
	va_end(ap);
	return rc;
}

#endif /* EIGRP_TEST_PRINTFRR_H */
