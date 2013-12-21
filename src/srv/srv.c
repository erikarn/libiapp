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

#define	NUM_THREADS		8ULL

#define	IO_SIZE			65536ULL

#define	MAX_NUM_CONNS		32768ULL

//#define	DO_DEBUG		1

struct thr;
struct conn;

typedef enum {
	CONN_STATE_NONE,
	CONN_STATE_CONNECTING,
	CONN_STATE_RUNNING,
	CONN_STATE_ERROR,
	CONN_STATE_CLOSING,
	CONN_STATE_FREEING
} conn_state_t;

struct conn {
	int fd;
	struct thr *parent;
	TAILQ_ENTRY(conn) node;
	struct fde_comm *comm;
	struct fde *ev_cleanup;
	conn_state_t state;
	struct {
		char *buf;
		int size;
	} r;
	struct {
		struct iapp_netbuf *nb;
	} w;
	uint64_t total_read, total_written;
	uint64_t write_close_thr;
};

struct thr {
	pthread_t thr_id;
	struct shm_alloc_state sm;
	int thr_sockfd;
	struct fde_head *h;
	struct fde_comm *comm_listen;
	TAILQ_HEAD(, conn) conn_list;
	uint64_t total_read, total_written;
	uint64_t total_opened, total_closed;
};

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


static void
client_ev_cleanup_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{

	struct conn *c = arg;

	/* Time to tidy up! */
#ifdef DO_DEBUG
	fprintf(stderr, "%s: %p: freeing\n", __func__, c);
#endif

	/* This MUST be free at this point */
	if (c->comm != NULL) {
		fprintf(stderr, "%s: %p: comm not null? Huh?\n",
		    __func__,
		    c);
	}

	fde_free(c->parent->h, c->ev_cleanup);
	TAILQ_REMOVE(&c->parent->conn_list, c, node);
	free(c->r.buf);
	iapp_netbuf_free(c->w.nb);
	free(c);
}

static void
client_ev_close_cb(int fd, struct fde_comm *fc, void *arg)
{
	struct conn *c = arg;

#ifdef DO_DEBUG
	fprintf(stderr, "%s: FD %d: %p: close called\n", __func__, fd, c);
#endif
	/* NULL this - it'll be closed for us when this routine completes */
	c->comm = NULL;

	/* Schedule the actual cleanup */
	/* XXX should ensure we only call this once */
	fde_add(c->parent->h, c->ev_cleanup);
}
/*
 * Initiate shutdown of a given conn.
 *
 * This should be called from the owner, as there's no notification
 * path for this.
 */
static void
conn_close(struct conn *c)
{

	if (c->state == CONN_STATE_CLOSING)
		return;

#ifdef DO_DEBUG
	fprintf(stderr, "%s: %p: called\n", __func__, c);
#endif

	c->state = CONN_STATE_CLOSING;

	/* Call comm_close(); when IO completes we'll get notified */
	/*
	 * The alternative is to track when we're writing and if we
	 * are, just wait until we're done
	 */
	comm_close(c->comm);
	c->comm = NULL;

	c->parent->total_closed++;

	/*
	 * The rest of close will occur once the close handler is called.
	 */
}

void
client_read_cb(int fd, struct fde_comm *fc, void *arg, fde_comm_cb_status s,
    int retval)
{
	struct conn *c = arg;

#ifdef DO_DEBUG
	fprintf(stderr, "%s: FD %d: %p: s=%d, ret=%d\n",
	    __func__,
	    fd,
	    c,
	    s,
	    retval);
#endif

	/*
	 * If we've hit an error, do a comm_close() and then wait until we
	 * get notification for that.
	 */
	if (s != FDE_COMM_CB_COMPLETED) {
		if (s != FDE_COMM_CB_EOF) {
			fprintf(stderr, "%s: %p: FD %d: error; status=%d errno=%d\n",
			    __func__,
			    c,
			    fc->fd,
			    s,
			    errno);
		}
		conn_close(c);
		return;
	}

	/* register for another read */
	(void) comm_read(c->comm, c->r.buf, c->r.size, client_read_cb, c);
}

static void
conn_write_cb(int fd, struct fde_comm *fc, void *arg,
    fde_comm_cb_status status, int nwritten)
{
	struct conn *c = arg;

#ifdef DO_DEBUG
	fprintf(stderr, "%s: %p: called; status=%d, retval=%d\n",
	    __func__, c, status, nwritten);
#endif

	/*
	 * Not running? Skip.
	 */
	if (c->state != CONN_STATE_RUNNING)
		return;

	/* Any error? Transition; notify upper layer */
	if (status != FDE_COMM_CB_COMPLETED) {
		c->state = CONN_STATE_ERROR;
		conn_close(c);
		return;
	}

	/* Update write statistics - local and parent app */
	c->total_written += nwritten;
	c->parent->total_written += nwritten;

	/*
	 * If the threshold value is set, bail out if we reach it.
	 */
	if (c->write_close_thr != 0 && c->total_written > c->write_close_thr) {
#ifdef	DO_DEBUG
		fprintf(stderr, "%s: %p: overflowed; finished\n",
		    __func__,
		    c);
#endif
		conn_close(c);
		return;
	}

	/* Did we write the whole buffer? If not, error */
	if (nwritten != iapp_netbuf_size(c->w.nb)) {
		fprintf(stderr, "%s: %p: nwritten (%d) != size (%d)\n",
		    __func__,
		    c,
		    (int) iapp_netbuf_size(c->w.nb),
		    nwritten);
		c->state = CONN_STATE_ERROR;
		return;
	}

#ifdef	DO_DEBUG
	fprintf(stderr, "%s: %p: another write!\n", __func__, c);
#endif
	/*
	 * Write some more data - the whole netbuf (again)
	 */
	comm_write(c->comm, c->w.nb, 0, iapp_netbuf_size(c->w.nb), conn_write_cb, c);
}

