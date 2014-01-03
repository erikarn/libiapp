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
#include <signal.h>
#include <pthread.h>

/* For thread affinity */
#include <pthread_np.h>
#include <sys/cpuset.h>

#include <sys/time.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/endian.h>
#include <netinet/in.h>

#include "fde.h"
#include "shm_alloc.h"
#include "netbuf.h"
#include "comm.h"
#include "iapp_cpu.h"

#include "cfg.h"
#include "thr.h"
#include "conn.h"

#ifndef	IP_FLOWID
#define	IP_FLOWID	25
#endif

struct thr *rp;

static int
thrsrv_newfd_enqueue(int newfd, uint32_t flowid, struct thr *dr)
{
	struct thrsrv_newfd *th;

	th = calloc(1, sizeof(*th));
	if (th == NULL) {
		warn("%s: calloc", __func__);
		return (-1);
	}

	th->newfd = newfd;
	th->flowid = flowid;

	pthread_mutex_lock(&dr->newfd_lock);
	TAILQ_INSERT_TAIL(&dr->newfd_list, th, node);
	pthread_mutex_unlock(&dr->newfd_lock);

	return (0);
}

static int
thrsrv_listenfd_setup(struct sockaddr_storage *sin, int type, int len)
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

static int
thrsrv_listenfd_v4(int port)
{
	struct sockaddr_storage s;
	struct sockaddr_in *sin;

	bzero(&s, sizeof(s));

	sin = (struct sockaddr_in *) &s;

	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = 0;
	sin->sin_port = htons(port);
	sin->sin_len = sizeof(struct sockaddr_in);

	return (thrsrv_listenfd_setup(&s, AF_INET, sizeof(struct sockaddr_in)));
}

static int
thrsrv_listenfd_v6(int port)
{
	struct sockaddr_storage s;
	struct sockaddr_in6 *sin6;

	bzero(&s, sizeof(s));

	sin6 = (struct sockaddr_in6 *) &s;

	sin6->sin6_family = AF_INET6;
	sin6->sin6_addr = in6addr_any;
	sin6->sin6_port = htons(port);
	sin6->sin6_len = sizeof(struct sockaddr_in6);

	return (thrsrv_listenfd_setup(&s, AF_INET6, sizeof(struct sockaddr_in6)));
}

static void
thrsrv_conn_update_cb(struct conn *c, void *arg, conn_state_t newstate)
{
	struct thr *r = arg;

	/* Error? Schedule a close. */
	if (newstate == CONN_STATE_ERROR) {
		conn_close(c);
		return;
	}

	if (newstate == CONN_STATE_FREEING) {
		TAILQ_REMOVE(&r->conn_list, c, node);
		r->total_closed++;
		r->num_clients--;
	}
}

static void
thrsrv_conn_stats_update_cb(struct conn *c, void *arg, size_t tx_bytes,
    size_t rx_bytes)
{
	struct thr *r = arg;

	r->total_read += rx_bytes;
	r->total_written += tx_bytes;
}

static int
thrsrv_flowid_to_thread(uint32_t flowid)
{
	if (flowid == 0)
		return (-1);

	/* for now we just assume this is correct - 8 CPUs */
	return (flowid & 0x7);
}

static void
thrsrv_finish_setup(struct thr *r, int newfd, uint32_t flowid)
{
	struct conn *c;

	/* XXX no callbacks for now */
	c = conn_new(r->h, r->cfg, &r->sm, newfd, thrsrv_conn_update_cb, r);
	if (c == NULL) {
		close(newfd);
		return;
	}
	conn_set_stats_cb(c, thrsrv_conn_stats_update_cb, r);

	c->flowid = flowid;

	/* Add it to the connection list */
	TAILQ_INSERT_TAIL(&r->conn_list, c, node);

	/* number of clients */
	r->num_clients ++;
}

static void
thrsrv_run_deferred(int fd, struct fde *f, void *arg, fde_cb_status s)
{
	struct timeval tv;
	struct thr *r = arg;
	struct thrsrv_newfd *th;

	/*
	 * XXX should just extract the list first!
	 */
	pthread_mutex_lock(&r->newfd_lock);
	while (! TAILQ_EMPTY(&r->newfd_list)) {
		th = TAILQ_FIRST(&r->newfd_list);
		TAILQ_REMOVE(&r->newfd_list, th, node);
		thrsrv_finish_setup(r, th->newfd, th->flowid);
		free(th);
	}
	pthread_mutex_unlock(&r->newfd_lock);

	/*
	 * Run 100ms in advance.
	 */
	(void) gettimeofday(&tv, NULL);
	tv.tv_usec += (100 * 1000);
	if (tv.tv_usec > (1000 * 1000)) {
		tv.tv_usec -= (1000 * 1000);
		tv.tv_sec += 1;
	}
	fde_add_timeout(r->h, r->ev_deferred, &tv);
}

