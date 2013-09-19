/*
 *
 * Copyright CEA/DAM/DIF (2012)
 * contributor : Dominique Martinet  dominique.martinet@cea.fr
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * ---------------------------------------
 */

/**
 * \file    trans_rdma.c
 * \brief   rdma helper
 *
 * This is (very) loosely based on a mix of diod, rping (librdmacm/examples)
 * and kernel's net/9p/trans_rdma.c
 *
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>	//printf
#include <stdlib.h>	//malloc
#include <string.h>	//memcpy
#include <limits.h>	//INT_MAX
#include <inttypes.h>	//uint*_t
#include <errno.h>	//ENOMEM
#include <sys/socket.h> //sockaddr
#include <sys/un.h>     //sockaddr_un
#include <pthread.h>	//pthread_* (think it's included by another one)
#include <semaphore.h>  //sem_* (is it a good idea to mix sem and pthread_cond/mutex?)
#include <arpa/inet.h>  //inet_ntop
#include <netinet/in.h> //sock_addr_in
#include <unistd.h>	//fcntl
#include <fcntl.h>	//fcntl
#include <sys/epoll.h>
#include <sys/eventfd.h>

#define EPOLL_MAX_EVENTS 16
#define NUM_WQ_PER_POLL 16

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#include "utils.h"
#include "mooshika.h"

#ifdef HAVE_VALGRIND_MEMCHECK_H
#  include <valgrind/memcheck.h>
#  ifndef VALGRIND_MAKE_MEM_DEFINED
#    warning "Valgrind support requested, but VALGRIND_MAKE_MEM_DEFINED not available"
#  endif
#endif
#ifndef VALGRIND_MAKE_MEM_DEFINED
#  define VALGRIND_MAKE_MEM_DEFINED(addr, len)
#endif



/**
 * \struct msk_ctx
 * Context data we can use during recv/send callbacks
 */
struct msk_ctx {
	enum msk_ctx_used {
		MSK_CTX_FREE = 0,
		MSK_CTX_PENDING,
		MSK_CTX_PROCESSING
	} used;				/**< 0 if we can use it for a new recv/send */
	uint32_t pos;			/**< current position inside our own buffer. 0 <= pos <= len */
	struct rdmactx *next;		/**< next context */
	msk_data_t *data;
	ctx_callback_t callback;
	ctx_callback_t err_callback;
	union {
		struct ibv_recv_wr rwr;
		struct ibv_send_wr wwr;
	} wr;
	void *callback_arg;
	struct ibv_sge sg_list[0]; 		/**< this is actually an array. note that when you malloc you have to add its size */
};

enum msk_lock_flag {
	MSK_HAS_NO_LOCK = 0,
	MSK_HAS_TRANS_CM_LOCK = 1,
	MSK_HAS_TRANS_CTX_LOCK = 1 << 2
};

#define MSK_HAS_TRANS_LOCKS (MSK_HAS_TRANS_CM_LOCK | MSK_HAS_TRANS_CTX_LOCK)

struct msk_worker_data {
	msk_trans_t *trans;
	struct msk_ctx *ctx;
	enum ibv_wc_status status;
	enum ibv_wc_opcode opcode;
};

struct worker_pool {
	pthread_t *thrids;
	struct msk_worker_data *wd_queue;
	int worker_count;
	int size;
	int w_head;
	int w_count;
	int w_efd;
	int m_tail;
	int m_count;
	int m_efd;
};

struct msk_internals {
	pthread_mutex_t lock;
	int debug;
	pthread_t cm_thread;		/**< Thread id for connection manager */
	pthread_t cq_thread;		/**< Thread id for completion queue handler */
	pthread_t stats_thread;
	unsigned int run_threads;
	int cm_epollfd;
	int cq_epollfd;
	int stats_epollfd;
	struct worker_pool worker_pool;
};

/* GLOBAL VARIABLES */

static struct msk_internals *internals = NULL;


void __attribute__ ((constructor)) msk_internals_init(void) {
	internals = malloc(sizeof(*internals));
	if (!internals) {
		ERROR_LOG("Out of memory");
	}

	memset(internals, 0, sizeof(*internals));

	internals->run_threads = 0;
	pthread_mutex_init(&internals->lock, NULL);
}

void __attribute__ ((destructor)) msk_internals_fini(void) {

	if (internals) {
		internals->run_threads = 0;

		if (internals->cm_thread) {
			pthread_join(internals->cm_thread, NULL);
			internals->cm_thread = 0;
		}
		if (internals->cq_thread) {
			pthread_join(internals->cq_thread, NULL);
			internals->cq_thread = 0;
		}
		if (internals->stats_thread) {
			pthread_join(internals->stats_thread, NULL);
			internals->stats_thread = 0;
		}

		pthread_mutex_destroy(&internals->lock);
		free(internals);
		internals = NULL;
	}
}

/* forward declarations */

static void *msk_cq_thread(void *arg);


/* UTILITY FUNCTIONS */

/**
 * msk_reg_mr: registers memory for rdma use (almost the same as ibv_reg_mr)
 *
 * @param trans   [IN]
 * @param memaddr [IN] the address to register
 * @param size    [IN] the size of the area to register
 * @param access  [IN] the access to grants to the mr (e.g. IBV_ACCESS_LOCAL_WRITE)
 *
 * @return a pointer to the mr if registered correctly or NULL on failure
 */

struct ibv_mr *msk_reg_mr(msk_trans_t *trans, void *memaddr, size_t size, int access) {
	return ibv_reg_mr(trans->pd, memaddr, size, access);
}

/**
 * msk_reg_mr: deregisters memory for rdma use (exactly ibv_dereg_mr)
 *
 * @param mr [INOUT] the mr to deregister
 *
 * @return 0 on success, errno value on failure
 */
int msk_dereg_mr(struct ibv_mr *mr) {
	return ibv_dereg_mr(mr);
}

/**
 * msk_make_rloc: makes a rkey to send it for remote host use
 * 
 * @param mr   [IN] the mr in which the addr belongs
 * @param addr [IN] the addr to give
 * @param size [IN] the size to allow (hint)
 *
 * @return a pointer to the rkey on success, NULL on failure.
 */
msk_rloc_t *msk_make_rloc(struct ibv_mr *mr, uint64_t addr, uint32_t size) {
	msk_rloc_t *rloc;
	rloc = malloc(sizeof(msk_rloc_t));
	if (!rloc) {
		INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "Out of memory!");
		return NULL;
	}

	rloc->raddr = addr;
	rloc->rkey = mr->rkey;
	rloc->size = size;

	return rloc;
}

void msk_print_devinfo(msk_trans_t *trans) {
	struct ibv_device_attr device_attr;
	ibv_query_device(trans->cm_id->verbs, &device_attr);
	uint64_t node_guid = ntohll(device_attr.node_guid);
	printf("guid: %04x:%04x:%04x:%04x\n",
		(unsigned) (node_guid >> 48) & 0xffff,
		(unsigned) (node_guid >> 32) & 0xffff,
		(unsigned) (node_guid >> 16) & 0xffff,
		(unsigned) (node_guid >>  0) & 0xffff);
}

/**
 * msk_create_thread: Simple wrapper around pthread_create
 */
#define THREAD_STACK_SIZE 2116488
static inline int msk_create_thread(pthread_t *thrid, void *(*start_routine)(void*), void *arg) {

	pthread_attr_t attr;
	int ret;

	/* Init for thread parameter (mostly for scheduling) */
	if ((ret = pthread_attr_init(&attr)) != 0) {
		INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "can't init pthread's attributes: %s (%d)", strerror(ret), ret);
		return ret;
	}

	if ((ret = pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM)) != 0) {
		INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "can't set pthread's scope: %s (%d)", strerror(ret), ret);
		return ret;
	}

	if ((ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE)) != 0) {
		INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "can't set pthread's join state: %s (%d)", strerror(ret), ret);
		return ret;
	}

	if ((ret = pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE)) != 0) {
		INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "can't set pthread's stack size: %s (%d)", strerror(ret), ret);
		return ret;
	}

	return pthread_create(thrid, &attr, start_routine, arg);
}

static inline int msk_check_create_epoll_thread(pthread_t *thrid, void *(*start_routine)(void*), void *arg, int *epollfd) {
	int ret;

	pthread_mutex_lock(&internals->lock);
	if (*thrid == 0) do {
		*epollfd = epoll_create(10);
		if (*epollfd == -1) {
			ret = errno;
			INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "epoll_create failed: %s (%d)", strerror(ret), ret);
			break;
		}

		if ((ret = msk_create_thread(thrid, start_routine, arg))) {
			INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "Could not create thread: %s (%d)", strerror(ret), ret);
			*thrid = 0;
			break;
		}
	} while (0);

	pthread_mutex_unlock(&internals->lock);
	return 0;
}

