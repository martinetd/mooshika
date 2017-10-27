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
 * \file	mooshika.h
 * \brief	rdma helper include file
 *
 * This is (very) loosely based on a mix of diod, rping (librdmacm/examples)
 * and kernel's net/9p/trans_rdma.c
 *
 */

#ifndef _MOOSHIKA_H
#define _MOOSHIKA_H

#include <rdma/rdma_cma.h>

#define MOOSHIKA_API_VERSION 5

typedef struct msk_trans msk_trans_t;
typedef struct msk_trans_attr msk_trans_attr_t;

/**
 * \struct msk_data
 * data size and content to send/just received
 */
typedef struct msk_data {
	uint32_t max_size; /**< size of the data field */
	uint32_t size; /**< size of the data to actually send/read */
	uint8_t *data; /**< opaque data */
	struct msk_data *next; /**< For recv/sends with multiple elements, used as a linked list */
	struct ibv_mr *mr;
} msk_data_t;

typedef union sockaddr_union {
	struct sockaddr sa;
	struct sockaddr_in sa_in;
	struct sockaddr_in6 sa_int6;
	struct sockaddr_storage sa_stor;
} sockaddr_union_t;

struct msk_stats {
	uint64_t rx_bytes;
	uint64_t rx_pkt;
	uint64_t rx_err;
	uint64_t tx_bytes;
	uint64_t tx_pkt;
	uint64_t tx_err;
	/* times only set if debug has MSK_DEBUG_SPEED */
	uint64_t nsec_callback;
	uint64_t nsec_compevent;
};

struct msk_pd {
	struct ibv_context *context;
	struct ibv_pd *pd;
	struct ibv_srq *srq;
	struct msk_ctx *rctx;
	void *private;
	uint32_t refcnt;
	uint32_t used;
};
#define PD_GUARD ((void*)-1)

typedef void (*disconnect_callback_t) (msk_trans_t *trans);

#define MSK_CLIENT 0
#define MSK_SERVER_CHILD -1

/**
 * \struct msk_trans
 * RDMA transport instance
 */
struct msk_trans {
	enum msk_state {
		MSK_INIT,
		MSK_LISTENING,
		MSK_ADDR_RESOLVED,
		MSK_ROUTE_RESOLVED,
		MSK_CONNECT_REQUEST,
		MSK_CONNECTED,
		MSK_CLOSING,
		MSK_CLOSED,
		MSK_ERROR
	} state;			/**< tracks the transport state machine for connection setup and tear down */
	struct rdma_cm_id *cm_id;	/**< The RDMA CM ID */
	struct rdma_event_channel *event_channel;
	struct ibv_comp_channel *comp_channel;
	struct msk_pd *pd;		/**< Protection Domain pointer list */
	struct ibv_qp *qp;		/**< Queue Pair pointer */
	struct ibv_srq *srq;		/**< Shared Receive Queue pointer */
	struct ibv_cq *cq;		/**< Completion Queue pointer */
	disconnect_callback_t disconnect_callback;
	void *private_data;
	long timeout;			/**< Number of mSecs to wait for connection management events */
	int sq_depth;			/**< The depth of the Send Queue */
	int max_send_sge;		/**< Maximum number of s/g elements per send */
	int rq_depth;			/**< The depth of the Receive Queue. */
	int max_recv_sge;		/**< Maximum number of s/g elements per recv */
	char *node;			/**< The remote peer's hostname */
	char *port;			/**< The service port (or name) */
	int conn_type;			/**< RDMA Port space, probably RDMA_PS_TCP */
	int server;			/**< 0 if client, connection backlog on server, -1 (MSK_SERVER_CHILD) if server's accepted connection */
	int destroy_on_disconnect;      /**< set to 1 if mooshika should perform cleanup */
	int privport;			/**< set to 1 if mooshika should use a reserved port for client side */
	uint32_t debug;
	struct rdma_cm_id **conn_requests; /**< temporary child cm_id, only used for server */
	struct msk_ctx *wctx;		/**< pointer to actual context data */
	struct msk_ctx *rctx;		/**< pointer to actual context data */
	pthread_mutex_t cm_lock;	/**< lock for connection events */
	pthread_cond_t cm_cond;		/**< cond for connection events */
	struct ibv_recv_wr *bad_recv_wr;
	struct ibv_send_wr *bad_send_wr;
	struct msk_stats stats;
	char *stats_prefix;
	int stats_sock;
};

struct msk_trans_attr {
	disconnect_callback_t disconnect_callback;
	int debug;			/**< verbose output to stderr if set */
	int server;			/**< 0 if client, connection backlog on server */
	int destroy_on_disconnect;      /**< set to 1 if mooshika should perform cleanup */
	int privport;			/**< set to 1 if mooshika should use a reserved port for client side */
	long timeout;			/**< Number of mSecs to wait for connection management events */
	int sq_depth;			/**< The depth of the Send Queue */
	int max_send_sge;		/**< Maximum number of s/g elements per send */
	int use_srq;			/**< Does the server use srq? */
	int rq_depth;			/**< The depth of the Receive Queue. */
	int max_recv_sge;		/**< Maximum number of s/g elements per recv */
	int worker_count;		/**< Number of worker threads - works only for the first init */
	int worker_queue_size;		/**< Size of the worker data queue - works only for the first init */
	enum rdma_port_space conn_type;	/**< RDMA Port space, probably RDMA_PS_TCP */
	char *node;			/**< The remote peer's hostname */
	char *port;			/**< The service port (or name) */
	struct msk_pd *pd;		/**< Protection Domain pointer */
	char *stats_prefix;
};

