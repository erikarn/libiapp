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
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <err.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include "fde.h"
#include "comm.h"

#define	XMIN(x,y)	((x) < (y) ? (x) : (y))

/*
 * This implements the 'socket' logic for sockets, pipes and such.
 * It isn't at all useful for disk IO.
 */

static int
comm_is_close_ready(struct fde_comm *fc)
{

	/* XXX should error out if fc->is_closing isn't 1 */
	if (fc->is_closing == 0)
		return (0);

	/*
	 * For now - no active read/write.
	 *
	 * XXX Later, we will need to also disable pending
	 * accept/connect machinery before doing this.
	 *
	 * XXX Later, we should wrap up UDP read/write first!
	 */
	return (fc->r.is_active == 0 && fc->w.is_active == 0 &&
	    fc->a.is_active == 0 && fc->co.is_active == 0);
}

static void
comm_start_cleanup(struct fde_comm *fc)
{

	/* XXX complain if we're called when not closing / ready */
	if (fc->is_closing == 0)
		return;

	/* We've already scheduled the cleanup */
	if (fc->is_cleanup == 1)
		return;

	if (! comm_is_close_ready(fc))
		return;

	/* Schedule the cleanup */
	fc->is_cleanup = 1;
	fde_add(fc->fh_parent, fc->ev_cleanup);
}

int
comm_fd_set_nonblocking(int fd, int enable)
{
	int a;

	a = fcntl(fd, F_GETFL, 0);
	/* XXX check */
	if (enable)
		a |= O_NONBLOCK;
	else
		a &= ~O_NONBLOCK;
	return (fcntl(fd, F_SETFL, a));
}

int
comm_set_nonblocking(struct fde_comm *c, int enable)
{

	return (comm_fd_set_nonblocking(c->fd, enable));
}

struct fde_comm_udp_frame *
fde_comm_udp_alloc(struct fde_comm *fc, int maxlen)
{
	struct fde_comm_udp_frame *fr;

	fr = calloc(1, sizeof(*fr));
	if (fr == NULL) {
		warn("%s: calloc", __func__);
		return (NULL);
	}

	fr->size = maxlen;

	fr->buf = malloc(maxlen);
	if (fr->buf == NULL) {
		warn("%s: malloc", __func__);
		free(fr);
		return (NULL);
	}

	fr->sl_lcl = sizeof(fr->sa_lcl);
	fr->sl_rem = sizeof(fr->sa_rem);

	return (fr);
}

void
fde_comm_udp_free(struct fde_comm *fc, struct fde_comm_udp_frame *fr)
{

	/* XXX ensure it's not on a linked list? */
	if (fr->buf)
		free(fr->buf);
	free(fr);
}

/*
 * Handle a read IO event.  This is just for socket reads; not
 * for accept.
 */
static void
comm_cb_read(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	int ret;
	struct fde_comm *c = arg;
	fde_comm_cb_status s;

	/* Closing? Don't do the IO; start the closing machinery */
	if (c->is_closing) {
		c->r.is_active = 0;
		c->r.cb(fd, c, c->r.cbdata, FDE_COMM_CB_CLOSING, 0);
		if (comm_is_close_ready(c)) {
			comm_start_cleanup(c);
			return;
		}
	}

	if (c->r.is_active == 0) {
		fprintf(stderr, "%s: %p: FD %d: comm_cb_read but not active?\n",
		    __func__,
		    c,
		    fd);
		return;
	}

	/* XXX validate that there's actually a buffer, len and callback */
	ret = read(fd, c->r.buf, c->r.len);

	/* If it's something we can restart, do so */
	if (ret < 0) {
		/*
		 * XXX should only fail this a few times before
		 * really failing.
		 */
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			fde_add(c->fh_parent, c->ev_read);
			return;
		}
	}

	/*
	 * Call the comm callback from this context.
	 */
	c->r.is_active = 0;
	if (ret == 0)
		s = FDE_COMM_CB_EOF;
	else if (ret < 0)
		s = FDE_COMM_CB_ERROR;
	else
		s = FDE_COMM_CB_COMPLETED;
	c->r.cb(fd, c, c->r.cbdata, s, ret);
}

