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


#include <stdio.h>	//printf
#include <stdlib.h>	//malloc
#include <string.h>	//memcpy
#include <inttypes.h>	//uint*_t
#include <errno.h>	//ENOMEM
#include <sys/socket.h> //sockaddr

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#include "log.h"
#include "trans_rdma.h"

/* UTILITY FUNCTIONS */

struct ibv_mr *register_mr(rdma_trans_t *trans, void *memaddr, size_t size, int access) {
	return ibv_reg_mr(trans->pd, memaddr, size, access);
}

int deregister_mr(struct ibv_mr *mr) {
	return ibv_dereg_mr(mr);
}

rdma_rloc_t *rdma_make_rkey(uint64_t addr, ibv_mr *mr, uint32_t size) {
	rdma_rloc_t *rkey;
	rkey = malloc(sizeof(rdma_rloc_t));
	if (!rkey) {
		ERROR_LOG("Out of memory!");
		return NULL;
	}

	rkey->rmemaddr = addr;
	rkey->rkey = mr->rkey;
	rkey->size = size;

	return rkey;
}

/* INIT/SHUTDOWN FUNCTIONS */

static int rdma_cma_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event) {
	int ret = 0;
	struct rdma_trans_t *trans = cma_id->context;

	INFO_LOG("cma_event type %s cma_id %p (%s)",
		 rdma_event_str(event->event), cma_id,
		(cma_id == trans->cm_id) ? "parent" : "child") //FIXME: so, do we want to keep both parent and child's id?

#if 0 //FIXME

 	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
	        cb->state = ADDR_RESOLVED;
	        ret = rdma_resolve_route(cma_id, 2000);
                if (ret) {
                        cb->state = ERROR;
                        perror("rdma_resolve_route");
                        sem_post(&cb->sem);
	        }
	        break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
	        cb->state = ROUTE_RESOLVED;
		sem_post(&cb->sem);
                break;
                    
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = CONNECT_REQUEST;
		cb->child_cm_id = cma_id;
		DEBUG_LOG("child cma %p\n", cb->child_cm_id);
	        sem_post(&cb->sem);
                break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("ESTABLISHED\n");

	        /*                                                                                                            
                 * Server will wake up when first RECV completes.                                                             
                 */
	        if (!cb->server) {
		        cb->state = CONNECTED;
		}
	        sem_post(&cb->sem);
                break;

        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_ROUTE_ERROR:
        case RDMA_CM_EVENT_CONNECT_ERROR:
        case RDMA_CM_EVENT_UNREACHABLE:
        case RDMA_CM_EVENT_REJECTED:
                fprintf(stderr, "cma event %s, error %d\n",
                        rdma_event_str(event->event), event->status);
                sem_post(&cb->sem);
                ret = -1;
                break;

        case RDMA_CM_EVENT_DISCONNECTED:
                fprintf(stderr, "%s DISCONNECT EVENT...\n",
                        cb->server ? "server" : "client");
                sem_post(&cb->sem);
                break;

        case RDMA_CM_EVENT_DEVICE_REMOVAL:
                fprintf(stderr, "cma detected device removal!!!!\n");
                ret = -1;
                break;

        default:
                fprintf(stderr, "unhandled event: %s, ignoring\n",
                        rdma_event_str(event->event));
                break;
        }

        return ret;

#endif

}

/**
 * rdma_init_common: part of the init that's the same for client and server
 * 
 * @param ptrans [INOUT]
 *
 * @return 0 on success, errno value on failure
 */
