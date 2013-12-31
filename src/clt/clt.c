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
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <netdb.h>

#include "fde.h"
#include "shm_alloc.h"
#include "netbuf.h"
#include "comm.h"

struct clt_app;
struct conn;

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
	uint64_t write_close_thr;
	struct {
		char *buf;
		int size;
	} r;
	struct {
		struct iapp_netbuf *nb;
	} w;
	struct {
		conn_owner_update_cb *cb;
		void *cbdata;
	} cb;
};

struct clt_app {
	pthread_t thr_id;
	struct shm_alloc_state sm;
	int app_id;
	int max_io_size;
	int nconns;
	int connrate;
	struct fde_head *h;
	struct fde *ev_stats;
	int num_clients;
	struct fde *ev_newconn;
	char *remote_host;
	char *remote_port;
	uint64_t total_read, total_written;
	uint64_t total_opened;
	uint64_t total_closed;
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
	if (c->w.nb)
		iapp_netbuf_free(c->w.nb);
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

	c->parent->total_closed++;

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

	/* Update write statistics - local and parent app */
	c->total_written += nwritten;
	c->parent->total_written += nwritten;

	/*
	 * If the threshold value is set, bail out if we reach it.
	 */
	if (c->write_close_thr != 0 && c->total_written > c->write_close_thr) {
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
		c->cb.cb(c, c->cb.cbdata, CONN_STATE_ERROR);
		return;
	}

	/*
	 * Write some more data - the whole netbuf (again)
	 */
	comm_write(c->comm, c->w.nb, 0, iapp_netbuf_size(c->w.nb), conn_write_cb, c);
}

static void
conn_read_cb(int fd, struct fde_comm *cb, void *arg, fde_comm_cb_status s,
    int retval)
{
	struct conn *c = arg;

	if (s != FDE_COMM_CB_COMPLETED) {
		if (s != FDE_COMM_CB_EOF)
			fprintf(stderr, "%s: non-EOF error?\n", __func__);
		conn_close(c);
		return;
	}

	if (retval > 0) {
		c->total_read += retval;
		c->parent->total_read += retval;
	}

	comm_read(c->comm, c->r.buf, c->r.size, conn_read_cb, c);
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
#if 1
		fprintf(stderr, "%s: FD %d: %p: called; status=%d, retval=%d\n",
		    __func__, fc->fd, c, status, retval);
#endif
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

	/* Total successfully opened */
	c->parent->total_opened++;

	comm_read(c->comm, c->r.buf, c->r.size, conn_read_cb, c);
//	comm_write(c->comm, c->w.nb, 0, iapp_netbuf_size(c->w.nb), conn_write_cb, c);
}

struct conn *
conn_new(struct clt_app *r, int type, conn_owner_update_cb *cb, void *cbdata)
{
	struct conn *c;
	int i;
	char *buf;

	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		warn("%s: calloc", __func__);
		return (NULL);
	}

	c->r.size = r->max_io_size;
	c->r.buf = malloc(c->r.size);
	if (c->r.buf == NULL) {
		warn("%s: malloc", __func__);
		free(c);
		return (NULL);
	}

	c->w.nb = iapp_netbuf_alloc(&r->sm, NB_ALLOC_MALLOC, r->max_io_size);
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

	/* Create an 'type' socket */
	/* XXX should be a method */
	c->fd = socket(type, SOCK_STREAM, 0);
	if (c->fd < 0) {
		warn("%s: socket", __func__);
		free(c);
		free(c->r.buf);
		iapp_netbuf_free(c->w.nb);
		return (NULL);
	}
	c->parent = r;
	c->comm = comm_create(c->fd, r->h, conn_close_cb, c);
	c->ev_cleanup = fde_create(r->h, -1, FDE_T_CALLBACK, 0,
	    conn_ev_cleanup_cb, c);
	comm_set_nonblocking(c->comm, 1);

	c->cb.cb = cb;
	c->cb.cbdata = cbdata;

#if 0
	/* If there's a threshold for closing; set it */
	c->write_close_thr = random() % 10485760;
#endif

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

#define	MAX_ADDRINFO	8

int
thrclt_open_new_conn(struct clt_app *r)
{
	struct conn *c;
	struct sockaddr_in sin;
	socklen_t slen;
	struct addrinfo *ai;
	struct addrinfo ai_hints;
	int rr;

	/*
	 * Do a lookup!
	 */
	bzero(&ai_hints, sizeof(ai_hints));
	ai_hints.ai_family = PF_UNSPEC;
	ai_hints.ai_socktype = 0;
	ai_hints.ai_protocol = IPPROTO_TCP;
	ai_hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;

	rr = getaddrinfo(r->remote_host, r->remote_port, &ai_hints, &ai);
	if (rr != 0) {
		warn("%s: getaddrinfo", __func__);
		return (-1);
	}

	/*
	 * Next, extract out the first entry.
	 */
	if (ai == NULL) {
		fprintf(stderr, "%s: ai=NULL\n", __func__);
		return -1;
	}
	/*
	 * For now, let's create one client object and kick-start it.
	 * XXX shuld also pass in res->ai_socktype and res->ai_protocol
	 */
	c = conn_new(r, ai->ai_family, thrclt_conn_update_cb, r);
	if (c == NULL) {
		freeaddrinfo(ai);
		return (-1);
	}

	/* Start connecting */
	c->state = CONN_STATE_CONNECTING;
	(void) comm_connect(c->comm, ai->ai_addr, ai->ai_addrlen,
	    conn_connect_cb, c);

	r->num_clients++;

	freeaddrinfo(ai);
	return (0);
}