static void
comm_cb_write(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	int ret;
	struct fde_comm *c = arg;
	fde_comm_cb_status s;

	/* Closing? Don't do the IO; start the closing machinery */
	if (c->is_closing) {
		c->w.is_active = 0;
		c->w.cb(fd, c, c->w.cbdata, FDE_COMM_CB_CLOSING, c->w.offset);
		if (comm_is_close_ready(c)) {
			comm_start_cleanup(c);
			return;
		}
	}

	if (c->w.is_active == 0) {
		fprintf(stderr, "%s: %p: FD %d: comm_cb_write but not active?\n",
		    __func__,
		    c,
		    fd);
		return;
	}

	/*
	 * Write out from the current buffer position.
	 */
	ret = write(fd, c->w.buf + c->w.offset, c->w.len - c->w.offset);
	if (ret < 0) {
		/*
		 * XXX should only fail this a few times before
		 * really failing.
		 */
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			fde_add(c->fh_parent, c->ev_write);
			return;
		}
	}

	/*
	 * Wrote more than 0 bytes? Bump the offset.
	 */
	if (ret > 0) {
		c->w.offset += ret;
	}

	/*
	 * If we have more left to write and we didn't error
	 * out, go for another pass.
	 */
	if (ret >= 0 && c->w.offset < c->w.len) {
		fde_add(c->fh_parent, c->ev_write);
		return;
	}

	/*
	 * If we wrote 0 bytes, we aren't going to make any further
	 * progress.  Signify EOF; the caller will note that it was
	 * a partial write.
	 */

	/* Time to notify! */
	c->w.is_active = 0;
	if (ret < 0) {
		s = FDE_COMM_CB_ERROR;
	} else if (ret == 0) {
		s = FDE_COMM_CB_EOF;
	} else {
		s = FDE_COMM_CB_COMPLETED;
	}

	c->w.cb(fd, c, c->w.cbdata, s, c->w.offset);
}

static void
comm_cb_accept(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	int ret;
	struct fde_comm *c = arg;
	//fde_comm_cb_status s;
	struct sockaddr_storage sin;
	socklen_t slen;

	/* Closing? Don't do the IO; start the closing machinery */
	if (c->is_closing) {
		c->a.is_active = 0;
		c->a.cb(fd, c, c->a.cbdata, FDE_COMM_CB_CLOSING, 0, NULL, 0, 0);
		if (comm_is_close_ready(c)) {
			comm_start_cleanup(c);
			return;
		}
	}

	if (c->a.is_active == 0) {
		fprintf(stderr, "%s: %p: FD %d: comm_cb_accept but not active?\n",
		    __func__,
		    c,
		    fd);
		return;
	}

	/*
	 * Loop over, accepting new connections.
	 *
	 * If the owner deletes the connection, we will drop out
	 * from the loop and not re-add things.
	 */
	while (1) {
		slen = sizeof(sin);
		ret = accept(fd, (struct sockaddr *) &sin, &slen);

		/* Break out on error; handle it elsewhere */
		if (ret < 0)
			break;

		/*
		 * Default - set non-blocking.
		 */
		(void) comm_fd_set_nonblocking(ret, 1);

		/*
		 * Call the callback.
		 */
		c->a.cb(fd, c, c->a.cbdata, FDE_COMM_CB_COMPLETED, ret,
		    (struct sockaddr *) &sin, slen, 0);
	}

	/*
	 * Handle error or transient error.
	 */

	/*
	 * Transient error.
	 */
	if (errno == EWOULDBLOCK || errno == EAGAIN) {
		fde_add(c->fh_parent, c->ev_accept);
		return;
	}

	/*
	 * Re-add for another event; the caller may choose to
	 * delete it and schedule things to close.
	 */
	fde_add(c->fh_parent, c->ev_accept);

	/* Non-transient error; inform the upper layer */
	c->a.cb(fd, c, c->a.cbdata, FDE_COMM_CB_ERROR, -1, NULL, 0, errno);
}

