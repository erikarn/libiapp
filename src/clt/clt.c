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
#include <signal.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "fde.h"
#include "comm.h"

struct clt_app;
struct conn;

#define	NUM_CLIENTS_PER_THREAD		4096

typedef enum {
	CONN_STATE_NONE,
	CONN_STATE_CONNECTING,
	CONN_STATE_RUNNING,
	CONN_STATE_ERROR,
	CONN_STATE_CLOSING,
	CONN_STATE_FREEING
} conn_state_t;

typedef void conn_owner_update_cb(struct conn *c, void *arg, conn_state_t newstate);

struct conn {
	int fd;
	struct clt_app *parent;
	TAILQ_ENTRY(conn) node;
	struct fde_comm *comm;
	struct fde *ev_cleanup;
	conn_state_t state;
	uint64_t total_read, total_written;
	struct {
		char *buf;
		int size;
	} r;
	struct {
		char *buf;
		int size;
	} w;
	struct {
		conn_owner_update_cb *cb;
		void *cbdata;
	} cb;
};

struct clt_app {
	pthread_t thr_id;
	struct fde_head *h;
	int num_clients;
	TAILQ_HEAD(, conn) conn_list;
};

static void
conn_ev_cleanup_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	struct conn *c = arg;

	/* XXX ensure we're closing and cleanup has been scheduled */

	if (c->comm != NULL) {
		fprintf(stderr, "%s: %p: comm not null?\n", __func__, c);
	}

	fde_delete(c->parent->h, c->ev_cleanup);

	/* Notify owner that I'm about to be freed */
	c->cb.cb(c, c->cb.cbdata, CONN_STATE_FREEING);

	if (c->r.buf)
		free(c->r.buf);
	if (c->w.buf)
		free(c->w.buf);
	TAILQ_REMOVE(&c->parent->conn_list, c, node);
	free(c);
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

#if 0
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

	/*
	 * The rest of close will occur once the close handler is called.
	 */
}

static void
conn_write_cb(int fd, struct fde_comm *fc, void *arg,
    fde_comm_cb_status status, int nwritten)
{
	struct conn *c = arg;

#if 0
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
		c->cb.cb(c, c->cb.cbdata, CONN_STATE_ERROR);
		return;
	}

	/* Update write statistics */
	c->total_written += nwritten;

	/* Did we write the whole buffer? If not, error */
	if (nwritten != c->w.size) {
		fprintf(stderr, "%s: %p: nwritten (%d) != size (%d)\n",
		    __func__,
		    c,
		    c->w.size,
		    nwritten);
		c->state = CONN_STATE_ERROR;
		c->cb.cb(c, c->cb.cbdata, CONN_STATE_ERROR);
		return;
	}

	/*
	 * Write some more data
	 */
	comm_write(c->comm, c->w.buf, c->w.size, conn_write_cb, c);
}

/*
 * Close callback - the close has finished; schedule the
 * rest of object tidyup.
 */
static void
conn_close_cb(int fd, struct fde_comm *fc, void *arg)
{
	struct conn *c = arg;

#if 0
	fprintf(stderr, "%s: %p: called, scheduling cleanup\n", __func__, c);
#endif

	/* Schedule the tidyup event */
	/* XXX should track this to only happen once */
	fde_add(c->parent->h, c->ev_cleanup);
}

static void
conn_connect_cb(int fd, struct fde_comm *fc, void *arg,
    fde_comm_cb_status status, int retval)
{
	struct conn *c = arg;

#if 0
	fprintf(stderr, "%s: %p: called; status=%d, retval=%d\n",
	    __func__, c, status, retval);
#endif

	/* Error? Notify the upper layer; finish */
	if (status != FDE_COMM_CB_COMPLETED) {
		fprintf(stderr, "%s: FD %d: %p: called; status=%d, retval=%d\n",
		    __func__, fc->fd, c, status, retval);
		c->state = CONN_STATE_ERROR;
		c->cb.cb(c, c->cb.cbdata, CONN_STATE_ERROR);
		return;
	}

	/* Success? Start writing data */
	c->state = CONN_STATE_RUNNING;

	/* Notify owner; they may decide to change state */
	c->cb.cb(c, c->cb.cbdata, CONN_STATE_RUNNING);

	/* If the caller changed state, don't do a write */
	if (c->state != CONN_STATE_RUNNING)
		return;

	comm_write(c->comm, c->w.buf, c->w.size, conn_write_cb, c);
}

