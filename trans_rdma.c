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
 * \file    trans_rdma.c
 * \brief   rdma helper
 *
 * This is (very) loosely based on a mix of diod, rping (librdmacm/examples)
 * and kernel's net/9p/trans_rdma.c
 *
 */


#include <inttypes.h>

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#include "log.h"
#include "trans_rdma.h"

/* UTILITY FUNCTIONS */

struct ibv_mr* register_mr(rdma_trans_t* trans, void* memaddr, size_t size, int access) {
	return ibv_reg_mr(trans->pd, memaddr, size, access);
}

int deregister_mr(struct ibv_mr* mr) {
	return ibv_dereg_mr(mr);
}

rdma_rloc_t* rdma_make_rkey(uint64_t addr, ibv_mr* mr, uint32_t size) {
	rdma_rloc_t* rkey;
	rkey = malloc(sizeof(rdma_rloc_t));
	if (!rkey) {
		DEBUG_LOG("Out of memory!");
		return NULL;
	}

	rkey->rmemaddr = addr;
	rkey->rkey = mr->rkey;
	rkey->size = size;

	return rkey;
}

/* INIT/SHUTDOWN FUNCTIONS */

/**
 * rdma_init_common: part of the init that's the same for client and server
 * 
 * @param trans [INOUT]
 *
 * @return 0 on success, errno value on failure
 */
static int rdma_init_common(rdma_trans_t* trans) {
	int ret;
	
	trans->event_channel = rdma_create_event_channel();
	if (!trans->event_channel) {
		ret = errno;
		DEBUG_LOG("create_event_channel failed: %d", ret);
		return ret;
	}

	ret = rdma_create_id(trans->event_channel, &trans->cm_id, trans, RDMA_PS_TCP);
	if (ret) {
		ret = errno;
		DEBUG_LOG("create_id failed: %d", ret);
		return ret;
	}
	
	trans->state = RDMA_INIT;
	trans->timeout = 30000; // 30s //TODO: find where to use that...
	trans->sq_depth = 10;
	ret = sem_init(&trans->sq_sem, 0, 0);
	if (ret) {
		ret = errno;
		DEBUG_LOG("sem_ini failed: %d", ret);
		return ret;
	}
	trans->rq_depth = 10;
	trans->rq_count = 0;

//	pthread_create(cmthread, NULL, cm_thread, trans?); == rdma_get_cm_event(cm_channel, &event) where struct rdma_cm_event* event + rdma_ack_cm_event(event) + switch on events
	return 0;
}

/**
 * rdma_bind_server
 * 
 * @param trans [INOUT]
 *
 * @return 0 on success, errno value on failure
 */
static int rdma_bind_server(rdma_trans_t* trans) {
	int ret;
	ret = rdma_bind_addr(trans->cm_id, (struct sockaddr*) &trans->sin);
	if (ret) {
		ret = errno;
		DEBUG_LOG("bind_addr failed with ret: %d", ret);
		return ret;
	}
	
	ret = rdma_listen(trans->cm_id, trans->num_accept);
	if (ret) {
		ret = errno;
		DEBUG_LOG("rdma_listen failed: %d", ret);
		return ret;
	}

	return 0;
}


/**
 * rdma_create: inits everything for server side.
 * 
 * @param addr [IN] contains the full address (i.e. both ip and port)
 */
rdma_trans_t* rdma_create(sockaddr_storage* addr) {  //TODO make it return an int an' use trans as argument. figure out who gotta malloc/free..
	rdma_trans_t* trans;

	trans = malloc(sizeof(rdma_trans_t));
	if (!trans) {
		DEBUG_LOG("Out of memory");
		return NULL;
	}

	if (rdma_init_common(trans)) {
		free(trans);
		return NULL;
	}

	trans->sin = addr;
	rdma_bind_server(trans);

	return trans;
}