static void
comm_cb_cleanup(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	struct fde_comm *c = arg;

	/* XXX complain if is_closing / is_cleanup isn't done! */

	/*
	 * Call the close callback if one was registered.
	 */
	if (c->c.cb != NULL)
		c->c.cb(c->fd, c, c->c.cbdata);

	/*
	 * Close the file descriptor if we're allowed to.
	 */
	if (c->do_close == 1)
		close(c->fd);

	/*
	 * Free this FDE. It's fine to do it in a callback handler
	 * as the callback API guarantees that we can do this.
	 * (Note: it's not guaranteed for freeing IO FDEs from within
	 * an IO callback!)
	 */
	fde_free(c->fh_parent, c->ev_read);
	fde_free(c->fh_parent, c->ev_write);
	fde_free(c->fh_parent, c->ev_accept);
	fde_free(c->fh_parent, c->ev_connect);
	fde_free(c->fh_parent, c->ev_connect_start);
	fde_free(c->fh_parent, c->ev_cleanup);

	if (c->ev_udp_read)
		fde_free(c->fh_parent, c->ev_udp_read);
	if (c->ev_udp_write)
		fde_free(c->fh_parent, c->ev_udp_write);

	/*
	 * Finally, free the fde_comm state.
	 */
	free(c);
}

/*
 * Check the progress of the connect().
 */
static void
comm_cb_connect(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	int x, err;
	struct fde_comm *c = arg;
	socklen_t slen;

#if 0
	fprintf(stderr, "%s: %p: FD %d: called\n", __func__, c, c->fd);
#endif

	slen = sizeof(err);
	x = getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &slen);
	if (x == 0)
		errno = err;

	/* Now, check */
	if (x == 0 && errno == EINPROGRESS) {
		/* Still in progress */
		fde_add(c->fh_parent, c->ev_connect);
	} else if (x == 0 && errno == 0) {
		/* Completed! */
		c->co.is_active = 0;
		c->co.cb(c->fd, c, c->co.cbdata, FDE_COMM_CB_COMPLETED, 0);
	} else {
		/* Failure? */
		c->co.is_active = 0;
		c->co.cb(c->fd, c, c->co.cbdata, FDE_COMM_CB_ERROR, errno);
	}
}

/*
 * Begin the first part of connect() - issue the connect()
 * and see if it succeeds.
 */
static void
comm_cb_connect_start(int fd_unused, struct fde *f, void *arg,
    fde_cb_status status)
{
	int ret;
	struct fde_comm *c = arg;
	fde_comm_cb_status s;

#if 0
	fprintf(stderr, "%s: %p: FD %d: called\n", __func__, c, fd);
#endif

	/* Closing? Don't do the IO; start the closing machinery */
	if (c->is_closing) {
		fprintf(stderr,
			    "%s: %p: FD %d: closing\n",
			    __func__,
			    c,
			    c->fd);
		c->co.is_active = 0;
		c->co.cb(c->fd, c, c->co.cbdata, FDE_COMM_CB_CLOSING, 0);
		if (comm_is_close_ready(c)) {
			comm_start_cleanup(c);
			return;
		}
	}

	if (c->co.is_active == 0) {
		fprintf(stderr,
		    "%s: %p: FD %d: comm_cb_connect_start but not active?\n",
		    __func__,
		    c,
		    c->fd);
		return;
	}

	/* Start the first connect() attempt */
	ret = connect(c->fd, (struct sockaddr *) &c->co.sin, c->co.slen);

#if 0
	fprintf(stderr, "%s: %p: FD %d: connect() returned %d (errno %d (%s))\n",
	    __func__,
	    c,
	    c->fd,
	    ret,
	    errno,
	    strerror(errno));
#endif
	/* In progress? Register for write-readiness */
	if (ret < 0 && errno == EINPROGRESS) {
#if 0
		fprintf(stderr, "%s: %p: FD %d: waiting\n", __func__, c, c->fd);
#endif
		fde_add(c->fh_parent, c->ev_connect);
		return;
	}

	/* Well, it completed; let's handle the error case */
	c->co.is_active = 0;
	if (ret < 0) {
		s = FDE_COMM_CB_ERROR;
	} else {
		s = FDE_COMM_CB_COMPLETED;
	}
	c->co.cb(c->fd, c, c->co.cbdata, s, ret == 0 ? 0 : errno);
}

