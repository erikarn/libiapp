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

/*
 * Create a listen socket on the given ipv4 socket.
 */

struct thr {
	pthread_t thr_id;
	int thr_sockfd;
	struct fde_head *h;
	struct fde *ev_listen;
};

static void
thrsrv_listen_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	struct thr *r = arg;
	int new_fd;
	struct sockaddr_storage s;
	socklen_t slen;

	bzero(&s, sizeof(s));
	slen = sizeof(s);

	new_fd = accept(r->thr_sockfd, (struct sockaddr *) &s, &slen);
	fprintf(stderr, "%s: %p: LISTEN: newfd=%d\n", __func__, r, new_fd);
	if (new_fd < 0) {
		fprintf(stderr, "%s: %p: err; errno=%d (%s)\n",
		    __func__,
		    r,
		    errno,
		    strerror(errno));
		goto finish;
	}

	/* XXX for now */
	close(new_fd);

finish:
	/* Re-add */
	fde_add(r->h, r->ev_listen);
}

void *
thrsrv_new(void *arg)
{
	struct thr *r = arg;
	int ret;
	struct timespec tv;

	fprintf(stderr, "%s: %p: created\n", __func__, r);

	/* register a local event for the listen FD */
	r->ev_listen = fde_create(r->h, r->thr_sockfd, FDE_T_READ,
	    thrsrv_listen_cb, r);
	fde_add(r->h, r->ev_listen);

	/* Loop around, listening for events; farm them off as required */
	while (1) {

		tv.tv_sec = 1;
		tv.tv_nsec = 0;
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

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "%s: socket() failed; errno=%d (%s)\n",
		    __func__,
		    errno,
		    strerror(errno));
		return (-1);
	}

	/* make non-blocking */
	a = fcntl(fd, F_GETFL, 0);
	/* XXX check */
	fcntl(fd, F_SETFL, a | O_NONBLOCK);

	/* make reuse */
	a = 1;
	(void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &a, sizeof(&a));

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

int
main(int argc, const char *argv[])
{
	int fd;
	struct thr *rp, *r;
	int i;

	/* Allocate thread pool */
	rp = calloc(4, sizeof(struct thr));
	if (rp == NULL)
		perror("malloc");

	/* Create listen socket */
	fd = thrsrv_listenfd(1667);
	if (fd < 0) {
		perror("listenfd");
	}

	/* Create listen threads */
	for (i = 0; i < 4; i++) {
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