struct conn *
conn_new(struct thr *r, int fd)
{
	struct conn *c;
	char *buf;
	int i;
	int sn;

	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		warn("%s: calloc", __func__);
		return (NULL);
	}

	c->r.size = IO_SIZE;
	c->r.buf = malloc(c->r.size);
	if (c->r.buf == NULL) {
		warn("%s: malloc", __func__);
		free(c);
		return (NULL);
	}

	c->w.nb = iapp_netbuf_alloc(&r->sm, NB_ALLOC_POSIXSHM, IO_SIZE);
	if (c->w.nb == NULL) {
		warn("%s: iapp_netbuf_alloc", __func__);
		free(c->r.buf);
		free(c);
		return (NULL);
	}

	/* Pre-populate with some data */
	buf = iapp_netbuf_buf_nonconst(c->w.nb);
	for (i = 0; i < iapp_netbuf_size(c->w.nb); i++) {
		buf[i] = (i % 10) + '0';
	}

	/*
	 * Limit the send size to one buffer for now.
	 *
	 * This isn't optimal but until we queue multiple buffers
	 * via sendfile, we will end up queueing the same memory region
	 * over and over again via different mbufs to the same socket
	 * and that isn't at all useful or correct.
	 *
	 * Once the shm allocator handles returning buffers, we can
	 * modify the transmit path to allocate buffers as required and
	 * then keep up to two in flight.  Then we can just remove
	 * this limit.
	 */
	sn = IO_SIZE;
	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sn, sizeof(sn)) < 0)
		warn("%s: setsockopt(SO_SNDBUF)", __func__);

	c->fd = fd;
	c->parent = r;
	c->comm = comm_create(fd, r->h, client_ev_close_cb, c);
	c->ev_cleanup = fde_create(r->h, -1, FDE_T_CALLBACK, 0,
	    client_ev_cleanup_cb, c);
	c->state = CONN_STATE_RUNNING;
	TAILQ_INSERT_TAIL(&r->conn_list, c, node);

#if 0
	/*
	 * Start reading!
	 */
	(void) comm_read(c->comm, c->r.buf, c->r.size, client_read_cb, c);
#endif

	/*
	 * Start writing!
	 */
	comm_write(c->comm, c->w.nb, 0, iapp_netbuf_size(c->w.nb), conn_write_cb, c);

	return (c);
}

static void
conn_acceptfd(int fd, struct fde_comm *fc, void *arg, fde_comm_cb_status s,
    int newfd, struct sockaddr *saddr, socklen_t slen, int xerrno)
{
	struct thr *r = arg;
	struct conn *c;

	if (s != FDE_COMM_CB_COMPLETED) {
		fprintf(stderr,
		    "%s: %p: LISTEN: status=%d, errno=%d, newfd=%d\n",
		    __func__, r, s, errno, newfd);
		return;
	}

	c = conn_new(r, newfd);
	if (c == NULL) {
		close(newfd);
		return;
	}
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

	/* Allocate thread pool */
	rp = calloc(NUM_THREADS, sizeof(struct thr));
	if (rp == NULL)
		perror("malloc");

	/* Create listen socket per thread - using SO_REUSEADDR/SO_REUSEPORT */
	fd = thrsrv_listenfd(1667);
	if (fd < 0) {
		perror("listenfd");
	}

	iapp_netbuf_init();

	/* Create listen threads */
	for (i = 0; i < NUM_THREADS; i++) {
		cpuset_t cp;

		r = &rp[i];
		/* Shared single listen FD, multiple threads interested */
		r->thr_sockfd = fd;
		r->h = fde_ctx_new();
		shm_alloc_init(&r->sm, MAX_NUM_CONNS*IO_SIZE, MAX_NUM_CONNS*IO_SIZE, 1);
		TAILQ_INIT(&r->conn_list);
		if (pthread_create(&r->thr_id, NULL, thrsrv_new, r) != 0)
			perror("pthread_create");

		/* Set affinity */
		CPU_ZERO(&cp);
		CPU_SET(i, &cp);
		if (pthread_setaffinity_np(r->thr_id, sizeof(cpuset_t), &cp) != 0)
			perror("pthread_setaffinity_np");
	}

	/* Join */
	for (i = 0; i < 4; i++) {
		pthread_join(rp[i].thr_id, NULL);
	}

	exit (0);
}
