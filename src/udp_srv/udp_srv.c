/*-
 * Copyright (c) 2013 Netflix, Inc.  All rights reserved.
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
#include <err.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "fde.h"
#include "comm.h"

#define	NUM_THREADS		16

#define	IO_SIZE			16384

struct thr;

struct thr {
	pthread_t thr_id;
	int thr_sockfd;
	struct fde_head *h;
	struct fde_comm *comm_recvfrom;
};

static void
conn_recvmsg(int fd, struct fde_comm *fc, void *arg,
    struct fde_comm_udp_frame *fr, fde_comm_cb_status s, int xerrno)
{
	struct thr *r = arg;
	struct conn *c;

	if (s != FDE_COMM_CB_COMPLETED) {
		if (xerrno == EAGAIN || xerrno == EWOULDBLOCK)
			return;

		fprintf(stderr,
		    "%s: %p: LISTEN: status=%d, errno=%d, newfd=%d\n",
		    __func__, r, s, errno, fd);
		return;
	}

#if 0
	/*
	 * Print things out.
	 */
	fprintf(stderr,
	    "%s: %p: RECV: len=%d\n",
	    __func__,
	    r,
	    fr->len);
#endif

	/*
	 * Free the UDP frame.
	 */
	if (fr != NULL) {
		fde_comm_udp_free(fc, fr);
	}
}

void *
thrsrv_new(void *arg)
{
	struct thr *r = arg;
	struct timeval tv;

	fprintf(stderr, "%s: %p: created\n", __func__, r);

	/* Create a listen comm object */
	r->comm_recvfrom = comm_create(r->thr_sockfd, r->h, NULL, NULL);
	comm_mark_nonclose(r->comm_recvfrom);
	(void) comm_udp_read(r->comm_recvfrom, conn_recvmsg, r, 8192);

	/* Loop around, listening for events; farm them off as required */
	while (1) {

		tv.tv_sec = 1;
		tv.tv_usec = 0;
		fde_runloop(r->h, &tv);
	}

	return (NULL);
}

int
thrsrv_listenfd(int port)
{
	int fd;
	struct sockaddr_in sin;
	int a;

	bzero(&sin, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = 0;
	sin.sin_port = htons(port);
	sin.sin_len = sizeof(sin);

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		fprintf(stderr, "%s: socket() failed; errno=%d (%s)\n",
		    __func__,
		    errno,
		    strerror(errno));
		return (-1);
	}

	/* Make non-blocking */
	(void) comm_fd_set_nonblocking(fd, 1);

	/* make reuse */
	a = 1;
	(void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &a, sizeof(a));

	/* and reuse port */
	a = 1;
	(void) setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &a, sizeof(a));

	if (bind(fd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
		fprintf(stderr, "%s: bind() failed; errno=%d (%s)\n",
		    __func__,
		    errno,
		    strerror(errno));
		close(fd);
		return (-1);
	}

	return (fd);
}

int
main(int argc, const char *argv[])
{
	int fd;
	struct thr *rp, *r;
	int i;

	/* Allocate thread pool */
	rp = calloc(NUM_THREADS, sizeof(struct thr));
	if (rp == NULL)
		perror("malloc");

	/* Create listen socket */
	fd = thrsrv_listenfd(1667);
	if (fd < 0) {
		perror("listenfd");
	}

	/* Create listen threads */
	for (i = 0; i < NUM_THREADS; i++) {
		r = &rp[i];
		r->thr_sockfd = fd;
		r->h = fde_ctx_new();
		if (pthread_create(&r->thr_id, NULL, thrsrv_new, r) != 0)
			perror("pthread_create");
	}

	/* Join */
	for (i = 0; i < 4; i++) {
		pthread_join(rp[i].thr_id, NULL);
	}

	exit (0);
}
