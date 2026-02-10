/*-
 * Copyright 2026 Adrian Chadd <adrian@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Netflix, Inc. nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <err.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include "fd_util.h"

int
comm_fd_set_nonblocking(int fd, int enable)
{
	int a;

	a = fcntl(fd, F_GETFL, 0);
	/* XXX check */
	if (enable)
		a |= O_NONBLOCK;
	else
		a &= ~O_NONBLOCK;
	return (fcntl(fd, F_SETFL, a));
}

static int
comm_fd_listenfd_setup_tcp(struct sockaddr_storage *sin, int type, int len)
{
	int fd;
	int a;

	fd = socket(type, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "%s: socket() failed; errno=%d (%s)\n",
		    __func__,
		    errno,
		    strerror(errno));
		return (-1);
	}

	/* Make non-blocking */
	(void) comm_fd_set_nonblocking(fd, 1);

#if 0
	/* make reuse */
	a = 1;
	(void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &a, sizeof(a));
#endif

	/* and reuse port */
	a = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &a, sizeof(a)) < 0) {
		err(1, "%s: setsockopt", __func__);
	}

	if (bind(fd, (struct sockaddr *) sin, len) < 0) {
		fprintf(stderr, "%s: bind() failed; errno=%d (%s)\n",
		    __func__,
		    errno,
		    strerror(errno));
		close(fd);
		return (-1);
	}

	if (listen(fd, -1) < 0) {
		fprintf(stderr, "%s: listen() failed; errno=%d (%s)\n",
		    __func__,
		    errno,
		    strerror(errno));
		close(fd);
		return (-1);
	}

	return (fd);
}

int
comm_fd_create_listen_tcp_v4(int port)
{
	struct sockaddr_storage s;
	struct sockaddr_in *sin;

	bzero(&s, sizeof(s));

	sin = (struct sockaddr_in *) &s;

	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = 0;
	sin->sin_port = htons(port);
	sin->sin_len = sizeof(struct sockaddr_in);

	return (comm_fd_listenfd_setup_tcp(&s, AF_INET,
	    sizeof(struct sockaddr_in)));
}

int
comm_fd_create_listen_tcp_v6(int port)
{
	struct sockaddr_storage s;
	struct sockaddr_in6 *sin6;

	bzero(&s, sizeof(s));

	sin6 = (struct sockaddr_in6 *) &s;

	sin6->sin6_family = AF_INET6;
	sin6->sin6_addr = in6addr_any;
	sin6->sin6_port = htons(port);
	sin6->sin6_len = sizeof(struct sockaddr_in6);

	return (comm_fd_listenfd_setup_tcp(&s, AF_INET6,
	    sizeof(struct sockaddr_in6)));
}