static inline void msk_worker_callback(msk_trans_t *trans, struct msk_ctx *ctx, enum ibv_wc_status status, enum ibv_wc_opcode opcode) {
	struct timespec ts_start, ts_end;
	if (status) {
		if (ctx && ctx->err_callback) {
			if (trans->debug & MSK_DEBUG_SPEED)
				clock_gettime(CLOCK_MONOTONIC, &ts_start);
			ctx->err_callback(trans, ctx->data, ctx->callback_arg);
			if (trans->debug & MSK_DEBUG_SPEED) {
				clock_gettime(CLOCK_MONOTONIC, &ts_end);
				sub_timespec(&trans->stats.nsec_callback, &ts_start, &ts_end);
			}
		}
		pthread_mutex_lock(&trans->ctx_lock);
		ctx->used = MSK_CTX_FREE;
		pthread_cond_broadcast(&trans->ctx_cond);
		pthread_mutex_unlock(&trans->ctx_lock);
		return;
	}

	switch (opcode) {
		case IBV_WC_SEND:
		case IBV_WC_RDMA_WRITE:
		case IBV_WC_RDMA_READ:
			if (ctx->callback) {
				if (trans->debug & MSK_DEBUG_SPEED)
					clock_gettime(CLOCK_MONOTONIC, &ts_start);
				ctx->callback(trans, ctx->data, ctx->callback_arg);
				if (trans->debug & MSK_DEBUG_SPEED) {
					clock_gettime(CLOCK_MONOTONIC, &ts_end);
					sub_timespec(&trans->stats.nsec_callback, &ts_start, &ts_end);
				}
			}

			pthread_mutex_lock(&trans->ctx_lock);
			ctx->used = MSK_CTX_FREE;
			pthread_cond_broadcast(&trans->ctx_cond);
			pthread_mutex_unlock(&trans->ctx_lock);
			break;

		case IBV_WC_RECV:
		case IBV_WC_RECV_RDMA_WITH_IMM:
			if (ctx->callback) {
				if (trans->debug & MSK_DEBUG_SPEED)
					clock_gettime(CLOCK_MONOTONIC, &ts_start);
				ctx->callback(trans, ctx->data, ctx->callback_arg);
				if (trans->debug & MSK_DEBUG_SPEED) {
					clock_gettime(CLOCK_MONOTONIC, &ts_end);
					sub_timespec(&trans->stats.nsec_callback, &ts_start, &ts_end);
				}
			}

			pthread_mutex_lock(&trans->ctx_lock);
			ctx->used = MSK_CTX_FREE;
			pthread_cond_broadcast(&trans->ctx_cond);
			pthread_mutex_unlock(&trans->ctx_lock);
			break;

		default:
			INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "worker thread got weird opcode: %d", opcode);
	}
	return;
}

static void* msk_worker_thread(void *arg) {
	struct worker_pool *pool = arg;
	struct msk_worker_data wd;
	uint64_t n;
	int i;

	while (internals->run_threads > 0) {

		if (pool->w_count > 0) {
			i = atomic_dec(pool->w_count);
			if (i <= 0) {
				atomic_inc(pool->w_count);
				continue;
			}
			i = atomic_inc(pool->w_head);
			if (i >= pool->size) {
				i = i & (pool->size-1);
				atomic_mask(pool->w_head, pool->size-1);
			}
		} else {
			if (eventfd_read(pool->w_efd, &n) || n > INT_MAX) {
				INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "eventfd_read failed");
				continue;
			}
			if (internals->run_threads == 0)
				break;
			printf("worker: %d\n", (int)n);
			atomic_add(pool->w_count, (int)n);
			continue;
		}

		INFO_LOG(internals->debug & (MSK_DEBUG_SEND | MSK_DEBUG_RECV), "thread %lx, depopping wd index %i, count %i, trans %p, ctx %p, used %i", pthread_self(), pool->w_head, pool->w_count, pool->wd_queue[i].trans, pool->wd_queue[i].ctx, pool->wd_queue[i].ctx->used);

		memcpy(&wd, &pool->wd_queue[i], sizeof(struct msk_worker_data));

		if (eventfd_write(pool->m_efd, 1))
			INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "eventfd_write failed");

		msk_worker_callback(wd.trans, wd.ctx, wd.status, wd.opcode);
	}

	pthread_exit(NULL);
}

static int msk_spawn_worker_threads() {
	int i, ret;
	/* alloc and stuff */

	pthread_mutex_lock(&internals->lock);
	do {
		if (internals->worker_pool.thrids != NULL || internals->worker_pool.worker_count == -1) {
			break;
		}

		internals->worker_pool.thrids = malloc(internals->worker_pool.worker_count*sizeof(pthread_t));
		if (internals->worker_pool.thrids == NULL) {
			ret = ENOMEM;
			break;
		}
		internals->worker_pool.wd_queue = malloc(internals->worker_pool.size*sizeof(struct msk_worker_data));
		if (internals->worker_pool.wd_queue == NULL) {
			ret = ENOMEM;
			break;
		}

		internals->worker_pool.w_head = 0;
		internals->worker_pool.w_count = 0;
		internals->worker_pool.m_tail = 0;
		internals->worker_pool.m_count = 0;
		internals->worker_pool.w_efd = eventfd(0, 0);
		internals->worker_pool.m_efd = eventfd(0, 0);

		for (i=0; i < internals->worker_pool.worker_count; i++) {
			ret = msk_create_thread(&internals->worker_pool.thrids[i], msk_worker_thread, &internals->worker_pool);
			if (ret)
				break;
		}
	} while (0);
	if (ret) {
		// join stuff?
		INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "Could not create workers: %s (%d)", strerror(ret), ret);
		if (internals->worker_pool.wd_queue) {
			free(internals->worker_pool.wd_queue);
			internals->worker_pool.wd_queue = NULL;
		}
		if (internals->worker_pool.thrids) {
			free(internals->worker_pool.thrids);
			internals->worker_pool.thrids = NULL;
		}
	}
	pthread_mutex_unlock(&internals->lock);
	return ret;
}

/** msk_kill_worker_threads: stops and joins worker threads, assume that we hold internals->lock and internals->run_thread == 0;
 */
static int msk_kill_worker_threads() {
	int i;

	if (internals->worker_pool.thrids == NULL || internals->worker_pool.worker_count == -1) {
		return 0;
	}

	/* wake up all threads - this value guarantees that we wait till a thread woke up before sending it again... */
	/* FIXME make sure none is awake before sending that :) */
	for (i=0; i < internals->worker_pool.worker_count; i++)
		eventfd_write(internals->worker_pool.w_efd, 0xfffffffffffffffe);

	for (i=0; i < internals->worker_pool.worker_count; i++) {
		pthread_join(internals->worker_pool.thrids[i], NULL);
	}

	close(internals->worker_pool.w_efd);
	close(internals->worker_pool.m_efd);

	free(internals->worker_pool.thrids);
	internals->worker_pool.thrids = NULL;
	free(internals->worker_pool.wd_queue);
	internals->worker_pool.wd_queue = NULL;


	return 0;
}


static int msk_signal_worker(msk_trans_t *trans, struct msk_ctx *ctx, enum ibv_wc_status status, enum ibv_wc_opcode opcode, enum msk_lock_flag flag) {
	struct msk_worker_data *wd;
	int i;

	INFO_LOG(trans->debug & (MSK_DEBUG_SEND | MSK_DEBUG_RECV), "signaling trans %p, ctx %p, used %i", trans, ctx, ctx->used);

	// Don't signal and do it directly if no worker
	if (internals->worker_pool.worker_count == -1) {
		if (flag & MSK_HAS_TRANS_CTX_LOCK)
			pthread_mutex_unlock(&trans->ctx_lock);
		msk_worker_callback(trans, ctx, status, opcode);
		if (flag & MSK_HAS_TRANS_CTX_LOCK)
			pthread_mutex_lock(&trans->ctx_lock);
		return 0;
	}


	// needs CM lock and not ctx lock because only flush cares about processing and it doesn't change anything else for other things holding ctx lock...
	if (flag & MSK_HAS_TRANS_CM_LOCK)
		ctx->used = MSK_CTX_PROCESSING;

	while (atomic_inc(internals->worker_pool.m_count) >= internals->worker_pool.size
	    && internals->run_threads > 0) {
		uint64_t n;
		if (flag & MSK_HAS_TRANS_CM_LOCK)
			pthread_mutex_unlock(&trans->cm_lock);
		if (flag & MSK_HAS_TRANS_CTX_LOCK)
			pthread_mutex_unlock(&trans->ctx_lock);
		if (eventfd_read(internals->worker_pool.m_efd, &n) || n > INT_MAX) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "eventfd_read failed");
		} else {
			printf("master: %d\n", (int)n);
			atomic_sub(internals->worker_pool.m_count, (int)n+1 /* we're doing inc again */);
		}
		if (flag & MSK_HAS_TRANS_CTX_LOCK)
			pthread_mutex_lock(&trans->ctx_lock);
		if (flag & MSK_HAS_TRANS_CM_LOCK)
			pthread_mutex_lock(&trans->cm_lock);
	}

	do {
		if (internals->run_threads == 0) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Had something to do but threads stopping?");
			break;
		}

		i = atomic_inc(internals->worker_pool.m_tail);
		if (i >= internals->worker_pool.size) {
			i = i & (internals->worker_pool.size-1);
			atomic_mask(internals->worker_pool.w_head, internals->worker_pool.size-1);
		}

		wd = &internals->worker_pool.wd_queue[i];
		wd->trans = trans;
		wd->ctx = ctx;
		wd->status = status;
		wd->opcode = opcode;

		if (eventfd_write(internals->worker_pool.w_efd, 1))
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "eventfd_write failed");

	} while (0);

	return 0;
}


/**
 * msk_cq_addfd: Adds trans' completion queue fd to the epoll wait
 * Returns 0 on success, errno value on error.
 */
