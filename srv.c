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

struct thr;
struct conn;

struct conn {
	int fd;
	struct thr *parent;
	TAILQ_ENTRY(conn) node;
	struct fde *ev_read, *ev_write;
	struct fde *ev_cleanup;
};

struct thr {
	pthread_t thr_id;
	int thr_sockfd;
	struct fde_head *h;
	struct fde *ev_listen;
	TAILQ_HEAD(, conn) conn_list;
};

/*
 * This is an experiment - do a cleanup as a delayed thing, rather than
 * inline with the current IO.
 *
 * The fde network notifications (and likely the disk notifications too)
 * doesn't handle being deleted from within a network callback.
 *
 * However, scheduling callbacks do handle being deleted from within
 * a callback.  There's no risk that some pending callback is going
 * to point to a stale fde struct that has been freed elsewhere.
 *
 * It's quite possible that I'll have to add that awareness to it in
 * a non-dirty fashion.  But for now, we defer this stuff to outside
 * the IO machinery via a cleanup callback.
 */
static void
conn_cleanup_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	struct conn *c = arg;

	/*
	 * XXX this is just a hack; we can cleanup here, ideally
	 * we'd just mark this thing as closing and stick it on
	 * a 'thr' dead list, to be reaped.
	 */
	fprintf(stderr, "%s: FD %d: %p: closing\n", __func__, fd, c);
	fde_delete(c->parent->h, c->ev_read);
	fde_delete(c->parent->h, c->ev_write);
	fde_delete(c->parent->h, c->ev_cleanup);	/* XXX not needed? */
	close(c->fd);
	TAILQ_REMOVE(&c->parent->conn_list, c, node);
	free(c);
}

static void
conn_read_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	struct conn *c = arg;

	/* Schedule the cleanup event to free things */
	fde_add(c->parent->h, c->ev_cleanup);
}

static void
conn_write_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{

}

struct conn *
conn_new(struct thr *r, int fd)
{
	struct conn *c;

	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		warn("%s: calloc", __func__);
		return (NULL);
	}
	c->fd = fd;
	c->parent = r;
	c->ev_read = fde_create(r->h, fd, FDE_T_READ, conn_read_cb, c);
	c->ev_write = fde_create(r->h, fd, FDE_T_WRITE, conn_write_cb, c);
	c->ev_cleanup = fde_create(r->h, fd, FDE_T_CALLBACK,
	    conn_cleanup_cb, c);
	TAILQ_INSERT_TAIL(&r->conn_list, c, node);

	/* Start reading */
	fde_add(r->h, c->ev_read);

	return (c);
}

static void
thrsrv_listen_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	struct thr *r = arg;
	int new_fd;
	struct sockaddr_storage s;
	socklen_t slen;
	struct conn *c;

	bzero(&s, sizeof(s));
	slen = sizeof(s);

	/*
	 * XXX
	 *
	 * It seems that all the threads wake up when a new connection
	 * comes in.  Those that didn't win will simply get EAGAIN.
	 */

	while (1) {
		new_fd = accept(r->thr_sockfd, (struct sockaddr *) &s, &slen);
		if (new_fd < 0) {
#if 0
			fprintf(stderr, "%s: %p: err; errno=%d (%s)\n",
			    __func__,
			    r,
			    errno,
			    strerror(errno));
#endif
			break;
		}
		fprintf(stderr, "%s: %p: LISTEN: newfd=%d\n", __func__, r, new_fd);
		c = conn_new(r, new_fd);
		if (c == NULL) {
			close(new_fd);
		}
	}

	/* Re-add the event, as it's a oneshot */
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
		TAILQ_INIT(&r->conn_list);
		if (pthread_create(&r->thr_id, NULL, thrsrv_new, r) != 0)
			perror("pthread_create");
	}

	/* Join */
	for (i = 0; i < 4; i++) {
		pthread_join(rp[i].thr_id, NULL);
	}

	exit (0);
}