static int rdma_init_common(rdma_trans_t **ptrans) {
	int ret;
	rdma_trans_t *trans = *ptrans;

        trans = malloc(sizeof(rdma_trans_t));
	if (!trans) {
		ERROR_LOG("Out of memory");
		return NULL;
	}
	memset(trans, 0, sizeof(rdma_trans_t));
	
	trans->event_channel = rdma_create_event_channel();
	if (!trans->event_channel) {
		ret = errno;
		ERROR_LOG("create_event_channel failed: %d", ret);
		rdma_destroy_trans(trans);
		return ret;
	}

	ret = rdma_create_id(trans->event_channel, &trans->cm_id, trans, RDMA_PS_TCP);
	if (ret) {
		ret = errno;
		ERROR_LOG("create_id failed: %d", ret);
		rdma_destroy_trans(trans);
		return ret;
	}
	
	trans->state = RDMA_INIT;
	trans->timeout = 30000; // 30s //TODO: find where to use that...
	trans->sq_depth = 10;
	ret = pthread_mutex_init(&trans->lock, NULL);
	if (ret) {
		ERROR_LOG("pthread_mutex_init failed: %d", ret);
		rdma_destroy_trans(trans);
		return ret;
	}
	ret = pthread_cond_init(&trans->cond, NULL);
	if (ret) {
		ERROR_LOG("pthread_cond_init failed: %d", ret);
		rdma_destroy_trans(trans);
		return ret;
	}
	trans->ctx_size = 4096;
	trans->rq_depth = 10;

//	pthread_create(cmthread, NULL, cm_thread, trans?); == rdma_get_cm_event(cm_channel, &event) where struct rdma_cm_event *event + rdma_ack_cm_event(event) + switch on events
	return 0;
}

/**
 * rdma_destroy_qp: destroys all qp-related stuff for us
 * 
 * @param trans [INOUT]
 *
 * @return void, even if the functions _can_ fail we choose to ignore it. //FIXME?
 */
static void rdma_destroy_qp(rdma_trans_t *trans) {
	if (trans->qp)
		ibv_destroy_qp(trans->qp);
	if (trans->cq)
		ibv_destroy_cq(trans->cq);
	if (trans->comp_channel)
		ibv_destroy_comp_channel(cb->comp_channel);
	if (trans->pd)
		ibv_destroy_pd(trans->pd);
}

/** 
 * transrdma_create_qp: create a qp associated with a trans
 *
 * @param trans [INOUT]
 * @param cm_id [IN]
 *
 * @ret 0 on success, errno value on error
 */
static int transrdma_create_qp(rdma_trans_t *trans, struct rdma_cm_id *cm_id) {
	struct ibv_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = trans->sq_depth;
	init_attr.cap.max_recv_wr = trans->rq_depth;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IBV_QPT_RC;
	init_attr.send_cq = trans->cq;
	init_attr.recv_cq = trans->cq;

	if (rdma_create_qp(cm_id, trans->pd, &init_attr)) {
		ret = errno;
		ERROR_LOG("rdma_create_qp: %d", ret);
		return ret;
	}

	trans->qp = cm_id->qp;
	return 0;
}

/**
 * rdma_setup_qp: setups pd, qp an' stuff
 *
 * @param trans [INOUT]
 * @param cm_id [IN]
 *
 * @return 0 on success, errno value on failure
 */
static int rdma_setup_qp(rdma_trans_t *trans, struct rdma_cm_id *cm_id) {
	int ret;

	trans->pd = ibv_alloc_pd(cm_id->verbs);
	if (!trans->pd) {
		ret = errno;
		ERROR_LOG("ibv_alloc_pd failed: %d", ret);
		return ret;
	}

	trans->comp_channel = ibv_create_comp_channel(cm_id->verbs);
	if (!trans->comp_channel) {
		ret = errno;
		ERROR_LOG("ibv_create_comp_channel failed: %d", ret);
		rdma_destroy_qp(trans);
		return ret;
	}

	trans->cq = ibv_create_cq(cm_id->verbs, trans->sq_depth + trans->rq_depth,
				  trans, trans->comp_channel, 0);
	if (!trans->cq) {
		ret = errno;
		ERROR_LOG("ibv_create_cq failed: %d", ret);
		rdma_destroy_qp(trans);
		return ret;
	}

	ret = ibv_req_notify_cq(trans->cq, 0);
	if (ret) {
		ERROR_LOG("ibv_req_notify_cq failed: %d", ret);
		rdma_destroy_qp(trans);
		return ret;
	}

	ret = transrdma_create_qp(trans, cm_id);
	if (ret) {
		ERROR_LOG("our own create_qp failed: %d", ret);
		rdma_destroy_qp(trans);
		return ret;
	}

	INFO_LOG("created qp %p", trans->qp);
	return 0;
}

