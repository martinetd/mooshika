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
 * \file    trans_rdma.h
 * \brief   rdma helper include file
 *
 * This is (very) loosely based on a mix of diod, rping (librdmacm/examples)
 * and kernel's net/9p/trans_rdma.c
 *
 */


/*========================================
QUESTIONS:
 - log mecanism
	-> nfs-ganesha/src/include/log.h, add a "RDMA" component n' LogInfo(component, etc), LogWarn, LogError...
		log_level_t? easy to use out of nfs-ganesha? Might want just to ceate dummy macroes for now
 - callbacks
 - ok to use pthread, or does nfs-ganesha have any specific ones? (same with semaphores/lock, pthread_cond_*?) 
	-> looks ok with pthread_cond or include/SemN.h
 - queue implem to use, make my own? easy way out = like diod = a fixed length array of bufers with a "used" flag
==========================================
not a question, but 9p only uses recv/send and never _ever_ does any read/write in its current implementation...
========================================*/

// public api;

typedef struct libercat_data libercat_data_t;
typedef struct libercat_trans libercat_trans_t;
typedef struct libercat_ctx libercat_ctx_t;
typedef struct libercat_rloc libercat_rloc_t;

/**
 * \struct libercat_data
 * data size and content to send/just received
 */
struct libercat_data {
	uint32_t size; /**< size of the data field */
	uint8_t *data; /**< opaque data */
}; // for 9p, the data would be npfcall which also contains size, but we can't really rely on that...

/**
 * \struct libercat_trans
 * RDMA transport instance
 */
struct libercat_trans {
	enum {
		LIBERCAT_INIT,
		LIBERCAT_ADDR_RESOLVED,
		LIBERCAT_ROUTE_RESOLVED,
		LIBERCAT_CONNECT_REQUEST,
		LIBERCAT_CONNECTED,
		LIBERCAT_FLUSHING,
		LIBERCAT_CLOSING,
		LIBERCAT_CLOSED,
		LIBERCAT_ERROR
	} state;			/**< tracks the transport state machine for connection setup and tear down */
	struct rdma_cm_id *cm_id;	/**< The RDMA CM ID */
	struct rdma_event_channel *event_channel;
	pthread_t cm_thread;		/**< Thread id for connection manager */
	struct ibv_comp_channel *comp_channel;
	struct ibv_pd *pd;		/**< Protection Domain pointer */
	struct ibv_qp *qp;		/**< Queue Pair pointer */
	struct ibv_cq *cq;		/**< Completion Queue pointer */
	pthread_t cq_thread;		/**< Thread id for completion queue handler */
	uint32_t lkey;			/**< The local access only memory region key */
	long timeout;			/**< Number of mSecs to wait for connection management events */
	int sq_depth;			/**< The depth of the Send Queue */
	struct ibv_mr *send_mr;		/**< send DMA Memory Region pointer */
	int rq_depth;			/**< The depth of the Receive Queue. */
	struct ibv_mr *recv_mr;		/**< receive DMA Memory Region pointer */
	struct sockaddr_storage addr;	/**< The remote peer's address */
	int server;
	int num_accept;
	size_t ctx_size;
	libercat_ctx_t *send_buf;		/**< pointer to actual context data */
	libercat_ctx_t *recv_buf;		/**< pointer to actual context data */
	pthread_mutex_t lock;		/**< lock for events */
	pthread_cond_t cond;		/**< cond for events */
	struct ibv_recv_wr *bad_recv_wr;
	struct ibv_send_wr *bad_send_wr;
//TODO: fill this, remember to init stuff.
};


typedef void (*ctx_callback_t)(libercat_trans_t *trans, void *arg);


/**
 * \struct libercat_ctx
 * Context data we can use during recv/send callbacks
 */
struct libercat_ctx {
	int used;			/**< 0 if we can use it for a new recv/send */
	enum ibv_wc_opcode wc_op;	/**< IBV_WC_SEND or IBV_WC_RECV */
	uint32_t pos;			/**< current position inside our own buffer. 0 <= pos <= len */
	uint32_t len;			/**< size of our own buffer */
	struct rdmactx *next;		/**< next context */
	libercat_data_t *data;
	ctx_callback_t callback;
	struct ibv_sge sge;
	union {
		struct ibv_recv_wr rwr;
		struct ibv_send_wr wwr;
	} wr;
	void *callback_arg;
};



/**
 * \struct libercat_rloc
 * stores one remote address to write/read at
 */
struct libercat_rloc {
	uint64_t rmemaddr; /**< remote address */
	uint32_t rkey; /**< remote key */
	uint32_t size; /**< size of the region we can write/read */
};


int libercat_post_recv(libercat_trans_t *trans, libercat_data_t **pdata, struct ibv_mr *mr, ctx_callback_t callback, void *callback_arg);
int libercat_post_send(libercat_trans_t *trans, libercat_data_t *data, struct ibv_mr *mr, ctx_callback_t callback, void *callback_arg);


int libercat_wait_recv(libercat_trans_t *trans, libercat_data_t **datap, struct ibv_mr *mr);
int libercat_wait_send(libercat_trans_t *trans, libercat_data_t *data, struct ibv_mr *mr);

/*// server side
int libercat_write(trans, libercat_rloc_t, size_t size);
int libercat_read(trans, libercat_rloc_t, size_t size);

// client side
int libercat_write_request(trans, libercat_rloc, size); // = ask for rdma_write server side ~= rdma_read
int libercat_read_request(trans, libercat_rloc, size); // = ask for rdma_read server side ~= rdma_write
*/


struct ibv_mr *libercat_reg_mr(libercat_trans_t *trans, void *memaddr, size_t size, int access);
int libercat_dereg_mr(struct ibv_mr *mr);

libercat_rloc_t *libercat_make_rkey(uint64_t addr, struct ibv_mr *mr, uint32_t size);


int libercat_init(libercat_trans_t **trans);

// server specific:
int libercat_bind_server(libercat_trans_t *trans);
int libercat_accept(libercat_trans_t *trans);
libercat_trans_t *libercat_accept_one(libercat_trans_t *trans);
void libercat_destroy_trans(libercat_trans_t *libercat_trans);
// do we want create/destroy + listen/shutdown, or can both be done in a single call?
// if second we could have create/destroy shared with client, but honestly there's not much to share...
// client
int libercat_connect(libercat_trans_t *trans);