struct conn *
conn_new(struct clt_app *r, conn_owner_update_cb *cb, void *cbdata)
{
	struct conn *c;
	int i;

	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		warn("%s: calloc", __func__);
		return (NULL);
	}

	c->r.size = 8192;
	c->r.buf = malloc(c->r.size);
	if (c->r.buf == NULL) {
		warn("%s: malloc", __func__);
		free(c);
		return (NULL);
	}

	c->w.size = 8000;
	c->w.buf = malloc(c->r.size);
	if (c->w.buf == NULL) {
		warn("%s: malloc", __func__);
		free(c->r.buf);
		free(c);
		return (NULL);
	}

	/* Pre-populate */
	for (i = 0; i < c->w.size; i++) {
		c->w.buf[i] = (i % 10) + '0';
	}

	/* Create an AF_INET socket */
	/* XXX should be a method */
	c->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (c->fd < 0) {
		warn("%s: socket", __func__);
		free(c);
		free(c->r.buf);
		free(c->w.buf);
		return (NULL);
	}
	c->parent = r;
	c->comm = comm_create(c->fd, r->h, conn_close_cb, c);
	c->ev_cleanup = fde_create(r->h, -1, FDE_T_CALLBACK,
	    conn_ev_cleanup_cb, c);
	comm_set_nonblocking(c->comm, 1);

	c->cb.cb = cb;
	c->cb.cbdata = cbdata;

	TAILQ_INSERT_TAIL(&r->conn_list, c, node);

	return (c);
}

/*
 * Owner related routines
 */

static void
thrclt_conn_update_cb(struct conn *c, void *arg, conn_state_t newstate)
{
	struct clt_app *r = arg;

#if 0
	fprintf(stderr, "%s: %p: called; state=%d\n", __func__, r, newstate);
#endif

	/* If we hit an error, we should schedule a close of this object */
	if (newstate == CONN_STATE_ERROR) {
		conn_close(c);
		return;
	}

	/*
	 * Freeing? Use this to track the number of open connectons.
	 */
	if (newstate == CONN_STATE_FREEING) {
		if (r->num_clients == 0) {
			fprintf(stderr, "%s: %p: num_clients=0 ? \n",
			    __func__, r);
		} else {
			r->num_clients--;
		}
#if 0
		fprintf(stderr, "%s: %p: client freed\n", __func__, r);
#endif
	}
}

int
thrclt_open_new_conn(struct clt_app *r)
{
	struct conn *c;
	struct sockaddr_in sin;
	socklen_t slen;

	/*
	 * For now, let's create one client object and kick-start it.
	 */
	c = conn_new(r, thrclt_conn_update_cb, r);
	if (c == NULL)
		return (-1);

	/* Start connecting */
	c->state = CONN_STATE_CONNECTING;
	slen = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = htons(1667);
	(void) inet_aton("127.0.0.1", &sin.sin_addr);
	sin.sin_len = sizeof(struct sockaddr_in);
	(void) comm_connect(c->comm, (struct sockaddr *) &sin, slen,
	    conn_connect_cb, c);

	r->num_clients++;

	return (0);
}

void *
thrclt_new(void *arg)
{
	struct clt_app *r = arg;
	struct timespec tv;
	struct conn *c;

	fprintf(stderr, "%s: %p: created\n", __func__, r);

	/* open up NUM_CLIENTS_PER_THREAD conncetions, stop if we fail */
	while (r->num_clients < NUM_CLIENTS_PER_THREAD) {
		if (thrclt_open_new_conn(r) < 0)
			break;
	}

	/* Loop around, listening for events; farm them off as required */
	while (1) {
		tv.tv_sec = 1;
		tv.tv_nsec = 0;
		fde_runloop(r->h, &tv);
	}

	return (NULL);
}

static void
null_signal_hdl(int sig)
{

}

int
main(int argc, const char *argv[])
{
	struct clt_app *rp, *r;
	int i;

	/* Allocate thread pool */
	rp = calloc(4, sizeof(struct clt_app));
	if (rp == NULL)
		perror("malloc");

	signal(SIGPIPE, null_signal_hdl);

	/* Create listen threads */
	for (i = 0; i < 4; i++) {
		r = &rp[i];
		r->h = fde_ctx_new();
		TAILQ_INIT(&r->conn_list);
		if (pthread_create(&r->thr_id, NULL, thrclt_new, r) != 0)
			perror("pthread_create");
	}

	/* Join */
	for (i = 0; i < 4; i++) {
		pthread_join(rp[i].thr_id, NULL);
	}

	exit (0);
}