void
thrsrv_acceptfd(int fd, struct fde_comm *fc, void *arg, fde_comm_cb_status s,
    int newfd, struct sockaddr *saddr, socklen_t slen, int xerrno)
{
	struct thr *r = arg;
	int rr, flowid;
	socklen_t sl;
	int thr_id;

	if (s != FDE_COMM_CB_COMPLETED) {
		fprintf(stderr,
		    "%s: %p: LISTEN: status=%d, errno=%d, newfd=%d\n",
		    __func__, r, s, errno, newfd);
		return;
	}

	/*
	 * Flowid!
	 */
	sl = sizeof(flowid);
	rr = getsockopt(newfd, IPPROTO_IP, IP_FLOWID, &flowid, &sl);
	if (rr == 0) {
		printf("%s: FD=%d, flowid=0x%08x, len=%d\n", __func__, newfd, flowid, (int) sl);
	}

	/*
	 * Figure out the correct destination thread.
	 */
	thr_id = thrsrv_flowid_to_thread(flowid);

	/*
	 * Only do thread affinity work if configured.
	 */
	if (r->cfg->do_fd_affinity == 1 && thr_id != -1 && thr_id != r->app_id) {
		/*
		 * Limit the thread ID to the number of CPUs we have.
		 */
		thr_id = thr_id % r->cfg->num_threads;
		printf("%s: FD=%d, mapping from CPU %d -> %d\n",
		    __func__,
		    newfd,
		    r->app_id, thr_id);
		if (thrsrv_newfd_enqueue(newfd, flowid, &rp[thr_id]) < 0)
			close(newfd);
	} else {
		thrsrv_finish_setup(r, newfd, flowid);
	}
}