#define MSK_DEBUG_EVENT 0x0001
#define MSK_DEBUG_SETUP 0x0002
#define MSK_DEBUG_SEND  0x0004
#define MSK_DEBUG_RECV  0x0008
#define MSK_DEBUG_WORKERS (MSK_DEBUG_SEND | MSK_DEBUG_RECV)
#define MSK_DEBUG_CM_LOCKS   0x0010
#define MSK_DEBUG_CTX   0x0020
#define MSK_DEBUG_SPEED 0x8000


typedef void (*ctx_callback_t)(msk_trans_t *trans, msk_data_t *data, void *arg);


/**
 * \struct msk_rloc
 * stores one remote address to write/read at
 */
typedef struct msk_rloc {
	uint64_t raddr; /**< remote memory address */
	uint32_t rkey; /**< remote key */
	uint32_t size; /**< size of the region we can write/read */
} msk_rloc_t;



int msk_post_n_recv(msk_trans_t *trans, msk_data_t *data, int num_sge, ctx_callback_t callback, ctx_callback_t err_callback, void *callback_arg);
int msk_post_n_send(msk_trans_t *trans, msk_data_t *data, int num_sge, ctx_callback_t callback, ctx_callback_t err_callback, void *callback_arg);
int msk_wait_n_recv(msk_trans_t *trans, msk_data_t *data, int num_sge);
int msk_wait_n_send(msk_trans_t *trans, msk_data_t *data, int num_sge);
int msk_post_n_read(msk_trans_t *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg);
int msk_post_n_write(msk_trans_t *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg);
int msk_wait_n_read(msk_trans_t *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc);
int msk_wait_n_write(msk_trans_t *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc);

static inline int msk_post_recv(msk_trans_t *trans, msk_data_t *data, ctx_callback_t callback, ctx_callback_t err_callback, void *callback_arg) {
	return msk_post_n_recv(trans, data, 1, callback, err_callback, callback_arg);
}
static inline int msk_post_send(msk_trans_t *trans, msk_data_t *data, ctx_callback_t callback, ctx_callback_t err_callback, void *callback_arg) {
	return msk_post_n_send(trans, data, 1, callback, err_callback, callback_arg);
}

static inline int msk_wait_recv(msk_trans_t *trans, msk_data_t *data) {
	return msk_wait_n_recv(trans, data, 1);
}

static inline int msk_wait_send(msk_trans_t *trans, msk_data_t *data) {
	return msk_wait_n_send(trans, data, 1);
}

static inline int msk_post_read(msk_trans_t *trans, msk_data_t *data, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	return msk_post_n_read(trans, data, 1, rloc, callback, err_callback, callback_arg);
}

static inline int msk_post_write(msk_trans_t *trans, msk_data_t *data, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	return msk_post_n_write(trans, data, 1, rloc, callback, err_callback, callback_arg);
}

static inline int msk_wait_read(msk_trans_t *trans, msk_data_t *data, msk_rloc_t *rloc) {
	return msk_wait_n_read(trans, data, 1, rloc);
}

static inline int msk_wait_write(msk_trans_t *trans, msk_data_t *data, msk_rloc_t *rloc) {
	return msk_wait_n_write(trans, data, 1, rloc);
}



int msk_init(msk_trans_t **ptrans, msk_trans_attr_t *attr);

// server specific:
int msk_bind_server(msk_trans_t *trans);
msk_trans_t *msk_accept_one_wait(msk_trans_t *trans, int msleep);
msk_trans_t *msk_accept_one_timedwait(msk_trans_t *trans, struct timespec *abstime);
static inline msk_trans_t *msk_accept_one(msk_trans_t *trans) {
	return msk_accept_one_timedwait(trans, NULL);
}
int msk_finalize_accept(msk_trans_t *trans);
void msk_destroy_trans(msk_trans_t **ptrans);

int msk_connect(msk_trans_t *trans);
int msk_finalize_connect(msk_trans_t *trans);


/* utility functions */

struct ibv_mr *msk_reg_mr(msk_trans_t *trans, void *memaddr, size_t size, int access);
int msk_dereg_mr(struct ibv_mr *mr);

msk_rloc_t *msk_make_rloc(struct ibv_mr *mr, uint64_t addr, uint32_t size);

void msk_print_devinfo(msk_trans_t *trans);

struct sockaddr *msk_get_dst_addr(msk_trans_t *trans);
struct sockaddr *msk_get_src_addr(msk_trans_t *trans);
uint16_t msk_get_src_port(msk_trans_t *trans);
uint16_t msk_get_dst_port(msk_trans_t *trans);

struct msk_pd *msk_getpd(msk_trans_t *trans);


#endif /* _MOOSHIKA_H */