static int msk_addfd(msk_trans_t *trans, int fd, int epollfd) {
	int flags, ret;
	struct epoll_event ev;

	//make get_cq_event_nonblocking for poll
	flags = fcntl(fd, F_GETFL);
	ret = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	if (ret < 0) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Failed to make the comp channel nonblock");
		return ret;
	}

	ev.events = EPOLLIN;
	ev.data.ptr = trans;

	ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
	if (ret == -1) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Failed to add fd to epoll: %s (%d)", strerror(ret), ret);
		return ret;
	}

	return 0;
}

static int msk_delfd(int fd, int epollfd) {
	int ret;

	ret = epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
	if (ret == -1) {
		ret = errno;
		INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "Failed to del fd to epoll: %s (%d)", strerror(ret), ret);
		return ret;
	}

	return 0;
}

static inline int msk_cq_addfd(msk_trans_t *trans) {
	return msk_addfd(trans, trans->comp_channel->fd, internals->cq_epollfd);
}

static inline int msk_cq_delfd(msk_trans_t *trans) {
	return msk_delfd(trans->comp_channel->fd, internals->cq_epollfd);
}

static inline int msk_cm_addfd(msk_trans_t *trans) {
	return msk_addfd(trans, trans->event_channel->fd, internals->cm_epollfd);
}

static inline int msk_cm_delfd(msk_trans_t *trans) {
	return msk_delfd(trans->event_channel->fd, internals->cm_epollfd);
}

static inline int msk_stats_add(msk_trans_t *trans) {
	int rc;
	struct sockaddr_un sockaddr;

	/* no stats if no prefix */
	if (!trans->stats_prefix)
		return 0;

	/* setup trans->stats_sock here */
	if ( (trans->stats_sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		rc = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "socket on stats socket failed, quitting thread: %d (%s)", rc, strerror(rc));
		return rc;
	}

	memset(&sockaddr, 0, sizeof(sockaddr));
	sockaddr.sun_family = AF_UNIX;
	snprintf(sockaddr.sun_path, sizeof(sockaddr.sun_path)-1, "%s%p", trans->stats_prefix, trans);

	unlink(sockaddr.sun_path);

	if (bind(trans->stats_sock, (struct sockaddr*)&sockaddr, sizeof(sockaddr)) == -1) {
		rc = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "bind on stats socket failed, quitting thread: %d (%s)", rc, strerror(rc));
		return rc;
	}

	if (listen(trans->stats_sock, 5) == -1) {
		rc = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "listen on stats socket failed, quitting thread: %d (%s)", rc, strerror(rc));
		return rc;
	}


	return msk_addfd(trans, trans->stats_sock, internals->stats_epollfd);
}

static inline int msk_stats_del(msk_trans_t *trans) {
	/* msk_delfd is called in stats thread when a close event comes in */
	char sun_path[108];
	snprintf(sun_path, sizeof(sun_path)-1, "%s%p", trans->stats_prefix, trans);
	unlink(sun_path);

	return close(trans->stats_sock);
}

/**
 * msk_stats_thread: unix socket thread
 *
 * Well, a thread. arg = trans
 */
void *msk_stats_thread(void *arg) {
	msk_trans_t *trans;
	struct epoll_event epoll_events[EPOLL_MAX_EVENTS];
	char stats_str[256];
	int nfds, n, childfd;
	int ret;

	while (internals->run_threads > 0) {
		nfds = epoll_wait(internals->stats_epollfd, epoll_events, EPOLL_MAX_EVENTS, 100);
		if (nfds == 0 || (nfds == -1 && errno == EINTR))
			continue;

		if (nfds == -1) {
			ret = errno;
			INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "epoll_pwait failed: %s (%d)", strerror(ret), ret);
			break;
		}

		for (n = 0; n < nfds; ++n) {
			trans = (msk_trans_t*)epoll_events[n].data.ptr;

			if (!trans) {
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "got an event on a fd that should have been removed! (no trans)");
				continue;
			}

			if (epoll_events[n].events == EPOLLERR || epoll_events[n].events == EPOLLHUP) {
				msk_delfd(trans->stats_sock, internals->stats_epollfd);
				continue;
			}
			if ( (childfd = accept(trans->stats_sock, NULL, NULL)) == -1) {
				if (errno == EINTR) {
					continue;
				} else {
					INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "accept on stats socket failed: %d (%s).", errno, strerror(errno));
					continue;
				}
			}

			ret = snprintf(stats_str, sizeof(stats_str), "stats:\n"
				"	tx_bytes\ttx_pkt\n"
				"	%10"PRIu64"\t%"PRIu64"\n"
				"	rx_bytes\trx_pkt\n"
				"	%10"PRIu64"\t%"PRIu64"\n"
				"	error: %"PRIu64"\n"
				"	callback time:   %lu.%06lu s\n"
				"	completion time: %lu.%06lu s\n",
				trans->stats.tx_bytes, trans->stats.tx_pkt,
				trans->stats.rx_bytes, trans->stats.rx_pkt,
				trans->stats.err,
				trans->stats.nsec_callback / 1000000, trans->stats.nsec_callback % 1000000,
				trans->stats.nsec_compevent / 1000000, trans->stats.nsec_compevent % 1000000);
			ret = write(childfd, stats_str, ret);
			ret = close(childfd);
		}
	}

	pthread_exit(NULL);
}

/**
 * msk_cma_event_handler: handles addr/route resolved events (client side) and disconnect (everyone)
 *
 */
static int msk_cma_event_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *event) {
	int i;
	int ret = 0;
	msk_trans_t *trans = cm_id->context;

	INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "cma_event type %s", rdma_event_str(event->event));

	if (trans->bad_recv_wr) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Something was bad on that recv");
	}
	if (trans->bad_send_wr) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Something was bad on that send");
	}

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "ADDR_RESOLVED");
		pthread_mutex_lock(&trans->cm_lock);
		trans->state = MSK_ADDR_RESOLVED;
		pthread_cond_broadcast(&trans->cm_cond);
		pthread_mutex_unlock(&trans->cm_lock);
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "ROUTE_RESOLVED");
		pthread_mutex_lock(&trans->cm_lock);
		trans->state = MSK_ROUTE_RESOLVED;
		pthread_cond_broadcast(&trans->cm_cond);
		pthread_mutex_unlock(&trans->cm_lock);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ESTABLISHED");
		pthread_mutex_lock(&trans->cm_lock);
		if ((ret = msk_check_create_epoll_thread(&internals->cq_thread, msk_cq_thread, trans, &internals->cq_epollfd))
		|| (ret = msk_check_create_epoll_thread(&internals->stats_thread, msk_stats_thread, trans, &internals->stats_epollfd))) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "msk_check_create_epoll_thread failed: %s (%d)", strerror(ret), ret);
			trans->state = MSK_ERROR;
		} else {
			trans->state = MSK_CONNECTED;
		}

		pthread_cond_broadcast(&trans->cm_cond);
		pthread_mutex_unlock(&trans->cm_lock);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "CONNECT_REQUEST");
		//even if the cm_id is new, trans is the good parent's trans.
		pthread_mutex_lock(&trans->cm_lock);

		//FIXME don't run through this stupidely and remember last index written to and last index read, i.e. use as a queue
		/* Find an empty connection request slot */
		for (i = 0; i < trans->server; i++)
			if (!trans->conn_requests[i])
				break;

		if (i == trans->server) {
			pthread_mutex_unlock(&trans->cm_lock);
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Could not pile up new connection requests' cm_id!");
			ret = ENOBUFS;
			break;
		}

		// write down new cm_id and signal accept handler there's stuff to do
		trans->conn_requests[i] = cm_id;
		pthread_cond_broadcast(&trans->cm_cond);
		pthread_mutex_unlock(&trans->cm_lock);

		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "cma event %s, error %d",
			rdma_event_str(event->event), event->status);
		pthread_mutex_lock(&trans->cm_lock);
		trans->state = MSK_ERROR;
		pthread_cond_broadcast(&trans->cm_cond);
		pthread_mutex_unlock(&trans->cm_lock);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "DISCONNECT EVENT...");

		// don't call completion again
		if (trans->comp_channel)
			msk_cq_delfd(trans);

		pthread_mutex_lock(&trans->cm_lock);
		trans->state = MSK_CLOSED;
		pthread_cond_broadcast(&trans->cm_cond);
		pthread_mutex_unlock(&trans->cm_lock);

		if (trans->disconnect_callback)
			trans->disconnect_callback(trans);

		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "cma detected device removal!!!!");
		ret = ENODEV;
		break;

	default:
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "unhandled event: %s, ignoring\n",
			rdma_event_str(event->event));
		break;
	}

	return ret;
}

/**
 * msk_cm_thread: thread function which waits for new connection events and gives them to handler (then ack the event)
 *
 */
