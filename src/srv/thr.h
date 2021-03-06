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
#ifndef	__THR_H__
#define	__THR_H__

struct thr;
struct conn;
struct cfg;
struct thrsrv_newfd;

struct thrsrv_newfd {
	int newfd;
	uint32_t flowid;
	TAILQ_ENTRY(thrsrv_newfd) node;
};

struct thr {
	pthread_t thr_id;
	int app_id;
	struct cfg *cfg;
	struct shm_alloc_state sm;
	int thr_sockfd_v4;
	int thr_sockfd_v6;
	struct fde_head *h;
	struct fde_comm *comm_listen_v4;
	struct fde_comm *comm_listen_v6;
	struct fde *ev_stats;
	TAILQ_HEAD(, conn) conn_list;

	struct fde *ev_deferred;
	TAILQ_HEAD(, thrsrv_newfd) newfd_list;
	pthread_mutex_t newfd_lock;

	uint64_t total_read, total_written;
	uint64_t total_opened, total_closed;
	uint64_t num_clients;
};

#endif	/* __THR_H__ */