static void
thrsrv_stat_print(int fd, struct fde *f, void *arg, fde_cb_status s)
{
	struct thr *r = arg;
	struct timeval tv;

	fprintf(stderr, "%s: [%d]: %lld clients; new=%lld, closed=%lld, TX=%lld bytes, RX=%lld bytes\n",
	    __func__,
	    r->app_id,
	    (unsigned long long) r->num_clients,
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
thrsrv_new(void *arg)
{
	struct thr *r = arg;
	struct timeval tv;

	fprintf(stderr, "%s: %p: created\n", __func__, r);

	/* Create a listen comm object for v4 */
	if (r->thr_sockfd_v4 != -1) {
		r->comm_listen_v4 = comm_create(r->thr_sockfd_v4, r->h, NULL, NULL);
		comm_mark_nonclose(r->comm_listen_v4);
		(void) comm_listen(r->comm_listen_v4, thrsrv_acceptfd, r);
	}

	if (r->thr_sockfd_v6 != -1) {
		r->comm_listen_v6 = comm_create(r->thr_sockfd_v6, r->h, NULL, NULL);
		comm_mark_nonclose(r->comm_listen_v6);
		(void) comm_listen(r->comm_listen_v6, thrsrv_acceptfd, r);
	}

	pthread_mutex_init(&r->newfd_lock, NULL);

	/* Create statistics timer */
	r->ev_stats = fde_create(r->h, -1, FDE_T_TIMER, 0,
	    thrsrv_stat_print, r);
	r->ev_deferred = fde_create(r->h, -1, FDE_T_TIMER, 0,
	    thrsrv_run_deferred, r);

	/* Add stat - to be called one second in the future */
	(void) gettimeofday(&tv, NULL);
	tv.tv_sec += 1;
	fde_add_timeout(r->h, r->ev_stats, &tv);

	/* .. and the deferred handler */
	(void) gettimeofday(&tv, NULL);
	fde_add_timeout(r->h, r->ev_deferred, &tv);

	/* Loop around, listening for events; farm them off as required */
	while (1) {
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		fde_runloop(r->h, &tv);
	}

	return (NULL);
}

/*
 * Parse the given configuration attrib=value combination.
 * Update 'cfg' as appropriate.
 */
static int
srv_parse_option(struct cfg *cfg, const char *opt)
{
	char *str = NULL;
	const char *sa, *sv;
	char *p;

	if (opt == NULL)
		return (0);

	/*
	 * Temporary copy!
	 */
	str = strdup(opt);
	if (str == NULL) {
		warn("%s: strdup", __func__);
		goto finish_err;
	}

	/*
	 * Parse out the attrib=value
	 */
	p = str;

	sa = strsep(&p, "=");
	if (sa == NULL)
		goto finish_err;

	sv = strsep(&p, "=");
	if (sv == NULL)
		goto finish_err;

	/* Parse option */
	if (strcmp("num_threads", sa) == 0) {
		cfg->num_threads = atoi(sv);
	} else if (strcmp("io_size", sa) == 0) {
		cfg->io_size = atoi(sv);
	} else if (strcmp("max_num_conns", sa) == 0) {
		cfg->max_num_conns = atoi(sv);
	} else if (strcmp("atype", sa) == 0) {
		if (strcmp("posixshm", sv) == 0) {
			cfg->atype = NB_ALLOC_POSIXSHM;
		} else if (strcmp("malloc", sv) == 0) {
			cfg->atype = NB_ALLOC_MALLOC;
		} else {
			printf("unknown atype "
			    "(posixshm or malloc, got '%s')\n", sv);
			goto finish_err;
		}
	} else if (strcmp("port", sa) == 0) {
		cfg->port = atoi(sv);
	} else if (strcmp("do_thread_pin", sa) == 0) {
		cfg->do_thread_pin = atoi(sv);
	} else if (strcmp("do_fd_affinity", sa) == 0) {
		cfg->do_fd_affinity = atoi(sv);
	} else {
		printf("unknown option '%s'\n", sa);
		goto finish_err;
	}

	free(str);
	return (0);

finish_err:
	if (str)
		free(str);
	return (-1);
}

int
main(int argc, const char *argv[])
{
	struct thr *r;
	int i;
	int fd_v4 = -1;
	int fd_v6 = -1;
	int ncpu;
	struct cfg srv_cfg;
	sigset_t ss;

	bzero(&srv_cfg, sizeof(srv_cfg));

	/* Initialise the local config */
	srv_cfg.num_threads = 2;
	srv_cfg.io_size = 16384;
	srv_cfg.max_num_conns = 32768;
	srv_cfg.atype = NB_ALLOC_MALLOC;
	srv_cfg.port = 1667;
	srv_cfg.do_thread_pin = 1;
	srv_cfg.do_fd_affinity = 0;

	/* Parse command line */
	for (i = 1; i < argc; i++) {
		if (srv_parse_option(&srv_cfg, argv[i]))
			exit(127);
	}

	/* Mark SIGPIPE as an ignore on all threads */
	sigemptyset(&ss);
	sigaddset(&ss, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &ss, NULL);

	/* Allocate thread pool */
	rp = calloc(srv_cfg.num_threads, sizeof(struct thr));
	if (rp == NULL)
		perror("malloc");

	/*
	 * Create a single listen FD; we'll create separate conn state
	 * for each, but it'll be a single listen queue.
	 */
	fd_v4 = thrsrv_listenfd_v4(1667);
	if (fd_v4 < 0) {
		perror("listenfd");
	}

	/* .. and v6 */
	fd_v6 = thrsrv_listenfd_v6(1667);
	if (fd_v6 < 0) {
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
		r->thr_sockfd_v4 = fd_v4;
		r->thr_sockfd_v6 = fd_v6;

		r->h = fde_ctx_new();
		r->cfg = &srv_cfg;
		r->app_id = i;

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
		TAILQ_INIT(&r->newfd_list);
		if (pthread_create(&r->thr_id, NULL, thrsrv_new, r) != 0)
			perror("pthread_create");

		if (srv_cfg.do_thread_pin) {
			/* Set affinity */
			CPU_ZERO(&cp);
			CPU_SET(i % ncpu, &cp);

			printf("%s: thread id %d -> CPU %d\n", argv[0], i, i % ncpu);

			if (pthread_setaffinity_np(r->thr_id, sizeof(cpuset_t), &cp) != 0)
				warn("pthread_setaffinity_np (id %d)", i);
		}
	}

	/* Join */
	for (i = 0; i < srv_cfg.num_threads; i++) {
		pthread_join(rp[i].thr_id, NULL);
	}

	exit (0);
}
