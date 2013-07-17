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

#ifndef	__COMM_H__
#define	__COMM_H__

struct fde_comm;

typedef enum {
	FDE_COMM_CB_NONE,
	FDE_COMM_CB_COMPLETED,
	FDE_COMM_CB_CLOSING,
	FDE_COMM_CB_ERROR,
	FDE_COMM_CB_EOF,
	FDE_COMM_CB_ABORTED
} fde_comm_cb_status;

typedef void	comm_accept_cb(int fd, struct fde_comm *fc, void *arg,
		    fde_comm_cb_status status, int newfd,
		    struct sockaddr *saddr, socklen_t slen, int xerrno);
typedef void	comm_read_cb(int fd, struct fde_comm *fc, void *arg,
		    fde_comm_cb_status status, int retval);
typedef void	comm_write_cb(int fd, struct fde_comm *fc, void *arg,
		    fde_comm_cb_status status, int nwritten);
typedef void	comm_close_cb(int fd, struct fde_comm *fc, void *arg);
typedef void	comm_connect_cb(int fd, struct fde_comm *fc, void *arg,
		    fde_comm_cb_status status, int retval);

struct fde_comm {
	int fd;
	int do_close;		/* Whether to close the FD */
	struct fde_head *fh_parent;

	/* Events */
	struct fde *ev_read;
	struct fde *ev_write;
	struct fde *ev_accept;
	struct fde *ev_connect;
	struct fde *ev_connect_start;
	struct fde *ev_cleanup;

	/* General state */
	int is_closing;		/* Are we getting ready to close? */
	int is_cleanup;		/* cleanup has been scheduled */

	/*
	 * Read state
	 */
	struct {
		int is_active;
		char *buf;	/* buffer to read into */
		int len;	/* buffer length */
		comm_read_cb *cb;
		void *cbdata;
	} r;

	/*
	 * Write state
	 */
	struct {
		int is_active;
		char *buf;
		int len;
		int offset;
		comm_write_cb *cb;
		void *cbdata;
	} w;

	/*
	 * Close state
	 */
	struct {
		comm_close_cb *cb;
		void *cbdata;
	} c;

	/*
	 * Accept state
	 */
	struct {
		int is_active;
		comm_accept_cb *cb;
		void *cbdata;
	} a;

	/*
	 * Connect state
	 */
	struct {
		int is_active;
		comm_connect_cb *cb;
		void *cbdata;
		struct sockaddr_storage sin;
		socklen_t slen;
	} co;
};

/*
 * Create a comm struct for an already-created FD.
 */
extern	struct fde_comm * comm_create(int fd, struct fde_head *fh,
	    comm_close_cb *cb, void *cbdata);

/*
 * Mark the comm struct as non-closing
 */
extern	void comm_mark_nonclose(struct fde_comm *fc);

/*
 * Schedule to have the given comm struct closed.
 *
 * + Any existing queued IO that can be cancelled will be cancelled
 *   and IO handlers will be called with FDE_COMM_CB_CLOSING.
 * + The close handler will then be called just before the comm
 *   state is freed.
 */
extern	void comm_close(struct fde_comm *fc);

/*
 * Set or clear the non-block flag on an open comm object.
 */
extern	int comm_fd_set_nonblocking(int fd, int enable);
extern	int comm_set_nonblocking(struct fde_comm *c, int enable);

/*
 * Schedule some data to be read.
 *
 * The buffer must stay valid for the lifetime of the read.
 */
extern	int comm_read(struct fde_comm *fc, char *buf, int len,
	    comm_read_cb *cb, void *cbdata);

/*
 * Schedule some data to be written.
 *
 * The buffer must stay valid for the lifetime of the write.
 */
extern	int comm_write(struct fde_comm *fc, char *buf, int len,
	    comm_write_cb *cb, void *cbdata);

/*
 * Start accept()ing on the given socket.
 *
 * This assumes that the socket is setup and listening.
 */
extern	int comm_listen(struct fde_comm *fc, comm_accept_cb *cb,
	    void *cbdata);

/*
 * Start a connect() to the remote address.
 */
extern	int comm_connect(struct fde_comm *fc, struct sockaddr *sin,
	    socklen_t slen, comm_connect_cb *cb, void *cbdata);

#endif	/* __COMM_H__ */
