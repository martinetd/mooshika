/*========================================
QUESTIONS:
 - log mecanism
 - callbacks
 - ok to use pthread, or does nfs-ganesha have any specific ones? (same with semaphores/lock, pthread_cond_*?)
==========================================
not a question, but 9p only uses recv/send and never _ever_ does any read/write in its current implementation...
========================================*/

// public api;

struct rdma_data {
	u32 size;
	void* data;
} rdma_data; // for 9p, the data would be npfcall which also contains size, but we can't really rely on that...

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
 * bad idea. do we want that one?
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


struct rdma_rloc {
	uint64_t addr;
	uint32_t rkey;
	uint32_t size;
}

// server specific:
/**
 * 
 */
int rdma_write(trans, rdma_rloc, size);
int rdma_read(trans, rdma_rloc, size);

// client specific:
int rdma_write_request(trans, rdma_rloc, size); // = ask for rdma_write server side ~= rdma_read
int rdma_read_request(trans, rdma_rloc, size); // = ask for rdma_read server side ~= rdma_write