static rdma_trans_t* clone_trans(rdma_trans_t* listening_trans) {
	rdma_trans_t* trans = malloc(sizeof(rdma_trans_t));
	if (!trans)
		return NULL;

	*trans = *listening_trans;
	trans->child_cm_id->trans = trans; //FIXME: define this...
	return trans;
}

rdma_trans_t* rdma_accept_one(rdma_trans_t* rdma_connection) { //TODO same as above...

	// FIXME: timeout?

	struct rdma_cm_event *event;
	struct rdma_cm_id *cmid;
	enum rdma_cm_event_type etype;
	int ret;

	ret = rdma_get_cm_event(trans->event_channel, &event);
	if (ret) {
		ret=errno;
		DEBUG_LOG("rdma_get_cm_event failed: %d", ret);
		return NULL;
	}

	trans = clone_trans(rdma_connection);
	rdma_setup_qp
	rdma_setup_buffers
	pthread_create(cq_thread);
	rdma_do_accept(trans)
}

/**
 * rdma_destroy_trans: does the final trans free()*/
int rdma_destroy_trans(rdma_trans_t* trans) {
	if (trans->cm_id)
		rdma_destroy_id(trans->cm_id);
	if (trans->cm_channel)
		rdma_destroy_event_channel(trans->cm_channel);
	free(trans);
}

// do we want create/destroy + listen/shutdown, or can both be done in a single call?
// if second we could have create/destroy shared with client, but honestly there's not much to share...
// client
rdma_trans_t* rdma_connect(sockaddr_storage* addr) {
	rdma_init_common();
	rdma_bind_client();
	rdma_setup_qp;
	rdma_setup_buffers;
	ibv_post_recv?
	pthread_create(cq_thread)
	rdmacat_connect_client()
}

/**
 * rdma_disconnect: disconnect an active connection.
 * Works both on client after rdma_connect and server after rdma_accept_one
 */
int rdma_disconnect(rdma_trans_t* trans) {
	rdma_disconnect
	pthread_join(cqthread, NULL)
	free_buffers
	free_qp
}



/**
 * rdma_recv: Post a receive buffer.
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param [IN] trans the trans thing! duh, obviously.
 * @param [IN] msize max size we can receive
 * @param [IN] callback function that'll be called with the received data
 *
 * @return 0 on success, the value of errno on error
 */
int rdma_recv(rdma_trans_t* trans, uint32_t msize, void (*callback)(rdma_trans_t* trans, rdma_data_t* data)) {
	return 0;
}

/**
 * Post a send buffer.
 * Same deal
 *
 */
int rdma_send(rdma_trans_t* trans, rdma_data* data, void (*callback)(rdma_trans_t* trans));
int rdma_send(rdma_trans_t* trans, rdma_data* data, ibv_mr* mr, void (*callback)(rdma_trans_t* trans, rdma_lloc* loc));

/**
 * Post a receive buffer and waits for _that one and not any other_ to be filled.
 * bad idea. do we want that one? Or place it on top of the queue? But sucks with asynchronism really
 */
int rdma_recv_wait(rdma_trans_t* trans, rdma_data** datap, uint32_t msize);

/**
 * Post a send buffer and waits for that one to be completely sent
 * @param trans
 * @param data the size + opaque data.
 */
int rdma_send_wait(rdma_trans_t* trans, rdma_data* data);

// callbacks would all be run in a big send/recv_thread


// server specific:
int rdma_write(rdma_trans_t*  trans, rdma_rloc_t* rdma_rloc, size_t size);
int rdma_read(rdma_trans_t* trans, rdma_rloc_t* rdma_rloc, size_t size);

// client specific:
int rdma_write_request(rdma_trans_t* trans, rdma_rloc_t* rdma_rloc, size_t size); // = ask for rdma_write server side ~= rdma_read
int rdma_read_request(rdma_trans_t* trans, rdma_rloc_t* rdma_rloc, size_t size); // = ask for rdma_read server side ~= rdma_write