static void
comm_cb_udp_read(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	struct fde_comm_udp_frame *fr;
	struct fde_comm *c = arg;
	int r, xerrno;

	if (c->udp_r.is_active == 0) {
		/* XXX disable the event */
		return;
	}

	fr = fde_comm_udp_alloc(c, c->udp_r.maxlen);

	/*
	 * XXX Allocation failure? Tell the caller; we likely should
	 * stop reading on this socket until the caller calls a 'restart'
	 * method.
	 */
	if (fr == NULL) {
		/*
		 * Reschedule; the parent can deschedule us if required.
		 */
		fde_add(c->fh_parent, c->ev_udp_read);
		/* XXX ENOMEM? */
		c->udp_r.cb(c->fd, c, c->udp_r.cbdata, NULL,
		    FDE_COMM_CB_ERROR, ENOMEM);
		return;
	}

	/* Do a read */
	r = recvfrom(c->fd, fr->buf, fr->size, MSG_DONTWAIT,
	    (struct sockaddr *) &fr->sa_rem, &fr->sl_rem);

	if (r < 0) {
		/* Free buffer, return errno */
		xerrno = errno;
		fde_comm_udp_free(c, fr);
		/*
		 * Reschedule; the parent can deschedule us if required.
		 */
		fde_add(c->fh_parent, c->ev_udp_read);
		c->udp_r.cb(c->fd, c, c->udp_r.cbdata, NULL,
		    FDE_COMM_CB_ERROR, xerrno);
		return;
	}

	/* Set socket length */
	fr->len = r;

	/*
	 * Re-schedule for read early; that way the callback
	 * can just disable things.
	 */
	fde_add(c->fh_parent, c->ev_udp_read);
	c->udp_r.cb(c->fd, c, c->udp_r.cbdata, fr, FDE_COMM_CB_COMPLETED, 0);
}

/*
 * Create an fde_comm object for the given file descriptor.
 *
 * This assumes the file descriptor is already non-blocking and such.
 */
struct fde_comm *
comm_create(int fd, struct fde_head *fh, comm_close_cb *cb, void *cbdata)
{
	struct fde_comm *fc;

	fc = calloc(1, sizeof(*fc));
	if (fc == NULL) {
		warn("%s: calloc", __func__);
		return (NULL);
	}

	fc->fd = fd;
	fc->do_close = 1;
	fc->fh_parent = fh;

	fc->c.cb = cb;
	fc->c.cbdata = cbdata;

	fc->ev_read = fde_create(fh, fd, FDE_T_READ, comm_cb_read, fc);
	if (fc->ev_read == NULL)
		goto cleanup;

	fc->ev_write = fde_create(fh, fd, FDE_T_WRITE, comm_cb_write, fc);
	if (fc->ev_write == NULL)
		goto cleanup;

	fc->ev_cleanup = fde_create(fh, -1, FDE_T_CALLBACK,
	    comm_cb_cleanup, fc);
	if (fc->ev_cleanup == NULL)
		goto cleanup;

	fc->ev_accept = fde_create(fh, fd, FDE_T_READ, comm_cb_accept, fc);
	if (fc->ev_accept == NULL)
		goto cleanup;

	fc->ev_connect = fde_create(fh, fd, FDE_T_WRITE, comm_cb_connect, fc);
	if (fc->ev_connect == NULL)
		goto cleanup;

	fc->ev_connect_start = fde_create(fh, -1, FDE_T_CALLBACK,
	     comm_cb_connect_start, fc);
	if (fc->ev_connect_start == NULL)
		goto cleanup;

	fc->ev_udp_read = fde_create(fh, fd, FDE_T_READ, comm_cb_udp_read, fc);
	if (fc->ev_udp_read == NULL)
		goto cleanup;

	return (fc);

cleanup:
	if (fc->ev_read)
		fde_free(fh, fc->ev_read);
	if (fc->ev_write)
		fde_free(fh, fc->ev_write);
	if (fc->ev_cleanup)
		fde_free(fh, fc->ev_cleanup);
	if (fc->ev_accept)
		fde_free(fh, fc->ev_accept);
	if (fc->ev_connect)
		fde_free(fh, fc->ev_connect);
	if (fc->ev_connect_start)
		fde_free(fh, fc->ev_connect_start);
	if (fc->ev_udp_read)
		fde_free(fh, fc->ev_udp_read);
	if (fc->ev_udp_write)
		fde_free(fh, fc->ev_udp_write);
	free(fc);
	return (NULL);
}

void
comm_mark_nonclose(struct fde_comm *fc)
{

	fc->do_close = 0;
}

/*
 * Schedule a comm object to be closed.
 *
 * This will attempt to cancel any pending IO.
 *
 * All cancelled IO will be called with FDE_COMM_CB_CLOSING without the
 * IO occuring.
 *
 * Once all IO on the given socket has completed or cancelled, the close
 * callback will be made, then the state will be freed.  If do_close is
 * set to 1, the FD will be closed.
 */