static void
thrclt_ev_newconn_cb(int fd, struct fde *f, void *arg, fde_cb_status s)
{
	int i;
	struct clt_app *r = arg;
	struct timeval tv;

	/*
	 * Attempt to open up new clients
	 */
	i = 0;
	while (r->num_clients < r->nconns) {
		if (thrclt_open_new_conn(r) < 0)
			break;
		i++;
		if (i > r->connrate)
			break;
	}

	/*
	 * .. and schedule another creation event in the future.
	 */
	(void) gettimeofday(&tv, NULL);
	tv.tv_usec += 100000;
	if (tv.tv_usec > 1000000) {
		tv.tv_usec -= 1000000;
		tv.tv_sec += 1;
	}

	fde_add_timeout(r->h, r->ev_newconn, &tv);
}

static void
thrclt_stat_print(int fd, struct fde *f, void *arg, fde_cb_status s)
{
	struct clt_app *r = arg;
	struct timeval tv;

	fprintf(stderr, "%s: [%d]: %d clients; new=%lld, closed=%lld, TX=%lld bytes, RX=%lld bytes\n",
	    __func__,
	    r->app_id,
	    r->num_clients,
	    (unsigned long long) r->total_opened,
	    (unsigned long long) r->total_closed,
	    (unsigned long long) r->total_written,
	    (unsigned long long) r->total_read);

	/* Blank this out, so we get per-second stats */
	r->total_read = 0;
	r->total_written = 0;
	r->total_opened = 0;
	r->total_closed = 0;

	/*
	 * Schedule for another second from now.
	 */
	/* Add stat - to be called one second in the future */
	(void) gettimeofday(&tv, NULL);
	tv.tv_sec += 1;
	fde_add_timeout(r->h, r->ev_stats, &tv);
}

void *
thrclt_new(void *arg)
{
	struct clt_app *r = arg;
	struct timeval tv;
	struct conn *c;

	fprintf(stderr, "%s: %p: created\n", __func__, r);

	r->ev_newconn = fde_create(r->h, -1, FDE_T_TIMER, 0,
	    thrclt_ev_newconn_cb, r);
	r->ev_stats = fde_create(r->h, -1, FDE_T_TIMER, 0,
	    thrclt_stat_print, r);

	/* Add stat - to be called one second in the future */
	(void) gettimeofday(&tv, NULL);
	tv.tv_sec += 1;
	fde_add_timeout(r->h, r->ev_stats, &tv);

	(void) gettimeofday(&tv, NULL);
	fde_add_timeout(r->h, r->ev_newconn, &tv);

	/* Loop around, listening for events; farm them off as required */
	while (1) {
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		fde_runloop(r->h, &tv);
	}

	return (NULL);
}

static void
null_signal_hdl(int sig)
{

}

static void
usage(const char *progname)
{
	printf("Usage: %s <numthreads> <numconns> <connrate> <bufsize> <remote IPv4 address> <port>\n",
	    progname);
	exit(127);
}

int
main(int argc, const char *argv[])
{
	struct clt_app *rp, *r;
	int i;
	int nthreads, connrate, bufsize, nconns;
	char *rem_ip, *rem_port;

	/* XXX validate command line parameters */
	if (argc < 7)
		usage(argv[0]);

	nthreads = atoi(argv[1]);
	nconns = atoi(argv[2]);
	connrate = atoi(argv[3]);
	bufsize = atoi(argv[4]);
	rem_ip = strdup(argv[5]);
	rem_port = strdup(argv[6]);

	/* XXX these should be done as part of a global setup */
	iapp_netbuf_init();

	/* Allocate thread pool */
	rp = calloc(nthreads, sizeof(struct clt_app));
	if (rp == NULL)
		perror("malloc");

	signal(SIGPIPE, null_signal_hdl);

	/* Create listen threads */
	for (i = 0; i < nthreads; i++) {
		r = &rp[i];
		r->app_id = i;
		r->h = fde_ctx_new();
		r->remote_host = strdup(rem_ip);
		r->remote_port = strdup(rem_port);
		r->max_io_size = bufsize;
		r->nconns = nconns;
		r->connrate = connrate;
		shm_alloc_init(&r->sm, nconns*bufsize, nconns*bufsize, 0);
		TAILQ_INIT(&r->conn_list);
		if (pthread_create(&r->thr_id, NULL, thrclt_new, r) != 0)
			perror("pthread_create");
	}

	/* Join */
	for (i = 0; i < nthreads; i++) {
		pthread_join(rp[i].thr_id, NULL);
	}

	exit (0);
}