static void *msk_cm_thread(void *arg) {
	msk_trans_t *trans;
	struct rdma_cm_event *event;
	struct epoll_event epoll_events[EPOLL_MAX_EVENTS];
	int nfds, n;
	int ret;

	while (internals->run_threads > 0) {
		nfds = epoll_wait(internals->cm_epollfd, epoll_events, EPOLL_MAX_EVENTS, 100);

		if (nfds == 0 || (nfds == -1 && errno == EINTR))
			continue;

		if (nfds == -1) {
			ret = errno;
			INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "epoll_wait failed: %s (%d)", strerror(ret), ret);
			break;
		}

		for (n = 0; n < nfds; ++n) {
			trans = (msk_trans_t*)epoll_events[n].data.ptr;
			if (!trans) {
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "got an event on a fd that should have been removed! (no trans)");
				continue;
			}

			if (epoll_events[n].events == EPOLLERR || epoll_events[n].events == EPOLLHUP) {
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "epoll error or hup (%d)", epoll_events[n].events);
				continue;
			}
			if (trans->state == MSK_CLOSED) {
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "got a cm event on a closed trans?");
				continue;
			}

			if (!trans->event_channel) {
				if (trans->state != MSK_CLOSED)
					INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "no event channel? :D");
				continue;
			}

			ret = rdma_get_cm_event(trans->event_channel, &event);
			if (ret) {
				ret = errno;
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_get_cm_event failed: %d.", ret);
				continue;
			}
			ret = msk_cma_event_handler(event->id, event);

			trans = event->id->context;
			if (ret && (trans->state != MSK_LISTENING || trans == event->id->context)) {
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "something happened in cma_event_handler: %d", ret);
			}
			rdma_ack_cm_event(event);

			if (trans->state == MSK_CLOSED && trans->destroy_on_disconnect)
				msk_destroy_trans(&trans);
		}
	}

	pthread_exit(NULL);
}

/**
 * msk_cq_event_handler: completion queue event handler.
 * marks contexts back out of use and calls the appropriate callbacks for each kind of event
 *
 * @return 0 on success, work completion status if not 0
 */
static int msk_cq_event_handler(msk_trans_t *trans, enum msk_lock_flag flag) {
	struct ibv_wc wc[NUM_WQ_PER_POLL];
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	msk_ctx_t *ctx;
	msk_data_t *data;
	int ret, npoll, i;
	uint32_t len;

	ret = ibv_get_cq_event(trans->comp_channel, &ev_cq, &ev_ctx);
	if (ret) {
		ret = errno;
		if (ret != EAGAIN)
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_get_cq_event failed: %d", ret);
		return ret;
	}
	if (ev_cq != trans->cq) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Unknown cq,");
		ibv_ack_cq_events(ev_cq, 1);
		return EINVAL;
	}

	ret = ibv_req_notify_cq(trans->cq, 0);
	if (ret) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_req_notify_cq failed: %d.", ret);
	}

	while (ret == 0 && (npoll = ibv_poll_cq(trans->cq, NUM_WQ_PER_POLL, wc)) > 0) {
		for (i=0; i < npoll; i++) {
			if (trans->bad_recv_wr) {
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Something was bad on that recv");
			}
			if (trans->bad_send_wr) {
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Something was bad on that send");
			}
			if (wc[i].status) {
				trans->stats.err++;
				msk_signal_worker(trans, (msk_ctx_t *)wc[i].wr_id, wc[i].status, -1, flag);

				if (trans->state != MSK_CLOSED && trans->state != MSK_CLOSING && trans->state != MSK_ERROR) {
					INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "cq completion failed status: %s (%d)", ibv_wc_status_str(wc[i].status), wc[i].status);
					ret = wc[i].status;

				}
				continue;
			}

			switch (wc[i].opcode) {
			case IBV_WC_SEND:
			case IBV_WC_RDMA_WRITE:
			case IBV_WC_RDMA_READ:
				INFO_LOG(trans->debug & MSK_DEBUG_SEND, "WC_SEND/RDMA_WRITE/RDMA_READ: %d", wc[i].opcode);
				trans->stats.tx_pkt++;

				ctx = (msk_ctx_t *)wc[i].wr_id;

				data = ctx->data;
				while (data) {
					trans->stats.tx_bytes += data->size;
					data = data->next;
				}

				if (wc[i].wc_flags & IBV_WC_WITH_IMM) {
					//FIXME ctx->data->imm_data = ntohl(wc.imm_data);
					INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "imm_data: %d", ntohl(wc[i].imm_data));
				}

				msk_signal_worker(trans, ctx, wc[i].status, wc[i].opcode, flag);
				break;

			case IBV_WC_RECV:
			case IBV_WC_RECV_RDMA_WITH_IMM:
				INFO_LOG(trans->debug & MSK_DEBUG_RECV, "WC_RECV");
				trans->stats.rx_pkt++;
				trans->stats.rx_bytes += wc[i].byte_len;

				if (wc[i].wc_flags & IBV_WC_WITH_IMM) {
					//FIXME ctx->data->imm_data = ntohl(wc.imm_data);
					INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "imm_data: %d", ntohl(wc[i].imm_data));
				}

				ctx = (msk_ctx_t *)wc[i].wr_id;
			
				// fill all the sizes in case of multiple sge
				len = wc[i].byte_len;
				data = ctx->data;
				while (data && len > data->max_size) {
					data->size = data->max_size;
					VALGRIND_MAKE_MEM_DEFINED(data->data, data->size);
					len -= data->max_size;
					data = data->next;
				}
				if (data) {
					data->size = len;
					VALGRIND_MAKE_MEM_DEFINED(data->data, data->size);
				} else if (len) {
					INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "received more than could fit? %d leftover bytes", len);
				}

				msk_signal_worker(trans, ctx, wc[i].status, wc[i].opcode, flag);
				break;

			default:
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "unknown opcode: %d", wc[i].opcode);
				ret = EINVAL;
			}
		}
	}

	ibv_ack_cq_events(trans->cq, 1);

	return -ret;
}

/**
 * msk_cq_thread: thread function which waits for new completion events and gives them to handler (then ack the event)
 *
 */
static void *msk_cq_thread(void *arg) {
	msk_trans_t *trans;
	struct epoll_event epoll_events[EPOLL_MAX_EVENTS];
	struct timespec ts_start, ts_end;
	int nfds, n;
	int ret;

	while (internals->run_threads > 0) {
		nfds = epoll_wait(internals->cq_epollfd, epoll_events, EPOLL_MAX_EVENTS, 100);
		if (nfds == 0 || (nfds == -1 && errno == EINTR))
			continue;

		if (nfds == -1) {
			ret = errno;
			INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "epoll_pwait failed: %s (%d)", strerror(ret), ret);
			break;
		}

		for (n = 0; n < nfds; ++n) {
			trans = (msk_trans_t*)epoll_events[n].data.ptr;

			if (!trans) {
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "got an event on a fd that should have been removed! (no trans)");
				continue;
			}

			if (epoll_events[n].events == EPOLLERR || epoll_events[n].events == EPOLLHUP) {
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "epoll error or hup (%d)", epoll_events[n].events);
				continue;
			}
			if (trans->debug & MSK_DEBUG_SPEED)
				clock_gettime(CLOCK_MONOTONIC, &ts_start);

			pthread_mutex_lock(&trans->cm_lock);
			if (trans->state == MSK_CLOSED || trans->state == MSK_ERROR) {
				// closing trans, skip this, will be done on flush
				pthread_mutex_unlock(&trans->cm_lock);
				continue;
			}

			ret = msk_cq_event_handler(trans, MSK_HAS_TRANS_CM_LOCK);
			if (ret) {
				if (trans->state != MSK_CLOSED && trans->state != MSK_CLOSING && trans->state != MSK_ERROR) {
					INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "something went wrong with our cq_event_handler");
					trans->state = MSK_ERROR;
					pthread_cond_broadcast(&trans->cm_cond);
				}
			}
			pthread_mutex_unlock(&trans->cm_lock);

			if (trans->debug & MSK_DEBUG_SPEED) {
				clock_gettime(CLOCK_MONOTONIC, &ts_end);
				sub_timespec(&trans->stats.nsec_compevent, &ts_start, &ts_end);
			}
		}
	}

	pthread_exit(NULL);
}

/**
 * msk_flush_buffers: Flush all pending recv/send
 *
 * @param trans [IN]
 *
 * @return void
 */
