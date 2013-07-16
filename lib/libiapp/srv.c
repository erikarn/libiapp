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
	struct fde_comm *comm;
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
client_read_cb(int fd, struct fde_comm *fc, void *arg, fde_comm_cb_status s,
    int retval)
{
	struct conn *c = arg;

	fprintf(stderr, "%s: FD %d: %p: s=%d, ret=%d\n",
	    __func__,
	    fd,
	    c,
	    s,
	    retval);

	/*
	 * start the close process
	 */
	comm_close(fc);
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
	c->comm = comm_create(fd, r->h);
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
	r->comm_listen = comm_create(r->thr_sockfd, r->h);
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
