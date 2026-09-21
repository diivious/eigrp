// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP portable byte stream.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "eigrpd/eigrp_stream.h"

static int eigrp_stream_ensure(eigrp_stream_t *stream, size_t needed)
{
	uint8_t *data;
	size_t size;

	if (!stream)
		return 0;
	if (needed <= stream->size)
		return 1;
	size = stream->size ? stream->size : 64;
	while (size < needed) {
		if (size > SIZE_MAX / 2)
			return 0;
		size *= 2;
	}
	data = realloc(stream->data, size);
	if (!data)
		return 0;
	stream->data = data;
	stream->size = size;
	return 1;
}

eigrp_stream_t *eigrp_stream_new(size_t size)
{
	eigrp_stream_t *stream = calloc(1, sizeof(*stream));
	if (!stream)
		return NULL;
	if (size && !eigrp_stream_ensure(stream, size)) {
		free(stream);
		return NULL;
	}
	return stream;
}

void eigrp_stream_free(eigrp_stream_t *stream)
{
	if (!stream)
		return;
	free(stream->data);
	free(stream);
}

void eigrp_stream_reset(eigrp_stream_t *stream)
{
	if (!stream)
		return;
	stream->getp = 0;
	stream->endp = 0;
}

void eigrp_stream_copy(eigrp_stream_t *dst, const eigrp_stream_t *src)
{
	if (!dst || !src || !eigrp_stream_ensure(dst, src->endp))
		return;
	if (src->endp)
		memcpy(dst->data, src->data, src->endp);
	dst->getp = src->getp;
	dst->endp = src->endp;
}

size_t eigrp_stream_get(void *dst, eigrp_stream_t *stream, size_t size)
{
	size_t available;
	if (!stream || !dst)
		return 0;
	available = stream->endp > stream->getp ? stream->endp - stream->getp : 0;
	if (size > available)
		size = available;
	if (size) {
		memcpy(dst, stream->data + stream->getp, size);
		stream->getp += size;
	}
	return size;
}

uint8_t eigrp_stream_getc(eigrp_stream_t *stream)
{
	uint8_t value = 0;
	(void)eigrp_stream_get(&value, stream, sizeof(value));
	return value;
}

uint16_t eigrp_stream_getw(eigrp_stream_t *stream)
{
	uint16_t value = 0;
	(void)eigrp_stream_get(&value, stream, sizeof(value));
	return ntohs(value);
}

uint32_t eigrp_stream_getl(eigrp_stream_t *stream)
{
	uint32_t value = 0;
	(void)eigrp_stream_get(&value, stream, sizeof(value));
	return ntohl(value);
}

size_t eigrp_stream_put(eigrp_stream_t *stream, const void *src, size_t size)
{
	if (!stream || (!src && size) || !eigrp_stream_ensure(stream, stream->endp + size))
		return 0;
	if (size) {
		memcpy(stream->data + stream->endp, src, size);
		stream->endp += size;
	}
	return size;
}

size_t eigrp_stream_putc(eigrp_stream_t *stream, uint8_t value)
{
	return eigrp_stream_put(stream, &value, sizeof(value));
}

size_t eigrp_stream_putw(eigrp_stream_t *stream, uint16_t value)
{
	value = htons(value);
	return eigrp_stream_put(stream, &value, sizeof(value));
}

size_t eigrp_stream_putl(eigrp_stream_t *stream, uint32_t value)
{
	value = htonl(value);
	return eigrp_stream_put(stream, &value, sizeof(value));
}

size_t eigrp_stream_put_ipv4(eigrp_stream_t *stream, uint32_t address)
{
	return eigrp_stream_put(stream, &address, sizeof(address));
}

void eigrp_stream_forward_getp(eigrp_stream_t *stream, size_t size)
{
	if (!stream)
		return;
	if (stream->getp + size > stream->endp)
		stream->getp = stream->endp;
	else
		stream->getp += size;
}

void eigrp_stream_forward_endp(eigrp_stream_t *stream, size_t size)
{
	if (!stream || !eigrp_stream_ensure(stream, stream->endp + size))
		return;
	stream->endp += size;
}

void eigrp_stream_set_getp(eigrp_stream_t *stream, size_t position)
{
	if (!stream)
		return;
	stream->getp = position <= stream->endp ? position : stream->endp;
}

void eigrp_stream_set_endp(eigrp_stream_t *stream, size_t position)
{
	if (!stream || !eigrp_stream_ensure(stream, position))
		return;
	stream->endp = position;
	if (stream->getp > stream->endp)
		stream->getp = stream->endp;
}

int eigrp_stream_recvmsg(eigrp_stream_t *stream, int fd, struct msghdr *msg,
			 int flags, size_t size)
{
	struct iovec local_iov;
	struct iovec *saved_iov;
	size_t saved_iovlen;
	ssize_t ret;

	if (!stream || !msg || !eigrp_stream_ensure(stream, size))
		return -1;
	local_iov.iov_base = stream->data;
	local_iov.iov_len = size;
	saved_iov = msg->msg_iov;
	saved_iovlen = msg->msg_iovlen;
	msg->msg_iov = &local_iov;
	msg->msg_iovlen = 1;
	ret = recvmsg(fd, msg, flags);
	msg->msg_iov = saved_iov;
	msg->msg_iovlen = saved_iovlen;
	if (ret >= 0) {
		stream->getp = 0;
		stream->endp = (size_t)ret;
	}
	return (int)ret;
}
