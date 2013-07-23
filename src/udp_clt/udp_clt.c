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

#include "fde.h"
#include "comm.h"

struct clt_app;

struct clt_app {
	pthread_t thr_id;
	int app_id;
	int max_io_size;
	int max_qdepth;
	int connrate;
	struct fde_head *h;
	struct fde *ev_stats;
	struct fde *ev_newconn;
	struct fde_comm *comm_wr;
	char *remote_host;
	int remote_port;
	uint64_t total_pkt_read, total_pkt_written;
	uint64_t total_byte_read, total_byte_written;
};

static void
thrclt_ev_newconn_cb(int fd, struct fde *f, void *arg, fde_cb_status s)
{
	struct timeval tv;
	struct clt_app *r = arg;
	struct fde_comm_udp_frame *fr;
	int i;

	fprintf(stderr, "%s: [%d]: sending\n",
	    __func__,
	    r->app_id);

	/*
	 * Send one frame for now - this function for now
	 * alloc's the buffer
	 */
	fr = fde_comm_udp_alloc(r->comm_wr, r->max_io_size);

	/*
	 * Fake some data
	 */
	for (i = 0; i < r->max_io_size; i++) {
		fr->buf[i] = 'A' + (i % 26);
	}
	fr->len = r->max_io_size;

	/*
	 * Set the remote socket information.
	 */
	((struct sockaddr_in *) &fr->sa_rem)->sin_family = AF_INET;
	((struct sockaddr_in *) &fr->sa_rem)->sin_len = sizeof(struct sockaddr_in);
	((struct sockaddr_in *) &fr->sa_rem)->sin_port = htons(r->remote_port);
	((struct sockaddr_in *) &fr->sa_rem)->sin_addr.s_addr = inet_addr(r->remote_host);
	fr->sl_rem = sizeof(struct sockaddr_in);

	if (comm_udp_write(r->comm_wr, fr) < 0) {
		fde_comm_udp_free(r->comm_wr, fr);
	}


	/* Add newconn - to be called one second in the future */
	(void) gettimeofday(&tv, NULL);
	tv.tv_sec += 1;
	fde_add_timeout(r->h, r->ev_newconn, &tv);
}

static void
thrclt_ev_stat_print(int fd, struct fde *f, void *arg, fde_cb_status s)
{
	struct timeval tv;
	struct clt_app *r = arg;

	fprintf(stderr, "%s: [%d]: written %lld packets, %lld bytes\n",
	    __func__,
	    r->app_id,
	    (unsigned long long) r->total_pkt_written,
	    (unsigned long long) r->total_byte_written);

	/* Add stat - to be called one second in the future */
	(void) gettimeofday(&tv, NULL);
	tv.tv_sec += 1;
	fde_add_timeout(r->h, r->ev_stats, &tv);
}

static void
thrsrv_comm_udp_write_cb(int fd, struct fde_comm *fc, void *arg,
    struct fde_comm_udp_frame *fr, fde_comm_cb_status status,
    int nwritten, int xerrno)
{
	struct clt_app *r = arg;

	fprintf(stderr, "%s: [%d]: called; status=%d, wr=%d, errno=%d\n",
	    __func__,
	    r->app_id,
	    status,
	    nwritten,
	    xerrno);

	/*
	 * For now, just account stuff and free the buffer.
	 */
	if (status == FDE_COMM_CB_COMPLETED && nwritten == fr->len) {
		r->total_pkt_written++;
		r->total_byte_written += nwritten;
	}

	fde_comm_udp_free(fc, fr);
}

void *
thrclt_new(void *arg)
{
	struct clt_app *r = arg;
	struct timeval tv;
	struct conn *c;
	int fd, a;
	struct sockaddr_in sin;

	fprintf(stderr, "%s: %p: created\n", __func__, r);

	r->ev_newconn = fde_create(r->h, -1, FDE_T_TIMER, thrclt_ev_newconn_cb, r);
	r->ev_stats = fde_create(r->h, -1, FDE_T_TIMER, thrclt_ev_stat_print, r);

	/* Create the local socket */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		warn("%s: socket", __func__);
		return (NULL);
	}

	/* Make non-blocking */
	(void) comm_fd_set_nonblocking(fd, 1);

	/* make reuse */
	a = 1;
	(void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &a, sizeof(a));

	/* and reuse port */
	a = 1;
	(void) setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &a, sizeof(a));

	/* Local bind */
	bzero(&sin, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = 0;
	sin.sin_port = 0;
	sin.sin_len = sizeof(sin);
	if (bind(fd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
		warn("%s bind\n", __func__);
		return (NULL);
	}

	/* Create comm */
	r->comm_wr = comm_create(fd, r->h, NULL, NULL);
	/* XXX check r */

	/* Setup the FD for writing */
	(void) comm_udp_write_setup(r->comm_wr, thrsrv_comm_udp_write_cb,
	    r, r->max_qdepth);

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
	printf("Usage: %s <numthreads> <qdepth> <pktrate> <bufsize> <remote IPv4 address> <port>\n",
	    progname);
	exit(127);
}

int
main(int argc, const char *argv[])
{
	struct clt_app *rp, *r;
	int i;
	int nthreads, connrate, bufsize, qdepth, rem_port;
	char *rem_ip;

	/* XXX validate command line parameters */
	if (argc < 7)
		usage(argv[0]);

	nthreads = atoi(argv[1]);
	qdepth = atoi(argv[2]);
	connrate = atoi(argv[3]);
	bufsize = atoi(argv[4]);
	rem_ip = strdup(argv[5]);
	rem_port = atoi(argv[6]);

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
		r->remote_port = rem_port;
		r->max_io_size = bufsize;
		r->max_qdepth = qdepth;
		r->connrate = connrate;
		if (pthread_create(&r->thr_id, NULL, thrclt_new, r) != 0)
			perror("pthread_create");
	}

	/* Join */
	for (i = 0; i < nthreads; i++) {
		pthread_join(rp[i].thr_id, NULL);
	}

	exit (0);
}
