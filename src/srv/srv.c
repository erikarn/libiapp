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

/* For thread affinity */
#include <pthread_np.h>
#include <sys/cpuset.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "fde.h"
#include "shm_alloc.h"
#include "netbuf.h"
#include "comm.h"
#include "iapp_cpu.h"

#include "cfg.h"
#include "thr.h"
#include "conn.h"

//#define	DO_DEBUG		1

static int
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

	fd = socket(AF_INET, SOCK_STREAM, 0);
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

	if (bind(fd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
		fprintf(stderr, "%s: bind() faioed; errno=%d (%s)\n",
		    __func__,
		    errno,
		    strerror(errno));
		close(fd);
		return (-1);
	}

	if (listen(fd, -1) < 0) {
		fprintf(stderr, "%s: listen() faioed; errno=%d (%s)\n",
		    __func__,
		    errno,
		    strerror(errno));
		close(fd);
		return (-1);
	}

	return (fd);
}

void *
thrsrv_new(void *arg)
{
	struct thr *r = arg;
	struct timeval tv;

	fprintf(stderr, "%s: %p: created\n", __func__, r);

	/* Create a listen comm object */
	r->comm_listen = comm_create(r->thr_sockfd, r->h, NULL, NULL);
	comm_mark_nonclose(r->comm_listen);
	(void) comm_listen(r->comm_listen, conn_acceptfd, r);

	/* Loop around, listening for events; farm them off as required */
	while (1) {

		tv.tv_sec = 1;
		tv.tv_usec = 0;
		fde_runloop(r->h, &tv);
	}

	return (NULL);
}

int
main(int argc, const char *argv[])
{
	struct thr *rp, *r;
	int i;
	int fd;
	int ncpu;
	struct cfg srv_cfg;


	bzero(&srv_cfg, sizeof(srv_cfg));

	/* Initialise the local config */
	srv_cfg.num_threads = 2;
	srv_cfg.io_size = 16384;
	srv_cfg.max_num_conns = 32768;
	srv_cfg.atype = NB_ALLOC_MALLOC;
	srv_cfg.port = 1667;

	/* Allocate thread pool */
	rp = calloc(srv_cfg.num_threads, sizeof(struct thr));
	if (rp == NULL)
		perror("malloc");

	/* Create listen socket per thread - using SO_REUSEADDR/SO_REUSEPORT */
	fd = thrsrv_listenfd(1667);
	if (fd < 0) {
		perror("listenfd");
	}

	iapp_netbuf_init();

	ncpu = iapp_get_ncpus();
	if (ncpu < 0)
		exit(127);	/* XXX */

	/* Create listen threads */
	for (i = 0; i < srv_cfg.num_threads; i++) {
		cpuset_t cp;

		r = &rp[i];
		/* Shared single listen FD, multiple threads interested */
		r->thr_sockfd = fd;
		r->h = fde_ctx_new();
		r->cfg = &srv_cfg;

		/*
		 * Only allocate the shared memory bits if we need them.
		 *
		 * + Allocate the whole lot at once;
		 * + mlock it.
		 */
		if (srv_cfg.atype == NB_ALLOC_POSIXSHM)
			shm_alloc_init(&r->sm,
			    srv_cfg.max_num_conns*srv_cfg.io_size,
			    srv_cfg.max_num_conns*srv_cfg.io_size,
			    1);
		TAILQ_INIT(&r->conn_list);
		if (pthread_create(&r->thr_id, NULL, thrsrv_new, r) != 0)
			perror("pthread_create");

		/* Set affinity */
		CPU_ZERO(&cp);
		CPU_SET(i % ncpu, &cp);

		printf("%s: thread id %d -> CPU %d\n", argv[0], i, i % ncpu);

		if (pthread_setaffinity_np(r->thr_id, sizeof(cpuset_t), &cp) != 0)
			warn("pthread_setaffinity_np (id %d)", i);
	}

	/* Join */
	for (i = 0; i < srv_cfg.num_threads; i++) {
		pthread_join(rp[i].thr_id, NULL);
	}

	exit (0);
}