void
comm_close(struct fde_comm *fc)
{

	/*
	 * If this socket is already closing, don't bother
	 * with the rest of this.
	 */
	if (fc->is_closing == 1)
		return;

	/*
	 * Begin the process of closing.
	 */
	fc->is_closing = 1;

	/*
	 * XXX complain if is_cleanup is set, it shouldn't be!
	 */

	/*
	 * Check to see if there's any pending IO.  If there is,
	 * let it complete (for now) - we'll later on add some
	 * stuff to abort IO that we can.
	 *
	 * If there isn't, we can just schedule the close callback.
	 */
	if (! comm_is_close_ready(fc))
		return;

	/*
	 * Ready to close!
	 */
	comm_start_cleanup(fc);
}

/*
 * Begin an asynchronous network read.
 *
 * Returns 0 if the read was scheduled, -1 if there is already
 * a pending asynchronous read.
 */
int
comm_read(struct fde_comm *fc, char *buf, int len, comm_read_cb *cb,
    void *cbdata)
{

	/* XXX should I be more vocal if this occurs */
	if (fc->r.is_active == 1)
		return (-1);

	/*
	 * XXX This is incompatible with doing accept/connect,
	 * so ensure they're not active.
	 */

	fc->r.cb = cb;
	fc->r.cbdata = cbdata;
	fc->r.buf = buf;
	fc->r.len = len;

	/*
	 * Begin doing read IO.
	 */
	fde_add(fc->fh_parent, fc->ev_read);
	fc->r.is_active = 1;

	return (1);
}

int
comm_write(struct fde_comm *fc, char *buf, int len, comm_write_cb *cb,
    void *cbdata)
{

	/* XXX should I be more vocal if this occurs */
	if (fc->w.is_active == 1)
		return (-1);

	/*
	 * XXX This is incompatible with doing accept/connect,
	 * so ensure they're not active.
	 */
	fc->w.cb = cb;
	fc->w.cbdata = cbdata;
	fc->w.buf = buf;
	fc->w.len = len;
	fc->w.offset = 0;

	/*
	 * Begin doing write IO.
	 */
	fde_add(fc->fh_parent, fc->ev_write);
	fc->w.is_active = 1;

	return (0);
}

int
comm_listen(struct fde_comm *fc, comm_accept_cb *cb, void *cbdata)
{

	/* XXX should I be more vocal if this occurs */
	if (fc->a.is_active == 1)
		return (-1);

	/*
	 * XXX This is incompatible with doing read/write, I should
	 * enforce this.
	 */
	fc->a.cb = cb;
	fc->a.cbdata = cbdata;

	/*
	 * Begin doing read IO.
	 */
	fde_add(fc->fh_parent, fc->ev_accept);
	fc->a.is_active = 1;

	return (0);
}

int
comm_connect(struct fde_comm *fc, struct sockaddr *sin, socklen_t slen,
    comm_connect_cb *cb, void *cbdata)
{

	if (fc->co.is_active == 1)
		return (-1);
	if (slen > sizeof(fc->co.sin))
		return (-1);

	fc->co.cb = cb;
	fc->co.cbdata = cbdata;
	memcpy(&fc->co.sin, sin, XMIN(slen, sizeof(fc->co.sin)));
	fc->co.slen = slen;

	/*
	 * Now, we can do the initial connect() here.
	 *
	 * Just keep in mind that once the connect() is done,
	 * subsequent attempts should use getsockopt() to see
	 * if it's connected.
	 *
	 * What we want to avoid doing is calling the connect
	 * completion handler from here.
	 */
	fc->co.is_active = 1;
	fde_add(fc->fh_parent, fc->ev_connect_start);

	return (0);
}

int
comm_udp_read(struct fde_comm *fc, comm_read_udp_cb *cb, void *cbdata,
    int maxlen)
{

	if (fc->udp_r.is_active == 1)
		return (-1);

	/* XXX fail if we're not a data socket */

	fc->udp_r.cb = cb;
	fc->udp_r.cbdata = cbdata;
	fc->udp_r.is_active = 1;
	fc->udp_r.maxlen = maxlen;
	fde_add(fc->fh_parent, fc->ev_udp_read);

	return (0);
}