/**
 * rdma_destroy_buffer
 *
 * @param trans [INOUT]
 *
 * @return void even if some stuff here can fail //FIXME?
 */
static void rdma_destroy_buffer(rdma_trans_t *trans) {
	if (trans->recv_mr)
		ibv_dereg_mr(trans->recv_mr);
	free(ctx) //FIXME
}


/**
 * rdma_setup_buffer
 */
static int rdma_setup_buffer(rdma_trans_t *trans) {
	int ret;

	trans->rfirst = malloc(trans->rq_depth * trans->ctx_size);
	if (!trans->rfirst) {
		ERROR_LOG("malloc failed");
		return ENOMEM;
	}

	trans->recv_mr = ibv_reg_mr(trans->pd, trans->rfirst,
				    trans->rq_depth * trans->ctx_size,
				    IBV_ACCESS_LOCAL_WRITE);
	if (!trans->recv_mr) {
		ret = errno;
		ERROR_LOG("ibv_reg_mr (recv_mr) failed: %d", ret);
		rdma_destroy_buffer(trans);
		return ret;
	}
	
}

/**
 * rdma_bind_server
 * 
 * @param trans [INOUT]
 *
 * @return 0 on success, errno value on failure
 */
static int rdma_bind_server(rdma_trans_t *trans) {
	int ret;
	ret = rdma_bind_addr(trans->cm_id, (struct sockaddr*) &trans->sin);
	if (ret) {
		ret = errno;
		ERROR_LOG("bind_addr failed with ret: %d", ret);
		return ret;
	}
	
	ret = rdma_listen(trans->cm_id, trans->num_accept);
	if (ret) {
		ret = errno;
		ERROR_LOG("rdma_listen failed: %d", ret);
		return ret;
	}

	return 0;
}


/**
 * rdma_create: inits everything for server side.
 * 
 * @param addr [IN] contains the full address (i.e. both ip and port)
 */
rdma_trans_t *rdma_create(sockaddr_storage *addr) {  //TODO make it return an int an' use trans as argument. figure out who gotta malloc/free..
	rdma_trans_t *trans;

	if (rdma_init_common(&trans)) {
		free(trans);
		return NULL;
	}

	trans->sin = addr;
	rdma_bind_server(trans);

	return trans;
}

static rdma_trans_t *clone_trans(rdma_trans_t *listening_trans) {
	rdma_trans_t *trans = malloc(sizeof(rdma_trans_t));
	if (!trans) {
		ERROR_LOG("malloc failed");
		return NULL;
	}

	memcpy(*trans, *listening_trans, sizeof(rdma_trans_t));

	trans->cm_id->context = trans;

	memset(&trans->lock, 0, sizeof(pthread_mutex_t));
	memset(&trans->cond, 0, sizeof(pthread_cond_t));

	ret = pthread_mutex_init(&trans->lock, NULL);
	if (ret) {
		ERROR_LOG("pthread_mutex_init failed: %d", ret);
		rdma_destroy_trans(trans);
		return NULL;
	}
	ret = pthread_cond_init(&trans->cond, NULL);
	if (ret) {
		ERROR_LOG("pthread_cond_init failed: %d", ret);
		rdma_destroy_trans(trans);
		return NULL;
	}

	return trans;
}

rdma_trans_t *rdma_accept_one(rdma_trans_t *rdma_connection) { //TODO make it return an int an' use trans as argument

	//TODO: timeout?

	struct rdma_cm_event *event;
	struct rdma_cm_id *cm_id;
	enum rdma_cm_event_type etype;
	int ret;

	ret = rdma_get_cm_event(trans->event_channel, &event);
	if (ret) {
		ret=errno;
		ERROR_LOG("rdma_get_cm_event failed: %d", ret);
		return NULL;
	}

	trans = clone_trans(rdma_connection);
	child_cm_id?!
	rdma_setup_qp(trans, trans->cm_id)
	rdma_setup_buffers(trans);
	pthread_create(cq_thread);
	rdma_do_accept(trans)
}

