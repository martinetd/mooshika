/*
 *
 * Copyright CEA/DAM/DIF (2012)
 * contributor : Dominique Martinet  dominique.martinet.ocre@cea.fr //TODO: use a real mail?
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

typedef struct msk_trans msk_trans_t;
typedef struct msk_trans_attr msk_trans_attr_t;

typedef struct msk_ctx msk_ctx_t;

/**
 * \struct msk_data
 * data size and content to send/just received
 */
typedef struct msk_data {
	uint32_t max_size; /**< size of the data field */
	uint32_t size; /**< size of the data to actually send/read */
	uint8_t *data; /**< opaque data */
} msk_data_t;

typedef union sockaddr_union {
	struct sockaddr sa;
	struct sockaddr_in sa_in;
	struct sockaddr_in6 sa_int6;
	struct sockaddr_storage sa_stor;
} sockaddr_union_t;

typedef void (*disconnect_callback_t) (msk_trans_t *trans);

/**
 * \struct msk_trans
 * RDMA transport instance
 */
struct msk_trans {
	enum { // FIXME: make volatile?
		MSK_INIT,
		MSK_LISTENING,
		MSK_ADDR_RESOLVED,
		MSK_ROUTE_RESOLVED,
		MSK_CONNECT_REQUEST,
		MSK_CONNECTED,
		MSK_FLUSHING,
		MSK_CLOSING,
		MSK_CLOSED,
		MSK_ERROR
	} state;			/**< tracks the transport state machine for connection setup and tear down */
	struct rdma_cm_id *cm_id;	/**< The RDMA CM ID */
	struct rdma_event_channel *event_channel;
	pthread_t cm_thread;		/**< Thread id for connection manager */
	struct ibv_comp_channel *comp_channel;
	struct ibv_pd *pd;		/**< Protection Domain pointer */
	struct ibv_qp *qp;		/**< Queue Pair pointer */
	struct ibv_cq *cq;		/**< Completion Queue pointer */
	pthread_t cq_thread;		/**< Thread id for completion queue handler */
	disconnect_callback_t disconnect_callback;
	void *private_data;
	long timeout;			/**< Number of mSecs to wait for connection management events */
	int sq_depth;			/**< The depth of the Send Queue */
	int max_send_sge;		/**< Maximum number of s/g elements per send */
	int rq_depth;			/**< The depth of the Receive Queue. */
	int max_recv_sge;		/**< Maximum number of s/g elements per recv */
	sockaddr_union_t addr;		/**< The remote peer's address */
	int server;			/**< 0 if client, number of connections to accept on server */
	msk_ctx_t *send_buf;		/**< pointer to actual context data */
	msk_ctx_t *recv_buf;		/**< pointer to actual context data */
	pthread_mutex_t lock;		/**< lock for events */
	pthread_cond_t cond;		/**< cond for events */
	struct ibv_recv_wr *bad_recv_wr;
	struct ibv_send_wr *bad_send_wr;
};

struct msk_trans_attr {
	disconnect_callback_t disconnect_callback;
	int server;			/**< 0 if client, number of connections to accept on server */
	long timeout;			/**< Number of mSecs to wait for connection management events */
	int sq_depth;			/**< The depth of the Send Queue */
	int max_send_sge;		/**< Maximum number of s/g elements per send */
	int rq_depth;			/**< The depth of the Receive Queue. */
	int max_recv_sge;		/**< Maximum number of s/g elements per recv */
	sockaddr_union_t addr;		/**< The remote peer's address */
	struct ibv_pd *pd;		/**< Protection Domain pointer */
};


typedef void (*ctx_callback_t)(msk_trans_t *trans, void *arg);


/**
 * \struct msk_rloc
 * stores one remote address to write/read at
 */
typedef struct msk_rloc {
	uint64_t raddr; /**< remote memory address */
	uint32_t rkey; /**< remote key */
	uint32_t size; /**< size of the region we can write/read */
} msk_rloc_t;


int msk_post_recv(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr, ctx_callback_t callback, void *callback_arg);
int msk_post_send(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr, ctx_callback_t callback, void *callback_arg);


int msk_wait_recv(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr);
int msk_wait_send(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr);

int msk_post_read(msk_trans_t *trans, msk_data_t *data, struct ibv_mr *mr, msk_rloc_t *rloc, ctx_callback_t callback, void* callback_arg);
int msk_post_write(msk_trans_t *trans, msk_data_t *data, struct ibv_mr *mr, msk_rloc_t *rloc, ctx_callback_t callback, void* callback_arg);
int msk_wait_read(msk_trans_t *trans, msk_data_t *data, struct ibv_mr *mr, msk_rloc_t *rloc);
int msk_wait_write(msk_trans_t *trans, msk_data_t *data, struct ibv_mr *mr, msk_rloc_t *rloc);

/*
// client side
int msk_write_request(trans, msk_rloc, size); // = ask for rdma_write server side ~= rdma_read
int msk_read_request(trans, msk_rloc, size); // = ask for rdma_read server side ~= rdma_write
*/


struct ibv_mr *msk_reg_mr(msk_trans_t *trans, void *memaddr, size_t size, int access);
int msk_dereg_mr(struct ibv_mr *mr);

msk_rloc_t *msk_make_rloc(struct ibv_mr *mr, uint64_t addr, uint32_t size);

void msk_print_devinfo(msk_trans_t *trans);


int msk_init(msk_trans_t **trans, msk_trans_attr_t *attr);

// server specific:
int msk_bind_server(msk_trans_t *trans);
msk_trans_t *msk_accept_one(msk_trans_t *trans);
int msk_start_cm_thread(msk_trans_t *trans);
int msk_finalize_accept(msk_trans_t *trans);
void msk_destroy_trans(msk_trans_t **ptrans);

int msk_connect(msk_trans_t *trans);
int msk_finalize_connect(msk_trans_t *trans);

#endif /* _MOOSHIKA_H */