static void msk_flush_buffers(msk_trans_t *trans) {
	struct msk_ctx *ctx;
	int i, wait, ret;

	INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "flushing %p", trans);

	pthread_mutex_lock(&trans->cm_lock);
	pthread_mutex_lock(&trans->ctx_lock);

	if (trans->state != MSK_ERROR) {
		do {
			ret = msk_cq_event_handler(trans, MSK_HAS_TRANS_LOCKS);
		} while (ret == 0);

		if (ret != EAGAIN)
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "couldn't flush pending data in cq: %d", ret);
	}

	for (i = 0, ctx = trans->recv_buf;
	     i < trans->qp_attr.cap.max_recv_wr;
	     i++, ctx = (msk_ctx_t*)((uint8_t*)ctx + sizeof(msk_ctx_t) + trans->qp_attr.cap.max_recv_sge*sizeof(struct ibv_sge)))
		if (ctx->used == MSK_CTX_PENDING) {
			ctx->used = MSK_CTX_PROCESSING;
			msk_signal_worker(trans, ctx, IBV_WC_FATAL_ERR, IBV_WC_RECV, MSK_HAS_TRANS_LOCKS);
		}

	for (i = 0, ctx = (msk_ctx_t *)trans->send_buf;
	     i < trans->qp_attr.cap.max_send_wr;
	     i++, ctx = (msk_ctx_t*)((uint8_t*)ctx + sizeof(msk_ctx_t) + trans->qp_attr.cap.max_send_sge*sizeof(struct ibv_sge)))
		if (ctx->used == MSK_CTX_PENDING) {
			ctx->used = MSK_CTX_PROCESSING;
			msk_signal_worker(trans, ctx, IBV_WC_FATAL_ERR, IBV_WC_SEND, MSK_HAS_TRANS_LOCKS);
		}



	do {
		wait = 0;
		for (i = 0, ctx = trans->recv_buf;
		     i < trans->qp_attr.cap.max_recv_wr;
		     i++, ctx = (msk_ctx_t*)((uint8_t*)ctx + sizeof(msk_ctx_t) + trans->qp_attr.cap.max_recv_sge*sizeof(struct ibv_sge)))
			if (ctx->used != MSK_CTX_FREE)
				wait++;

		for (i = 0, ctx = (msk_ctx_t *)trans->send_buf;
		     i < trans->qp_attr.cap.max_send_wr;
		     i++, ctx = (msk_ctx_t*)((uint8_t*)ctx + sizeof(msk_ctx_t) + trans->qp_attr.cap.max_send_sge*sizeof(struct ibv_sge)))
			if (ctx->used != MSK_CTX_FREE)
				wait++;

	} while (wait && pthread_cond_wait(&trans->ctx_cond, &trans->ctx_lock) == 0);

	pthread_mutex_unlock(&trans->ctx_lock);
	pthread_mutex_unlock(&trans->cm_lock);
}

/**
 * msk_destroy_buffer
 *
 * @param trans [INOUT]
 *
 * @return void even if some stuff here can fail //FIXME?
 */
static void msk_destroy_buffer(msk_trans_t *trans) {
	if (trans->send_buf) {
		free(trans->send_buf);
		trans->send_buf = NULL;
	}
	if (trans->recv_buf) {
		free(trans->recv_buf);
		trans->recv_buf = NULL;
	}
}

/**
 * msk_destroy_qp: destroys all qp-related stuff for us
 *
 * @param trans [INOUT]
 *
 * @return void, even if the functions _can_ fail we choose to ignore it. //FIXME?
 */
static void msk_destroy_qp(msk_trans_t *trans) {
	if (trans->qp) {
		// flush all pending receive/send buffers to error callback
		msk_flush_buffers(trans);

		ibv_destroy_qp(trans->qp);
		trans->qp = NULL;
	}
	if (trans->cq) {
		ibv_destroy_cq(trans->cq);
		trans->cq = NULL;
	}
	if (trans->comp_channel) {
		ibv_destroy_comp_channel(trans->comp_channel);
		trans->comp_channel = NULL;
	}
	if (trans->pd) {
		ibv_dealloc_pd(trans->pd);
		trans->pd = NULL;
	}
}


/**
 * msk_destroy_trans: disconnects and free trans data
 *
 * @param ptrans [INOUT] pointer to the trans to destroy
 */
void msk_destroy_trans(msk_trans_t **ptrans) {

	msk_trans_t *trans = *ptrans;

	if (trans) {
		trans->destroy_on_disconnect = 0;
		if (trans->state == MSK_CONNECTED || trans->state == MSK_CLOSED) {
			pthread_mutex_lock(&trans->cm_lock);
			if (trans->state != MSK_CLOSED && trans->state != MSK_LISTENING && trans->state != MSK_ERROR)
				trans->state = MSK_CLOSING;

			if (trans->cm_id && trans->cm_id->verbs)
				rdma_disconnect(trans->cm_id);

			while (trans->state != MSK_CLOSED && trans->state != MSK_LISTENING && trans->state != MSK_ERROR) {
				INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "we're not closed yet, waiting for disconnect_event");
				pthread_cond_wait(&trans->cm_cond, &trans->cm_lock);
			}
			trans->state = MSK_CLOSED;
			pthread_mutex_unlock(&trans->cm_lock);
		}

		if (trans->cm_id) {
			rdma_destroy_id(trans->cm_id);
			trans->cm_id = NULL;
		}

		if (trans->stats_sock)
			msk_stats_del(trans);

		// event channel is shared between all children, so don't close it unless it's its own.
		if ((trans->server != MSK_SERVER_CHILD) && trans->event_channel) {
			msk_cm_delfd(trans);
			rdma_destroy_event_channel(trans->event_channel);
			trans->event_channel = NULL;

			// likewise for stats prefix
			if (trans->stats_prefix)
				free(trans->stats_prefix);

			if (trans->node)
				free(trans->node);
			if (trans->port)
				free(trans->port);
		}

		// these two functions do the proper if checks
		msk_destroy_qp(trans);
		msk_destroy_buffer(trans);

		pthread_mutex_lock(&internals->lock);
		internals->run_threads--;
		if (internals->run_threads == 0) {
			if (internals->cm_thread) {
				pthread_join(internals->cm_thread, NULL);
				internals->cm_thread = 0;
			}
			if (internals->cq_thread) {
				pthread_join(internals->cq_thread, NULL);
				internals->cq_thread = 0;
			}
			if (internals->stats_thread) {
				pthread_join(internals->stats_thread, NULL);
				internals->stats_thread = 0;
			}
			msk_kill_worker_threads();
		}
		pthread_mutex_unlock(&internals->lock);


		//FIXME check if it is init. if not should just return EINVAL but.. lock.__lock, cond.__lock might work.
		pthread_mutex_unlock(&trans->cm_lock);
		pthread_mutex_destroy(&trans->cm_lock);
		pthread_cond_destroy(&trans->cm_cond);
		pthread_cond_destroy(&trans->ctx_cond);

		free(trans);
		*ptrans = NULL;
	}
}

/**
 * msk_init: part of the init that's the same for client and server
 *
 * @param ptrans [INOUT]
 * @param attr   [IN]    attributes to set parameters in ptrans. attr->addr must be set, others can be either 0 or sane values.
 *
 * @return 0 on success, errno value on failure
 */
int msk_init(msk_trans_t **ptrans, msk_trans_attr_t *attr) {
	int ret;

	msk_trans_t *trans;

	if (!ptrans || !attr) {
		INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "Invalid argument");
		return EINVAL;
	}

	trans = malloc(sizeof(msk_trans_t));
	if (!trans) {
		INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "Out of memory");
		return ENOMEM;
	}

	do {
		memset(trans, 0, sizeof(msk_trans_t));

		trans->event_channel = rdma_create_event_channel();
		if (!trans->event_channel) {
			ret = errno;
			INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "create_event_channel failed: %s (%d)", strerror(ret), ret);
			break;
		}

		trans->conn_type = (attr->conn_type ? attr->conn_type : RDMA_PS_TCP);

		ret = rdma_create_id(trans->event_channel, &trans->cm_id, trans, trans->conn_type);
		if (ret) {
			ret = errno;
			INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "create_id failed: %s (%d)", strerror(ret), ret);
			break;
		}

		trans->state = MSK_INIT;

		if (!attr->node || !attr->port) {
			INFO_LOG(internals->debug & MSK_DEBUG_EVENT, "node and port have to be defined");
			ret = EDESTADDRREQ;
			break;
		}
		trans->node = strdup(attr->node);
		if (!trans->node) {
				ret = ENOMEM;
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "couldn't malloc trans->node");
				break;
		}
		trans->port = strdup(attr->port);
		if (!trans->port) {
				ret = ENOMEM;
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "couldn't malloc trans->port");
				break;
		}
		/*memcpy(trans->qp_attr, attr->qp_attr, sizeof(struct ibv_qp_init_attr));*/

		/* fill in default values */
		trans->qp_attr.cap.max_send_wr = attr->sq_depth ? attr->sq_depth : 50;
		trans->qp_attr.cap.max_recv_wr = attr->rq_depth ? attr->rq_depth : 50;
		trans->qp_attr.cap.max_recv_sge = attr->max_recv_sge ? attr->max_recv_sge : 1;
		trans->qp_attr.cap.max_send_sge = attr->rq_depth ? attr->rq_depth : 50;
		trans->qp_attr.cap.max_inline_data = 0; // change if IMM
		trans->qp_attr.qp_type = (attr->conn_type == RDMA_PS_UDP ? IBV_QPT_UD : IBV_QPT_RC);
		trans->qp_attr.sq_sig_all = 1;

		trans->server = attr->server;
		trans->debug = attr->debug;
		trans->timeout = attr->timeout   ? attr->timeout  : 3000000; // in ms
		trans->disconnect_callback = attr->disconnect_callback;
		trans->destroy_on_disconnect = attr->destroy_on_disconnect;
		if (attr->stats_prefix) {
			ret = strlen(attr->stats_prefix)+1;
			trans->stats_prefix = malloc(ret);
			if (!trans->stats_prefix) {
				ret = ENOMEM;
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "couldn't malloc trans->stats_prefix");
				break;
			}

			strncpy(trans->stats_prefix, attr->stats_prefix, ret);
		}

		if (attr->pd)
			trans->pd = attr->pd;

		ret = pthread_mutex_init(&trans->cm_lock, NULL);
		if (ret) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "pthread_mutex_init failed: %s (%d)", strerror(ret), ret);
			break;
		}
		ret = pthread_cond_init(&trans->cm_cond, NULL);
		if (ret) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "pthread_cond_init failed: %s (%d)", strerror(ret), ret);
			break;
		}
		ret = pthread_mutex_init(&trans->ctx_lock, NULL);
		if (ret) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "pthread_mutex_init failed: %s (%d)", strerror(ret), ret);
			break;
		}
		ret = pthread_cond_init(&trans->ctx_cond, NULL);
		if (ret) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "pthread_cond_init failed: %s (%d)", strerror(ret), ret);
			break;
		}

		pthread_mutex_lock(&internals->lock);
		internals->debug = trans->debug;
		if (internals->run_threads == 0) {
			internals->worker_pool.worker_count = attr->worker_count ? attr->worker_count : -1;

			/* round up worker_pool.size to the next bigger power of two */
			attr->worker_queue_size = attr->worker_queue_size ? attr->worker_queue_size : 64;
			internals->worker_pool.size = 2;
			while (internals->worker_pool.size < attr->worker_queue_size)
				internals->worker_pool.size *= 2;
		}
		internals->run_threads++;
		pthread_mutex_unlock(&internals->lock);
		ret = msk_spawn_worker_threads();

		if (ret) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Could not start worker threads: %s (%d)", strerror(ret), ret);
			break;
		}
	} while (0);

	if (ret) {
		msk_destroy_trans(&trans);
		return ret;
	}

	*ptrans = trans;

	return 0;
}

