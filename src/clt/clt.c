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
#include <arpa/inet.h>

#include "fde.h"
#include "comm.h"

struct clt_app;
struct conn;

typedef enum {
	CONN_STATE_NONE,
	CONN_STATE_CONNECTING,
	CONN_STATE_RUNNING,
	CONN_STATE_ERROR,
	CONN_STATE_CLOSING
} conn_state_t;

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
};

struct clt_app {
	pthread_t thr_id;
	struct fde_head *h;
	TAILQ_HEAD(, conn) conn_list;
};

#if 0
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
#endif

static void
conn_ev_cleanup_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	struct conn *c = arg;

	/* XXX ensure we're closing and cleanup has been scheduled */

	if (c->comm != NULL) {
		fprintf(stderr, "%s: %p: comm not null?\n", __func__, c);
	}

	fde_delete(c->parent->h, c->ev_cleanup);
	if (c->r.buf)
		free(c->r.buf);
	if (c->w.buf)
		free(c->w.buf);
	TAILQ_REMOVE(&c->parent->conn_list, c, node);
	free(c);
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
		/* XXX notify */
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
		/* XXX notify */
		return;
	}

	/*
	 * Write some more data
	 */
	comm_write(c->comm, c->w.buf, c->w.size, conn_write_cb, c);
}

static void
conn_connect_cb(int fd, struct fde_comm *fc, void *arg,
    fde_comm_cb_status status, int retval)
{
	struct conn *c = arg;

	fprintf(stderr, "%s: %p: called; status=%d, retval=%d\n",
	    __func__, c, status, retval);

	/* Error? Notify the upper layer; finish */
	if (status != FDE_COMM_CB_COMPLETED) {
		c->state = CONN_STATE_ERROR;
		/* XXX notify */
		return;
	}

	/* Success? Start writing data */
	c->state = CONN_STATE_RUNNING;

	comm_write(c->comm, c->w.buf, c->w.size, conn_write_cb, c);
}

struct conn *
conn_new(struct clt_app *r)
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
	if (c->fd == 0) {
		warn("%s: socket", __func__);
		free(c);
		free(c->r.buf);
		free(c->w.buf);
		return (NULL);
	}
	c->parent = r;
	c->comm = comm_create(c->fd, r->h, NULL, NULL);
	c->ev_cleanup = fde_create(r->h, -1, FDE_T_CALLBACK,
	    conn_ev_cleanup_cb, c);
	comm_set_nonblocking(c->comm, 1);
	TAILQ_INSERT_TAIL(&r->conn_list, c, node);

	return (c);
}

void *
thrclt_new(void *arg)
{
	struct clt_app *r = arg;
	struct timespec tv;
	struct conn *c;
	struct sockaddr_in sin;
	socklen_t slen;

	fprintf(stderr, "%s: %p: created\n", __func__, r);

	/*
	 * For now, let's create one client object and kick-start it.
	 */
	c = conn_new(r);

	/* Start connecting */
	c->state = CONN_STATE_CONNECTING;
	slen = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = htons(1667);
	(void) inet_aton("127.0.0.1", &sin.sin_addr);
	sin.sin_len = sizeof(struct sockaddr_in);
	(void) comm_connect(c->comm, (struct sockaddr *) &sin, slen,
	    conn_connect_cb, c);



	/* Loop around, listening for events; farm them off as required */
	while (1) {
		tv.tv_sec = 1;
		tv.tv_nsec = 0;
		fde_runloop(r->h, &tv);
	}

	return (NULL);
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
