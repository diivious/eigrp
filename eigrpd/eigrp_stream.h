// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP portable byte stream.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _EIGRP_STREAM_H_
#define _EIGRP_STREAM_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

typedef struct eigrp_stream {
	uint8_t *data;
	size_t size;
	size_t getp;
	size_t endp;
} eigrp_stream_t;

eigrp_stream_t *eigrp_stream_new(size_t size);
void eigrp_stream_free(eigrp_stream_t *stream);
void eigrp_stream_reset(eigrp_stream_t *stream);
void eigrp_stream_copy(eigrp_stream_t *dst, const eigrp_stream_t *src);
size_t eigrp_stream_get(void *dst, eigrp_stream_t *stream, size_t size);
uint8_t eigrp_stream_getc(eigrp_stream_t *stream);
uint16_t eigrp_stream_getw(eigrp_stream_t *stream);
uint32_t eigrp_stream_getl(eigrp_stream_t *stream);
size_t eigrp_stream_put(eigrp_stream_t *stream, const void *src, size_t size);
size_t eigrp_stream_putc(eigrp_stream_t *stream, uint8_t value);
size_t eigrp_stream_putw(eigrp_stream_t *stream, uint16_t value);
size_t eigrp_stream_putl(eigrp_stream_t *stream, uint32_t value);
size_t eigrp_stream_put_ipv4(eigrp_stream_t *stream, uint32_t address);
void eigrp_stream_forward_getp(eigrp_stream_t *stream, size_t size);
void eigrp_stream_forward_endp(eigrp_stream_t *stream, size_t size);
void eigrp_stream_set_getp(eigrp_stream_t *stream, size_t position);
void eigrp_stream_set_endp(eigrp_stream_t *stream, size_t position);
int eigrp_stream_recvmsg(eigrp_stream_t *stream, int fd, struct msghdr *msg,
			 int flags, size_t size);

static inline size_t eigrp_stream_get_getp(const eigrp_stream_t *stream)
{
	return stream ? stream->getp : 0;
}

static inline size_t eigrp_stream_get_endp(const eigrp_stream_t *stream)
{
	return stream ? stream->endp : 0;
}

static inline uint8_t *eigrp_stream_data(eigrp_stream_t *stream)
{
	return stream ? stream->data : NULL;
}

static inline const uint8_t *eigrp_stream_const_data(const eigrp_stream_t *stream)
{
	return stream ? stream->data : NULL;
}

static inline uint8_t *eigrp_stream_pnt(eigrp_stream_t *stream)
{
	return stream ? stream->data + stream->getp : NULL;
}

#endif /* _EIGRP_STREAM_H_ */
