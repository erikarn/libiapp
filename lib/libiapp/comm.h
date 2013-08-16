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

/*
 * Represent a frame, both for transmit and receive.
 */
struct fde_comm_udp_frame {
	TAILQ_ENTRY(fde_comm_udp_frame) node;
	char *buf;
	int size;
	int len;
	int frame_id;		/* assigned by fde_comm */
	int u_cookie;		/* assigned by owner */
	void *p_cookie;		/* assigned by owner */
	socklen_t sl_lcl;
	socklen_t sl_rem;
	struct sockaddr_storage sa_lcl;
	struct sockaddr_storage sa_rem;
};

/* General - close */
typedef void	comm_close_cb(int fd, struct fde_comm *fc, void *arg);

/* Stream - accept/connect */
typedef void	comm_connect_cb(int fd, struct fde_comm *fc, void *arg,
		    fde_comm_cb_status status, int retval);
typedef void	comm_accept_cb(int fd, struct fde_comm *fc, void *arg,
		    fde_comm_cb_status status, int newfd,
		    struct sockaddr *saddr, socklen_t slen, int xerrno);

/* Stream - read/write */
typedef void	comm_read_cb(int fd, struct fde_comm *fc, void *arg,
		    fde_comm_cb_status status, int retval);
typedef void	comm_write_cb(int fd, struct fde_comm *fc, void *arg,
		    fde_comm_cb_status status, int nwritten);

/* Datagram - read/write */
typedef void	comm_read_udp_cb(int fd, struct fde_comm *fc, void *arg,
		    struct fde_comm_udp_frame *fr, fde_comm_cb_status status,
		    int xerrno);
typedef void	comm_write_udp_cb(int fd, struct fde_comm *fc, void *arg,
		    struct fde_comm_udp_frame *fr, fde_comm_cb_status status,
		    int nwritten, int xerrno);

struct fde_comm {
	int fd;
	int do_close;		/* Whether to close the FD */
	struct fde_head *fh_parent;

	/* Events */
	struct fde *ev_read;
	struct fde *ev_read_cb;

	struct fde *ev_write;
	struct fde *ev_write_cb;

	struct fde *ev_accept;
	struct fde *ev_connect;
	struct fde *ev_connect_start;

	struct fde *ev_cleanup;

	struct fde *ev_udp_read;
	struct fde *ev_udp_write;

	/* General state */
	int is_closing;		/* Are we getting ready to close? */
	int is_cleanup;		/* cleanup has been scheduled */

	/*
	 * Stream read state
	 */
	struct {
		int is_active;
		int is_ready;	/* 1 when the read-ready event has fired */
		int is_read;	/* have we scheduled the read event? */
		char *buf;	/* buffer to read into */
		int len;	/* buffer length */
		comm_read_cb *cb;
		void *cbdata;
	} r;

	/*
	 * Stream write state
	 */
	struct {
		int is_active;
		int is_ready;	/* 1 when the write-ready event has fired */
		int is_write;	/* have we scheduled the write event? */
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

	/*
	 * Datagram read state
	 */
	struct {
		int maxlen;
		int is_active;
		comm_read_udp_cb *cb;
		void *cbdata;
	} udp_r;

	/*
	 * Datagram write state
	 */
	struct {
		int is_active;
		int is_primed;
		int max_qlen;
		int qlen;
		TAILQ_HEAD(udp_w_q, fde_comm_udp_frame) w_q;
		comm_write_udp_cb *cb;
		void *cbdata;
	} udp_w;
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

/*
 * Allocate a UDP frame.
 */
extern	struct fde_comm_udp_frame * fde_comm_udp_alloc(struct fde_comm *fc,
	    int maxlen);

/*
 * Free the given UDP frame.
 */
extern	void fde_comm_udp_free(struct fde_comm *fc,
	    struct fde_comm_udp_frame *fr);

/*
 * Start receiving UDP frames from the given file descriptor.
 */
extern	int comm_udp_read(struct fde_comm *fc, comm_read_udp_cb *cb,
	    void *cbdata, int maxlen);

/*
 * Set the callback and the number of UDP frames that we will
 * queue on this comm FD.  Any attempt to queue more will result
 * in the queue request failing.
 */
extern	int comm_udp_write_setup(struct fde_comm *fc, comm_write_udp_cb *cb,
	    void *cbdata, int qlen);

/*
 * Queue a new UDP frame to go out.  It will return -1 if the
 * queue failed.  It's up to the caller to free the buffer
 * if the transmit fails.
 *
 * Each queued item will have the callback called, either on
 * success or failure/close.  So the caller is responsible
 * for freeing the frame.
 */
extern	int comm_udp_write(struct fde_comm *fc,
	    struct fde_comm_udp_frame *fr);

#endif	/* __COMM_H__ */