/**
 * rdma_destroy_trans: does the final trans free()
 *
 * @param trans [INOUT] the trans to destroy
 */
void rdma_destroy_trans(rdma_trans_t *trans) {
	if (trans->cm_id)
		rdma_destroy_id(trans->cm_id);
	if (trans->cm_channel)
		rdma_destroy_event_channel(trans->cm_channel);
	if (trans->lock)
		pthread_mutex_destroy(&trans->lock);
	if (trans->cond)
		pthread_cond_destroy(&trans->cond);
	free(trans);
}

/*
 * rdma_bind_client
 *
 * 
 *
 */
static int rdma_bind_client(rdma_trans_t *trans) {
	int ret;

	ret = rdma_resolve_addr(trans->cm_id, NULL, (struct sockaddr*) trans->sin, trans->timeout);
	if (ret) {
		ret = errno;
		ERROR_LOG("rdma_resolve_addr failed: %d", ret);
		return ret;
	}

	sem_wait(&trans->sem);
}

// do we want create/destroy + listen/shutdown, or can both be done in a single call?
// if second we could have create/destroy shared with client, but honestly there's not much to share...
// client
rdma_trans_t *rdma_connect(sockaddr_storage *addr) {
	rdma_init_common();
	rdma_bind_client();
	rdma_setup_qp(trans, trans->cm_id);
	rdma_setup_buffers(trans);
	ibv_post_recv?
	pthread_create(cq_thread)
	rdmacat_connect_client()
}

/**
 * rdma_disconnect: disconnect an active connection.
 * Works both on client after rdma_connect and server after rdma_accept_one
 */
int rdma_disconnect(rdma_trans_t *trans) {
	rdma_disconnect
	pthread_join(cqthread, NULL)
	rdma_destroy_buffers(trans);
	rdma_destroy_qp(trans);
}



/**
 * rdma_recv: Post a receive buffer.
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param trans    [IN]
 * @param msize    [IN] max size we can receive
 * @param callback [IN] function that'll be called with the received data
 *
 * @return 0 on success, the value of errno on error
 */
int rdma_recv(rdma_trans_t *trans, uint32_t msize, void (*callback)(rdma_trans_t *trans, rdma_data_t *data)) {
	return 0;
}

/**
 * Post a send buffer.
 * Same deal
 *
 */
int rdma_send(rdma_trans_t *trans, rdma_data *data, void (*callback)(rdma_trans_t *trans));
int rdma_send(rdma_trans_t *trans, rdma_data *data, ibv_mr *mr, void (*callback)(rdma_trans_t *trans, rdma_lloc *loc));

/**
 * Post a receive buffer and waits for _that one and not any other_ to be filled.
 * bad idea. do we want that one? Or place it on top of the queue? But sucks with asynchronism really
 */
int rdma_recv_wait(rdma_trans_t *trans, rdma_data **datap, uint32_t msize);

/**
 * Post a send buffer and waits for that one to be completely sent
 * @param trans
 * @param data the size + opaque data.
 */
int rdma_send_wait(rdma_trans_t *trans, rdma_data *data);

// callbacks would all be run in a big send/recv_thread


// server specific:
int rdma_write(rdma_trans_t *trans, rdma_rloc_t *rdma_rloc, size_t size);
int rdma_read(rdma_trans_t *trans, rdma_rloc_t *rdma_rloc, size_t size);

// client specific:
int rdma_write_request(rdma_trans_t *trans, rdma_rloc_t *rdma_rloc, size_t size); // = ask for rdma_write server side ~= rdma_read
int rdma_read_request(rdma_trans_t *trans, rdma_rloc_t *rdma_rloc, size_t size); // = ask for rdma_read server side ~= rdma_write
