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
#ifndef	__CONN_H__
#define	__CONN_H__

struct thr;
struct conn;

typedef enum {
	CONN_STATE_NONE,
	CONN_STATE_CONNECTING,
	CONN_STATE_RUNNING,
	CONN_STATE_ERROR,
	CONN_STATE_CLOSING,
	CONN_STATE_FREEING
} conn_state_t;

typedef void conn_owner_update_cb(struct conn *c, void *arg, conn_state_t newstate);

struct conn {
	int fd;
	struct thr *parent;
	TAILQ_ENTRY(conn) node;
	struct fde_comm *comm;
	struct fde *ev_cleanup;
	struct fde_head *h;
	conn_state_t state;

	/* read state / buffer */
	struct {
		char *buf;
		int size;
	} r;

	/* write state / buffer */
	struct {
		struct iapp_netbuf *nb;
	} w;

	struct {
		conn_owner_update_cb *cb;
		void *cbdata;
	} cb;

	/* Configuration related information */
	struct {
	} cfg;

	uint64_t total_read, total_written;
	uint64_t write_close_thr;
};

extern	struct conn * conn_new(struct thr *r, int fd,
	    conn_owner_update_cb *cb, void *cbdata);

#endif	/* __CONN_H__ */