/**
 * msk_create_qp: create a qp associated with a trans
 *
 * @param trans [INOUT]
 * @param cm_id [IN]
 *
 * @ret 0 on success, errno value on error
 */
static int msk_create_qp(msk_trans_t *trans, struct rdma_cm_id *cm_id) {
	int ret;

	if (rdma_create_qp(cm_id, trans->pd, &trans->qp_attr)) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_create_qp: %s (%d)", strerror(ret), ret);
		return ret;
	}

	trans->qp = cm_id->qp;
	return 0;
}

/**
 * msk_setup_qp: setups pd, qp an' stuff
 *
 * @param trans [INOUT]
 *
 * @return 0 on success, errno value on failure
 */
static int msk_setup_qp(msk_trans_t *trans) {
	int ret;

	INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "trans: %p", trans);

	if (!trans->pd)
		trans->pd = ibv_alloc_pd(trans->cm_id->verbs);
	if (!trans->pd) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_alloc_pd failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	trans->comp_channel = ibv_create_comp_channel(trans->cm_id->verbs);
	if (!trans->comp_channel) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_create_comp_channel failed: %s (%d)", strerror(ret), ret);
		msk_destroy_qp(trans);
		return ret;
	}

	trans->cq = ibv_create_cq(trans->cm_id->verbs, trans->qp_attr.cap.max_send_wr + trans->qp_attr.cap.max_recv_wr,
				  trans, trans->comp_channel, 0);
	if (!trans->cq) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_create_cq failed: %s (%d)", strerror(ret), ret);
		msk_destroy_qp(trans);
		return ret;
	}
	trans->qp_attr.send_cq = trans->cq;
	trans->qp_attr.recv_cq = trans->cq;

	ret = ibv_req_notify_cq(trans->cq, 0);
	if (ret) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_req_notify_cq failed: %s (%d)", strerror(ret), ret);
		msk_destroy_qp(trans);
		return ret;
	}

	ret = msk_create_qp(trans, trans->cm_id);
	if (ret) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "our own create_qp failed: %s (%d)", strerror(ret), ret);
		msk_destroy_qp(trans);
		return ret;
	}

	INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "created qp %p", trans->qp);
	return 0;
}


/**
 * msk_setup_buffer
 */
static int msk_setup_buffer(msk_trans_t *trans) {
	trans->recv_buf = malloc(trans->qp_attr.cap.max_recv_wr * (sizeof(msk_ctx_t) + trans->qp_attr.cap.max_recv_sge * sizeof(struct ibv_sge)));
	if (!trans->recv_buf) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "couldn't malloc trans->recv_buf");
		return ENOMEM;
	}
	memset(trans->recv_buf, 0, trans->qp_attr.cap.max_recv_wr * (sizeof(msk_ctx_t) + trans->qp_attr.cap.max_recv_sge * sizeof(struct ibv_sge)));

	trans->send_buf = malloc(trans->qp_attr.cap.max_send_wr * (sizeof(msk_ctx_t) + trans->qp_attr.cap.max_send_sge * sizeof(struct ibv_sge)));
	if (!trans->send_buf) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "couldn't malloc trans->send_buf");
		return ENOMEM;
	}
	memset(trans->send_buf, 0, trans->qp_attr.cap.max_send_wr * (sizeof(msk_ctx_t) + trans->qp_attr.cap.max_send_sge * sizeof(struct ibv_sge)));

	return 0;
}

/**
 * msk_bind_server
 *
 * @param trans [INOUT]
 *
 * @return 0 on success, errno value on failure
 */
int msk_bind_server(msk_trans_t *trans) {
	struct rdma_addrinfo hints, *res;
	int ret;

	if (!trans || trans->state != MSK_INIT) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "trans must be initialized first!");
		return EINVAL;
	}

	if (trans->server <= 0) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Must be on server side to call this function");
		return EINVAL;
	}


	trans->conn_requests = malloc(trans->server * sizeof(struct rdma_cm_id*));
	if (!trans->conn_requests) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Could not allocate conn_requests buffer");
		return ENOMEM;
	}

	memset(trans->conn_requests, 0, trans->server * sizeof(struct rdma_cm_id*));

	memset(&hints, 0, sizeof(struct rdma_addrinfo));
	hints.ai_flags = RAI_PASSIVE;
	hints.ai_port_space = trans->conn_type;

	ret = rdma_getaddrinfo(trans->node, trans->port, &hints, &res);
	if (ret) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_getaddrinfo: %s (%d)", strerror(ret), ret);
		return ret;
	}
	ret = rdma_bind_addr(trans->cm_id, res->ai_src_addr);
	if (ret) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_bind_addr: %s (%d)", strerror(ret), ret);
		return ret;
	}
	rdma_freeaddrinfo(res);

	ret = rdma_listen(trans->cm_id, trans->server);
	if (ret) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_listen failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	trans->state = MSK_LISTENING;

	if ((ret = msk_check_create_epoll_thread(&internals->cm_thread, msk_cm_thread, trans, &internals->cm_epollfd))) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "msk_check_create_epoll_thread failed: %s (%d)", strerror(ret), ret);
		return ret;
	}
	msk_cm_addfd(trans);

	return 0;
}


static msk_trans_t *clone_trans(msk_trans_t *listening_trans, struct rdma_cm_id *cm_id) {
	msk_trans_t *trans = malloc(sizeof(msk_trans_t));
	int ret;

	if (!trans) {
		INFO_LOG(listening_trans->debug & MSK_DEBUG_EVENT, "malloc failed");
		return NULL;
	}

	memcpy(trans, listening_trans, sizeof(msk_trans_t));

	trans->cm_id = cm_id;
	trans->cm_id->context = trans;
	trans->state = MSK_CONNECT_REQUEST;
	trans->server = MSK_SERVER_CHILD;

	memset(&trans->cm_lock, 0, sizeof(pthread_mutex_t));
	memset(&trans->cm_cond, 0, sizeof(pthread_cond_t));
	memset(&trans->ctx_lock, 0, sizeof(pthread_mutex_t));
	memset(&trans->ctx_cond, 0, sizeof(pthread_cond_t));

	ret = pthread_mutex_init(&trans->cm_lock, NULL);
	if (ret) {
		INFO_LOG(listening_trans->debug & MSK_DEBUG_EVENT, "pthread_mutex_init failed: %s (%d)", strerror(ret), ret);
		msk_destroy_trans(&trans);
		return NULL;
	}
	ret = pthread_cond_init(&trans->cm_cond, NULL);
	if (ret) {
		INFO_LOG(listening_trans->debug & MSK_DEBUG_EVENT, "pthread_cond_init failed: %s (%d)", strerror(ret), ret);
		msk_destroy_trans(&trans);
		return NULL;
	}
	ret = pthread_mutex_init(&trans->ctx_lock, NULL);
	if (ret) {
		INFO_LOG(listening_trans->debug & MSK_DEBUG_EVENT, "pthread_mutex_init failed: %s (%d)", strerror(ret), ret);
		msk_destroy_trans(&trans);
		return NULL;
	}
	ret = pthread_cond_init(&trans->ctx_cond, NULL);
	if (ret) {
		INFO_LOG(listening_trans->debug & MSK_DEBUG_EVENT, "pthread_cond_init failed: %s (%d)", strerror(ret), ret);
		msk_destroy_trans(&trans);
		return NULL;
	}

	pthread_mutex_lock(&internals->lock);
	internals->run_threads++;
	pthread_mutex_unlock(&internals->lock);

	return trans;
}

/**
 * msk_finalize_accept: does the real connection acceptance and wait for other side to be ready
 *
 * @param trans [IN]
 *
 * @return 0 on success, the value of errno on error
 */
