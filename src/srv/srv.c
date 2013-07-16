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

struct thr;
struct conn;

struct conn {
	int fd;
	struct thr *parent;
	TAILQ_ENTRY(conn) node;
	int is_closing;
	int close_called;
	int is_cleanup;
	struct fde_comm *comm;
	struct fde *ev_cleanup;
	struct {
		char *buf;
		int size;
	} r;
};

struct thr {
	pthread_t thr_id;
	int thr_sockfd;
	struct fde_head *h;
	struct fde_comm *comm_listen;
	TAILQ_HEAD(, conn) conn_list;
};

static void
client_ev_cleanup_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{

	struct conn *c = arg;

	/* Time to tidy up! */
	fprintf(stderr, "%s: %p: freeing\n", __func__, c);

	/* This MUST be free at this point */
	if (c->comm != NULL) {
		fprintf(stderr, "%s: %p: comm not null? Huh?\n",
		    __func__,
		    c);
	}

	fde_delete(c->parent->h, c->ev_cleanup);
	TAILQ_REMOVE(&c->parent->conn_list, c, node);
	free(c->r.buf);
	free(c);
}

static void
client_ev_close_cb(int fd, struct fde_comm *fc, void *arg)
{
	struct conn *c = arg;

	fprintf(stderr, "%s: FD %d: %p: close called\n", __func__, fd, c);
	/* NULL this - it'll be closed for us when this routine completes */
	c->comm = NULL;

	/* Schedule the actual cleanup */
	if (c->is_cleanup == 0) {
		fde_add(c->parent->h, c->ev_cleanup);
		c->is_cleanup = 1;
	}
}

static void
client_read_cb(int fd, struct fde_comm *fc, void *arg, fde_comm_cb_status s,
    int retval)
{
	struct conn *c = arg;

#if 0
	fprintf(stderr, "%s: FD %d: %p: s=%d, ret=%d\n",
	    __func__,
	    fd,
	    c,
	    s,
	    retval);
#endif

	/* Error or EOF? Begin the close process */
	if (s != FDE_COMM_CB_COMPLETED) {
		c->is_closing = 1;
	}

	/*
	 * If we're closing, do a comm_close() and then wait until we
	 * get notification for that.
	 */
	if (c->is_closing && (c->close_called == 0)) {
		c->close_called = 1;
		comm_close(fc);
		return;
	}

	/* register for another read */
	(void) comm_read(c->comm, c->r.buf, c->r.size, client_read_cb, c);
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

	c->r.size = 8192;
	c->r.buf = malloc(c->r.size);
	if (c->r.buf == NULL) {
		warn("%s: malloc", __func__);
		free(c);
		return (NULL);
	}

	c->fd = fd;
	c->parent = r;
	c->comm = comm_create(fd, r->h, client_ev_close_cb, c);
	c->ev_cleanup = fde_create(r->h, -1, FDE_T_CALLBACK,
	    client_ev_cleanup_cb, c);
	TAILQ_INSERT_TAIL(&r->conn_list, c, node);

	/*
	 * Start reading!
	 */
	(void) comm_read(c->comm, c->r.buf, c->r.size, client_read_cb, c);

	return (c);
}

static void
conn_acceptfd(int fd, struct fde_comm *fc, void *arg, fde_comm_cb_status s,
    int newfd, struct sockaddr *saddr, socklen_t slen, int xerrno)
{
	struct thr *r = arg;
	struct conn *c;

	fprintf(stderr, "%s: %p: LISTEN: newfd=%d\n", __func__, r, newfd);

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
	struct timespec tv;

	fprintf(stderr, "%s: %p: created\n", __func__, r);

	/* Create a listen comm object */
	r->comm_listen = comm_create(r->thr_sockfd, r->h, NULL, NULL);
	comm_mark_nonclose(r->comm_listen);
	(void) comm_listen(r->comm_listen, conn_acceptfd, r);

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

	/* Make non-blocking */
	(void) comm_fd_set_nonblocking(fd, 1);

	/* make reuse */
	a = 1;
	(void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &a, sizeof(&a));

	/* and reuse port */
	a = 1;
	(void) setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &a, sizeof(&a));

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
