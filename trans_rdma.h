/*========================================
QUESTIONS:
 - log mecanism
	-> nfs-ganesha/src/include/log.h, add a "RDMA" component n' LogInfo(component, etc), LogWarn, LogError...
		log_level_t? easy to use out of nfs-ganesha? Might want just to ceate dummy macroes for now
 - callbacks
 - ok to use pthread, or does nfs-ganesha have any specific ones? (same with semaphores/lock, pthread_cond_*?) 
	-> looks ok with pthread_conf or include/SemN.h
 - queue implem to use, make my own? easy way out = like diod = a fixed length array of bufers with a "used" flag
==========================================
not a question, but 9p only uses recv/send and never _ever_ does any read/write in its current implementation...
========================================*/

// public api;

typedef struct rdma_data rdma_data;
typedef struct rdma_trans rdma_trans;
typedef struct rdma_ctx rdma_ctx;
typedef struct rdma_rloc rdma_rloc;

/**
 * \struct rdma_data
 * data size and content to send/just received
 */
struct rdma_data {
	uint32_t size; /**< size of the data field */
	uint8_t* data; /**< opaque data */
}; // for 9p, the data would be npfcall which also contains size, but we can't really rely on that...

/**
 * \struct rdma_trans
 * RDMA transport instance
 */
struct rdma_trans {
	enum {
		RCAT_RDMA_INIT,
		RCAT_RDMA_ADDR_RESOLVED,
		RCAT_RDMA_ROUTE_RESOLVED,
		RCAT_RDMA_CONNECTED,
		RCAT_RDMA_FLUSHING,
		RCAT_RDMA_CLOSING,
		RCAT_RDMA_CLOSED,
	} state;			/**< tracks the transport state machine for connection setup and tear down */
	struct rdma_cm_id* cm_id;	/**< The RDMA CM ID */
	struct ib_pd* pd;		/**< Protection Domain pointer */
	struct ib_qp* qp;		/**< Queue Pair pointer */
	struct ib_cq* cq;		/**< Completion Queue pointer */
	struct ib_mr* dma_mr;		/**< DMA Memory Region pointer */
	uint32_t lkey;			/**< The local access only memory region key */
	long timeout;			/**< Number of uSecs to wait for connection management events */
	int sq_depth;			/**< The depth of the Send Queue */
	struct semaphore sq_sem;	/**< Semaphore for the SQ */
	int rq_depth;			/**< The depth of the Receive Queue. */
	atomic_t rq_count;		/**< Count of requests in the Receive Queue. */
	struct sockaddr_in addr;	/**< The remote peer's address */
	spinlock_t req_lock;		/**< Protects the active request list */

	struct completion cm_done;	/**< Completion event for connection management tracking */

};


/**
 * \struct rdma_ctx
 * Context data we can use during recv/send callbacks
 */
struct rdma_ctx {
	int used;			/**< 0 if we can use it for a new recv/send */
	enum ibv_wc_opcode wc_op;	/**< IBV_WC_SEND or IBV_WC_RECV */
	struct rdma_trans* rdma;	/**< the main rdma_trans, actually not used... (copied from diod) */
	uint32_t pos;			/**< current position inside our own buffer. 0 <= pos <= len */
	uint32_t len;			/**< size of our own buffer */
        struct rdmactx* next;		/**< next context */
	uint8_t* buf;			/**< data starts here. */
};



/**
 * \struct rdma_rloc
 * stores one remote address to write/read at
 */
struct rdma_rloc {
	uint64_t addr; /**< remote address */
	uint32_t rkey; /**< remote key */
	uint32_t size; /**< size of the region we can write/read */
}


/**
 * Post a receive buffer.
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param trans the trans thing! duh, obviously.
 * @param msize max size we can receive
 * @param callback function that'll be called with the received data
 */
int rdma_recv(rdmatrans* trans, u32 msize, void (*callback)(rdma_data* data)); // or give trans a recv function an' do trans->recv

/**
 * Post a send buffer.
 * Same deal
 * 
 */
int rdma_send(rdmatrans* trans, rdma_data* data, void (*callback)(void));

/**
 * Post a receive buffer and waits for _that one and not any other_ to be filled.
 * bad idea. do we want that one? Or place it on top of the queue? But sucks with asynchronism really
 */
int rdma_recv_wait(rdmatrans* trans, rdma_data** datap, u32 msize);

/**
 * Post a send buffer and waits for that one to be completely sent
 * @param trans
 * @param data the size + opaque data.
 */
int rdma_send_wait(rdmatrans* trans, rdma_data* data);

// callbacks would all be run in a big send/recv_thread
// OR no callback, but two recv/send_thread that just waits for semaphore an' are left to the client.
// (or spawn callbacks as a separate thread, but that's overkill)


// server specific:
/**
 * 
 */
int rdma_write(trans, rdma_rloc, size);
int rdma_read(trans, rdma_rloc, size);

// client specific:
int rdma_write_request(trans, rdma_rloc, size); // = ask for rdma_write server side ~= rdma_read
int rdma_read_request(trans, rdma_rloc, size); // = ask for rdma_read server side ~= rdma_write