int msk_finalize_accept(msk_trans_t *trans) {
	struct rdma_conn_param conn_param;
	int ret;

	if (!trans || trans->state != MSK_CONNECT_REQUEST) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "trans isn't from a connection request?");
		return EINVAL;
	}

	memset(&conn_param, 0, sizeof(struct rdma_conn_param));
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.private_data = NULL;
	conn_param.private_data_len = 0;
	conn_param.rnr_retry_count = 10;

	pthread_mutex_lock(&trans->cm_lock);
	do {
		ret = rdma_accept(trans->cm_id, &conn_param);
		if (ret) {
			ret = errno;
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_accept failed: %s (%d)", strerror(ret), ret);
			break;
		}
		while (trans->state == MSK_CONNECT_REQUEST) {
			pthread_cond_wait(&trans->cm_cond, &trans->cm_lock);
			INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "Got a cond, state: %i", trans->state);
		}

		if (trans->state == MSK_CONNECTED) {
			msk_cq_addfd(trans);
			msk_stats_add(trans);
		} else {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Accept failed");
			ret = ECONNRESET;
			break;
		}
	} while (0);

	pthread_mutex_unlock(&trans->cm_lock);

	return ret;
}

/**
 * msk_accept_one: given a listening trans, waits till one connection is requested and accepts it
 *
 * @param trans [IN] the parent trans
 *
 * @return a new trans for the child on success, NULL on failure
 */
msk_trans_t *msk_accept_one_timedwait(msk_trans_t *trans, struct timespec *abstime) { //TODO make it return an int an' use trans as argument

	//TODO: timeout?

	struct rdma_cm_id *cm_id = NULL;
	msk_trans_t *child_trans = NULL;
	int i, ret;

	if (!trans || trans->state != MSK_LISTENING) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "trans isn't listening (after bind_server)?");
		return NULL;
	}

	pthread_mutex_lock(&trans->cm_lock);
	ret = 0;
	while (!cm_id && ret == 0) {
		/* See if one of the slots has been taken */
		for (i = 0; i < trans->server; i++)
			if (trans->conn_requests[i])
				break;

		if (i == trans->server) {
			INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "Waiting for a connection to come in");
			if (abstime)
				ret = pthread_cond_timedwait(&trans->cm_cond, &trans->cm_lock, abstime);
			else
				ret = pthread_cond_wait(&trans->cm_cond, &trans->cm_lock);
		} else {
			cm_id = trans->conn_requests[i];
			trans->conn_requests[i] = NULL;
		}
	}
	pthread_mutex_unlock(&trans->cm_lock);

	if (ret == ETIMEDOUT) {
		return NULL;
	}
	if (ret) {
		return NULL;
	}

	INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "Got a connection request - creating child");

	child_trans = clone_trans(trans, cm_id);

	if (child_trans) {
		if ((ret = msk_setup_qp(child_trans))) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Could not setup child trans's qp: %s (%d)", strerror(ret), ret);
			msk_destroy_trans(&child_trans);
			return NULL;
		}
		if ((ret = msk_setup_buffer(child_trans))) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Could not setup child trans's buffer: %s (%d)", strerror(ret), ret);
			msk_destroy_trans(&child_trans);
			return NULL;
		}
	}
	return child_trans;
}

msk_trans_t *msk_accept_one_wait(msk_trans_t *trans, int msleep) {
	struct timespec ts;

	if (msleep == 0)
		return msk_accept_one(trans);

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += msleep / 1000;
	ts.tv_nsec += (msleep % 1000) * 1000000;
	if (ts.tv_nsec >= 1000000000) {
		ts.tv_nsec -= 1000000000;
		ts.tv_sec++;
	}

	return msk_accept_one_timedwait(trans, &ts);
}

/**
 * msk_bind_client: resolve addr and route for the client and waits till it's done
 * (the route and pthread_cond_signal is done in the cm thread)
 *
 */
static int msk_bind_client(msk_trans_t *trans) {
	struct rdma_addrinfo hints, *res;
	int ret;

	pthread_mutex_lock(&trans->cm_lock);

	do {
		memset(&hints, 0, sizeof(struct rdma_addrinfo));
		hints.ai_port_space = trans->conn_type;

		ret = rdma_getaddrinfo(trans->node, trans->port, &hints, &res);
		if (ret) {
			ret = errno;
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_getaddrinfo: %s (%d)", strerror(ret), ret);
			break;
		}

		ret = rdma_resolve_addr(trans->cm_id, res->ai_src_addr, res->ai_dst_addr, trans->timeout);
		if (ret) {
			ret = errno;
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_resolve_addr failed: %s (%d)", strerror(ret), ret);
			break;
		}
		rdma_freeaddrinfo(res);


		while (trans->state == MSK_INIT) {
			pthread_cond_wait(&trans->cm_cond, &trans->cm_lock);
			INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "Got a cond, state: %i", trans->state);
		}
		if (trans->state != MSK_ADDR_RESOLVED) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Could not resolve addr");
			ret = EINVAL;
			break;
		}

		ret = rdma_resolve_route(trans->cm_id, trans->timeout);
		if (ret) {
			trans->state = MSK_ERROR;
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_resolve_route failed: %s (%d)", strerror(ret), ret);
			break;
		}

		while (trans->state == MSK_ADDR_RESOLVED) {
			pthread_cond_wait(&trans->cm_cond, &trans->cm_lock);
			INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "Got a cond, state: %i", trans->state);
		}

		if (trans->state != MSK_ROUTE_RESOLVED) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Could not resolve route");
			ret = EINVAL;
			break;
		}
	} while (0);

	pthread_mutex_unlock(&trans->cm_lock);

	return ret;
}

/**
 * msk_finalize_connect: tells the other side we're ready to receive stuff (does the actual rdma_connect) and waits for its ack
 *
 * @param trans [IN]
 *
 * @return 0 on success, errno value on failure
 */
int msk_finalize_connect(msk_trans_t *trans) {
	struct rdma_conn_param conn_param;
	int ret;

	if (!trans || trans->state != MSK_ROUTE_RESOLVED) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "trans isn't half-connected?");
		return EINVAL;
	}


	memset(&conn_param, 0, sizeof(struct rdma_conn_param));
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.rnr_retry_count = 10;
	conn_param.retry_count = 10;

	pthread_mutex_lock(&trans->cm_lock);

	do {
		ret = rdma_connect(trans->cm_id, &conn_param);
		if (ret) {
			ret = errno;
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_connect failed: %s (%d)", strerror(ret), ret);
			break;
		}

		while (trans->state == MSK_ROUTE_RESOLVED) {
			pthread_cond_wait(&trans->cm_cond, &trans->cm_lock);
			INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "Got a cond, state: %i", trans->state);
		}

		if (trans->state == MSK_CONNECTED) {
			msk_cq_addfd(trans);
			msk_stats_add(trans);
		} else {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Connection failed");
			ret = ECONNREFUSED;
			break;
		}
	} while (0);

	pthread_mutex_unlock(&trans->cm_lock);

	return ret;
}

/**
 * msk_connect: connects a client to a server
 *
 * @param trans [INOUT] trans must be init first
 *
 * @return 0 on success, the value of errno on error 
 */
int msk_connect(msk_trans_t *trans) {
	int ret;

	if (!trans || trans->state != MSK_INIT) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "trans must be initialized first!");
		return EINVAL;
	}

	if (trans->server) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Must be on client side to call this function");
		return EINVAL;
	}

	if ((ret = msk_check_create_epoll_thread(&internals->cm_thread, msk_cm_thread, trans, &internals->cm_epollfd))) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "msk_check_create_epoll_thread failed: %s (%d)", strerror(ret), ret);
		return ret;
	}
	msk_cm_addfd(trans);

	if ((ret = msk_bind_client(trans)))
		return ret;
	if ((ret = msk_setup_qp(trans)))
		return ret;
	if ((ret = msk_setup_buffer(trans)))
		return ret;

	return 0;
}



/**
 * msk_post_n_recv: Post a receive buffer.
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param trans        [IN]
 * @param data         [OUT] the data buffer to be filled with received data
 * @param num_sge      [IN]  the number of elements in data to register
 * @param callback     [IN]  function that'll be called when done
 * @param err_callback [IN]  function that'll be called on error
 * @param callback_arg [IN]  argument to give to the callback
 *
 * @return 0 on success, the value of errno on error
 */
int msk_post_n_recv(msk_trans_t *trans, msk_data_t *data, int num_sge, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	INFO_LOG(trans->debug & MSK_DEBUG_RECV, "posting recv");
	msk_ctx_t *rctx;
	int i, ret;

	if (!trans || (trans->state != MSK_CONNECTED && trans->state != MSK_ROUTE_RESOLVED && trans->state != MSK_CONNECT_REQUEST)) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "trans (%p) isn't connected?", trans);
		if (trans)
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "trans state: %d", trans->state);
		return EINVAL;
	}


	pthread_mutex_lock(&trans->ctx_lock);

	do {
		for (i = 0, rctx = trans->recv_buf;
		     i < trans->qp_attr.cap.max_recv_wr;
		     i++, rctx = (msk_ctx_t*)((uint8_t*)rctx + sizeof(msk_ctx_t) + trans->qp_attr.cap.max_recv_sge*sizeof(struct ibv_sge)))
			if (!rctx->used != MSK_CTX_FREE)
				break;

		if (i == trans->qp_attr.cap.max_recv_wr) {
			INFO_LOG(trans->debug & MSK_DEBUG_RECV, "Waiting for cond");
			pthread_cond_wait(&trans->ctx_cond, &trans->ctx_lock);
		}

	} while ( i == trans->qp_attr.cap.max_recv_wr );
	INFO_LOG(trans->debug & MSK_DEBUG_RECV, "got a free context");
	rctx->used = MSK_CTX_PENDING;

	pthread_mutex_unlock(&trans->ctx_lock);

	rctx->pos = 0;
	rctx->next = NULL;
	rctx->callback = callback;
	rctx->err_callback = err_callback;
	rctx->callback_arg = callback_arg;
	rctx->data = data;

	for (i=0; i < num_sge; i++) {
		if (!data || !data->mr) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "You said to recv %d elements (num_sge), but we only found %d! Not requesting.", num_sge, i);
			return EINVAL;
		} 
		rctx->sg_list[i].addr = (uintptr_t) data->data;
		INFO_LOG(trans->debug & MSK_DEBUG_RECV, "addr: %lx\n", rctx->sg_list->addr);
		rctx->sg_list[i].length = data->max_size;
		rctx->sg_list[i].lkey = data->mr->lkey;
		if (i == num_sge-1)
			data->next = NULL;
		else
			data = data->next;
	}

	rctx->wr.rwr.next = NULL;
	rctx->wr.rwr.wr_id = (uint64_t)rctx;
	rctx->wr.rwr.sg_list = rctx->sg_list;
	rctx->wr.rwr.num_sge = num_sge;

	ret = ibv_post_recv(trans->qp, &rctx->wr.rwr, &trans->bad_recv_wr);
	if (ret) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_post_recv failed: %s (%d)", strerror(ret), ret);
		return ret; // FIXME np_uerror(ret)
	}

	return 0;
}

static int msk_post_send_generic(msk_trans_t *trans, enum ibv_wr_opcode opcode, msk_data_t *data, int num_sge, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	INFO_LOG(trans->debug & MSK_DEBUG_SEND, "posting a send with op %d", opcode);
	msk_ctx_t *wctx;
	int i, ret;
	uint32_t totalsize = 0;

	if (!trans || trans->state != MSK_CONNECTED) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "trans (%p) isn't connected?", trans);
		if (trans)
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "trans state: %d", trans->state);
		return EINVAL;
	}

	// opcode-specific checks:
	if (opcode == IBV_WR_RDMA_WRITE || opcode == IBV_WR_RDMA_READ) {
		if (!rloc) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Cannot do rdma without a remote location!");
			return EINVAL;
		}
	} else if (opcode == IBV_WR_SEND || opcode == IBV_WR_SEND_WITH_IMM) {
	} else {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "unsupported op code: %d", opcode);
		return EINVAL;
	}


	pthread_mutex_lock(&trans->ctx_lock);

	do {
		for (i = 0, wctx = (msk_ctx_t *)trans->send_buf;
		     i < trans->qp_attr.cap.max_send_wr;
		     i++, wctx = (msk_ctx_t*)((uint8_t*)wctx + sizeof(msk_ctx_t) + trans->qp_attr.cap.max_send_sge*sizeof(struct ibv_sge)))
			if (!wctx->used != MSK_CTX_FREE)
				break;

		if (i == trans->qp_attr.cap.max_send_wr) {
			INFO_LOG(trans->debug & MSK_DEBUG_SEND, "waiting for cond");
			pthread_cond_wait(&trans->ctx_cond, &trans->ctx_lock);
		}

	} while ( i == trans->qp_attr.cap.max_send_wr );
	INFO_LOG(trans->debug & MSK_DEBUG_SEND, "got a free context");
	wctx->used = MSK_CTX_PENDING;

	pthread_mutex_unlock(&trans->ctx_lock);

	wctx->pos = 0;
	wctx->next = NULL;
	wctx->callback = callback;
	wctx->err_callback = err_callback;
	wctx->callback_arg = callback_arg;
	wctx->data = data;

	for (i=0; i < num_sge; i++) {
		if (!data || !data->mr) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "You said to send %d elements (num_sge), but we only found %d! Not sending.", num_sge, i);
			// or send up to previous one? It's probably an error though...
			return EINVAL;
		} 
		if (data->size == 0) {
			num_sge = i; // only send up to previous sg, do we want to warn about this?
			break;
		}

		wctx->sg_list[i].addr = (uintptr_t)data->data;
		INFO_LOG(trans->debug & MSK_DEBUG_SEND, "addr: %lx\n", wctx->sg_list[i].addr);
		wctx->sg_list[i].length = data->size;
		wctx->sg_list[i].lkey = data->mr->lkey;
		totalsize += data->size;

		if (i == num_sge-1)
			data->next = NULL;
		else
			data = data->next;
	}

	if (rloc && totalsize > rloc->size) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "trying to send or read a buffer bigger than the remote buffer (shall we truncate?)");
		return EMSGSIZE;
	}

	wctx->wr.wwr.next = NULL;
	wctx->wr.wwr.wr_id = (uint64_t)wctx;
	wctx->wr.wwr.opcode = opcode;
//FIXME	wctx->wr.wwr.imm_data = htonl(data->imm_data);
	wctx->wr.wwr.send_flags = IBV_SEND_SIGNALED;
	wctx->wr.wwr.sg_list = wctx->sg_list;
	wctx->wr.wwr.num_sge = num_sge;
	if (rloc) {
		wctx->wr.wwr.wr.rdma.rkey = rloc->rkey;
		wctx->wr.wwr.wr.rdma.remote_addr = rloc->raddr;
	}

	ret = ibv_post_send(trans->qp, &wctx->wr.wwr, &trans->bad_send_wr);
	if (ret) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_post_send failed: %s (%d)", strerror(ret), ret);
		return ret; // FIXME np_uerror(ret)
	}

	return 0;
}

/**
 * Post a send buffer.
 *
 * @param trans        [IN]
 * @param data         [IN] the data buffer to be sent
 * @param num_sge      [IN] the number of elements in data to send
 * @param callback     [IN] function that'll be called when done
 * @param err_callback [IN] function that'll be called on error
 * @param callback_arg [IN] argument to give to the callback
 *
 * @return 0 on success, the value of errno on error
 */
int msk_post_n_send(msk_trans_t *trans, msk_data_t *data, int num_sge, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	return msk_post_send_generic(trans, IBV_WR_SEND, data, num_sge, NULL, callback, err_callback, callback_arg);
}

/**
 * msk_wait_callback: send/recv callback that just unlocks a mutex.
 *
 */
static void msk_wait_callback(msk_trans_t *trans, msk_data_t *data, void *arg) {
	pthread_mutex_t *lock = arg;
	pthread_mutex_unlock(lock);
}

/**
 * Post a receive buffer and waits for _that one and not any other_ to be filled.
 * Generally a bad idea to use that one unless only that one is used.
 *
 * @param trans   [IN]
 * @param data    [OUT] the data buffer to be filled with the received data
 * @param num_sge [IN]  the number of elements in data to register
 *
 * @return 0 on success, the value of errno on error
 */
int msk_wait_n_recv(msk_trans_t *trans, msk_data_t *data, int num_sge) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	pthread_mutex_lock(&lock);
	ret = msk_post_n_recv(trans, data, num_sge, msk_wait_callback, msk_wait_callback, &lock);

	if (!ret) {
		pthread_mutex_lock(&lock);
		pthread_mutex_unlock(&lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}

/**
 * Post a send buffer and waits for that one to be completely sent
 * @param trans   [IN]
 * @param data    [IN] the data to send
 * @param num_sge [IN] the number of elements in data to send
 *
 * @return 0 on success, the value of errno on error
 */
int msk_wait_n_send(msk_trans_t *trans, msk_data_t *data, int num_sge) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	pthread_mutex_lock(&lock);
	ret = msk_post_n_send(trans, data, num_sge, msk_wait_callback, msk_wait_callback, &lock);

	if (!ret) {
		pthread_mutex_lock(&lock);
		pthread_mutex_unlock(&lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}

// callbacks would all be run in a big send/recv_thread


// server specific:


int msk_post_n_read(msk_trans_t *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	return msk_post_send_generic(trans, IBV_WR_RDMA_READ, data, num_sge, rloc, callback, err_callback, callback_arg);
}

int msk_post_n_write(msk_trans_t *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	return msk_post_send_generic(trans, IBV_WR_RDMA_WRITE, data, num_sge, rloc, callback, err_callback, callback_arg);
}

int msk_wait_n_read(msk_trans_t *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	pthread_mutex_lock(&lock);
	ret = msk_post_n_read(trans, data, num_sge, rloc, msk_wait_callback, msk_wait_callback, &lock);

	if (!ret) {
		pthread_mutex_lock(&lock);
		pthread_mutex_unlock(&lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}


int msk_wait_n_write(msk_trans_t *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	pthread_mutex_lock(&lock);
	ret = msk_post_n_write(trans, data, num_sge, rloc, msk_wait_callback, msk_wait_callback, &lock);

	if (!ret) {
		pthread_mutex_lock(&lock);
		pthread_mutex_unlock(&lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}


struct sockaddr *msk_get_dst_addr(msk_trans_t *trans) {
	return rdma_get_peer_addr(trans->cm_id);
}

struct sockaddr *msk_get_src_addr(msk_trans_t *trans) {
	return rdma_get_local_addr(trans->cm_id);
}

uint16_t msk_get_src_port(msk_trans_t *trans) {
	return rdma_get_src_port(trans->cm_id);
}

uint16_t msk_get_dst_port(msk_trans_t *trans) {
	return rdma_get_dst_port(trans->cm_id);
}



